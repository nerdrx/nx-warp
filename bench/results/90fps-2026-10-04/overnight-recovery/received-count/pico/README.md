# Pico CPU-only shard-set count check

Standalone harness A/B on Pico (A8110). No APK, system
property, headset session, GPU, network route, or clock/frequency setting was
changed. This measures average CPU time inside synthetic shard-window helper
loops only; it says nothing about frame time, XR locks, GPU work, or photon
latency.

Before and after the run, `dumpsys power` reported `mWakefulness=Asleep` and
`Display Power: state=OFF`; `dumpsys thermalservice` reported `Thermal Status:
0`. The own `/data/local/tmp/nx-received-count-pico-20261005` directory was
removed and verified absent after the run. Harness process used `nice -n 10`; no
CPU affinity was set.

## Inputs and build

- Harness: `../cost.cpp`, SHA-256
  `3e371e86e073ad8e2cc4417658062635b5f6d8db7213efa77e2de2b0406ada47`.
- Baseline header snapshot (bb76c0ef): SHA-256
  `16c678699b2b12060295bb301d160ec2b529322517454498f712e760a345e83b`.
- Count header from source revision `8d33945257d3c0347b8a77d9c753f2dfcc930b98`:
  SHA-256 `d404a9f90deb57d82f7a33b51200848d2398b9480004d6f6097fbdcab2d73e05`.
- Cross-built binaries used NDK 29, `-O2 -std=c++23 -static-libstdc++`, and
  the checkout's existing Android OpenSSL static library. `common/smp.cpp` and
  OpenSSL are needed because packet-header static initialization references
  those symbols even though the benchmark does not exercise SMP/reconstruction.
- Host precheck compiled both variants with `g++ -O2 -std=c++23`, linked
  `common/smp.cpp -lcrypto`, and ran both harnesses successfully. Logs and
  resulting CSVs are kept beside this file.

The historical commands below pin this host. The supplied `build.sh SOURCE CONFIGURED_HOST_BUILD ANDROID_OPENSSL_PREFIX NDK OUTPUT` builds the public copied harness and baseline; it does not push or launch anything. Captures are trimmed to the relevant power/thermal state lines.

Historical device builds from the checkout root:

```sh
d=/run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/received-count-pico-20261005
ndk=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
cxx=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++
deps='-I client -I common -I external -I build-server/_deps/boost-src/libs/pfr/include -I build-server/_deps/monado-src/src/external/openxr_includes -I .cxx/RelWithDebInfo/33s4w1c5/arm64-v8a/_deps/openssl/include'
lib=.cxx/RelWithDebInfo/33s4w1c5/arm64-v8a/_deps/openssl/lib/libcrypto.a
$cxx -O2 -std=c++23 -static-libstdc++ $deps "$d/../received-count-final/cost.cpp" common/smp.cpp "$lib" -ldl -lm -o "$d/final-pico"
$cxx -O2 -std=c++23 -static-libstdc++ -I "$d/../received-count-final/baseline" $deps "$d/../received-count-final/cost.cpp" common/smp.cpp "$lib" -ldl -lm -o "$d/baseline-pico"
```

Run order was ABBA: baseline, count, count, baseline. Each process ran five
10,000-iteration blocks per scenario/phase. `deadline_present` matched the
expected result for all 120 rows: late interior hole (before/due), unknown tail
(before/due), and parity-suppressed hole (before/due). Raw output is in
`baseline-run0.csv`, `final-run1.csv`, `final-run2.csv`, `baseline-run3.csv`.

## CPU loop means

Means across the 10 block samples per variant and case; `change` is count vs
baseline. Lower is less CPU time in this measured helper loop.

| Scenario | Phase | Baseline ns/call | Count ns/call | Change |
|---|---:|---:|---:|---:|
| Late interior hole | Before quiet gate | 2674.0 | 54.8 | -97.9% |
| Late interior hole | Due | 5287.2 | 2931.9 | -44.5% |
| Unknown tail | Before quiet gate | 1931.7 | 34.1 | -98.2% |
| Unknown tail | Due | 2577.3 | 38.1 | -98.5% |
| Parity-suppressed hole | Before quiet gate | 2419.1 | 41.6 | -98.3% |
| Parity-suppressed hole | Due | 5210.9 | 2781.8 | -46.6% |

Interpretation is limited to this tiny standalone CPU loop. The count removes
the repeated empty/full data scan in these cases; due interior-hole cases still
scan for missing shards and remain materially slower than before the gate.

## Commands and cleanup

Device launch form (repeated in the order above):

```sh
adb shell 'nice -n 10 /data/local/tmp/nx-received-count-pico-20261005/baseline-pico > /data/local/tmp/nx-received-count-pico-20261005/baseline0.csv'
adb shell 'nice -n 10 /data/local/tmp/nx-received-count-pico-20261005/final-pico > /data/local/tmp/nx-received-count-pico-20261005/final1.csv'
adb shell 'nice -n 10 /data/local/tmp/nx-received-count-pico-20261005/final-pico > /data/local/tmp/nx-received-count-pico-20261005/final2.csv'
adb shell 'nice -n 10 /data/local/tmp/nx-received-count-pico-20261005/baseline-pico > /data/local/tmp/nx-received-count-pico-20261005/baseline3.csv'
```

After pulls and state capture, the device directory was removed with
`adb shell rm -rf /data/local/tmp/nx-received-count-pico-20261005` and absence
was verified. Device state captures, exact source hashes, host precheck output,
and raw device CSVs are retained in this folder. Local build binaries are
removed after recording their hashes in `binary-hashes.txt`.

CPU affinity and frequency were not controlled; frequency was not sampled. These are observed helper-loop averages under the scheduler, not a CPU-utilization or power result.

The public `build.sh` was also rebuilt locally against the final source, producing both Android variants successfully. This check did not launch either binary on the device.
