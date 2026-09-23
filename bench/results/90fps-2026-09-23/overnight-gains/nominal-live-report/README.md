# Nominal BBR live comparison

Four complete fixed-ceiling runs compare nominal BBR before the acute-cap guard:

- A: `nominal_bbr=0`, repeats 1 and 4.
- B: `nominal_bbr=1`, repeats 2 and 3.

All runs use cache1, predictor0, requested ceiling 500, 90 Hz, JIT 45000, window 0, tail 64, and the bounded dark duplicated-photo fixture. The report uses actual per-window codec budgets from server encode lines; startup 433.604 Mbps is not substituted for those means.

B is faster on average fresh FPS, but its quality still collapses/oscillates as the budget moves. This pre-acute-cap result is not promoted. Payload excludes transport/FEC/padding; derived software delay is not photon latency.

`build_report.py` rebuilds plots from the included numeric CSVs only.
