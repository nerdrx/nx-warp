# 100% resolution: pacing check

Current source resolution is 2176×2176 per eye, restored from the 2688×2688 experiment. HEVC 10-bit, four retained sources, past-source preference, capped warp; tiny blur off.

Two short 20-second headless moving-scene trials compare the existing maximum JIT sleep of 5ms with 10ms. Both finished with client alive. The 10ms variant is **not retained**: no improvement in the measured decode-to-display-target interval.

| After first two logging windows | 5ms maximum sleep | 10ms maximum sleep |
|---|---:|---:|
| Render iterations/s | 90.00 | 90.01 |
| Fresh-source selections/s | 59.17 | 59.33 |
| Decoder-reported duration | 5.97ms | 6.09ms |
| Decode completion to selection | 56.74ms | 57.33ms |
| Selection to predicted display | 33.53ms | 33.88ms |
| Reported late/overrun counters | 0 | 0 |

The complete logs include startup, which lowers whole-run mean render rates to 80.75/s and 81.91/s. Those figures must not be confused with steady-state rates above. Ninety render submissions do not establish ninety physically distinct panel images or accurate motion prediction.

The reported time intervals are timestamp diagnostics, not optical motion-to-photon measurements. No causal latency reduction or statistically significant difference is established by these sequential runs. Increasing the sleep limit moved CPU scheduling but did not improve the measured decode-completion-to-predicted-display interval.

The scheduler learns wall time and fence waits, not the current frame's complete asynchronous GPU duration. Zero reported misses in a short run therefore does not prove GPU deadline safety. No physical head-motion or thermal soak was performed.

Restored `debug.wivrn.nx.jit_max_sleep_us=5000` and restarted the client. Resolution remains 100%; the user-accepted retained capped-warp settings remain enabled. Scripts and logs are included; local scratch paths are retained.
