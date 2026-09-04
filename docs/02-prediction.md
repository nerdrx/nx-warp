# 2. Prediction, references and loss concealment

This section specifies the inter-frame predictor of the codec: pose-warped prediction with per-tile motion corrections, the reference model shared between encoder and decoder, what happens when tiles are missing, and how the same machinery drives frame-rate scaling and the hybrid HEVC mode. Numbers assume the Pico 4 target: two eyes at 2048x2048 streamed (8.4 Mpix per frame), 90 Hz, 32x32 tiles (8192 tiles) or 64x64 tiles (2048 tiles).

## 2.1 The predictor in one sentence

For every tile, the prediction is the previous decoded frame resampled through a global per-eye homography (from the head pose delta) plus a per-tile 2D vector. Nothing else. Depth, engine velocity buffers, stencil masks and the search all live on the encoder; the decoder only ever sees "homography + per-tile vector + mode".

Rationale: any positional (translation-induced) parallax, once the depth is approximated as constant per tile, collapses to a per-tile 2D shift (the plane-induced homography H = K (R - t n^T / d) K^-1 differs from the rotation-only homography by a term that is constant within a fronto-parallel tile to first order). So a per-tile motion vector subsumes "positional warp with per-tile depth" and also covers moving objects. Sending depth to the decoder would carry no extra information for the same bit cost, and per-pixel depth warping (forward splatting, hole filling) is out of the Adreno 650 budget and creates the disocclusion problem we otherwise do not have.

Decision: rotation-only global warp, per-tile MV correction, no depth on the decoder. Per-pixel depth warp rejected (cost, holes, needs a depth stream). Per-tile plane homography rejected (second-order gain over a shift for 32 px tiles, costs 4 extra parameters per tile).

## 2.2 Rotation-only reprojection

The server rendered frame N-1 with view rotation R_{N-1} and frame N with R_N (per eye, including the IPD offset in the eye pose; only the rotation part is used). For a target pixel x in frame N the reference position is

    x_ref = H x,   H = K_e * R_{N-1}^T * R_N * K_e^{-1}

with K_e the eye's (asymmetric) projection in streamed-pixel units. This is the same matrix Oculus TimeWarp and the WiVRn reprojection shader already use, only applied in the opposite direction (target to source). Roll and the perspective divide are handled exactly, which is where block-translation codecs lose: a 3 degree roll moves the frame corners by 50 px in opposite directions and HEVC needs a different MV for every block, plus sub-pel refinement, to follow it.

Magnitudes at 90 Hz: a 300 deg/s head turn is 3.3 deg per frame, about 70 px at the 2048 px / 95 deg FOV of the Pico 4. HEVC hardware encoders at low-latency presets have effective search ranges of 32 to 64 px and fall back to intra on such frames, which is the visible "bitrate spike on head turn" WiVRn users know. The warp makes that motion cost zero bits.

Disocclusion: with rotation only there is none except at the frame border. The strip revealed on the leading edge (up to 70 px at 300 deg/s, i.e. two or three tile columns) is predicted from clamp-to-edge reference samples and the encoder will pick intra for those tiles. Because the predictor is always dense (a homography plus a shift never produces holes) the decoder never needs a hole-filling pass.

### Determinism: integer warp

The encoder runs the decoder to build its references, so both sides must compute identical predictions. Floating point in shaders is not portable: Vulkan permits 2.5 ULP for fp32 division, FMA contraction differs between compilers, Adreno honours RelaxedPrecision aggressively, and AMD, NVIDIA and Qualcomm all round differently. A single ULP difference in a sampling coordinate flips a rounding decision and the mismatch then propagates through every later frame. The warp is therefore defined in integer arithmetic only:

1. The server computes H per eye in double precision, then quantises it to nine int32 in Q8.24 (after scaling so h22 = 2^24). These 36 bytes per eye go in the frame header. The encoder uses the quantised matrix itself; the quantisation error lands in the residual.
2. Per tile, the decoder computes source coordinates for the four tile corners only:
   num_x = h00*x + h01*y + h02, num_y = h10*x + h11*y + h12, den = h20*x + h21*y + h22, all as 64-bit products via OpUMulExtended/OpSMulExtended (core SPIR-V, no shaderInt64 required), then x_src = (num_x << 6) / den with a fixed 32-iteration restoring division. Result in Q.6 (1/64 pel). Four divisions per tile, not per pixel.
