# Pointer live ABBA report

Four complete current pointer-candidate repeats are summarized as ABBA over 2-second windows:

- **A baseline:** `cache=0`, `predictor=0`, repeats 1 and 4.
- **B cache+predictor:** `cache=1`, `predictor=1`, repeats 2 and 3.

Experiment settings were fixed 500 Mbps requested (**433.604 codec budget**), 90 Hz, JIT 45000 us, window 0, tail 64, 120 seconds per run. The dark duplicated-photo fixture shifted by 8 pixels every four frames; analysis begins at source upload +10 seconds.

Payload is full-frame codec output including detail and safety, excluding transport, FEC, and optional padding. Both A and B include network dips, so minimum windows and holes are shown alongside means. `p95_window` is over 2-second summary windows, not per-frame p95. Derived software delay is not photon latency.

The old scalar results are a distinct earlier experiment and are not a direct paired baseline. No quality change is inferred here; byte exactness is covered by separate GPU evidence in [compression-report](../compression-report/README.md). The predictor remains opt-in and this report does not promote a default.

`build_report.py` rebuilds the report from the included numeric CSV files only; it does not read private logs, photos, or original run directories.
