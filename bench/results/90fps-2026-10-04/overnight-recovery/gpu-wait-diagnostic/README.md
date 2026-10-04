# GPU wait / timestamp diagnostic (rejected)

This was a small diagnostic probe for a reported mismatch between GPU query
 timestamps and CPU fence-wait return time. It is not an optimization and does
 not establish a driver fault. The captured run used an AMD Radeon RX 7900 XTX
 with RADV and Vulkan timestamp period 10 ns/tick.

## Recorded run

Run window: 2026-10-04 20:03:58–20:03:59 local time, immediately after the
separate paced sender benchmark completed. The probe submitted an empty command
buffer and a one-workgroup shader (64 invocations, 32 integer iterations each),
then waited on a fence. It ran 20 warmups per condition followed by 50 measured
samples per condition (100 measured samples total), alternating conditions.
These are tiny clocked command probes, not rendered frames or physical-photon
latency measurements.

| Condition | Fence-wait p50 / p95 | GPU query duration p50 / p95 |
|---|---:|---:|
| Empty command buffer | 6.857 / 8.569 ms | 0.44 / 0.44 µs |
| Tiny compute dispatch | 7.104 / 8.499 ms | 1.36 / 1.40 µs |

A device-clock sample was taken before submission and another after fence wait.
For all 50 samples in each condition, the GPU query timestamp did not lie
between those device-clock samples. Both fixed-period mapping and an affine map
between the two calibration samples therefore produced an apparent ~1.45 ms
ordering inversion. **Those derived CPU/GPU margins are invalid and should not
be interpreted as a wait breakdown or latency estimate.** The affine map did
not repair the ordering failure; this probe cannot explain its cause.

The DRM load samples around this one-second run were GPU busy 100% before and
99% after, memory busy 34% both times, and active shader clock 2378 MHz before
and 2354 MHz after. Workload ownership at those sampling instants is unknown;
the run immediately followed the owned sender benchmark, and these readings
do not establish external contention.

## Build and run

Requires a C++20 compiler, Vulkan headers/loader, and a Vulkan implementation
with `VK_EXT_calibrated_timestamps` and a compute queue with timestamp support.
The included `short.spv` is the shader binary used for the recorded run;
`short.comp` is its source.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build
./gpu_wait_probe ./short.spv
```

The program writes `probe.csv` in its working directory. Its device selection
currently requires an RX 7900 XTX, so this exact executable is not a portable
benchmark. Do not compare runs unless workload, active GPU load, device, driver,
and clock state are recorded alongside the samples.

## Files

- `probe.cpp`: standalone Vulkan probe.
- `short.comp`, `short.spv`: shader source and exact compiled shader binary.
- `probe.csv`: raw 100 measured samples.
- `probe-summary.txt`: console summary from the captured run.
