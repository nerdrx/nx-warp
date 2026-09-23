# Fixed-JIT live comparison

Four complete bounded runs compare JIT caps with cache1, predictor1, nominal_bbr0, fixed500, window0, tail64, and 120 seconds per run:

- A: JIT 45000 us, repeats 1 and 4.
- B: JIT 5000 us, repeats 2 and 3.

B is inconsistent on fresh FPS (80.95 and 88.84 FPS), while A is 88.75 and 88.85 FPS. The derived software-delay tradeoff is not photon latency. This result does not justify default promotion.

`build_report.py` rebuilds the graph from the included numeric CSV only.