3. Inside the tile the source coordinate is bilinearly interpolated from the four corners with integer adds (the homography is smooth enough that the interior error at 32 px is under 1/32 pel for any head rotation that occurs at 90 Hz; at 64 px tiles it stays under 1/16 pel below 250 deg/s, which is one reason 32x32 is the default in the Full profile).
4. The per-tile MV (1/4 pel, in Q.2) is added, the sum is rounded to 1/16 pel, and the sample is taken with an integer filter: bilinear (weights 0..16, Lite profile) or 4-tap Catmull-Rom with integer coefficients over 64 from a 16-entry table (Full profile). Rounding is "add half, shift", defined once.

The result is bit-exact on every Vulkan implementation by construction; the CPU reference decoder used for fuzzing implements the same 30 lines. Prior art: MPEG-4 Part 2 global motion compensation (sprite warping, 1999, patents expired) also warped corner points with integer arithmetic and interpolated; AV1's global motion uses the same corner-then-interpolate structure under a royalty-free licence. Neither ties the parameters to a tracked pose; that step should get a patent search (Meta, Qualcomm and NVIDIA have filings on pose-based prediction for split rendering, 2016 to 2020). The AV1 and MPEG-4 prior art makes the mechanism itself safe.

Resampling blur: with a moving head every tile is resampled at a fractional position every frame, unlike a static camera where skip blocks copy exactly. Bilinear resampling applied 90 times per second turns fine texture to mush within about a second unless the residual keeps correcting it, and correcting costs bits. This is why Catmull-Rom (slight sharpening lobe) is the Full profile default despite 4x the sampling cost; Phase 2 must measure the PSNR decay of a warp-only chain over 2 s for both filters.

### Decoder cost

Per pixel: coordinate interpolation 4 ops, MV add and rounding 3, filter fetch 4 (bilinear) or 16 (Catmull-Rom) LDS reads with 8 or 24 MACs, residual add and clamp 3. Total about 20 ops (Lite) or 50 ops (Full). The reference region of a tile (tile plus MV range plus filter margin, at most 48x48 for a 32x32 tile with |MV| <= 6 px after warp) is loaded once into shared memory: 2.25 texel reads per pixel amortised. Larger vectors fall back to direct texture fetches for that tile. Memory traffic: about 5 bytes per pixel (read reference 4:2:0, write output), 3.8 GB/s at 8.4 Mpix x 90 Hz, under 10 percent of the memory bandwidth. Prediction is roughly a quarter of the decoder's per-pixel budget; entropy decode and the inverse transform take the rest.

## 2.3 Residual motion: per-tile vectors

Mode per tile (3 bits, in the tile header):

| Mode | Reference | Vector | Residual | Typical bits per 32x32 tile |
|---|---|---|---|---|
| WARP_SKIP | warp(prev) | 0 | none | 3 to 4 |
| WARP_MV | warp(prev) | coded | coded | 40 to 1500 |
| STATIC_MV | prev (identity, no warp) | coded | coded | as WARP_MV |
| STEREO | decoded left eye of this frame | coded (disparity) | coded | as WARP_MV |
| INTRA | none | none | coded | 1500 to 4000 |

STATIC_MV exists for head-locked content (menus, HUDs, laser pointers, the WiVRn transport HUD): the warp is exactly wrong there and the identity predictor is exactly right.

MV coding: the MV is coded in the tile's own substream as a delta from the same tile's previous vector (temporal prediction from per-tile decoder state; see 2.6), signed Exp-Golomb, range +-64 px at 1/4 pel. Spatial prediction from neighbouring tiles (H.264 median) is rejected because it makes tiles depend on each other; the temporal predictor costs nothing and gives zero-delta vectors for constant parallax and constant-velocity objects. Cost at 8192 tiles and a typical 3 bits per coded vector: about 2 Mbit/s at 90 Hz, negligible above 50 Mbit/s and the reason the Lite profile at 20 Mbit/s uses 64x64 tiles.

Sub-pel: yes, 1/4 pel. The warp already lands on 1/64 pel positions, so sub-pel MVs cost no extra filter machinery; the two fractions simply add. Measured in every codec since H.264, quarter-pel is worth 10 to 15 percent on textured content over integer-pel and the marginal gain of 1/8 pel is not worth the coding cost.

Encoder search (compute shader, one workgroup per tile, runs on the PC GPU):

