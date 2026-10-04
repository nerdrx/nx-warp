# FEC recovery direct-span XOR

This host-only synthetic probe measures `fec::reconstruct` before and after removing the temporary full serialized blob for each present shard. Recovery uses the same serialization traits and exact field order (`view_info`, `timing_info`, payload length, payload); it still copies the parity bytes and creates an owned recovered-shard buffer.

Baseline source: `27026bbd9b2a3703b7acdffc6e49b88f81b9ab35`. Final source: `78b93a0540bfac9354292630892d01d322e74bc1` (FEC span-XOR and checked-bool fix). The source snapshot files here are from the final source commit.

## Wire compatibility

The baseline and final probes (`wire-baseline.txt`, `wire-final.txt`) compare equal byte-for-byte. Protocol/revision hash: `416750f9c8bf9b90`; bool type hash: `54ddd5c6099bcc3e`; view-info type hash: `6eb8b9b7a69b4fbc`. Four video FEC blobs matched exactly: `alpha=false/true` crossed with timing absent/present; sizes are 117/149 bytes with a 7-byte varied payload.

## Matched host probe

Five ABBA blocks ran pinned to CPU 3 of an AMD Ryzen 9 9950X3D using GCC 16.2.1 and `-O2`. The 360 CSV rows are 5 blocks × 4 invocations × 18 conditions; there are 180 rows for each treatment. Conditions cover K=4/8/16, stride=1/4, and first/middle/last missing positions. Each case performs 2,500 real `fec::reconstruct` calls. Input payload sizes repeat 1400, 1400, 613, 97 bytes with deterministic seeded randomized bytes; the first shard includes view metadata and the last includes timing metadata.

| Per-reconstruction metric | Baseline median / p95 | Final median / p95 |
|---|---:|---:|
| Time | 1.844 / 4.085 µs | 1.665 / 3.859 µs |
| `operator new` calls | 69 / 134 | 65 / 130 |
| Requested allocation bytes | 6,135 / 9,264 | 4,727 / 7,462 |

The p95 is calculated over per-case averages of 2,500 calls across repeats. It is not individual-call tail latency. Median time change by K4/K8/K16 is −12.4%/−9.7%/−5.9%; median requested bytes fall 26.9%/23.0%/17.8%. Allocation figures come from benchmark-local global `operator new` instrumentation, not a production counter. CPU frequency was not locked. These results do not establish headset, network, FPS, or live performance.

`matched.csv` is the raw final dataset. `fec-span-xor.png` plots its median time and allocation metrics. `benchmark.cpp`, `run_abba.py`, and `plot.py` are the benchmark/figure sources. `fec.h`, `wivrn_serialization.h`, and `fec_test.cpp` preserve the final implementation and focused tests. No private image or payload data is included.

## Reproduction

Set `SRC` to the WiVRn checkout at the final source commit and `ART` to this report directory. The checkout needs its build-server Boost/PFR dependency fetched:

```sh
SRC=/path/to/wt-pyrowave-probe
ART=/path/to/fec-span-xor
mkdir -p /tmp/nx-fec-span-xor/baseline
git -C "$SRC" show 27026bbd9b2a3703b7acdffc6e49b88f81b9ab35:common/fec.h > /tmp/nx-fec-span-xor/baseline/fec.h
git -C "$SRC" show 27026bbd9b2a3703b7acdffc6e49b88f81b9ab35:common/wivrn_serialization.h > /tmp/nx-fec-span-xor/baseline/wivrn_serialization.h
cp "$ART/benchmark.cpp" /tmp/nx-fec-span-xor/bench.cpp
cd "$SRC"
g++ -std=c++23 -O2 -I /tmp/nx-fec-span-xor/baseline -I common -I build-server/common -I external -I build-server/_deps/boost-src/libs/pfr/include /tmp/nx-fec-span-xor/bench.cpp common/smp.cpp -lcrypto -o /tmp/nx-fec-span-xor/bench-baseline-final
g++ -std=c++23 -O2 -I common -I build-server/common -I external -I build-server/_deps/boost-src/libs/pfr/include /tmp/nx-fec-span-xor/bench.cpp common/smp.cpp -lcrypto -o /tmp/nx-fec-span-xor/bench-final
python3 "$ART/run_abba.py"
```

The FEC test passed normally and with ASan/UBSan (`4292 checks, 0 failures`; halt-on-error enabled). Root reran the same tests as `/tmp/fec_test_bool` and `/tmp/fec_test_bool_san`. Test build commands are documented at the top of `fec_test.cpp`.
