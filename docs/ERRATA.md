# Paper errata

Corrections to [PAPER.md](PAPER.md) found during implementation. The paper text is
left as written; the normative documents (SYNTAX.md, WARP.md, TRANSPORT.md) carry
the corrected values.

| Date | Section | Error | Correction | Found by |
|---|---|---|---|---|
| 2026-09-04 | 1.4 | "7 bits after the first dimension, 12 after the second" does not normalise a 9-bit-constant Loeffler pair; the flow graph gains 2^10 per dimension, so 7 leaves the output 2x hot | The normative chain in SYNTAX.md 6.3 and ref/ is 7 then 13 (total shift 20, unit gain); the bench derived 8 then 12, which is the same total. Any split that sums to 20 with the documented intermediate ranges is correct; the paper's 7 then 12 is not | Phase 0 bench, Pass B |
| 2026-09-04 | 3.2.5 | Traffic table counts one coefficient per pixel, i.e. luma only | 4:2:0 adds 32 chroma blocks per 64x64 tile, about +50 percent on coefficient traffic and transform work; K3/K5 in the bench understate a production decoder by the same margin | Phase 0 bench |
| 2026-09-04 | 3.6 | Encoder imports an RGB render target | On Linux the compositor delivers 4:2:0 YCbCr, already foveated; stream header gains `color_space` (see INTEGRATION-DECISIONS.md) | Integration spike |
| 2026-09-04 | 1.3 | References stored in display format (RGBA8 / RGB10A2) | For the 4:2:0 passthrough path references are two-plane 4:2:0, halving Pico memory and matching the client's existing sampler | Integration spike |
| 2026-09-04 | 4.3 | 4-slot ring assumed to replace the client's frame ring | The WiVRn client's ring (3 published frames) and the decoder's reference ring are different objects; both are needed | Integration spike |
| 2026-09-04 | 5.2 | `dQ_act` written with a minus sign while the prose says busy tiles get coarser | Plus sign; rc/ implements the prose; text tiles exempt | rc |
| 2026-09-04 | 4.6.1 | Text tiles assumed to have high gradient coherence | Measured coherence of glyph fields is about 0.03 (strokes run in every direction). rc/ detects text by gradient energy, contrast and a normalised spatial-frequency ratio; coherence separates edge from texture only | rc |
| 2026-09-04 | 5.1 / 5.2 | `fov_t` and `dQ_ecc` both apply eccentricity | Split into sample density (s_fov squared) and perception (2^(-dQ_ecc/6)) | rc |
| 2026-09-04 | 1.5 / 4.6.1 | Weighting matrix selected per frame, but the degradation ladder needs it per tile | 2 reserved tile-header bits become `wm_id` (SYNTAX.md, Phase 1 intra work) | rc |
| 2026-09-04 | 2.2 | Homography in Q8.24 | Overflows int32 at Pico 4 width (coefficient ~ f = 940 px); WARP.md fixes the Q format and guaranteed range | stereo |
| 2026-09-04 | 3.9 | SwiftShader as a CI stand-in for lavapipe | The available SwiftShader has subgroup size 4 and no 16-bit storage; it is hybrid-only. lavapipe is the only usable software ICD for the pure compute path | vk/common |
| 2026-09-04 | 3.10 | SPIR-V 1.4 target | The Pico 4 Adreno driver is Vulkan 1.1 without VK_KHR_spirv_1_4; the build targets SPIR-V 1.3 | vk/common |

## Paper-internal inconsistencies

Sections of the paper were drafted in parallel and disagree with each other in the
places below. In every case the normative documents (SYNTAX.md, WARP.md,
TRANSPORT.md) and the ADRs under adr/ are authoritative; the paper text is left as
drafted.

| Sections | Disagreement | Resolution |
|---|---|---|
| 2 vs 1.1, 3.1, 4.1, 6.2 | Section 2 assumes 32x32 tiles in its bit costs and state sizes | 64x64 (ADR 0002); section 2 numbers scale accordingly |
| 1.2 vs 2.3 vs 6.5 | Tile mode field is 2 bits with four names, or 3 bits with five | Five modes, field width per SYNTAX.md |
| 1.6 vs 3.2.2, 6.3 | 1 to 32 rANS substreams per tile, or eight | Up to eight, chosen per tile (SYNTAX.md) |
| 1.6 vs 3.2.2 | 12 contexts x 16 symbols vs 8 contexts with a different alphabet | 12 x 16 (SYNTAX.md) |
| 1.6 vs 3.2.2 | Probability tables per frame in v1, or static per QP class | Eight trained built-in sets plus optional per-frame transmitted tables (SYNTAX.md) |
| 3.2.3 vs 1.4, 1.9 | Step 2 offers AV1-style or 5-3 wavelet transforms | 8x8 integer Loeffler DCT only (ADR 0004 context) |
| 2.6 vs 1.2, 1.3, 4.5, 6.6 | One previous frame and no DPB, vs a four-slot reference ring | Four-slot ring (ADR 0006) |
| 2.6 vs 4.4 | Per-frame 1 kB bitmap on the pose packet, vs per-band 100-byte feedback | Per-band feedback (TRANSPORT.md) |
| 2, 3 vs 4, 5 | 2048x2048 (2048 tiles) vs 2160x2160 (2312 tiles) per eye | Both appear as examples; the Pico 4 panel is 2160, level limits in spec/08 |
| 1.12 vs 4.2, 6.7 | Frame header replicated per tile row (34) vs per band (6) | Per band (ADR 0007) |
| 4.1 | Listed datagram header fields total 178 bits against a stated 192 | 14 bits unassigned; TRANSPORT.md carries the final layout |
| 1.7, 3.5 vs 2.9 vs 4.2 | Hybrid base decoder latency 8 to 12, 10 to 20, 8 to 15 ms | Unmeasured until Phase 0 K6; treat as 8 to 20 ms |
| 1.10 vs 5.1.3 | Foveation level shares 20/30/50 vs 40/35/25 for the Pico 4 | 5.1.3's derivation; fov/ implements it |
| 5.1.2 | Cites a foveation assumption in section 3's budget that does not exist | No such assumption; the 3.1 budget is unfoveated |
| 1.2, 1.8, 2.3, 2.7 | WARP_SKIP / SKIP_WARP and STATIC_MV / SKIP_STATIC used interchangeably | WARP_SKIP, STATIC_MV (SYNTAX.md) |
