# Rejected nominal-paced matrix

Four complete auto-mode runs compare nominal BBR 0/1 with requested 500 Mbps, 90 Hz, JIT45000, cache1, predictor1, window0.3, tail0, and 120 seconds each. A is nominal0 (repeats 1 and 4); B is nominal1 (repeats 2 and 3).

The actual codec budget collapses far below the requested 500 Mbps ceiling. This experiment does not demonstrate quality retention and was removed from production. Freshness uses the corrected count/render-rate calculation; derived software delay is not photon latency.

`build_report.py` rebuilds the graph from included numeric CSVs only.

## Rejected implementation

[Archived patch](rejected-experiment.patch) preserves the final guarded nominal-BBR
experiment against the integration tree after `fa427b29`. It is research evidence,
not an enabled feature or an installation recommendation. It was removed after
these runs because the quality budget still collapsed. Earlier unguarded results
are a different version of this experiment.
