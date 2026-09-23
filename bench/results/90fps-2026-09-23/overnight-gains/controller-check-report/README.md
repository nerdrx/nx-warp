# Controller check

Three complete bounded checks compare AIMD (runs 1 and 3) with BBR (run 2) at requested500, 90 Hz, JIT5000, ready4000, cache1, predictor1, window0, tail64, and 120 seconds.

The chart uses actual per-window server byte budgets. The startup 433.604 Mbps line is excluded from means. AIMD collapses to a single-digit codec budget with low freshness; BBR also remains far below requested500. This is a bad-quality/default500 collapse result, not quality retention evidence.

Freshness uses the corrected count/render-rate calculation. Derived software delay is not photon latency. `build_report.py` rebuilds from bundled numeric CSVs only.
