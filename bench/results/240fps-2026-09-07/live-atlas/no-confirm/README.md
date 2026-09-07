# No-confirmation diagnostic

This is a diagnostic `NXWARP_FRAME_HELD=0` run, using APK/server inputs from the no-confirm capture. It is not an optimization result and does not justify disabling confirmation by default.

The active-window medians were 6.05 ms decoder GPU time and 36 new sources/s. The corresponding confirmation-enabled patterned run reported 17.9 ms and 45 new sources/s. The lower GPU value coincided with roughly 400 skipped tiles per server report versus zero in the comparison, while client startup telemetry accumulated at least 360 dropped-late and 157 withheld frames. Thus the run trades work for delivery and cannot support a quality or throughput win claim; rates are reported-window measurements, not physical display FPS.

The retained screenshots ([awake 1](awake-1.png), [awake 8](awake-8.png)) show the checkerboard/cube scene and are visual validity evidence only. The separate bootstrap attempt timed out its wire-3 confirmation after 500 ms before resuming; see [bootstrap-server-filtered.log](bootstrap-server-filtered.log). Recompute the numeric record from the retained filtered logs with the parent summary tooling; `raw-summary.json` retains the original source hashes and boundary caveat.
