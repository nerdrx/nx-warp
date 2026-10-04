# Source evidence

Sources are current files in `wt-pyrowave-probe`, captured for provenance only.
No test was built or run for this report.

`client/scenes/stream.cpp` (lines 1717–1742, 2935–2941): the per-refresh
accounting guard starts with `gpu_pass_submitted = false`; only the actual
queue-submit path changes it to `true`; the guard passes that flag to
`jit_scheduler::account`.

`client/scenes/stream_jit.h` (lines 195–209, 212–250): `sleep_ns` returns
zero until `frames_seen >= warmup_frames`; warm-up is 90 frames. `account`
updates pass-cost peak and `frames_seen` only when `gpu_pass_submitted` is true.
Idle refreshes still update lead/deadline counters.

`tests/stream_jit_test.cpp` (lines 10–27) codifies the model: 90 idle calls
with a 3 ms cost and `gpu_pass_submitted=false` leave frames and cost at zero;
90 submitted-pass calls reach the gate and retain a 3 ms peak. This file was
inspected but not run in this task.
