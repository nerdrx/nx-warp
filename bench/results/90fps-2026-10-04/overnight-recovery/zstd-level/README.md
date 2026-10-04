# Native ASTC packet compression comparison

The probe compares four q6 ASTC packet paths: ordinary Zstd level 3 (current baseline), ordinary level 1, compact 14-byte block packing with level 1, and compact packing with level 3. For these two full-size inputs, **compact level 1 had the lowest measured CPU-plus-calculated-wire model at both 250 and 500 Mbit/s**. It does not change the ASTC image: the production decoder recovered the original ASTC block bytes exactly for every mode and eye.

This is a pinned, CPU-only packet test, not a Vulkan, network, compositor, headset, or FPS test. Inputs were read from their private fixture directory and were not copied into this report.

## Inputs and provenance

The two existing ASTC files are standard 2176x2176x1 images with 8x8x1 blocks, 1,183,760 bytes including the 16-byte ASTC header, and 1,183,744 raw block bytes. Their q6 sidecars identify fit 3 and six quantization bits. SHA-256 values are in `manifest.json`; no source image or ASTC file is included.

The harness and NX packet helpers are from `wt-pyrowave-probe` at revision `27026bbd9b2a3703b7acdffc6e49b88f81b9ab35`. It calls the production `nxastc_packet::compact_blocks`, `make_header`, `parse_packet`, and `decode_payload` functions; it does not reimplement compaction or packet decode. Compact packets use the v4 compact-zstd packet encoding, so this mode requires a client that supports v4.

## Method

Build uses C++20 and `-O2`. The process reads its allowed CPU set, pins itself to the CPU on which it starts (CPU 3 in this run), verifies that affinity, and checks after every measured sample that it stayed on that CPU. This avoids migration between cores; it does not isolate the core from other processes.

Each condition reuses one ZSTD_CCtx and output buffers per eye. The two independent eyes are compressed serially, as in the default compositor encoder loop. The four conditions are interleaved in symmetric order `L3, L1, compact-L1, compact-L3, compact-L3, compact-L1, L1, L3`. This gives 20 warmups and 200 measured two-eye samples for each condition.

Selection follows the current q6 production policy: try Zstd; run LZ4 on the raw ASTC bytes only if Zstd errors, returns zero, or exceeds half the raw size; then select Zstd only when it is at least 10% smaller than the fallback. Compact Zstd is admitted only when it also fits within the compact stream size. Compacting and packet assembly are timed. For these inputs every condition selected Zstd (compact-zstd for compact modes); LZ4 was not called.

Packet decompression and compact expansion happen after timing. One representative emitted packet for each condition and eye (eight packet decodes total) is passed through the production `decode_payload` helper; each must return success and reproduce all 1,183,744 ASTC bytes exactly. Percentiles use sorted index `floor((n-1)*p)`.

Run time was 2026-10-04 18:59:21–18:59:24 UTC. The program verified CPU affinity on all 800 measured rows. The host had 32 logical CPUs; one-minute load average was 8.62 before and 8.62 after. No GPU, device, server, UI, or network test was run.

## Results

Milliseconds are p50 / p95 across 200 paired-eye samples. Packet sizes include the 24-byte NX header for each eye. The wire values are calculated from both packet sizes and the listed rate; they are not network measurements. `CPU + wire model` adds serial measured packet CPU p50 to the calculated two-eye wire duration.

| Condition | Two-eye CPU p50 / p95 | Mean packet bytes dark / forest | Calculated wire at 250 / 500 Mbit/s | CPU + wire model p50 at 250 / 500 Mbit/s |
|---|---:|---:|---:|---:|
| Ordinary Zstd 3 | 3.812 / 4.597 | 416,403 / 258,395 | 21.594 / 10.797 ms | 25.405 / 14.608 ms |
| Ordinary Zstd 1 | 2.447 / 2.788 | 426,443 / 268,400 | 22.235 / 11.118 ms | 24.682 / 13.564 ms |
| Compact 14 B + Zstd 1 | 2.505 / 2.913 | 400,066 / 254,856 | 20.958 / 10.479 ms | **23.463 / 12.984 ms** |
| Compact 14 B + Zstd 3 | 3.604 / 4.475 | 392,386 / 245,461 | 20.411 / 10.206 ms | 24.015 / 13.809 ms |

Compact level 1 costs just 0.058 ms more CPU p50 than ordinary level 1, while reducing the combined packet size by 39,921 bytes (5.75%) and modeled wire time by 1.278 ms at 250 Mbit/s (0.639 ms at 500). Against the current ordinary level-3 baseline, compact level 1 reduces measured CPU p50 by 1.307 ms and modeled wire time by 0.636 ms at 250 Mbit/s (0.318 ms at 500). The simple CPU-plus-wire model improves by 1.943 ms at 250 Mbit/s and 1.625 ms at 500 Mbit/s.

Compact level 3 produces the smallest packets, but its slower compression gives a worse combined CPU-plus-wire model than compact level 1 at both rates. On this input pair, compact level 1 is the strongest measured candidate. This result is limited to fixed q6, fit-3 packets from these two images, serial CPU packet work, and calculated wire duration; it does not predict transport behavior or closed-loop quality-controller changes.

## Reproduce

Requirements: CMake, a C++20 compiler, Zstd, and LZ4 development packages.

```sh
cmake -S . -B build
cmake --build build -j4
./build/zstd_compact /private/path/dark-q6.astc /private/path/forest-q6.astc results/zstd-compact.csv
python3 plot.py
```
