# Serial versus right-eye async recheck

Scratch-only, same-input follow-up to `../fence-recheck/`. It compares **only** ordinary Zstd level 3: serial (eye 0 then eye 1 on caller) versus parallel (right eye readback/packing on `std::async`, left eye on caller, then join). Both modes record and submit both eye command buffers on the same queue before either fence/readback path begins. Five warmups per mode and 20 matched measured pairs alternate serial→parallel and parallel→serial. Two unrelated photographic fixtures occupy simulated eye slots; these are not binocular captures. Private paths and all image-derived payloads remain local.

The 2176×2176 fixtures and ASTC output match the previous fence recheck exactly. The same archived Vulkan 1.1 SPIR-V was used for both treatments. Production `server/shaders/astc_encode.comp` at d3f428bb and the copied shader source have identical hashes. The SPIR-V hash is recorded; root independently confirmed `glslc -O --target-env=vulkan1.1` reproduces it. Host C++ was built Release (`-O3 -DNDEBUG`, plus target `-O3 -Wall -Wextra -Wpedantic`). See `compiler-flags.txt`, `current-production-hashes.txt`, and `source-hashes.txt`.

## Result

All 20 measured calls per mode passed production packet decoding. Per-eye ASTC bytes were identical between modes and match `fence-recheck`; per-eye packet bytes were also identical (eye 0 414,921 bytes, eye 1 257,982 bytes). See `byte-checks.txt`; raw ASTC/packet outputs remain local.

| Complete-call wall | Serial L3 | Parallel L3 |
| --- | ---: | ---: |
| p50 | 7.696 ms | 4.886 ms |
| p95 | 7.843 ms | 5.002 ms |
| Per-pair parallel-minus-serial p50 | — | −2.717 ms |
| Per-pair parallel-minus-serial mean | — | −2.620 ms |

Parallel was faster in all 20 pairs; paired deltas ranged from −2.967 to −2.015 ms. GPU dispatch p50 sums were effectively unchanged (1.240 ms serial, 1.242 ms parallel), as were dispatch-through-readback sums (1.404 and 1.405 ms). Per-eye Zstd timing was similar across modes; packet bytes were exactly equal. Eye 1 async packet-path p95 was 0.145 ms higher than serial, while complete-call p95 improved by 2.841 ms. This supports a host-side overlap benefit in this harness, not a faster GPU stage or a live compositor claim.

Fence wait-call p50s were serial 0.936 ms (eye 0) and 0.0014 ms (eye 1); parallel 0.915 ms (eye 0) and 1.519 ms (eye 1). Parallel wait calls overlap, so summing them does not measure elapsed wall time. Eye 1's async wait-call duration rises to 1.519 ms p50, but it overlaps the left-eye path. In serial mode, eye 1's wait starts only after eye 0 readback and packet work; a late observation at that point includes the other eye's CPU work. The per-eye wait call is not a GPU duration, and no wait/query subtraction is interpreted as queue latency.

Read-only DRM busy samples were 0% on both cards before and 0–1% afterward; card-1 VRAM stayed at 2,442,977,280 bytes. This is an idle-ish sample context, not evidence for why the earlier fence wait was longer. No cause is inferred.

`run.log`, `run.exitcode`, the 40-row `three-mode.csv`, `summary.csv`, `load-snapshots.csv`, build flags, hashes, and raw payloads/local-only input paths kept privately preserve the experiment. No production code, runtime/profile/device settings, applications, clocks, or network configuration were changed. No private image was published.

Root recomputed all40rows/20 paired deltas; every pair improves. p50/p95 use sorted index`floor((n-1)*p)`. The public CMake reuses exact headers/shaders from`../fence-recheck`; its compiled SPIR-V must match SHA`a237f4e5bf52e5bda3ad325d50edaafcd79e6812dc6e3f34365064a3857314c4`. Run from this directory after a Release build, supplying your own headerless2176² RGBA8 inputs:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
nice -n 10 timeout 480s ./build/stereo-gpu "$LEFT_RGBA" "$RIGHT_RGBA" /tmp/overlap-recheck-results
```

No live compositor, transport/FEC, Pico or photon latency was measured. Source parallel-eye option remains default off; this replay does not install or activate it. No validation-layer run is claimed. Compared to the previous serial-only recheck, CPU timings differ; use matched pairs here, not cross-run CPU comparisons.

![Matched complete-call timings](overlap.png)
