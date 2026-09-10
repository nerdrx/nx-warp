# Five-minute larger-resolution soak

Selected 2688² per-eye profile: ready wait 1 ms, JIT maximum 5 ms, decode priority 1, FDM 3, static-post 2, smoothing 3. Synthetic full-field animation on continuously awake Pico. Client `ef05194d`, codec `ce340da`. Completed 300 seconds; **zero session stops**.

After ten telemetry seconds, logged source selections average **70.91/covered wall-second**, source-offset proxy **71.87ms**, presentation GPU **4.82ms**. Decode GPU averages **6.47ms**. Source freshness therefore remains well below 90/s.

Vendor GPU-temperature telemetry reached 81°C and ended 79.5°C; initial 0°C samples are invalid/uninitialized readings, not real starting temperature. Higher sustained source-age and presentation cost warrant thermal/load investigation. This single run does not establish thermal throttling as the cause.

Source offset and fresh selections are software metrics, not photon latency or panel FPS. The headset was not mechanically moved. Logs and status are archived, with analysis scripts. No 240 FPS claim.
