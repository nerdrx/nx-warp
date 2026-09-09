# Round-colour mode-3 remap pair

One completed 60-second Pico pair, run order **remap ON then control OFF**.
Both used the fresh mode-3 shader-coordinate-remap APK, round-colour and
colour encoder enabled, priority 1, wait 4000 us, compact borrowed FDM 1, and
the full-field scene. Remap used `peripheral_smooth=3`; control used `0`.

Remap was the user-selected mild-softening option. Relative to control, the
single pair measured own-GPU `5.714 -> 6.283 ms` (+0.570 ms), fresh source
rate `80.60 -> 73.59/s` (-8.70%), source offset `58.08 -> 65.91 ms`, and
decoder GPU `3.610 -> 3.933 ms`. Both status files report complete 60-second
runs; client logs contain no ERROR/ANR/crash text.

This is one descriptive pair, not statistical proof. Timings are harness
own-GPU/source-offset measurements, not photon-to-photon latency. Repeated
visibility/session transitions are present in both captures and remain a
possible source of run variability.

`logs.tgz` contains both raw client, scene, server, and status captures;
`summary.json` is produced by `summarize_round_colour_remap.py` using the
repository's `analyze_live.py`. `ACTIVE_USER_PROFILE.json` preserves the
harness snapshot.
