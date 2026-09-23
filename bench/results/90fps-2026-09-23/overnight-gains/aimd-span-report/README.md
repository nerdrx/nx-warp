# AIMD span-only ABBA

Four complete AIMD runs compare ordinary and loss-only span gating:

- A: `aimd_loss_only=0`, repeats 1 and 4.
- B: `aimd_loss_only=1`, repeats 2 and 3.

B2 preserves initial quality, then a real 25-hole burst cuts the actual budget through approximately 500 → 400 → 320 Mbps. B3 holds healthy budget and freshness. A remains at a low budget with low freshness. This is opt-in AIMD span-gate evidence; it does not claim 500 Mbps throughout or unique physical-display quality.

`build_report.py` rebuilds the timeline and aggregate chart from bundled numeric CSVs. Freshness uses the corrected count/render-rate calculation. Derived delay is not photon latency.
