# ASTC GPU timing diagnostic

Set `WIVRN_NX_ASTC_GPU_TIMING=1` on the server to opt in. Each ASTC slot records
timestamps from the compute dispatch start through the readback copy completion;
the 180-frame summary reports valid count, mean, nearest-rank p50, and p95 in
milliseconds.
This is a same-queue GPU interval, not kernel-only time, a calibrated CPU/GPU
comparison, queue delay, frame latency, or photon latency. It may include device
contention. Timestamp counters are masked to the queue family's valid bits, which
handles one wrap; multiple wraps within a single submission cannot be disambiguated.
Unsupported timestamp queues and timestamp/query failures leave normal encoding
active and omit samples. With the variable unset or not exactly `1`, no timestamp
pool or timestamp commands are created.

When enabled, nonblocking query retrieval runs inside the existing CPU
fence-plus-invalidate interval. That adds host-side work and can perturb scheduling;
compare the GPU interval with the CPU measurement as separate clocks.
