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
