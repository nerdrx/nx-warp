# Bounded XUASTC encoder thread check

Host-only single-run comparison on `dark-native.png`, using XUASTC 8x8, quality 25, effort 0, XUASTC Zstd syntax, KTX2 Zstandard level 6. Each thread cap ran sequentially once. Command logs contain exact arguments and full encoder output; `cli_wall_seconds` is measured around the launched `basisu` process by Python `perf_counter`.

| max threads | CLI wall | Basis `c.process()` timer | output bytes |
| ---: | ---: | ---: | ---: |
| 1 | 1.760899 s | 1.685 s | 359128 |
| 4 | 0.734585 s | 0.663 s | 359128 |
| 8 | 0.539498 s | 0.471 s | 359128 |
| 16 | 0.396839 s | 0.331 s | 359128 |

All four KTX2 outputs have the same SHA-256 in `hash-manifest.txt`. Host: AMD Ryzen 9 9950X3D, 32 logical CPUs. Basis source revision: `99f52d63aa6799cbdaecfe977111dc5ec3b31d47`; encoder binary hash and fixture hash are in the manifest.

Interpretation: CLI startup, PNG load, and initialization add about 66–76 ms in these runs, so startup is a small, fairly constant part of the 331–1685 ms `c.process()` time. Threading materially reduces wall time through the largest tested cap (16), reaching 397 ms total. This does not test the default 32-thread cap, repeated encodes, or Pico, so it cannot establish whether the default oversubscribes or explain the exact 389 ms/frame report. The upstream success timer wraps `basis_compressor::process()` after `init()`; treat it as process-stage time, not pure kernel time.

`basisu` help/source says `-max_threads` caps the total pool including the main thread; multithreading is enabled by default. `-xuastc_zstd` selects XUASTC full-Zstandard syntax; KTX2 Zstandard defaults to level 6, also passed explicitly here.
