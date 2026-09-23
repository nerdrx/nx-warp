# Final churn pointer matrix

Four usable labeled runs are included:

- A1 baseline: cache0 + predictor0.
- B2/B3 candidate: cache1 + predictor1.
- A4 replacement baseline: cache0 + predictor0.

The original A4 control disconnected immediately after source upload and remains excluded as invalid. The A4 replacement ran only after the 15-minute changing-loss-only soak, so A1/B2/B3/A4 are not a contiguous ABBA sequence; the gap is preserved in the metadata.

Freshness uses the corrected count/render-rate calculation. Derived delay is not photon latency. `build_report.py` rebuilds the chart from bundled numeric CSV only.
