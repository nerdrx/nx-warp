# Native plane-workgroup split — 2026-09-11

## Result

The opt-in two-workgroup prototype activated in the live client: `native plane split 1, reject mask 0` (repeated in the activation log). It produced no useful first-ABBA win. The source prototype was removed/restored to HEAD after testing; core and client patches are retained under `archive/`.

## First live ABBA

Pooled post-warm means from the 60 s ABBA arms (2 s summary windows; these are means of summary values, not per-frame percentiles):

| arm | fresh | PassB | nxvc GPU |
|---|---:|---:|---:|
| control | 52.979 | 8.088 ms | 13.150 ms |
| split | 52.833 | 8.188 ms | 13.084 ms |

The split activated but provided no useful first-ABBA gain.

## LDS follow-up ABBA

Pooled post-warm means show a clear regression:

| arm | fresh | PassB | nxvc GPU |
|---|---:|---:|---:|
| control | 52.927 | 8.234 ms | 12.972 ms |
| split | 42.229 | 11.704 ms | 16.654 ms |

All four 60 s trials completed with the client alive. These are 2 s summary-window means, not per-frame percentile statistics.

## Exactness

Host compact tests passed for R2 and R4 with split disabled and enabled. Each pair returned identical 6,220,800-byte output and identical SHA-256:

- R2: `78fbc95b6214590cb07fc821ae484a1a5c98791b0f094b09ef9915b6064e53a6`
- R4: `ab25923f09141749fe12f7c5968d73fde60b88672f27f9d326447d9490aacca4`

Exactness covers the exercised compact cases; it does not establish a live performance gain.

## Disposition

Activation and correctness are demonstrated, but the prototype has no measured GPU benefit and the LDS refinement is slower. The prototype is retired and removed from the active source.

Raw small logs, statuses, test scripts, and source patches are in `archive/`; APKs and YUV payloads are omitted.
