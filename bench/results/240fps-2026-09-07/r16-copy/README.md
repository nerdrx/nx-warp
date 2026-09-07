# R16 atlas copy experiment

This records four retained GPU runs comparing the optional R16 atlas view's
full refresh via three `vkCmdCopyBufferToImage` copies (`new`) with the prior
`atlas_view.comp` conversion dispatch (`old`). Frame 0 is excluded because its
GPU timestamps are unavailable; invalid-reference motion runs are excluded.

Across the retained still runs, the new path measured GPU p50/p95 of
**0.755 / 2.498 ms**, versus **1.095 / 3.495 ms** for the old path. Wall-time
p50/p95 was **1.017 / 3.384 ms**, versus **1.356 / 5.172 ms**. These are four
short runs and are evidence for this optional R16 path, not an R8 live-FPS
claim. The normal R8 path remains separate.

Reproduce the per-run and pooled p50/p95 extraction from this directory with:

```sh
python3 summarize.py
```

`provenance.sha256` and `final-provenance.sha256` record the Android
tool binary plus the still and motion fixtures used by the benchmark setup;
`commands.log` records the retained run labels. The motion log is retained as
an excluded failure record.

Percentiles use linear interpolation over 62 warm frames per arm (two runs of
31 frames). New/old dispatch counts are 1/2. The first-frame startup cost is
retained in the logs but excluded from warm timings.

Full-resolution stereo output passed host synchronization validation and the
Pico atlas-view comparison: 2,367,488 luma and 591,872 chroma samples. The small
odd-stride fixture selects the compute fallback and cannot validate the copy
path on its own. A stereo extent bug was caught by the full-resolution test
and corrected before the retained timings.

The still fixture is included as `still.nxv`. Run the final CLI as follows,
alternating arms twice; keep output readback disabled for this measurement:

```sh
nxvc-vkdec --in still.nxv --atlas-view r16 --no-out --stats
NXVC_VKD_ATLAS_R16_COMPUTE=1 nxvc-vkdec --in still.nxv --atlas-view r16 --no-out --stats
NXVC_TEST_ATLAS_VIEW_INPUT=still.nxv test_vk_decoder_conformance --only-atlas-view
```
