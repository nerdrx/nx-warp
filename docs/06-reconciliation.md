# 6. Reconciliation: decisions across sections

The five sections were written in parallel from one shared brief. Where they disagreed, or where one
section overturned a line of the brief, this section records the decision that the rest of the paper
now follows.

## 6.1 The datagram is a tile run, not a tile

The brief said "tile = packet". Sections 3 and 4 independently rejected it by arithmetic: at 150 Mbit
and 90 Hz a 64x64 tile averages about 90 bytes, so one datagram per tile would double the bitrate in
headers and push the headset past its UDP receive ceiling.

| Quantity | Value |
|---|---|
| Average tile payload at 150 Mbit, 90 Hz, 64x64 | ~90 bytes |
| Datagrams per second if tile = packet | 208 k |
| XR2 Gen 1 practical UDP receive ceiling | 50 to 100 k pps |
| Datagrams per second with 1400-byte tile runs | ~13 k |

Decision: a datagram carries a run of contiguous tiles from one tile row, packed to a 1400-byte
budget, with a 4-byte per-tile directory. Tiles remain independent bitstreams. The datagram is the
loss unit, the tile is the concealment unit. Only lossless UI tiles may fragment, to at most four
datagrams, and only in the Pro profile. Every mention of tile-as-packet elsewhere reads as tile run.

## 6.2 Tile size is 64x64

Sections 1, 3 and 4 all arrived at 64x64 for v1, for the same reason: per-tile header and rANS flush
cost must stay under about 8 bytes against a 75 to 90 byte average payload. 32x32 stays reserved by a
stream-header bit for high-bitrate USB profiles where the payload per tile is large enough to carry it.

## 6.3 Entropy coding: eight rANS lanes per tile in v1

Section 1 proposed 1 to 32 substreams chosen per tile so each lane decodes about 128 symbols. Section
3 designed the decoder around a fixed eight lanes so eight tiles fill one 64-wide Adreno wave and the
same code runs on wave32, wave128 and lavapipe's width of eight. Decision: v1 fixes eight lanes. The
`nsub_log2` header field stays in the syntax so v2 can vary it under a tool bit. Probabilities are
10-bit, state is 32-bit, tables are static per frame with built-in defaults, exactly as Section 1
specifies. Fixed state and precision keep the construction inside Duda's published rANS and outside
the 2022 Microsoft claims, pending the review in 6.9.

## 6.4 Intra is DC-plane, not directional

Sections 1 and 3 agree: v1 intra tiles carry a second-level DCT over the 64 block DCs with planar
interpolation, and no directional modes, because directional prediction needs an in-tile wavefront
that serializes the workgroup. Directional intra is a v2 tool bit, promoted to v1 only if Phase 1
shows more than a 40 percent bit gap against x264 intra on VR captures.

## 6.5 One quarter-pel vector per tile, five modes

Sections 1 and 2 agree on one quarter-pel motion vector per tile in v1, coded as a delta from the same
tile's previous vector, and on five tile modes: WARP_SKIP, WARP_MV, STATIC_MV, STEREO, INTRA.
Four vectors per tile is a v2 tool bit. Depth never reaches the decoder; per-tile plane parallax
collapses into the vector.

## 6.6 References: newest fully-acknowledged neighbourhood, four-slot ring

Section 2 modelled a single previous-frame reference with an encoder-side shadow of the client.
Section 4 pointed out that the warp reads across tile borders, so a tile's reference is only safe if
its 3x3 neighbourhood in the reference frame was received. Decision, combining both:

- The client holds a four-slot reference ring in display format (Section 1's RGBA8 or RGB10A2 slots).
- A tile references the newest frame among N-1, N-2, N-3 whose 3x3 neighbourhood is fully
  acknowledged, signalled by a 2-bit `ref_delta`, where 3 means intra.
- Missing tiles are filled by the deterministic concealment warp, identical to WARP_SKIP with the
  last vector, and the encoder replays the same fill on its mirror ring, so the shadow stays exact.
- Per-band feedback carries the received-tile bitmap; the encoder keeps eight frames of history.
- Rolling intra refresh at 1/180 of tiles per frame stays as a safety net. The IDR ladder and the
  NVENC DPB workaround in WiVRn NX are retired when the codec is active.

The estimated cost of lower bands referencing N-2 on WiFi is 5 to 10 percent of bits. It is
unmeasured and is a Phase 3 number to collect.

## 6.7 Pose travels twice, cheaply

Section 1 put the 26-byte pose in the frame header. Section 4 sends only a `pose_seq` that indexes the
client's own two-second pose ring. Both are kept: the frame header, replicated in the first datagram of
every band, carries the pose so a client with a gap in its ring still decodes, and every datagram
header carries the 16-bit sequence so the warp delta is computed from shared history. The cost is
26 bytes per band, six bands per frame.

## 6.8 Foveation and the warp

Section 2 requires the homography to run in linear render space. Section 5 defines the per-frame
foveation map as one R8 texel per tile, an integer lookup that composes deterministically. Decision:

- Phase 2 runs unfoveated to prove the predictor.
- Foveation in the codec is per-tile resolution level and QP, from Section 5's map. The predictor is
  formed at the coded resolution and the client always holds a full-resolution reference, so the warp
  never sees a non-uniform grid.
- WiVRn NX's continuous foveation remap stays for the hardware codecs and is bypassed when the codec
  is active. The render-cost win moves to Variable Rate Shading driven by the same map.

## 6.9 Multicast is out, multi-user shares the encode

Section 4 rejects multicast outright: consumer 802.11 sends multicast at the basic rate,
unacknowledged. Multi-user therefore shares the encoder's work, not the air. Each headset gets its own
unicast stream, and the shared base tiles are encoded once and sent twice.

## 6.10 The compute budget verdict

Section 3's estimate for the full compute decoder on Adreno 650 is 4 to 6 ms at p50 for two eyes at
2048 squared. At 90 Hz that is the same band as the CAS pass that hurt in the field. The paper
therefore expects the Pico 4 to land in hybrid mode, with the hardware HEVC decoder carrying the base
layer and compute carrying the pose-warped enhancement, and pure compute becoming the default on the
next Adreno generation and on PC-class clients. Phase 0 decides this with numbers, not hope.

## 6.11 Motion smoothing hands over

Section 2 replaces WiVRn NX's server-side block matcher with the codec's own per-tile correction
vectors as the extrapolation field, and excludes STATIC_MV tiles from extrapolation. The client warp
stays. This resolves the server-smoothing jitter problem by construction rather than by tuning.

## 6.12 Freedom-to-operate review, scoped

Before Phase 3 ships, a formal review of exactly four items: pose delta as global motion parameters
for a streamed VR frame (nearest relatives are expired MPEG-4 GMC and royalty-free AV1 global motion,
but Meta, Qualcomm, NVIDIA and Microsoft filings must be searched), view synthesis prediction in
3D-HEVC against the STEREO mode, the LCEVC family against the enhancement layer, and the 2022
Microsoft rANS patents against Section 1's fixed-precision construction. H.264 and HEVC are only ever
touched through the device's licensed hardware decoder.
