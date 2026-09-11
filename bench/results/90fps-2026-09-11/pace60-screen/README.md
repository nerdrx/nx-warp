# Pace 60 screen — 2026-09-11

## Result

A short pacing probe compared the original automatic pacing with an opt-in pace-60 setting. All three 30 s runs completed with the client alive. The pace-60 setting was retired; the original automatic pacing was restored.

| run | Fresh selections/s | Source proxy (ms) |
|---|---:|---:|
| baseline (`zero-base-b`) | 45.22 | 83.18 |
| pace-60 A | 48.11 | 94.40 |
| restored automatic B | 45.78 | 82.43 |

These are screen-level short-run means from the recorded analyzer. They are not per-frame percentile or physical-latency measurements. The pace-60 arm has no stable evidence of improving the target path and is not selected.

Raw small logs, statuses, and scripts are in `archive/`; the retired pace-60 configuration is included as pace60.json alongside the actual orchestration script. No APK or YUV payload is included.
