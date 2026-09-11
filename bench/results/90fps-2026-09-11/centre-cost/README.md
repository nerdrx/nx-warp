# Where the larger centre costs time

Three exploratory 90-second full-field motion runs on Pico, client87f2e9bb,
core decoder2f58ed8, R2 ungrouped cells, smoothing3, SHA2off, flat64on.
All completed with45 current render/decode windows. These are sequential
screening runs, not an ABBA experiment or a sustained performance guarantee.

| Native eye size | Centre width | Fresh selections/s | Decode GPU | Source-offset proxy |
|---|---:|---:|---:|---:|
| 2688² | 1024px | 53.38 | 12.958 ms | 77.40 ms |
| 2688² | 640px | 81.99 | 5.510 ms | 53.38 ms |
| 2176² | 512px | 89.62 | 3.492 ms | 45.22 ms |

Post-warm-up means. The smaller2176 profile approaches90 genuinely fresh
selections/s in this trial; it is not the requested large-centre configuration.
The **1024px centre at2688² has been restored and is a user requirement**.
The smaller runs isolate cost, not a selected quality downgrade. Both PassA
and PassB grow strongly with native-centre work. This motivates removing
unnecessary transforms while preserving the centre.

No physical motion-to-photon measurement is claimed. Raw logs, summaries,
completion records and run script are preserved alongside this report.
