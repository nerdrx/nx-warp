# Production motion encoder A/B

This offline Vulkan test compares the original motion codec at baseline commit `143d6210bdda289621996b15304298adc23cbfa5` with final candidate commit `070b671b1e2cbd18cd1c676bc88c0d12391cf553`. The candidate uses one predicted-Zstd compression pass for the temporal residual; the independent-frame selector and 10% whole-envelope savings gate remain in place.

The photo fixtures provide only the 256×256 native RGB center, duplicated for both eyes. The rest of the Vulkan input and encoded image stays a fixed synthetic periphery. These results are not full-photo, live-headset, game-FPS, network-loss, or end-to-end latency measurements.

## Result

Each case/build has 48 measured samples from two runs, with six warmup frames before 24 measured frames per run. Baseline/candidate order was ABBA. The harness checks each decoded output byte-for-byte against an independent production codec with motion disabled. For every matched frame ID, raw/detail/wire sizes and recorded raw/wire FNV fingerprints match across baseline and candidate. Cross-build compressed-wire equality was checked by size and fingerprint, not by retaining and directly comparing every wire buffer.

| Scenario | Baseline p50 / p95 ms | Candidate p50 / p95 ms |
|---|---:|---:|
| Scene A, moving native center | 8.577 / 8.954 | 7.671 / 8.075 |
| Scene A, zero held-ACK mask | 3.581 / 3.873 | 3.897 / 4.323 |
| Scene B, moving native center | 9.823 / 11.666 | 8.279 / 9.607 |
| Scene A→B cut in warmup, then static Scene B | 7.462 / 8.273 | 4.151 / 4.751 |

The moving-center cases improved median encode time by about 10.6% and 15.7% with identical output sizes. The post-cut case is a static repeated-center workload after the first cut frame occurs during warmup; it is not a general scene-cut performance claim. The zero-ACK case was 0.316 ms slower at the pooled median. A reversed-order BAAB control gave 3.898 ms baseline and 3.931 ms candidate median, with run-to-run timing variation; treat the difference as inconclusive.

Candidate-stage profiling on moving frames found roughly 2.86–2.99 ms for the independent baseline selector, 93–97 μs for motion search, 78–80 μs for residual construction, and 729 μs (Scene A) / 1.15 ms (Scene B) for predicted-Zstd residual compression. Search and residual construction are a small share of the measured work. The zero-ACK case mostly hit the existing compression cache, so its roughly 12 μs selector time does not represent a fresh independent compression.

`summary.json` has the p50/p95 values used for plotting. `samples.csv` contains every final ABBA sample and both digests; `noack-control.csv` contains the extra reversed-order control. `profile-stages.csv` contains the candidate stage timings.

## Reproduce

Fixtures are private and are not included. Check out the two revisions above and configure a build directory for each with `compile_commands.json` and `server/CMakeFiles/wivrn-server.dir/link.txt`. Then build the offline executables:

```sh
python3 build.py \
  --baseline-repo /path/to/baseline-checkout \
  --baseline-build /path/to/baseline-build \
  --candidate-repo /path/to/candidate-checkout \
  --candidate-build /path/to/candidate-build \
  --output /tmp/motion-encoder-ab
```

Point the harness at local private 2160×2160 RGBA fixtures, then run baseline, candidate, candidate, baseline:

```sh
export NX_DIRECT_BENCH_SEQUENCE=1
export NX_BENCH_FOREST=/path/to/private/scene-a-rotate.rgba
export NX_BENCH_DARK=/path/to/private/scene-b-rotate.rgba
/tmp/motion-encoder-ab/baseline/direct_motion_bench > /tmp/baseline-1.log 2>&1
/tmp/motion-encoder-ab/candidate/direct_motion_bench > /tmp/candidate-1.log 2>&1
/tmp/motion-encoder-ab/candidate/direct_motion_bench > /tmp/candidate-2.log 2>&1
/tmp/motion-encoder-ab/baseline/direct_motion_bench > /tmp/baseline-2.log 2>&1
```

The executable runs four cases on one reused codec per case: a small horizontal native-center translation; no usable held ACK; a second photo-derived center with translation; and a Scene A-to-B change during warmup followed by a static center. Forced periodic anchors remain part of the moving-frame measurements. A separate two-sample scratch spot check covered a controlled frame-ID gap and ACK reference ages two and three; the ID gap models age only and does not inject packet loss.

The final candidate source hashes are motion helper `f97341574862e07d744ca0ccca4e399c222524250cce31153d8819402b3b33a6`, Zstd helper `7bf9c8e01202236fdb86113032aa5274a8b4ddc74cd48c16706a545524ea92e2`, and codec `20ea67b72233c8dbd0dc7ec47bc7a1ba71aad1b8df474d9f605b627415d141c9`. Baseline source snapshots match commit `143d6210bdda289621996b15304298adc23cbfa5`.
