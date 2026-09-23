# Overnight gains evidence

This directory contains sanitized numeric reports only. It records bounded codec and live-repeat evidence; it does not contain private photos, raw logs, source paths, addresses, or run identifiers.

## Measured wins

- The compression matrix shows an 8–11% reduction in complete full-frame codec payload (detail+safety), excluding transport/FEC/padding, with raw byte-exact fixture checks.
- Reusable encode cache samples save roughly 0.6–1.0 ms.
- The current pointer ABBA matrix shows about a 10.7% payload reduction and lower server encode time in the bounded 120-second repeats.

## Rejected or limited evidence

- Contended NEON timings are excluded; the Pico table reports production pointer-local versus old scalar decoder measurements.
- Copy-bypass and pretransform experiments were negative or too costly and are not promoted.
- Pointer live repeats still show network dips and fresh-frame variability; these results do not justify a default enablement.
- The old scalar live comparison is retained as a distinct earlier experiment, not a direct paired baseline.

## Pending

- Nominal BBR remains rejected in `nominal-live-report/`: nominal1 is faster on fresh FPS in this bounded fixture, but actual codec budget oscillation/collapse and holes make it unsuitable for promotion; the experiment was removed from production.
- The fixed-JIT matrix is included in `jit-clean-report/`: the 5000 us cap is inconsistent on fresh FPS and does not justify default promotion.

Each subdirectory contains its own README and, where applicable, a graph rebuild script that reads only its bundled numeric data.

- The ready ABBA matrix is included in `ready-abba-report/`; it reports a bounded software-delay change with corrected freshness accounting and no physical-display claim.

- `nominal-paced-report/` is an explicit rejection: auto mode collapses far below requested 500 Mbps and does not demonstrate quality retention.

- `controller-check-report/` shows actual AIMD/BBR budget collapse far below requested 500; it is a rejection report with no quality-retention claim.

- `aimd-span-report/` documents opt-in span-only AIMD: B2 keeps the initial budget until a 25-hole burst cuts 500→400→320, while B3 stays healthy; this is not a claim of maintaining 500 throughout.
