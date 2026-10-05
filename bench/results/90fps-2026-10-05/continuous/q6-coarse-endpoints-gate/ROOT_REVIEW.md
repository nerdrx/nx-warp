# Root review

Root independently rebuilt published gate.cpp using the matched AVX2 static reference library and generic fixture arguments. The complete two-row CSV reproduced byte-for-byte (combined compile/run/cmp exit0). No pixel/ASTC output was written. Baseline SSE and compressed bytes match prior native gates. All other modes stay copied, and every eligible block is checked for legal parse and unchanged non-endpoint bits. The 17/4 endpoint-order flips are guarded.

Root retained the supplied normal/sanitized evidence and corrected the first-failure description: the original crash stderr was overwritten; first-attempt.log is retrospective. Sanitizers cover the harness, not a rebuilt/instrumented astcenc library. Root strengthened the reproduction wrapper to record nonzero build/run exits instead of leaving stale successful exit files, without changing the gate algorithm.

The predeclared byte goal (5% each fixture) passes; quality loss <=0.1dB fails both. No actual motion, distraction, speed or live quality conclusion follows from static RGB error alone. No production integration.

After strengthening the wrapper, root ran the published wrapper in isolated scratch: normal and ASan/UBSan builds/runs all exit0, their CSVs match the original archive exactly. [Retained replay context and exits](root-replay/context.txt).
