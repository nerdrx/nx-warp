# DC-only transform shortcut: not selected

An opt-in 8×8 DC-only shortcut was tested with the required1024px centre at
2688² per eye, R2 ungrouped, smoothing3. Host control/candidate output
comparisons matched, including R2/R4 compact NV12 and constant fixtures at
four QPs. Actual branch coverage was not instrumented, so exact output alone
does not prove the optimized branch was exercised. The patch is preserved
for reproducibility; it has been removed from active source.

| Completed60s run | Fresh selections/s | Decode GPU | PassB |
|---|---:|---:|---:|
| ControlA |48.15|12.817 ms|8.267 ms|
| CandidateA |50.10|13.967 ms|9.008 ms|

The second candidate run crashed during startup after a
`waitForFences: ErrorInitializationFailed` worker error. The plannedABBA
sequence therefore did not complete; no repeatable improvement is established.
The completed candidate also had higher GPU time. Similar fence errors had
occurred before this experiment, so this does not establish shader causality.
The control APK was restored. This experiment does not justify retaining
extra shader complexity. Raw logs, completion records and candidate patch
are included. Metrics are post-warm-up window means, not physical latency.
