# Ready ABBA live comparison

Four complete bounded runs compare ready pacing:

- A: `ready=0 us`, JIT 45000 us, repeats 1 and 4.
- B: `ready=4000 us`, JIT 5000 us, repeats 2 and 3.

All runs use fixed500, cache1, predictor1, nominal_bbr0, window0, tail64, and 120 seconds on the dark duplicated-photo fixture. Freshness uses the corrected `fresh_count / render_iterations * reported render_fps` calculation. Safety fallback counts exclude the source+10 s startup period.

Ready4000 reduces derived software delay in these runs, but this is bounded evidence. It makes no claim about unique rendered content or full physical display at 90 Hz. Derived delay is not photon latency, and there is no default-promotion conclusion here.

`build_report.py` rebuilds plots from the included numeric CSV only.
