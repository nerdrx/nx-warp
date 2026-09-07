# Experiment: update only changed atlas-view tiles

2026-09-07. Base commit `c274df1`, with the local changes described in
`docs/240FPS.md`. This is an opt-in decoder experiment, not a live WiVRn FPS
measurement. The installed application was not replaced.

## Method

Pico A8110 / Adreno 650. WiVRn discovery client stopped before measurement.
Release Android build, R8/R8G8 sampled view, no output readback during timing.
One identical 16-frame 2176x1088 YUV420 stereo stream in all runs. First two
frames excluded from each process: 14 observations per row. Full/dirty runs
interleaved three times. Device-side timeout 45 seconds per process; all
timing runs exited 0. GPU clocks sampled about every 100 ms; the `.clocks`
files also include process startup. Temperature before/after each process is
the first/last numeric line of its `.log` (millidegrees C, thermal_zone25).
No clock forcing. This is too short to establish sustained p99 or thermals.

Final timed binary SHA256:
`2a8504aa8021f87996e5708e908876db77a265ffd8b96c95a1bbcbaefb95d760`.
`fixture.nxv` SHA256:
`38f1115ffbde91855c9feb835a49bbb245179c1d6589b4355e5fa02953666d5a`.
The fixture was recovered from local `nx-scratch/atlasdec/vr-rest-on-atlas.nxv`;
its original encoder command is not independently verified. It is a sparse
update workload and must not stand in for dense motion or a complete corpus.

## Results

| Pair | Full GPU p50 ms | Dirty GPU p50 ms | Full call p50 ms | Dirty call p50 ms |
|---|---:|---:|---:|---:|
| 1 | 6.833 | 5.186 | 9.843 | 7.185 |
| 2 | 6.029 | 5.865 | 8.521 | 8.494 |
| 3 | 6.550 | 5.322 | 9.374 | 9.116 |

GPU p50 decreased in each pair, by approximately 24%, 3%, and 19%. Host call
improvement varied markedly. Clocks and temperature varied: these are observed
paired results, not a controlled estimate of a universal speedup. Median
reported dispatch count increased from 4 to 12.5 because this first prototype
dispatches one update rectangle per coded tile. It may lose on dense updates.
The historical counter also omitted compose, MATGEN, WRITEBACK, and ASSEMBLE
dispatches; the source counter was corrected after these experiments.
Quality and bytes are unchanged for the tested stream: both paths consume
identical bytes and the views compare exactly. No thermal-efficiency claim.

**Decision: retain opt-in for further measurement; do not change defaults.**
`NXVC_VKD_ATLAS_VIEW_DIRTY=1` enables the experiment for CT_NONE 4:2:0.
`NXVC_VKD_ATLAS_VIEW_FULL=1` overrides it. Other formats use full refresh.
Creation/recreation, PICTURE frames, and the frame after an external atlas
patch force full refresh. The [single-dispatch follow-up](single-dispatch/README.md) replaces the
per-tile loop, but measured latency is mixed and remains experimental.

## Correctness and unresolved result

`--only-atlas-view` compares full and dirty views after **every** fixture
frame, in R8 and R16, then checks the final samples against atlas storage.
The stereo fixture passes on Pico. The generated 194x130 fixture passes on
RADV 7900 XTX with Vulkan validation enabled and no validation messages.
That validation run exposed a missing TRANSFER_SRC usage flag on the atlas
readback buffer, now fixed.

The generated 194x130 mono fixture with default directional prediction **fails
on Pico in full-view arm 0, frame 1**: non-INTRA tile 0 refers to an INVALID
atlas entry. The dirty path has not run at that point. [Further isolation](directional-isolation/README.md)
ties the failure boundary to directional reconstruction; the exact cause
remains unresolved. The clipped no-direction 194x130 mono fixture and the
2176x1088 stereo fixture pass full/dirty R8 and R16 view comparison after every
frame on Pico (`pico-final-view.log`). This directional failure is not counted
as a pass and blocks broad correctness and default-enablement claims. Recreated
views, external patches, and dense workloads still need dedicated device
coverage.

## Live-session observation (16:26 in the device log, separate workload)

The existing custom WiVRn NX session reached its stream scene on the first
connection attempt. In one approximately two-second window it logged 143
decoded frames, mean decoder GPU time 10.8 ms, mean decoder call 13.0 ms,
and 171 render iterations (85.3/s). The render log also reported 39 cached
re-presentations and a 1.9 ms display GPU pass. These counters measure
different work: render-loop rate is not unique decoded-frame rate or proof
of physical presentation. Mean displayed pose age was 68.1 ms in that window.
This makes the old ~60 FPS claim plausible in scale but still insufficiently
specified; it is not evidence of 240 Hz performance.

Defoveation was 1088x1088 per eye at scale 0.50, atlas-mode 0. No candidate
APK was installed. `live-package.txt` records the installed package's reported
version/update time; `live-telemetry.log` contains the observed counters.
This was a brief observation, not a controlled A/B. Android `screencap` returned
an all-black image despite stream telemetry, so no headset image is presented
as visual evidence. The reference frame in the README is labeled separately.

## Reproduction commands

From the repository root, build the existing Android configuration:

```sh
cmake --build build-vkdec-android --target nxvc-vkdec test_vk_decoder_conformance -j4
```

Push the binaries and `fixture.nxv` to a dedicated device directory. With
that directory current, compare these commands, interleaved rather than batched:

```sh
NXVC_VKD_ATLAS_VIEW_DIRTY=1 NXVC_VKD_ATLAS_VIEW_FULL=1 timeout 45 ./nxvc-vkdec --stats --no-out --atlas-view r8 --in fixture.nxv
NXVC_VKD_ATLAS_VIEW_DIRTY=1 NXVC_VKD_ATLAS_VIEW_FULL=0 timeout 45 ./nxvc-vkdec --stats --no-out --atlas-view r8 --in fixture.nxv
NXVC_TEST_ATLAS_VIEW_INPUT=fixture.nxv timeout 45 ./test_vk_decoder_conformance --only-atlas-view
```

Capture stderr with stdout, preserve process exit codes, and collect clocks and
temperature. Summarize each log separately with
`python3 scripts/summarize-vkdec.py --warmup 2 LOG`. The committed JSON records
raw-log hashes and observed percentiles; its p99 is descriptive, not a gate.
