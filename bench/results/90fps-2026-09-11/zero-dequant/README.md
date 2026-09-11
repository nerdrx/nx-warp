# Sparse zero-dequant experiment — 2026-09-11

## Scope

The candidate added an early return in `dqCoef()` for sparse coefficients where `coefIn()` has already returned zero, gated to sparse, non-large transforms. It retains the existing IDCT, integer rounding, barriers, prediction, and stores. The source was reverted and the control APK restored after the screen.

## Exactness

Host R2 and R4 fixtures were byte exact. The recorded output hashes match the expected values in `exactness.json`; no output differences were observed.

## Short live screen

The 30 s base/candidate/base screen completed with the client alive. Post-startup means from the consistent warm-window analyzer (discarding the startup portion) were:

| arm | Fresh selections/s | PassA | PassB | nxvc GPU |
|---|---:|---:|---:|---:|
| base A | 44.67 fresh | 5.61 ms | 9.66 ms | 15.25 ms |
| candidate A | 45.50 fresh | 5.74 ms | 9.41 ms | 15.15 ms |
| base B | 45.22 fresh | 6.07 ms | 9.18 ms | 15.23 ms |

Source-proxy means were 79.33, 85.13, and 83.18 ms respectively. These are 2 s summary-window means, not per-frame percentiles or physical latency measurements. The short screen is noisy and does not prove an improvement; candidate PassB/GPU is effectively unchanged.

## Disposition

Exactness is established, but performance benefit is unproven. Retain the patch only as a bounded experiment; it is removed from the active source.

Small raw logs, statuses, scripts, and the patch are in `archive/`. APKs and YUV payloads are omitted.