1. Candidate seeds: zero; the tile's previous MV; the parallax vector f * t_lateral / d_tile from head translation and per-tile depth if a depth buffer exists (XR_KHR_composition_layer_depth is already a standard extension and Monado exposes it); the median of the engine velocity buffer over the tile if supplied; the disparity seed f * IPD / d_tile for STEREO.
2. Coarse search: 4x downsampled tile (8x8 samples for a 32x32 tile), full search +-16 px (33x33 = 1089 candidates x 64 samples = 70 k SAD ops per tile, 0.6 G ops per frame, under 0.2 ms on an RX 580) around the best seed.
3. Refinement: +-1 px integer diamond, then the 8 quarter-pel neighbours, on the full-resolution warped reference with the real interpolation filter.
4. Decision by rate-distortion cost D + lambda * R for each mode using SATD (4x4 Hadamard) for D and a bit estimate for R; head translation per frame (11 mm at walking speed) gives parallax of 12 px at 1 m and 40 px at 30 cm, so the +-16 px coarse range is enough for the world and the depth seed carries the hands.

Engine inputs via a proposed vendor OpenXR extension (one extra composition layer struct chained per projection view): velocity buffer (RG16F, screen-space motion in pixels per frame, the same buffer engines produce for TAA), depth (already standard), and an 8-bit stencil with bits for head-locked, lossless text and transparent. They plug in as: velocity replaces search step 2 with a single candidate (verified by SATD, never trusted blindly); depth seeds parallax and STEREO; stencil forces STATIC_MV, lossless intra, or higher lambda tolerance for alpha regions. None of this changes the bitstream.

## 2.4 Where the warp fails and what it costs

Content the warp cannot predict: objects moving in the world, head-locked UI, the player's own hands and controllers, mirrors, transparent and additive layers whose visible result changes with the background, full-screen post effects (bloom flashes in Beat Saber light shows), and disoccluded borders. Estimated area coverage and bit cost per frame at a quality equivalent to HEVC at 150 Mbit/s, 32x32 tiles, 8.4 Mpix:

| Content | VRChat | Beat Saber | HL: Alyx | Mode | bits per pixel |
|---|---|---|---|---|---|
| Static world after warp | 60 to 75 % | 75 to 85 % | 80 to 88 % | WARP_SKIP / WARP_MV small residual | 0.01 to 0.08 |
| Moving avatars / enemies / blocks | 10 to 20 % | 5 to 10 % | 3 to 8 % | WARP_MV | 0.4 to 1.0 |
| Own hands, controllers, weapon | 3 to 5 % | 3 % | 6 to 10 % | WARP_MV (depth seed) | 0.3 to 0.8 |
| Head-locked UI / HUD | 0 to 30 % | 2 % | 1 % | STATIC_MV, mostly skip | 0.02, text tiles lossless 1 to 2 |
| Mirrors, particles, light shows | 0 to 30 % | 0 to 40 % (bursts) | 2 % | WARP_MV / INTRA | 1.0 to 3.0 |
| Border disocclusion (300 deg/s) | 3 % | 3 % | 3 % | INTRA | 2.0 to 3.0 |

Worked frame (VRChat, moderate head motion, no mirror): 70 % x 0.04 + 15 % x 0.7 + 4 % x 0.5 + 5 % x 0.02 + 3 % x 1.5 + 3 % x 2.5 = 0.28 bpp, about 2.3 Mbit per frame, 210 Mbit/s at 90 Hz for HEVC-150 quality. That is honest: at rest the codec is roughly at parity with a good HEVC encoder, because HEVC's per-block translation approximates a small rotation reasonably well. The bitrate win is concentrated in the frames where HEVC breaks (fast turns, roll, sub-pel drift over textured floors), typically a 2x to 4x reduction on those frames, which is exactly where the AIMD controller today drops quality. The rest of the case for the predictor is latency and loss behaviour, not average bitrate.

## 2.5 Stereo inter-view prediction

The right eye can predict from the decoded left eye of the same frame (STEREO mode), with a per-tile disparity vector seeded from f * IPD / d and refined by the search. MVC (H.264 Annex H) reports 20 to 25 percent savings on the dependent view versus simulcast, MV-HEVC 25 to 30 percent, on camera-captured content. Rendered VR content is ideal for it (perfect vertical alignment, identical lighting) but the temporal reference right(N-1) is a better predictor than left(N) for everything that was already visible last frame. Inter-view mode therefore mostly replaces INTRA tiles: content that is new to both eyes at once (disoccluded strips, spawned objects, scene transitions) is coded once and copied. Expected overall gain: 5 to 10 percent on average, 30 to 40 percent on intra-heavy frames, which flattens exactly the bit spikes we care about.

