# 4. Transport, rate control, timing and scalability

This section defines how tiles leave the encoder, cross the air, and reach the display, and which control loops keep that path inside its time and bit budgets. Numbers are for the first target: 2 eyes x 2160x2160 rendered side by side (4320x2160), 64x64 tiles (68 x 34 = 2312 tiles per frame), 90 Hz (11.1 ms period), 150 Mbit/s baseline, WiFi 6 or USB 3 tethering.

## 4.1 Datagram layout: tile runs, not tile-as-packet

The brainstorm consensus said "tile = packet". The arithmetic does not support it. At 150 Mbit/s and 90 Hz a frame is 208 KB, so the average 64x64 tile is 90 bytes (22 bytes at 32x32). A 52-byte UDP/IP/codec header per tile would double the bitrate and produce 208k packets/s (832k at 32x32); the Android UDP receive path on an XR2 tops out between 50k and 100k packets/s even with `recvmmsg`. Conversely, at 1 Gbit/s a fovea tile is 4 to 8 KB, above any MTU.

Decision: the datagram is a **tile run**: a contiguous sequence of tiles from one tile row, packed until the payload budget is reached. Each tile remains an independently decodable bitstream; the datagram is only the loss unit. The header enumerates which tiles it carries, so a lost datagram maps to a known set of lost tiles and the per-tile reference tracking (section 4.5) still works. At 150 Mbit/s this gives about 150 datagrams per frame (13.5k pps); at 1 Gbit/s about 1000 per frame (90k pps), which is the honest ceiling of the client receive path and is discussed in 4.10.

Header (fixed 24 bytes, little endian, sent in the clear as AEAD associated data):

| Field | Bits | Notes |
|---|---|---|
| version/flags | 8 | 4 bits version, 4 bits flags: keyframe-run, partial-frame, lossless, last-run-of-frame |
| stream_id | 8 | WiVRn stream (per quad layer / eye pair geometry) |
| frame_id | 16 | wraps every 12 min at 90 Hz |
| tile_first | 16 | linear tile index in lens-space grid |
| tile_count | 8 | tiles in this run (1..255) |
| layer_id | 4 | 0 = base, 1..3 = enhancement |
| ref_delta | 2 | reference frame = frame_id - 1 - ref_delta; 3 = intra (see 4.5) |
| frag_idx / frag_count | 2 + 2 | fragments of an oversize tile (lossless tiles only) |
| pose_seq | 16 | index into the client's own pose history: the render pose the server used |
| path_id / path_seq | 2 + 14 | per-path sequence for loss detection and reordering |
| fec_group / fec_idx / fec_k | 8 + 4 + 4 | see 4.4; fec_idx >= fec_k marks a parity datagram |
| tx_ts | 32 | server clock, microseconds, wraps 71 min |
| payload_len | 16 | bytes of encrypted payload |
| enc_us | 16 | encode-finish minus render-finish for this row band, telemetry |

Total 192 bits = 24 bytes. Per-tile decoder data (QP, mode, byte length) is a 4-byte tile directory entry inside the payload, so a run of 20 average tiles costs 104 header bytes against 1800 payload bytes: 5.5% overhead, versus 30-50% for tile-as-packet.

