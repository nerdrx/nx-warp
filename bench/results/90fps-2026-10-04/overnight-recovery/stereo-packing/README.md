# Stereo ASTC CPU encode benchmark

Scratch-only benchmark for two independent 2176x2176, ASTC 8x8, quality-6 block streams. Inputs are `/tmp/nx-astc-quality-test/results/dark-q6.astc` and `forest-q6.astc`; the standard 16-byte ASTC file header is checked and excluded from compression. No GPU, compositor, network, headset, or application timing is involved.

Each run has 20 warmups and 200 measured samples, interleaving serial two-eye encoding, two persistent `std::jthread` workers, and per-frame `std::async(std::launch::async)` for the right eye while the caller encodes the left. The future is obtained before the iteration ends, so the input remains alive and the async job is complete before reuse. Every resulting eye packet was byte-identical across all paths and repeats. Both selected packets were decompressed and verified against the source ASTC bytes.

Default mode matches the quality-6 policy: Zstd level 3 is preferred; LZ4 runs only if Zstd fails or exceeds half the raw ASTC size; Zstd is selected only when at least 10% smaller than the raw/LZ4 fallback. `--compact` models explicit compact mode: compact each eye's ASTC blocks, perform one Zstd level-3 pass on that compact input, and select compact+Zstd only when at least 10% smaller than the fallback and no larger than the compact input. This does not do the earlier, incorrect raw-Zstd plus compact-Zstd dual pass.

## Current run

Host load averages at run start were 7.49/7.55/7.86 for legacy and 7.37/7.52/7.85 for compact (1/5/15 min; host has 32 logical CPUs). Times are median/p95 milliseconds over 200 samples:

| Mode | Serial two-eye batch | Persistent-worker batch | `std::async` batch | `std::async` launch overhead |
|---|---:|---:|---:|---:|
| Legacy default | 3.516 / 4.096 | 2.172 / 2.742 | 2.130 / 2.474 | 0.0186 / 0.0303 |
| Explicit compact, one Zstd pass | 4.059 / 4.558 | 2.433 / 3.265 | 2.461 / 3.142 | 0.0458 / 0.0646 |

Compact packing p50/p95 was 0.099/0.134 ms for dark and 0.106/0.144 ms for forest in serial mode. Legacy packet sizes including the 24-byte NX header were 416,403 / 258,395 bytes. Compact packets were 392,386 / 245,461 bytes; compact won 200/200 samples for both eyes. LZ4 was skipped in these runs because Zstd output was below half of raw input size. Packet encoding IDs were 2/2 (legacy Zstd) and 5/5 (compact+Zstd).

These CPU-only measurements suggest `std::async` can overlap independent eye compression here, despite measurable per-call launch cost. They do not prove a compositor frame-time improvement: no GPU transport, scheduling integration, network, or Pico decode was measured. Host load was nonzero and changed slightly between runs; do not interpret the close async/persistent differences as a controlled significance test.

Published CSV rows are in `results/current-legacy.csv` and `results/current-compact.csv`. Verified packet payloads remain private and are not included. Run with:

```sh
./build/stereo-pack /tmp/nx-astc-quality-test/results/dark-q6.astc /tmp/nx-astc-quality-test/results/forest-q6.astc results/current-legacy
./build/stereo-pack /tmp/nx-astc-quality-test/results/dark-q6.astc /tmp/nx-astc-quality-test/results/forest-q6.astc results/current-compact --compact
```

![CPU batch time and unchanged packet bytes](stereo-packing.png)

Production overlap is an opt-in source experiment: [configuration and lifecycle](https://github.com/nerdrx/wivrn-nx/blob/pyrowave-probe/docs/ASTC_PARALLEL_EYES.md). No running session was changed.