Pipeline consequence: right-eye tiles that use STEREO must decode after their left-eye reference. Dispatch order per frame is L row r, R row r, interleaved, so with tile-row pipelining the right eye lags by one row's decode time (about 40 us) and total decode time is unchanged. A STEREO tile whose left reference tile has not arrived by the deadline is treated as lost (concealed, NACKed); the encoder's shadow model (2.6) handles it like any other loss. With multipath striping, left and right tiles of the same row are put on the same path where possible so out-of-order arrival between paths does not stall the right eye. Alternative rejected: predicting right(N) from warped left(N-1), which needs no ordering but loses the one case (new content) where stereo helps. STEREO is Phase 4 and off in the Lite profile.

## 2.6 Reference model

References: exactly one previous decoded frame per eye (the frame buffer is ping-ponged, because the warp reads outside the tile the previous frame must be complete and immutable while the next decodes) plus, for the right eye, the current left eye. No DPB, no long-term references, no B-frames.

Per-tile decoder state (16 bytes, 128 kB at 8192 tiles):

| Field | Size | Use |
|---|---|---|
| held_frame_id | 32 bit | frame whose data this tile last decoded successfully |
| last_mv | 2 x 16 bit | temporal MV predictor and concealment vector |
| age_since_intra | 8 bit | refresh scheduling, drift bound |
| concealed_count | 8 bit | consecutive frames concealed |
| mode, qp, flags | 32 bit | last mode, quantiser, "pixels are extrapolated" flag |

The client sends, once per frame, a bitmap of received tiles (1 kB at 8192 tiles) for the last four frames, piggybacked on the pose packet that already goes at 500 Hz or so; loss of that packet costs nothing since the next one repeats the history.

Encoder shadow: the encoder keeps the last K = 8 frames (90 ms) as bitstream plus decoded pictures, and a "client shadow" frame buffer that mirrors what the headset holds. It encodes frame N+1 optimistically from its own decode of N. When the bitmap reports that tile t of frame N was lost, the encoder replays: shadow_N = for each tile, decode(N) from shadow_{N-1} if received, else conceal(shadow_{N-1}) with the same integer kernel the client used. The replay is a full-frame decode on the PC GPU (about 0.2 ms on a 7900 XTX, 1 ms on an RX 580) and is exact because concealment is deterministic and the client's decode of received tiles used the same shadow reference. From that frame on, the encoder predicts from the true client state; every tile whose prediction footprint touched the concealed region gets a residual computed against what the client actually shows, and the drift is corrected in a single frame at the tile's normal quantiser.

This is the key property: after a loss, visible drift lasts one RTT plus jitter (10 to 30 ms on WiFi 6), then heals completely without an intra refresh and without a frame-level IDR. The existing invalidate -> refresh -> IDR ladder in WiVRn NX degenerates to: shadow resync (always), per-tile intra (if the loss is older than K frames or the tile has been concealed 3 times), full intra (only on stream start, profile change, or bitmap history gap).