The pose is never sent downstream. The headset generated the render pose, the server echoes `pose_seq`, and both ends compute the pose delta for warped prediction from the same history. The client keeps a 2 s pose ring (about 1000 entries at WiVRn's tracking rate). This is also what makes frameless presentation (4.3) free: every tile knows the pose it was rendered for.

**MTU.** Ethernet gives 1472 bytes of UDP payload over IPv4; we budget 1400 to leave room for WireGuard/Tailscale-style tunnels users do run WiVRn over, and for 802.11 encapsulation. WiFi A-MPDU/A-MSDU aggregation amortizes per-frame air overhead, so smaller datagrams cost little on the air; they cost in packets-per-second on the headset CPU. USB NCM/RNDIS tethering sometimes accepts a 9000 byte MTU; the connect handshake probes DF datagrams of 1400, 4000 and 8900 bytes on each path and uses the largest that echoes. On a jumbo USB path the run budget rises and pps falls by 6x, which is what makes 1 Gbit/s realistic on USB.

**Oversize tiles.** A tile that exceeds the run budget is not fragmented in normal modes: the rate controller caps each tile at `max_tile_bytes` (1400 - 24 - 4 = 1372 bytes) and the encoding workgroup re-encodes the tile with QP + 6 if the entropy coder overruns, at most twice, in the same dispatch. The exception is lossless mode (UI text tiles), whose worst case is 12 KB for a 64x64 4:4:4 8-bit tile; those are split into up to 4 fragments, and a tile with any fragment missing is a lost tile. The rate controller keeps fragmented tiles below 1% of the stream by demoting lossless tiles to near-lossless when the budget is tight.

**Encryption.** AES-256-GCM per datagram, 16-byte tag, header as associated data, nonce derived from (stream_id, path_id, path_seq, epoch counter) so no nonce goes on the wire; per-path subkeys via HKDF(session_key, path_id). GCM is a stream mode, so no padding and no alignment constraint on the tile bitstreams. ChaCha20-Poly1305 is the negotiated fallback for hosts without AES instructions. Decryption stays on the CPU network thread (ARMv8 crypto extensions do 2 to 4 GB/s per core, far above 125 MB/s at 1 Gbit/s), writing plaintext straight into a host-visible ring buffer the decoder dispatch reads. This is the one deliberate exception to "nothing on the CPU in the hot path": the CPU moves bytes and checks tags, it never parses the bitstream.

Rejected: RTP (12 bytes and a timestamp model that fights per-tile poses), QUIC datagrams (userspace stack cost on the XR2, no gain over raw UDP with our own FEC), IP fragmentation (one lost fragment loses the datagram).

## 4.2 Tile-row pipelining timeline

Encode, send, decode and present overlap per **row band** of 6 tile rows (384 pixel lines; 6 bands per frame, the last one 4 rows). Bands rather than single rows because each Vulkan submit on Adreno costs 50 to 100 us of overhead and each `sendmmsg` call has a floor; 34 dispatches per frame would waste more than they save.

Assumptions: 7900 XTX encodes the whole frame in 3.0 ms (0.5 ms per band), RX 580 in 8 ms; XR2 compute decode budget 4.0 ms per frame (0.67 ms per band); one-way network delay 3 ms on WiFi 6 with a quiet channel, 1 ms on USB; pacing spreads a band's datagrams over its encode time.

| t (ms) | Band 0 | Band 5 |
|---|---|---|
| 0.0 | render finished, band 0 encode starts | |
| 0.5 | encode done, packetize, first send | |
| 3.6 | last datagram of band 0 arrives (WiFi) | |
| 4.3 | band 0 decoded into the frame ring | |
| 3.0 | | encode done, send |
| 6.1 | | arrives |
| 6.8 | | decoded, frame complete |

Frame complete 6.8 ms after render finish on WiFi (4.8 ms on USB), versus the serial path where encode (3 ms), transfer (208 KB at 300 Mbit/s effective = 5.5 ms) and hardware HEVC decode (8 to 15 ms for 4K-class frames through MediaCodec, plus its 1 to 2 frame queue) add to 17 to 25 ms. On the RX 580 the encoder is the long pole (8 ms) and the pipeline finishes 12 ms after render, since network and decoder trail each band by a constant 3.8 ms.

Latency floor, render-finish to photons: 6.8 ms pipeline + compositor phase wait (0 to 11.1 ms, average 5.5) + runtime reprojection and scanout (about 5 ms on the Pico 4 LCD) = 12 to 23 ms. Motion to photons adds pose uplink (1 to 2 ms) and render (up to 11 ms) for a floor of 25 to 35 ms, against about 100 ms today. The gap is mostly waiting: whole-frame encode, MediaCodec's queue, a de-jitter buffer sized for whole-frame bursts. The air term (3 ms plus jitter) is not the long pole; the phase wait is, and only frameless presentation (4.3) and render-side pacing (server section) attack it.

## 4.3 Frameless, deadline-driven presentation

Every Android OpenXR runtime owns the panel. We submit a swapchain image plus the pose it was rendered for; the runtime reprojects to its own predicted display pose and scans out. We cannot present per row to the panel and we cannot change Pico's timewarp. "Frameless" therefore lives inside the WiVRn client's reprojection pass, immediately before `xrEndFrame`.

Mechanism:

1. The decoder writes into a **frame ring** of 4 full-size images per stream (4320x2160, 4:2:0 10-bit, about 60 MB total). Tile (N, t) lands in slot N mod 4.
2. Every tile position has a 4-byte metadata entry in a per-slot SSBO: `pose_seq`, `age` (frames since this position was last decoded, 0 = fresh), and `state` (decoded, concealed, undecodable).
3. When a band's tiles are missing at the deadline, the concealment dispatch fills them by warping the same tile region from slot N-1 by the pose delta between `pose_seq(N-1, t)` and the frame's render pose. This warp is deterministic and bit-exact, and the server reproduces it (4.5), so concealed pixels are legal references.
4. The client's frame loop wakes at `predicted_display_time - reproject_budget - runtime_margin` (runtime margin about 3 ms on Pico). It takes slot N as it stands, runs the reprojection shader with a per-tile pose lookup (each output tile warps from its own `pose_seq` to the requested display pose, sampling from slot N), and submits with the display pose. The runtime's timewarp then has almost nothing left to correct.
5. Tiles arriving after the deadline are still decoded into slot N so they serve as references and so the next frame's concealment starts from the best data. They are reported in the feedback as received-late, which for reference tracking is the same as received.

A frame with any concealed tile carries the partial-frame flag in telemetry; the HUD heatmap shows `age` per tile. Deadline policy: if more than 10% of tiles miss the deadline for 5 consecutive frames, the deadline moves 1 ms earlier (up to 4 ms), trading latency for fewer holes; it relaxes 0.2 ms per clean second. This is WiVRn's adaptive de-jitter buffer restated per band.

True per-row scanout ordering of the panel is out of reach: no Android runtime accepts per-row poses. The design stops at per-tile poses in our own warp.

## 4.4 Prioritized FEC

Loss on WiFi is bursty (an A-MPDU drop takes 8 to 64 consecutive datagrams) and on USB it is rare but comes as stalls. FEC repairs random single losses at fixed latency; re-prediction from acknowledged references (4.5) repairs bursts at the cost of one round trip of degraded prediction. We use both, with FEC deliberately small.

Tile classes are assigned by the foveation map: class A = fovea and base-layer tiles of quad layers (UI), class B = mid-eccentricity, class C = periphery and enhancement layers. Reed-Solomon over GF(256), systematic, k = 10 data datagrams per group, groups never cross a band boundary (so no extra latency), and parity count by class:

| Class | Share of bits | Parity per 10 | Overhead | Rationale |
|---|---|---|---|---|
| A | about 35% | 3 | 30% | a fovea hole is the one artifact users notice at once |
| B | about 40% | 1 | 10% | concealment is good here |
| C | about 25% | 0 | 0% | warped reference is near-indistinguishable |

Blended overhead is about 14.5% of bits, close to today's XOR 8+1 (12.5%) but concentrated where it matters; parity scales to 2/0/0 below 0.1% measured loss and 4/2/1 above 2%. RS rather than XOR because XOR cannot recover 3 of 13, and the CPU cost is under 1% of one core at 150 Mbit/s with NEON GF tables. A low-bitrate band with fewer than 10 datagrams uses k = its datagram count. GPU parity was rejected: the packetized bytes are already in host memory for `sendmmsg`.

**Feedback packet** (client to server, one per band, about 100 bytes, cumulative over the last 3 bands so a lost feedback costs nothing):

```
feedback {
  u16 frame_id; u8 band; u8 flags;
  u32 rx_ts_first, rx_ts_last;           // client clock, us
  u16 decode_us, conceal_tiles, late_tiles;
  u8  received_bitmap[tiles_in_band / 8]; // 68*6 = 408 bits = 51 bytes
  u8  path_loss[2], path_rtt_ms[2];       // per path
  u8  fec_recovered, fec_failed;
}
```

Uplink cost: 6 per frame x 90 Hz x about 100 bytes = 0.4 Mbit/s. The `received_bitmap` covers tiles, not datagrams: a tile is set only if its datagram decrypted and its bitstream decoded without error, so a corrupt-but-delivered tile is treated as lost.

## 4.5 Per-tile reference tracking and the reference epoch

The core problem: pose-warped prediction for tile t reads reference pixels from a window that crosses into neighbouring tiles. If the encoder assumes those neighbours hold frame N-1 and the client actually lost them, prediction diverges silently and the error spreads. The encoder therefore only references frame states it has been told are exact.

Rule: for tile t of frame N, the reference is the newest frame M in {N-1, N-2, N-3} whose 3x3 tile neighbourhood around t is fully acknowledged (received, or lost and therefore concealed by the deterministic warp, which the encoder replays on its mirror ring once the feedback says which tiles were lost). `ref_delta = N-1-M`. If no such M exists, the tile is coded intra (`ref_delta = 3`). Both ends keep the 4-slot ring; the encoder's ring is a mirror of the client's, reconstructed from feedback rather than assumed.

On USB (feedback RTT about 2 ms) nearly all tiles reference N-1; on WiFi (5 to 8 ms) the top bands reference N-1 and the bottom bands often N-2, because feedback for the bottom of N-1 has not arrived when the bottom of N is encoded. Pose-warped prediction makes this cheap: head motion between M and N is compensated by the warp, only scene motion is one frame older. Estimated cost, from HEVC experiments with a fixed 2-frame reference distance, is 5 to 10% more bits at equal PSNR; it is the price of never sending an IDR and is a Phase 2 measurement. With no feedback for 4 frames every tile goes intra at the capped size: a QP jump, not a stall. The invalidate/refresh/IDR ladder and the NVENC DPB=4 workaround both disappear.

## 4.6 Rate control

Inputs: the frame bit budget `B` from the existing controller (AIMD for loss, BBR-style for delivery-rate and RTT-gradient; both produce a target bit/s, and `B = target / fps` minus FEC and header overhead), the foveation map, and the per-tile analysis pass the encoder runs anyway (warped-SAD, variance, mean luminance, warp confidence).

Per-tile weight:

```
w_t = fov_t * percep_t * cplx_t
fov_t    = foveation weight from eccentricity, 1.0 at centre down to 0.15 at the edge
percep_t = 1 / (1 + 0.5 * head_speed_norm) * lum_mask(mean_luma) * class_boost
cplx_t   = clamp(warped_SAD_t / mean_warped_SAD, 0.25, 4)
bits_t   = B * w_t / sum(w)
```

QP from bits with the standard model where bits halve per 6 QP steps: `QP_t = 6 * log2(a_t / bits_t)`, with per-tile state `a_t <- a_t * (actual_t / predicted_t)^0.6` after each frame, clamped to 4x. One-pass predictive, like the R-lambda model in the HEVC reference software but per tile rather than per CTU row; it converges in 2 to 3 frames because a tile position sees similar content frame to frame. Two-pass was rejected: it doubles encoder time on the RX 580, the long pole.

GPU dispatch shape per frame:

1. Analysis: 2312 workgroups, per-tile stats to an SSBO (already required for mode decision).
2. Allocation: one workgroup of 256 lanes does the weight reduction and a prefix sum over 2312 entries, writes QP_t, class_t, budget_t. About 20 us.
3. Encode: 2312 workgroups, each writes its tile bitstream into a fixed-stride slot (1372 bytes, the cap) with its length; overrun handling as in 4.1.
4. Packetize: one workgroup per row computes a prefix sum over tile lengths, greedily assigns tiles to runs under the payload budget, writes headers and copies bitstreams into the host-visible send ring in send order. The CPU thread wakes per band on a timeline semaphore, encrypts in place, computes parity, and issues one `sendmmsg` per band per path.

Overrun at frame level: if `sum(actual) > 1.15 B`, the pacer stretches the sends over the remaining frame period rather than dropping, and the next frame's `B` is reduced by the excess. WiVRn's pacing already does the stretch.

**Scene cuts.** When the analysis pass reports more than 50% of tiles above the intra threshold, the allocator applies a global QP offset so the frame fits 1.5 B (about 3 ms of extra transfer at 150 Mbit/s, absorbed by the band deadline) and quality recovers over the following frames through the rolling refresh, fovea first. HEVC IDR spikes of 4 to 8x the budget stall the current pipeline; a capped 1.5x burst at lower quality replaces them.

**Bitrate range.** The same loop runs from 20 Mbit/s to 1 Gbit/s. Below about 40 Mbit/s an average tile gets 12 to 25 bytes, so periphery tiles drop to the half-resolution base layer and static content goes to skip tiles while the fovea keeps full resolution. Above 400 Mbit/s the QP floor is reached and remaining bits go to lossless UI tiles and 4:4:4 fovea. No codec or profile switch along the way.

### 4.6.1 The degradation ladder: blur, never block

A design requirement, not a tuning detail: when the budget is short the picture must lose texture
before it loses structure. H.264 at low bitrate raises QP everywhere and the eye sees 8x8 and 16x16
blocks. This codec must instead look like a scene whose textures were blurred, or whose surfaces went
low-poly, while edges, outlines and text stay crisp. Almost all of the machinery already exists;
the ladder only fixes the order in which rate control spends it.

Per tile, the encoder classifies content once per frame from two cheap statistics it already
computes for rate control: the log-variance activity term (Section 5.2) and a gradient-coherence
measure (ratio of structure-tensor eigenvalues over the tile, one pass over the pixels). Four classes
come out: **text** (UI stencil or high coherence plus high contrast), **edge** (high coherence),
**texture** (high activity, low coherence), **flat** (low activity). Under budget pressure the
classes descend different ladders:

| Step | Texture tiles | Flat tiles | Edge tiles | Text tiles |
|---|---|---|---|---|
| 1 | Low-pass weighting matrix (Section 1.5) drops high-frequency AC first | Same | QP +2 only | Untouched |
| 2 | `res_level` 1/2, bilinear upsample | `res_level` 1/2 | Untouched | Untouched |
| 3 | `res_level` 1/4 | `res_level` 1/4 | Low-pass weighting, QP +4 | Untouched |
| 4 | DC-plane only: 64 block DCs, planar interpolation | DC-plane only | `res_level` 1/2 | QP +4 within the lossless-or-near class floor |

Each step is what produces the wanted look. Step 1 is blur, because zeroing high-frequency DCT
coefficients is a low-pass filter, and the weighting matrix makes it a smooth one instead of a
threshold. Steps 2 and 3 are blur with no blocking at all, because the tile is coded small and
resampled up. Step 4 is the low-poly look: a tile reduced to its block DCs and planar interpolation
is a smooth gradient field, the piecewise-planar surface the user described, not a mosaic. Edge and
text tiles hold their step until the others are exhausted, so outlines and glyphs survive the frame
that has turned to soft shapes around them.

Cost: the classification is one pass on the encoder GPU per tile and reuses the activity statistic.
The decoder pays nothing it does not already pay; `res_level` upsampling and DC-plane reconstruction
are existing v1 tools. No new syntax. The only new thing is the encoder's ordering, and the ladder is
expressed as the per-class QP floor and `res_level` cap that Section 4.6 feeds into the per-tile bit
allocation.

Two optional v2 refinements, each behind a tool bit: a 1-bit-per-8x8 edge mask carried in the tile
payload so the decoder's `res_level` upsampler picks a sharper 4-tap kernel on edge blocks and the
bilinear elsewhere (about 8 bytes per tile, decoder cost one branch per block); and a per-tile
"contour" mode that codes the tile as a DC plane plus one straight edge with two side values, which
is the true low-poly primitive at a few bytes per tile. Neither is needed for the look; both make it
cheaper.

What "cheaper than H.264" means here, stated plainly: on the Pico 4 the H.264 decoder is fixed
function and costs no GPU time at all, so this codec cannot beat it on headset GPU cycles. It beats
it on bits per frame during head motion, on latency by the row-band pipeline, on loss behaviour by
per-tile references, and on what a low bitrate looks like. Those four are the contest.

## 4.7 Decode-time governor

The client reports `decode_us` per band and per frame plus a GPU frequency state bit from the Adreno sysfs node. The target is decode at or under 40% of the frame period (4.4 ms at 90 Hz), leaving the reprojection pass and the runtime's own compositor their share.

Knobs, in order, each step applied server-side at a band boundary:

1. Drop enhancement layers on class C tiles.
2. Class C tiles to the half-resolution base layer.
3. Shrink the full-resolution fovea radius by 10%.
4. Drop enhancement layers on class B.
5. Refresh rate 90 to 72 Hz (a runtime call on the client, coordinated over the control channel).

Step down when 3 consecutive frames exceed 110% of target or any frame exceeds 150%; step up one knob after 180 consecutive frames (2 s) under 70% with no deadline misses. The asymmetry is deliberate: XR2 thermal throttling arrives in seconds and leaves in minutes, and a governor that hunts every 100 ms makes the periphery shimmer.

The governor never touches bits, only pixels-of-work; the bits it frees are redistributed by the allocator. The two loops run on different timescales (bitrate: 100 ms to 1 s; governor: 3 frames down, 2 s up) and different variables, so they do not fight. The coupling is entropy decode, which is proportional to bits (about 1 bit per lane-cycle with rANS): 4800 bits per tile per frame at 1 Gbit/s is comfortable on 64-lane workgroups, and the fifth knob exists for when it is not.

## 4.8 Multipath and multi-user

WiVRn NX already stripes USB and WiFi. The tile-run header carries `path_id` and a per-path sequence so loss and reordering are measured per path, and the datagram rather than the byte stream is the unit, so a stall on one path never blocks the other.

Striping policy: class A runs are sent on both paths when the combined bandwidth allows it (duplication costs at most 35% of bits and only when both paths are up); class B and C runs are split by weighted round robin on the measured delivery rate of each path (BBR's estimate, per path). Reordering window: tiles are position-addressed and land in the ring by (frame_id, tile) regardless of arrival order, so no reorder buffer exists for decoding; the only wait is the FEC group's, which is bounded by the band deadline. Per-path keys as in 4.1. When a path stalls (RTT sample above 3x its baseline, or 20 ms with no datagram received while the other path is flowing), its weight goes to zero within one band and probing resumes with class C runs only.

**Multicast, honestly.** 802.11 multicast goes out at a basic rate (6 Mbit/s or lower), unacknowledged and unretried; a 100 Mbit/s base layer cannot be multicast on any consumer access point, and APs that convert multicast to unicast just send N copies with more loss than the server would. Decision: no multicast. Multi-user shares the encode, not the air: one encode of the shared base tiles, N unicast sends, per-user fovea tiles per user. Wired multi-user through a switch could use IP multicast later.

## 4.9 Latency telemetry

Stamps, per band, in two clocks:

| Stamp | Clock | Carried in |
|---|---|---|
| pose sample time | client | pose history (via pose_seq) |
| render finish | server | frame metadata, first run of band 0 |
| encode finish | server | `enc_us` in header |
| tx | server | `tx_ts` in header |
| rx first / last | client | feedback |
| decode finish | client | feedback (`decode_us`) |
| compose submit, predicted display | client | local, feedback flags |

WiVRn's clock offset estimator (ping/pong, minimum-filtered) gives the offset to about 0.3 ms on USB and 1 ms on WiFi, estimated per path since the two have different asymmetries; one-way delay = rx - tx - offset. The HUD shows a stacked bar per stage at p50/p99 over the last second (render, encode, queue, air, decode, deadline wait, runtime), the staleness heatmap, per-path rate/loss/RTT, FEC recovered/failed, re-predicted tiles per frame, decode ms against the governor target, and a histogram of frame-ready-to-deadline margin, the one number that says whether the deadline should move.

Audio and pose/input are out of scope; two couplings: the client pose ring must outlive the longest `pose_seq` in flight (2 s), and audio runs on the server presentation clock, so it does not stall on partial frames.

## 4.10 What others do and what is different here

- **ALVR**: hardware H.264/HEVC/AV1, NAL units over UDP, no FEC in current versions, loss handled by requesting an IDR; foveation by rendering a warped image, not in the codec.
- **Meta Link / Air Link**: hardware codecs through MediaCodec; Meta's developer documentation describes sliced encoding so encode, transfer and decode overlap per slice, dynamic bitrate, and Phase Sync for frame timing. Slices of a standard codec, frame-level references.
- **Steam Link**: H.264/HEVC with Valve's own transport and dynamic bitrate/resolution; codec detail not public.
- **NVIDIA CloudXR**: sliced hardware encode with per-slice packetization, frame-level references.
- **PSVR2**: DisplayPort, no compression. It sets the latency bar this design aims to approach on WiFi.
- **Apple Vision Pro Mac Virtual Display**: reported as a proprietary low-latency foveated stream over WiFi 6E; details not public.

Differences: the loss unit is a run of self-contained tiles, not a slice whose reference is a whole frame; the reference is what the client acknowledged, so there is no IDR path; the pose is shared state indexed by sequence number, not transmitted; partial frames are presented on a deadline with deterministic concealment that remains a valid reference; feedback is per band; FEC strength follows the foveation map.

## 4.11 Failure modes and risks

- **WiFi bufferbloat.** APs and the headset driver hold hundreds of milliseconds of queue. Pacing and BBR keep our own queue shallow; another client saturating the AP shows as RTT growth within a band and is answered with bitrate, not buffering. It appears in the heatmap as bottom-band holes first.
- **Android receive path.** WiFi power save adds 100 ms and must stay off (WiVRn's high-performance lock); the receive interrupt sits on one core of the XR2 and saturates around 80k pps. Jumbo MTU on USB is the mitigation; 1 Gbit/s over WiFi is not a first-release target.
- **USB tether stalls.** RNDIS on Linux hosts stalls 50 to 200 ms at intervals; NCM is better. Multipath switches within one band; a single-path USB user gets one warped frame.
- **Socket buffers.** Android defaults to about 200 KB of `SO_RCVBUF`; a 1.5x burst at 400 Mbit/s is 800 KB. Request 8 MB, verify it was granted (vendor ROMs cap `rmem_max`), else lower the burst cap.
- **Clock offset error.** A 1 ms error shifts every one-way measurement; the deadline controller therefore works only on the client-clock margin, so telemetry error never steers presentation.
- **Reference distance on WiFi.** The N-2 cost is estimated, not measured; if it exceeds 15% the fix is feedback per half band at 0.8 Mbit/s uplink.
- **Thermal.** Compute decode heats the XR2 more than the hardware decoder; the Phase 0 gate must measure decode watts, not only milliseconds.
- **Adreno submit overhead and timeline semaphore latency** (assumed 50 to 100 us and under 200 us) are unverified; if worse, bands become 3 per frame and the floor grows by about 1.5 ms.
