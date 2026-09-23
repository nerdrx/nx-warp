# Fixed pacing numeric report

Eight completed pacing runs are summarized in `runs.csv` and `summary.json`. The candidate order is ABBA-ish, but the runs are not strict paired comparisons. `tail_packets` identifies the optional 0 versus 64 tail setting; the JIT cap label is retained as a run label only.

`derived_delay_mean_ms` is the logged client-clock sum of wire + queue + decode + decode-to-selection + selection-to-predicted. It is a pipeline timing proxy, not photon latency. `holes` and `closures` are reported directly from each run.

The fixture is a shifted still-photo sequence, not a game, head-motion, photon-latency, or radio-outage proof. Bad windows are listed in `summary.json` by low fresh-source FPS and high derived-delay maximum.

Rebuild the graph from the included numeric CSV:

```text
python3 build_report.py
```