Rolling intra refresh stays, for three reasons: bitmap gaps, shadow model bugs, and late joiners in multi-user. Every frame, 1/T of the tiles are coded INTRA regardless of mode decision, T = 180 (2 s), selected by a fixed pseudo-random permutation of tile indices (no visible wave, unlike x264's column-based refresh), and forced also when age_since_intra > T. Cost: 1/180 of the frame at 2.5 bpp is about 0.014 bpp, under 5 percent of the budget.

Multipath and ordering: within a frame tiles are independent and arrive in any order; a tile of frame N that arrives after the frame N deadline is discarded (the frame buffer has moved on) and stays reported as lost, so the shadow model remains consistent. The only intra-frame dependency is STEREO, handled above.

## 2.7 Loss concealment

At the presentation deadline (vsync minus decode time minus reprojection time minus margin, measured by the adaptive de-jitter logic already in the client) the decoder dispatches with whatever arrived. A missing tile runs the same prediction kernel in WARP_SKIP with vector = last_mv (objects keep sliding, the world stays locked to the head), sets the "extrapolated" flag and increments concealed_count. This makes concealment identical to a legitimately skipped tile, which is why the encoder can replay it exactly. There is no separate concealment code path to test.

Stale reference policy: the error of a concealed tile is bounded by the true per-frame change of its content, so a static world stays perfect indefinitely while a moving hand becomes wrong after two or three frames. The client does nothing clever about that; it keeps reporting the tile as missing and the encoder escalates: first replay-resync (inter from shadow), then after concealed_count >= 3 or when the lost frame is older than K, INTRA for that tile with elevated FEC priority. The reprojection shader may optionally read the extrapolated flag per tile and blend the tile 20 percent toward the previous output to hide flicker, which is a client-side choice and does not affect the reference. Rejected: any client-side inpainting of lost tiles (non-deterministic relative to the encoder, and worse than the warp on VR content).

## 2.8 Temporal decoupling and frame-rate scaling

When the server sends at 45 Hz and the panel runs at 90 Hz, the client synthesises the in-between frame from its latest decoded frame using the same warp with its own newest pose (this is ordinary asynchronous timewarp and already exists in the reprojection shader) plus the per-tile residual-motion field (the coded MV minus the warp-induced shift at the tile centre, i.e. the part of the motion that is not the head) extrapolated by half a frame. That field is exactly what the WiVRn NX motion-smoothing feature computes today with a server-side block matcher and sends alongside the video. Decision: when the codec is active, the block matcher is retired and its client-side warp consumes the codec's MV field, which is free and already tile-aligned. Caveat, the same one Oculus ASW 1.0 had when it used the video encoder's motion estimation: coded MVs are rate-distortion choices, not true motion, so the encoder search adds a smoothness penalty (lambda_s times the difference from the neighbouring tiles' vectors) that biases toward physically plausible fields at negligible bit cost. Tiles in STATIC_MV are excluded from extrapolation (head-locked content must not be warped), which fixes a known motion-smoothing artefact on menus for free. The frame-rate governor can then trade server render rate for bitrate per frame without changing the decoder's per-frame cost.

## 2.9 Hybrid mode (hardware HEVC base)

For headsets that cannot run the full decoder, the base layer is a plain HEVC stream through MediaCodec and the enhancement layer is decoded in compute. HEVC decoding is normative and bit-exact, so the encoder can mirror the base by decoding its own HEVC stream (hardware decode on the PC, adding 1 to 2 ms to the encode pipeline). Each enhancement tile chooses between two predictors: (a) the upsampled base tile of frame N (LCEVC-style, drift-free, no motion needed) and (b) warp(Out(N-1)), the pose-warped previous full-resolution output, which retains the detail the base lacks. The residual codes Out(N) minus the chosen predictor with the same transform and entropy tools; the mode table gains one entry (BASE) and everything else is unchanged. The hybrid mode gives up per-tile loss behaviour on the base (a lost base packet goes through HEVC's own reference invalidation) and inherits the 10 to 20 ms MediaCodec latency, so it is the compatibility fallback, not the low-latency path. Its value is that the enhancement layer, the tiling, the transport and the shadow model are shared with the full codec, so it is not a second codec to maintain.

## 2.10 Pseudo-code

Decoder, one workgroup (16x16 threads, 4 pixels each) per 32x32 tile:

```
predict_tile(tile, hdr, frame_hdr, ref, left_ref, out, state):
    H    = frame_hdr.H[tile.eye]                        // 9 x int32, Q8.24
    mv   = state[tile].last_mv + hdr.mv_delta           // Q.2 (1/4 pel)
    if hdr.mode == INTRA:           pred = 0
    elif hdr.mode == STEREO:        src = left_ref; corner[i] = tile_corner[i] << 6
    elif hdr.mode == STATIC_MV:     src = ref;      corner[i] = tile_corner[i] << 6
    else:                           src = ref
        for i in 0..3:                                 // 4 corners, 64-bit int math
            nx = H00*cx[i] + H01*cy[i] + H02;  ny = H10*cx[i] + H11*cy[i] + H12
            d  = H20*cx[i] + H21*cy[i] + H22
            corner[i] = ( (nx << 6) / d, (ny << 6) / d )   // Q.6, restoring division
    bbox = bounds(corner) + mv + filter_margin
    if bbox fits 48x48: load src[bbox] into shared memory (clamp-to-edge)
    for each pixel p owned by this thread:
        c   = bilerp_int(corner, p.local)                // Q.6, integer adds only
        c  += mv << 4;  c = (c + 2) >> 2                 // Q.4 (1/16 pel)
        v   = filter16(src_or_lds, c)                    // bilinear or Catmull-Rom, integer
        out[p] = clamp(v + residual[p], 0, maxval)       // residual from IDCT stage
    if hdr.mode != INTRA and hdr.mode != STATIC_MV: state[tile].last_mv = mv
    state[tile].held_frame_id = frame_hdr.id; state[tile].age_since_intra++ or = 0
```

Encoder, per tile, after the frame's global H is quantised:

```
decide_tile(tile, cur, shadow_prev, cur_left_decoded, depth, velocity, stencil, state, lambda):
    seeds = {0, state.last_mv}
    if depth:    seeds += parallax(head_translation, depth_median(tile))
    if velocity: seeds += median(velocity[tile])
    if stencil.head_locked: force = STATIC_MV
    if stencil.text:        force = INTRA_LOSSLESS
    cands = []
    cands += (WARP_SKIP,  mv=state.last_mv)              // free vector, no residual
    for ref, mode in [(warp(shadow_prev), WARP_MV), (shadow_prev, STATIC_MV), (cur_left_decoded, STEREO if right eye)]:
        mv = refine_qpel(diamond(full_search_4x(ref, seeds, +-16)))
        cands += (mode, mv, residual = cur - sample(ref, mv))
    cands += (INTRA)
    if rolling_refresh_due(tile) or state.age_since_intra > T or state.concealed_count >= 3: cands = [INTRA]
    best = argmin over cands of SATD(cur - reconstruct(cand, qp)) + lambda * bits(cand) + lambda_s * |mv - neighbour_mv|
    encode(best); update shadow tile with reconstruct(best)  // encoder runs the real decoder
```

## 2.11 Risks and the Phase 2 experiments

1. Parity risk: the warp may not beat HEVC's block ME at rest by any margin, and the gain during head motion may be smaller than the 2x to 4x estimated. Kill test: record 60 s each of VRChat, Beat Saber and Alyx as raw frames plus pose logs from WiVRn; encode with x265 (zerolatency, P-only) and with the Phase 2 codec; report BD-rate overall and on the 20 percent of frames with the highest angular velocity. Success: within 10 percent at rest and at least 30 percent better on the motion frames. Failure means the codec's case rests on latency and loss behaviour alone, which should be decided explicitly rather than assumed.
2. Resampling blur: warp-only chains may degrade faster than the residual can affordably fix. Test: PSNR of a 2 s warp-only chain under recorded head motion with bilinear and Catmull-Rom; if the Full profile filter does not hold above 35 dB for 30 frames on textured content the per-tile refresh rate must rise and the bit budget in 2.4 is wrong.
3. Adreno budget: the prediction kernel plus entropy decode plus inverse transform must fit about 4 ms at 8.4 Mpix x 90 Hz including the LDS staging. Phase 0 gates this; Phase 2 re-measures with the real kernel, and the fallback is 64x64 tiles with bilinear.
4. Shadow model correctness: any divergence between encoder shadow and client state is a permanent artefact until the next refresh. Test: loss injection (random, bursty, per path) in the PC-side simulator with a bit-exact assertion of shadow versus the real decoder every frame; the fuzzer must run for hours with zero mismatches before Phase 3.
5. Missing depth and velocity: most OpenXR applications submit neither, so the search must carry hands at 40 px parallax alone; the coarse +-16 px range plus the temporal seed may miss fast hands and the encoder must fall back to intra gracefully rather than smear.
6. Head-locked and mirror content: STATIC_MV covers the UI, but mirrors (very common in VRChat) are effectively a second moving camera and will cost intra-level bits; no tool here fixes that.
7. Patents: the integer warp follows expired MPEG-4 GMC and royalty-free AV1 global motion, but "pose delta as global motion parameters for a streamed VR frame" needs a proper search against Meta, Qualcomm, NVIDIA and Microsoft filings before anything ships.
8. Foveation interaction (cross-section): the homography is exact in linear render space, not in a foveated (non-uniform) streamed space. The foveation section must define the foveation map as an integer LUT so the warp can be composed as foveated -> linear -> warp -> linear -> foveated deterministically, at one extra LUT read per pixel. Phase 2 runs without foveation.
