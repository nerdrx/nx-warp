# 4×4 inverse — rejected

Each invocation shares ancestor coefficients between four final 2×2 cells. Desktop 4:2:0 and 4:4:4 outputs matched the known-good fused output byte-for-byte. Pico 4:2:0 readback also matched exactly after rebuilding a stale shader object in the private test archive. Only the corrected artifact was timed.

Control/candidate/control, native 4352×2176 4:2:0, same Haar packet, 12 warmups +30 samples per run, no readback:

| Run | GPU p50 | GPU p95 |
|---|---:|---:|
| 2×2 control before | 12.4145 ms | 12.9421 ms |
| 4×4 candidate | 13.1730 ms | 13.7270 ms |
| 2×2 control after | 12.4702 ms | 12.9777 ms |

The candidate was approximately 6% slower; it is not integrated. Fewer ancestor fetches did not guarantee a faster shader on this GPU. Register pressure and occupancy were not measured, so they are hypotheses rather than an established cause.
