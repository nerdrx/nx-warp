# Compression at equal perceived quality: the rate-control library on the encoder

Branch `tourney/percept`. What this measures is the thing the codec is
actually built for and had never been tested on: **does spending bits where
the eye is, and taking them away where it is not, buy compression at equal
*perceived* quality?**

Until this branch nothing connected `rc/` and `fov/` to `ref/`.
`docs/RATECONTROL.md` 8.7 said so in as many words -- *"Phase 2 hook. Nothing
in `ref/` has been changed by this work"* -- and `rc/RESULTS.md` and
`rc/RESULTS-temporal.md` are simulator runs against a bit model of the
encoder's own functional form, which as appendix A.6 admits "says nothing
about whether the form itself is right for this codec". This branch makes the
connection and answers that question with the real encoder.

**The answer is no, not yet, and the reasons are specific and fixable.** The
mechanism works: on `vr-mixed-1024-v2` the fovea reaches **50.4 dB where a
flat-QP encode at half the rate reaches 42.5**, which is the codec doing
exactly what PAPER.md 4.6.1 asks. But the price charged in the periphery --
11.6 dB -- is larger than any of the four perceptual metrics will forgive, and
the equal-foveated-quality saving is negative at every rate, on every
sequence, under every metric including FovVideoVDP.

Section 7 splits the loss and none of it is the wiring:

* about half of it is the **allocator**, not the foveation at all -- the arm
  with the foveation map and the temporal ladder switched off is already
  5.6 dB and 0.57 JOD behind flat QP;
* a permanent bit-model bias inside that allocator is found, explained and
  **fixed** here (section 7.1, worth 2.7 dB on the controlled test);
* the rest is the resolution ladder, which is calibrated for a
  2160-px-per-eye panel and cannot be scored honestly on a 1024-px clip
  (section 7.2), and a scene-cut recovery horizon three times longer than the
  clips (section 7.4).

The deliverable is therefore a working wire, four named defects with
measurements attached, and a numbered list of what to change before the
question is asked again.

---

## 1. What was built

`nxrc::EncDriver` (`rc/include/nxrc/encdrive.hpp`, library `nxvc_rcenc`) runs
the pipeline of `docs/RATECONTROL.md` 2 once per frame and hands the encoder
four per-tile arrays:

```
  luma plane -> compute_tile_stats -> classify_tiles ---+
  lens + gaze -> foveation_map -----------------------+ |
                                                      v v
                          RefreshScheduler::schedule() -> force_warp_skip[]
                          RateController::allocate()   -> qp[], res_level[],
                                                          wm_id[]
                                                      |
      nxvc_encoder_encode_frame(img, qp_map, res_map) |
      nxvc_encoder_set_wm_map(wm_map)                 |
      nxvc_encoder_set_skip_map(skip_map) <-----------+
                                                      |
      nxvc_encoder_tiles() -> payload_len, warp_mad_q8, mv
                           -> update_model(), next frame's complexity
```

`nxv-enc --rc` is that driver on the command line, with `--rc-bitrate`,
`--rc-fov on|off`, `--rc-temporal on|off`, `--gaze x,y`, plus `--rc-fps`,
`--rc-panel`, `--rc-fov-deg`, `--rc-act` and `--rc-map` (a CSV of every
per-tile decision).

**No bitstream syntax changed.** All four maps address fields the v1 stream
has carried since syntax v1.2: `qp_delta` and `res_level` in tile-header
word0/word1, `wm_id` in word1 bits 26-27 behind tool bit 20, and `WARP_SKIP`
through the row `skip_bitmap`. Every conformance vector is byte-identical and
all 54 ctests pass. Two encoder-side entry points are new and neither is
visible to a decoder:

* `nxvc_encoder_set_wm_map()` -- the per-tile form of `wm_id`. RATECONTROL.md
  appendix A.5 asked for exactly this and listed it as unresolved; it was
  resolved in syntax v1.2 and nothing had used it.
* `nxvc_tile_info::warp_mad_q8` -- the mean absolute residual of the tile's
  `WARP_SKIP` predictor, which the mode search computes anyway. This is the
  `complexity` input RATECONTROL.md 4.1 asks the rate controller for, and it
  is measured rather than guessed.

One library change: in `nxrc::RefreshScheduler::admissible_divisor` the foveal
floor is now tested **before** the static-tile shortcut. `is_static` comes
from the previous frame's complexity, so a fovea tile that was static and
starts moving would otherwise have a residual it really does have withheld on
the frame the motion starts. Taking the floor first costs nothing -- a
genuinely static tile is skipped by the encoder's own mode search for free.

---

## 2. How this is measured, and the one thing that is easy to get wrong

`tools/quality/percept_run.py`. Four things differ from `compare.py`:

**Target rate, not QP.** The rate controller has no QP input, so every rc arm
is driven at a target bit rate and scored at the rate it actually delivered.
The flat-QP arm is a QP sweep, and the comparison is made by interpolating
that curve, in log-rate, at the quality the rc point reached. No
extrapolation: a point outside the flat curve is reported as `--`.

**Rate scaling.** The 20 / 40 / 80 / 150 Mbit targets are stated for the
reference geometry 2048x1024 at 90 Hz (one `vr-mixed-1024-v2` frame, both
eyes) = 0.106 / 0.212 / 0.424 / 0.795 bpp. A smaller clip is driven at the
same bits per pixel, because 40 Mbit/s into a 512x256 clip is 3.4 bpp and
measures nothing. Every table prints the scale it used.

**Four metrics, not one.** PSNR-Y (expected to fall), eccentricity-weighted
PSNR/SSIM at both acuity powers (`acuity` = `1/(1+e/e2)`, `acuity2` = its
square, which is the right exponent when the thing weighted is a pixel rather
than a line), the hard fovea/periphery split at 8 degrees, and **FovVideoVDP**
(`pyfvvdp` 1.2.2 on the 7900 XTX), which PAPER.md 5.3 names as the primary
objective metric. The FovVideoVDP plumbing (`nxq/fvvdp.py`, and
`nxq.yuv.yuv_to_rgb` with it) is **borrowed verbatim from branch
`tourney/metric`, commit 25bc7a4**, and is marked as borrowed in the file
header.

**The panel-versus-clip problem, which is the trap.** The foveation ladder
asks "is the detail in this tile above what the eye can resolve there?". That
compares acuity at the tile's eccentricity against the angular pixel density
of **the panel**, not of the test clip. The Pico 4 is 2160 px per eye over
81.2 degrees = 22.0 ppd on axis; `vr-mixed-1024-v2` is 1024 px per eye over
the same angle = 10.4 ppd. So there are two defensible configurations and both
are measured:

| arm | `--rc-panel` | what it is |
|---|---|---|
| `rc-spatial`, `rc-full` | 2160 | the decision the codec makes **on the Pico 4** |
| `rc-matched` | the clip's own per-eye width | the ladder told the truth about **this clip** |

They are not the same experiment. At 22 ppd, quarter resolution at 30 degrees
eccentricity leaves 5.5 ppd against the 4.3 ppd the acuity model asks for, so
the Pico 4 decision is correct for the Pico 4. On a 10.4 ppd clip the same
decision leaves 2.6 ppd, one full ladder step below what those pixels can
justify, and the metrics -- correctly -- charge for it. **Every number in the
`rc-spatial` and `rc-full` rows is therefore a lower bound on what those
settings would score on the panel they were computed for, and the gap cannot
be closed without a 2160-px-per-eye sequence.** `rc-matched` is the arm whose
foveated metrics mean exactly what they say.

Two smaller confounds, stated once: `--rc` sets the frame weighting matrix to
the flat one (matrix 0), because `wm_id == 0` in a tile header means "the
frame's matrix" and the ladder needs to be able to name all four; on
`vr-turn-256-v2` at QP 26 that costs 3.7 % more bits than the default matrix 1
the flat arm uses. And the 1024 sequences are scored over their first 8
frames, the 256 ones over all of theirs, for wall-clock reasons.

Command lines, CPU discipline and the exact result JSONs are in section 8.

---

## 3. Operating points

Generated by `tools/quality/percept_report.py` from the result JSONs. Read the
last two PSNR columns first: `PSNR fovea` and `PSNR periph` are the whole
result in two numbers.

**The mechanism works.** On `vr-mixed-1024-v2` 444, `rc-full-80` delivers
**50.4 dB inside the 8-degree fovea** where the flat-QP encode at half the
rate delivers 42.5, and `rc-full-150` reaches 52.8. That is the codec doing
exactly what PAPER.md 4.6.1 says it should.

**And it is not worth it, on these clips.** The same tiles cost 11.6 dB in the
periphery (31.9 against 43.5), and no metric here -- not eccentricity-weighted
PSNR at either acuity power, not eccentricity-weighted SSIM, and not
FovVideoVDP -- values the fovea enough to pay for that. Section 7 is why.

Two more things to read out of these tables. `rc-spatial` and `rc-full` differ
by less than 0.05 dB everywhere: the **temporal ladder is nearly free** at
these rates, which is the one unambiguously good result in the file -- it
withholds 38 to 49 % of peripheral residuals (section 5) for no measurable
quality cost. And `rc-nofov` on `vr-mixed-1024-v2`, at 63.0 Mbit/s, reaches
44.18 dB fovPSNR against 49.77 for flat QP at the same rate: **the allocator
is 5.6 dB behind before any foveation decision is made.**

### vr-mixed-1024-v2.yuv444p  (2048x1024 yuv444p, 8 frames, sbs, 10.43 ppd on axis, rate scale 1)

| point | Mbit/s | bpp | PSNR-Y | fovPSNR | fov2PSNR | PSNR fovea | PSNR periph | fovSSIM | JOD |
|---|---|---|---|---|---|---|---|---|---|
| flat-q44 | 2.26 | 0.0120 | 26.61 | 26.94 | 27.65 | 27.67 | 26.59 | 0.9193 | 7.728 |
| flat-q38 | 4.02 | 0.0213 | 30.33 | 30.21 | 30.06 | 28.13 | 30.39 | 0.9396 | 8.164 |
| flat-q32 | 7.14 | 0.0378 | 34.69 | 34.36 | 34.10 | 32.32 | 34.76 | 0.9635 | 8.625 |
| flat-q26 | 13.32 | 0.0706 | 39.00 | 38.69 | 38.57 | 37.32 | 39.04 | 0.9814 | 9.109 |
| flat-q20 | 27.28 | 0.1445 | 43.45 | 43.21 | 43.20 | 42.46 | 43.47 | 0.9909 | 9.397 |
| flat-q14 | 53.10 | 0.2813 | 48.56 | 48.43 | 48.58 | 48.20 | 48.57 | 0.9956 | 9.700 |
| flat-q8 | 95.00 | 0.5034 | 53.04 | 52.99 | 53.29 | 53.17 | 53.04 | 0.9978 | 9.853 |
| rc-full-20 | 19.46 | 0.1031 | 30.71 | 32.27 | 34.82 | 39.32 | 30.63 | 0.9725 | 8.590 |
| rc-full-40 | 36.15 | 0.1915 | 31.68 | 33.37 | 36.31 | 44.96 | 31.59 | 0.9813 | 8.932 |
| rc-full-80 | 54.99 | 0.2914 | 31.99 | 33.75 | 36.86 | 50.37 | 31.89 | 0.9853 | 9.379 |
| rc-full-150 | 61.61 | 0.3264 | 32.11 | 33.90 | 37.07 | 52.77 | 32.01 | 0.9871 | 9.524 |
| rc-matched-20 | 20.93 | 0.1109 | 31.33 | 32.82 | 35.19 | 38.28 | 31.26 | 0.9756 | 8.870 |
| rc-matched-40 | 36.62 | 0.1940 | 32.65 | 34.22 | 36.76 | 40.68 | 32.57 | 0.9825 | 8.924 |
| rc-matched-80 | 57.24 | 0.3033 | 33.42 | 35.15 | 38.15 | 47.29 | 33.33 | 0.9886 | 9.335 |
| rc-matched-150 | 65.62 | 0.3476 | 33.62 | 35.40 | 38.54 | 51.36 | 33.53 | 0.9907 | 9.572 |
| rc-matched-noact-20 | 21.26 | 0.1126 | 31.03 | 32.50 | 34.78 | 37.06 | 30.96 | 0.9732 | 8.659 |
| rc-matched-noact-40 | 37.95 | 0.2011 | 32.81 | 34.41 | 37.01 | 41.17 | 32.73 | 0.9837 | 9.059 |
| rc-matched-noact-80 | 57.85 | 0.3065 | 33.48 | 35.23 | 38.26 | 48.08 | 33.39 | 0.9892 | 9.350 |
| rc-matched-noact-150 | 67.24 | 0.3563 | 33.63 | 35.42 | 38.57 | 51.45 | 33.54 | 0.9909 | 9.586 |
| rc-nofov-80 | 62.98 | 0.3337 | 44.14 | 44.18 | 44.40 | 43.86 | 44.15 | 0.9928 | 9.172 |
| rc-spatial-20 | 20.29 | 0.1075 | 30.71 | 32.27 | 34.85 | 39.56 | 30.63 | 0.9724 | 8.590 |
| rc-spatial-40 | 37.06 | 0.1963 | 31.68 | 33.38 | 36.31 | 45.02 | 31.59 | 0.9814 | 8.932 |
| rc-spatial-80 | 57.80 | 0.3062 | 32.01 | 33.78 | 36.89 | 50.39 | 31.91 | 0.9854 | 9.383 |
| rc-spatial-150 | 66.00 | 0.3497 | 32.13 | 33.92 | 37.09 | 52.79 | 32.04 | 0.9872 | 9.527 |
| x265-p-refresh-crf38 | 8.97 | 0.0475 | 36.06 | 36.83 | 38.37 | 41.22 | 35.99 | 0.9813 | 9.268 |
| x265-p-refresh-crf32 | 15.65 | 0.0829 | 39.91 | 40.60 | 42.02 | 44.38 | 39.85 | 0.9897 | 9.609 |
| x265-p-refresh-crf26 | 28.14 | 0.1491 | 43.77 | 44.39 | 45.78 | 48.17 | 43.71 | 0.9943 | 9.811 |
| x265-p-refresh-crf20 | 52.04 | 0.2757 | 47.72 | 48.33 | 49.73 | 52.47 | 47.66 | 0.9969 | 9.908 |
| x265-p-refresh-crf16 | 77.74 | 0.4119 | 50.36 | 50.93 | 52.34 | 55.62 | 50.30 | 0.9979 | 9.939 |
| x265-p-refresh-crf10 | 142.95 | 0.7574 | 54.11 | 54.60 | 55.94 | 59.88 | 54.04 | 0.9987 | 9.968 |

**Encode and decode time per frame** (whole frame, both eyes; one core at `nice -n 19`)

| arm | points | encode s/frame | decode s/frame |
|---|---|---|---|
| flat | 7 | 1.296 | 0.110 |
| rc-full | 4 | 1.356 | 0.124 |
| rc-matched | 4 | 1.918 | 0.176 |
| rc-matched-noact | 4 | 1.502 | 0.142 |
| rc-nofov | 1 | 1.360 | 0.077 |
| rc-spatial | 4 | 1.128 | 0.092 |

---

### vr-mixed-1024-v2.yuv420p  (2048x1024 yuv420p, 8 frames, sbs, 10.43 ppd on axis, rate scale 1)

| point | Mbit/s | bpp | PSNR-Y | fovPSNR | fov2PSNR | PSNR fovea | PSNR periph | fovSSIM | JOD |
|---|---|---|---|---|---|---|---|---|---|
| flat-q20 | 24.16 | 0.1280 | 43.45 | 43.22 | 43.24 | 42.58 | 43.47 | 0.9909 | 9.389 |
| flat-q26 | 12.54 | 0.0664 | 39.00 | 38.70 | 38.59 | 37.29 | 39.05 | 0.9815 | 9.097 |
| flat-q32 | 7.11 | 0.0377 | 34.75 | 34.43 | 34.17 | 32.36 | 34.82 | 0.9645 | 8.635 |
| flat-q38 | 4.08 | 0.0216 | 30.38 | 30.25 | 30.11 | 28.21 | 30.44 | 0.9406 | 8.167 |
| rc-full-40 | 33.89 | 0.1795 | 31.72 | 33.43 | 36.39 | 45.62 | 31.63 | 0.9820 | 8.939 |
| rc-full-80 | 46.76 | 0.2477 | 32.00 | 33.76 | 36.87 | 50.53 | 31.90 | 0.9854 | 9.381 |
| rc-matched-40 | 34.35 | 0.1820 | 32.70 | 34.28 | 36.85 | 40.96 | 32.62 | 0.9832 | 8.921 |
| rc-matched-80 | 49.99 | 0.2648 | 33.43 | 35.17 | 38.18 | 47.92 | 33.34 | 0.9888 | 9.331 |
| x265-p-refresh-crf26 | 28.22 | 0.1495 | 43.77 | 44.39 | 45.77 | 48.21 | 43.71 | 0.9943 | 9.810 |
| x265-p-refresh-crf32 | 15.67 | 0.0830 | 39.99 | 40.68 | 42.08 | 44.28 | 39.94 | 0.9899 | 9.623 |

**Encode and decode time per frame** (whole frame, both eyes; one core at `nice -n 19`)

| arm | points | encode s/frame | decode s/frame |
|---|---|---|---|
| flat | 4 | 0.482 | 0.056 |
| rc-full | 2 | 0.968 | 0.081 |
| rc-matched | 2 | 0.895 | 0.061 |

---

### vr-turn-256-v2.yuv444p  (512x256 yuv444p, 12 frames, sbs, 2.61 ppd on axis, rate scale 0.0625)

| point | Mbit/s | bpp | PSNR-Y | fovPSNR | fov2PSNR | PSNR fovea | PSNR periph | fovSSIM | JOD |
|---|---|---|---|---|---|---|---|---|---|
| flat-q14 | 9.65 | 0.8177 | 45.95 | 45.80 | 45.44 | 44.41 | 45.99 | 0.9905 | 9.381 |
| flat-q20 | 5.24 | 0.4439 | 40.83 | 40.58 | 39.97 | 38.49 | 40.90 | 0.9795 | 9.002 |
| flat-q26 | 2.17 | 0.1838 | 35.70 | 35.15 | 33.90 | 31.57 | 35.86 | 0.9566 | 8.522 |
| flat-q32 | 1.09 | 0.0927 | 30.81 | 30.30 | 29.26 | 27.12 | 30.94 | 0.9118 | 8.025 |
| flat-q38 | 0.67 | 0.0569 | 27.31 | 27.06 | 26.35 | 24.54 | 27.40 | 0.8581 | 7.611 |
| flat-q44 | 0.28 | 0.0235 | 23.76 | 23.97 | 23.38 | 21.61 | 23.82 | 0.8086 | 7.134 |
| rc-nofov-80 | 5.23 | 0.4432 | 40.05 | 39.53 | 38.71 | 36.59 | 40.17 | 0.9794 | 8.765 |
| rc-spatial-20 | 2.00 | 0.1692 | 27.44 | 28.75 | 30.69 | 32.53 | 27.37 | 0.9280 | 8.041 |
| rc-spatial-40 | 3.25 | 0.2754 | 28.35 | 29.92 | 32.55 | 38.15 | 28.27 | 0.9535 | 8.388 |
| rc-spatial-80 | 5.83 | 0.4946 | 28.62 | 30.28 | 33.14 | 41.01 | 28.53 | 0.9640 | 8.692 |
| rc-spatial-150 | 9.90 | 0.8390 | 28.75 | 30.48 | 33.58 | 49.47 | 28.66 | 0.9725 | 8.864 |
| rc-full-20 | 1.97 | 0.1671 | 27.46 | 28.77 | 30.69 | 32.41 | 27.39 | 0.9286 | 8.041 |
| rc-full-40 | 3.18 | 0.2697 | 28.35 | 29.91 | 32.52 | 37.86 | 28.26 | 0.9533 | 8.380 |
| rc-full-80 | 5.53 | 0.4685 | 28.60 | 30.25 | 33.09 | 40.82 | 28.51 | 0.9633 | 8.680 |
| rc-full-150 | 9.05 | 0.7676 | 28.73 | 30.46 | 33.55 | 49.22 | 28.64 | 0.9720 | 8.858 |
| rc-matched-20 | 2.15 | 0.1826 | 27.63 | 28.78 | 29.96 | 30.14 | 27.59 | 0.9308 | 8.114 |
| rc-matched-40 | 3.16 | 0.2675 | 35.93 | 35.87 | 35.62 | 34.24 | 35.98 | 0.9635 | 8.372 |
| rc-matched-80 | 5.37 | 0.4552 | 40.09 | 39.78 | 39.21 | 37.34 | 40.17 | 0.9803 | 8.798 |
| rc-matched-150 | 9.32 | 0.7900 | 43.45 | 43.19 | 42.63 | 41.15 | 43.51 | 0.9883 | 9.181 |
| rc-matched-noact-20 | 2.22 | 0.1886 | 29.84 | 30.83 | 32.06 | 32.38 | 29.80 | 0.9413 | 8.070 |
| rc-matched-noact-40 | 3.34 | 0.2830 | 36.55 | 36.48 | 36.29 | 35.04 | 36.59 | 0.9661 | 8.437 |
| rc-matched-noact-80 | 5.54 | 0.4693 | 40.39 | 39.98 | 39.24 | 37.17 | 40.50 | 0.9810 | 8.883 |
| rc-matched-noact-150 | 9.44 | 0.8005 | 43.54 | 43.20 | 42.52 | 40.84 | 43.62 | 0.9885 | 9.217 |
| x265-p-refresh-crf18 | 2.48 | 0.2104 | 43.26 | 43.93 | 45.04 | 46.49 | 43.21 | 0.9919 | 9.658 |
| x265-p-refresh-crf24 | 1.29 | 0.1094 | 38.52 | 39.19 | 40.12 | 41.00 | 38.48 | 0.9835 | 9.398 |
| x265-p-refresh-crf30 | 0.72 | 0.0612 | 34.21 | 34.95 | 35.88 | 36.27 | 34.17 | 0.9672 | 9.041 |
| x265-p-refresh-crf36 | 0.42 | 0.0356 | 30.55 | 31.37 | 32.49 | 33.35 | 30.50 | 0.9437 | 8.645 |
| x265-p-refresh-crf42 | 0.30 | 0.0253 | 26.97 | 27.77 | 28.72 | 28.59 | 26.95 | 0.9020 | 8.191 |

**Encode and decode time per frame** (whole frame, both eyes; one core at `nice -n 19`)

| arm | points | encode s/frame | decode s/frame |
|---|---|---|---|
| flat | 6 | 0.150 | 0.007 |
| rc-nofov | 1 | 0.162 | 0.008 |
| rc-spatial | 4 | 0.121 | 0.008 |
| rc-full | 4 | 0.108 | 0.008 |
| rc-matched | 4 | 0.107 | 0.006 |
| rc-matched-noact | 4 | 0.123 | 0.006 |

---

### panel-static-256-v2.yuv444p  (512x256 yuv444p, 6 frames, sbs, 2.61 ppd on axis, rate scale 0.0625)

| point | Mbit/s | bpp | PSNR-Y | fovPSNR | fov2PSNR | PSNR fovea | PSNR periph | fovSSIM | JOD |
|---|---|---|---|---|---|---|---|---|---|
| flat-q14 | 2.69 | 0.2283 | 47.18 | 47.28 | 47.95 | 51.45 | 47.13 | 0.9899 | 9.292 |
| flat-q20 | 1.15 | 0.0979 | 43.13 | 43.18 | 43.96 | 49.23 | 43.06 | 0.9811 | 9.006 |
| flat-q26 | 0.70 | 0.0597 | 39.23 | 39.25 | 40.08 | 47.68 | 39.15 | 0.9663 | 8.650 |
| flat-q32 | 0.44 | 0.0373 | 35.25 | 35.08 | 35.74 | 44.47 | 35.17 | 0.9402 | 8.275 |
| flat-q38 | 0.36 | 0.0307 | 31.47 | 31.03 | 31.44 | 42.13 | 31.39 | 0.9080 | 8.013 |
| flat-q44 | 0.30 | 0.0257 | 29.53 | 29.33 | 30.06 | 39.33 | 29.45 | 0.8933 | 7.809 |
| rc-nofov-80 | 2.80 | 0.2370 | 42.83 | 42.79 | 43.44 | 49.10 | 42.76 | 0.9821 | 8.762 |
| rc-spatial-20 | 1.27 | 0.1073 | 34.02 | 34.63 | 36.35 | 47.00 | 33.93 | 0.9507 | 8.141 |
| rc-spatial-40 | 2.11 | 0.1793 | 36.08 | 36.76 | 38.65 | 49.02 | 35.99 | 0.9690 | 8.506 |
| rc-spatial-80 | 4.06 | 0.3442 | 37.55 | 38.23 | 40.17 | 51.63 | 37.46 | 0.9813 | 8.848 |
| rc-spatial-150 | 5.28 | 0.4473 | 37.90 | 38.61 | 40.63 | 52.68 | 37.81 | 0.9849 | 9.093 |
| rc-full-20 | 1.25 | 0.1063 | 34.02 | 34.63 | 36.35 | 47.19 | 33.93 | 0.9509 | 8.140 |
| rc-full-40 | 2.07 | 0.1752 | 36.05 | 36.74 | 38.63 | 49.03 | 35.97 | 0.9690 | 8.519 |
| rc-full-80 | 3.34 | 0.2833 | 37.54 | 38.22 | 40.17 | 51.67 | 37.45 | 0.9812 | 8.856 |
| rc-full-150 | 4.18 | 0.3541 | 37.89 | 38.61 | 40.63 | 52.76 | 37.80 | 0.9849 | 9.101 |
| rc-matched-20 | 1.59 | 0.1349 | 34.53 | 34.87 | 36.29 | 47.62 | 34.44 | 0.9575 | 8.302 |
| rc-matched-40 | 1.82 | 0.1546 | 38.42 | 38.53 | 39.47 | 47.99 | 38.34 | 0.9668 | 8.394 |
| rc-matched-80 | 2.88 | 0.2445 | 42.71 | 42.82 | 43.66 | 49.03 | 42.64 | 0.9822 | 8.758 |
| rc-matched-150 | 3.01 | 0.2555 | 45.41 | 45.49 | 46.29 | 50.81 | 45.35 | 0.9890 | 9.078 |
| rc-matched-noact-20 | 1.12 | 0.0946 | 36.13 | 35.95 | 36.61 | 46.29 | 36.04 | 0.9501 | 8.138 |
| rc-matched-noact-40 | 1.72 | 0.1458 | 39.54 | 39.66 | 40.58 | 47.71 | 39.46 | 0.9712 | 8.481 |
| rc-matched-noact-80 | 2.35 | 0.1994 | 43.56 | 43.75 | 44.64 | 49.82 | 43.49 | 0.9838 | 8.854 |
| rc-matched-noact-150 | 2.84 | 0.2410 | 45.95 | 46.18 | 47.07 | 51.39 | 45.89 | 0.9899 | 9.139 |
| x265-p-refresh-crf18 | 1.33 | 0.1131 | 50.29 | 50.82 | 52.20 | 58.17 | 50.21 | 0.9959 | 9.715 |
| x265-p-refresh-crf24 | 0.86 | 0.0728 | 47.47 | 47.96 | 49.28 | 54.48 | 47.40 | 0.9928 | 9.518 |
| x265-p-refresh-crf30 | 0.61 | 0.0518 | 44.20 | 44.69 | 46.05 | 52.42 | 44.12 | 0.9873 | 9.216 |
| x265-p-refresh-crf36 | 0.47 | 0.0402 | 40.74 | 41.23 | 42.67 | 50.60 | 40.66 | 0.9786 | 8.914 |
| x265-p-refresh-crf42 | 0.41 | 0.0348 | 36.62 | 37.17 | 38.73 | 49.17 | 36.53 | 0.9608 | 8.570 |

**Encode and decode time per frame** (whole frame, both eyes; one core at `nice -n 19`)

| arm | points | encode s/frame | decode s/frame |
|---|---|---|---|
| flat | 6 | 0.039 | 0.004 |
| rc-nofov | 1 | 0.086 | 0.005 |
| rc-spatial | 4 | 0.076 | 0.007 |
| rc-full | 4 | 0.072 | 0.006 |
| rc-matched | 4 | 0.082 | 0.006 |
| rc-matched-noact | 4 | 0.075 | 0.005 |

---

### mono-mixed-256-v2.yuv444p  (256x256 yuv444p, 12 frames, mono, 2.61 ppd on axis, rate scale 0.03125)

| point | Mbit/s | bpp | PSNR-Y | fovPSNR | fov2PSNR | PSNR fovea | PSNR periph | fovSSIM | JOD |
|---|---|---|---|---|---|---|---|---|---|
| flat-q14 | 4.35 | 0.7372 | 45.91 | 45.82 | 45.72 | 44.87 | 45.94 | 0.9903 | 9.311 |
| flat-q20 | 2.29 | 0.3889 | 40.92 | 40.68 | 40.40 | 38.89 | 40.97 | 0.9795 | 8.927 |
| flat-q26 | 1.06 | 0.1790 | 35.88 | 35.45 | 34.87 | 32.42 | 35.99 | 0.9571 | 8.466 |
| flat-q32 | 0.61 | 0.1029 | 31.14 | 30.74 | 30.13 | 27.57 | 31.26 | 0.9151 | 7.984 |
| flat-q38 | 0.41 | 0.0698 | 27.55 | 27.33 | 27.03 | 25.25 | 27.62 | 0.8575 | 7.571 |
| flat-q44 | 0.15 | 0.0256 | 23.81 | 24.04 | 23.97 | 21.99 | 23.86 | 0.8059 | 7.092 |
| rc-nofov-80 | 2.83 | 0.4794 | 40.74 | 40.16 | 39.56 | 37.44 | 40.85 | 0.9806 | 8.704 |
| rc-spatial-20 | 1.03 | 0.1751 | 26.91 | 28.04 | 29.69 | 30.29 | 26.86 | 0.9095 | 7.920 |
| rc-spatial-40 | 1.85 | 0.3142 | 28.43 | 29.97 | 32.63 | 38.27 | 28.34 | 0.9528 | 8.344 |
| rc-spatial-80 | 3.17 | 0.5379 | 28.72 | 30.37 | 33.27 | 42.43 | 28.63 | 0.9651 | 8.665 |
| rc-spatial-150 | 5.05 | 0.8565 | 28.84 | 30.54 | 33.61 | 49.61 | 28.74 | 0.9721 | 8.845 |
| rc-full-20 | 1.01 | 0.1721 | 26.94 | 28.08 | 29.76 | 30.62 | 26.89 | 0.9099 | 7.919 |
| rc-full-40 | 1.83 | 0.3094 | 28.41 | 29.95 | 32.62 | 38.44 | 28.32 | 0.9526 | 8.347 |
| rc-full-80 | 3.15 | 0.5335 | 28.71 | 30.36 | 33.25 | 42.08 | 28.63 | 0.9648 | 8.662 |
| rc-full-150 | 4.43 | 0.7510 | 28.83 | 30.53 | 33.60 | 49.56 | 28.74 | 0.9718 | 8.842 |
| rc-matched-20 | 1.34 | 0.2268 | 29.33 | 30.50 | 32.23 | 33.07 | 29.28 | 0.9466 | 8.097 |
| rc-matched-40 | 1.73 | 0.2935 | 36.83 | 36.64 | 36.44 | 34.68 | 36.89 | 0.9661 | 8.350 |
| rc-matched-80 | 2.85 | 0.4826 | 41.00 | 40.63 | 40.24 | 38.22 | 41.08 | 0.9820 | 8.765 |
| rc-matched-150 | 4.88 | 0.8270 | 44.62 | 44.44 | 44.33 | 43.49 | 44.64 | 0.9897 | 9.124 |
| rc-matched-noact-20 | 1.25 | 0.2111 | 32.01 | 32.59 | 33.33 | 32.70 | 31.99 | 0.9460 | 8.029 |
| rc-matched-noact-40 | 1.74 | 0.2958 | 37.38 | 37.23 | 37.16 | 35.49 | 37.43 | 0.9688 | 8.407 |
| rc-matched-noact-80 | 2.84 | 0.4822 | 41.39 | 41.05 | 40.74 | 38.92 | 41.46 | 0.9835 | 8.856 |
| rc-matched-noact-150 | 4.80 | 0.8146 | 44.69 | 44.36 | 43.98 | 42.09 | 44.77 | 0.9902 | 9.182 |
| x265-p-refresh-crf18 | 1.22 | 0.2070 | 43.69 | 44.36 | 45.63 | 47.62 | 43.63 | 0.9929 | 9.632 |
| x265-p-refresh-crf24 | 0.70 | 0.1180 | 39.63 | 40.31 | 41.44 | 42.00 | 39.59 | 0.9868 | 9.380 |
| x265-p-refresh-crf30 | 0.42 | 0.0720 | 35.09 | 35.84 | 37.00 | 37.11 | 35.05 | 0.9727 | 9.016 |
| x265-p-refresh-crf36 | 0.28 | 0.0481 | 31.48 | 32.25 | 33.41 | 33.34 | 31.45 | 0.9499 | 8.623 |
| x265-p-refresh-crf42 | 0.22 | 0.0377 | 28.52 | 29.17 | 30.05 | 28.72 | 28.52 | 0.9151 | 8.160 |

**Encode and decode time per frame** (whole frame, both eyes; one core at `nice -n 19`)

| arm | points | encode s/frame | decode s/frame |
|---|---|---|---|
| flat | 6 | 0.040 | 0.003 |
| rc-nofov | 1 | 0.063 | 0.004 |
| rc-spatial | 4 | 0.053 | 0.004 |
| rc-full | 4 | 0.049 | 0.004 |
| rc-matched | 4 | 0.057 | 0.004 |
| rc-matched-noact | 4 | 0.054 | 0.003 |

---

## 4. Bits at equal foveated quality, and the PSNR-Y cost

The number the branch was asked for. Every rate-controlled point is compared
against the flat-QP curve of the same sequence, interpolated in log-rate at
the quality that point reached; a positive saving means fewer bits for the
same foveated quality. **Every one of them is negative**, and the PSNR-Y
column next to it is the cost that was supposed to buy them.

Summary, `vr-mixed-1024-v2` 444, over the four target rates (range across the
rate points; the full per-point tables follow):

| arm | `fov_psnr_y` saving | `fov2_psnr_y` saving | FovVideoVDP saving |
|---|---|---|---|
| `rc-nofov` (allocator only) | -104 % | -99 % | -304 % |
| `rc-spatial` (+ foveation, Pico 4 ladder) | -881 to -279 % | -509 to -156 % | -249 to -82 % |
| `rc-full` (+ temporal ladder) | -819 to -264 % | -470 to -146 % | -241 to -71 % |
| `rc-matched` (clip-density ladder) | -690 to -262 % | -395 to -152 % | -248 to -64 % |
| `rc-matched-noact` (and `dQ_act` off) | -708 to -285 % | -405 to -171 % | -204 to -63 % |
| `x265-p-refresh`, for scale | -6 to +13 % | +8 to +31 % | +55 to +65 % |

Read the bottom row first: the foveated HEVC anchor saves 13 % of the bits
against `nxv-enc`'s own flat-QP curve at equal eccentricity-weighted PSNR, and
55 to 65 % at equal FovVideoVDP. That is the target. Every rate-controlled arm
is on the wrong side of zero by a factor.

The four rate-controlled arms differ from each other by far less than they
differ from zero, which is itself a finding: the choice of ladder calibration
(`rc-full` versus `rc-matched`) and the activity term (`rc-matched` versus
`rc-matched-noact`) are second-order next to the resampling ceiling of section
7.2 and the allocator deficit of 7.1. FovVideoVDP is the mildest of the four
judges, as expected -- it is the one that actually models peripheral contrast
sensitivity -- but it still asks for 1.6x to 3.4x the bits.

At the top of the rate range the comparison runs off the end of the flat-QP
curve and is reported as `--` rather than extrapolated.

### vr-mixed-1024-v2.yuv444p  (2048x1024 yuv444p, 8 frames, sbs, 10.43 ppd on axis, rate scale 1)

**Bits at equal `fov_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov_psnr_y | flat Mbit/s at the same fov_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-full-20 | 19.46 | 32.265 | 5.35 | -263.8 | 30.71 |
| rc-full-40 | 36.15 | 33.375 | 6.23 | -479.9 | 31.68 |
| rc-full-80 | 54.99 | 33.754 | 6.57 | -737.1 | 31.99 |
| rc-full-150 | 61.61 | 33.898 | 6.70 | -819.3 | 32.11 |
| rc-matched-20 | 20.93 | 32.824 | 5.78 | -262.2 | 31.33 |
| rc-matched-40 | 36.62 | 34.220 | 7.01 | -422.7 | 32.65 |
| rc-matched-80 | 57.24 | 35.151 | 8.01 | -615.0 | 33.42 |
| rc-matched-150 | 65.62 | 35.404 | 8.30 | -690.3 | 33.62 |
| rc-matched-noact-20 | 21.26 | 32.503 | 5.53 | -284.6 | 31.03 |
| rc-matched-noact-40 | 37.95 | 34.412 | 7.20 | -427.3 | 32.81 |
| rc-matched-noact-80 | 57.85 | 35.227 | 8.09 | -614.7 | 33.48 |
| rc-matched-noact-150 | 67.24 | 35.420 | 8.32 | -708.1 | 33.63 |
| rc-nofov-80 | 62.98 | 44.176 | 30.85 | -104.1 | 44.14 |
| rc-spatial-20 | 20.29 | 32.270 | 5.35 | -279.1 | 30.71 |
| rc-spatial-40 | 37.06 | 33.381 | 6.24 | -494.0 | 31.68 |
| rc-spatial-80 | 57.80 | 33.775 | 6.59 | -777.2 | 32.01 |
| rc-spatial-150 | 66.00 | 33.922 | 6.72 | -881.5 | 32.13 |
| x265-p-refresh-crf38 | 8.97 | 36.828 | 10.19 | 12.0 | 36.06 |
| x265-p-refresh-crf32 | 15.65 | 40.602 | 18.04 | 13.3 | 39.91 |
| x265-p-refresh-crf26 | 28.14 | 44.394 | 31.72 | 11.3 | 43.77 |
| x265-p-refresh-crf20 | 52.04 | 48.327 | 52.42 | 0.7 | 47.72 |
| x265-p-refresh-crf16 | 77.74 | 50.934 | 73.08 | -6.4 | 50.36 |
| x265-p-refresh-crf10 | 142.95 | 54.596 | -- | -- | 54.11 |

**Bits at equal `fov2_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov2_psnr_y | flat Mbit/s at the same fov2_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-full-20 | 19.46 | 34.824 | 7.90 | -146.2 | 30.71 |
| rc-full-40 | 36.15 | 36.307 | 9.72 | -272.0 | 31.68 |
| rc-full-80 | 54.99 | 36.862 | 10.50 | -423.8 | 31.99 |
| rc-full-150 | 61.61 | 37.066 | 10.80 | -470.4 | 32.11 |
| rc-matched-20 | 20.93 | 35.191 | 8.32 | -151.6 | 31.33 |
| rc-matched-40 | 36.62 | 36.757 | 10.35 | -254.0 | 32.65 |
| rc-matched-80 | 57.24 | 38.146 | 12.56 | -355.9 | 33.42 |
| rc-matched-150 | 65.62 | 38.540 | 13.26 | -394.7 | 33.62 |
| rc-matched-noact-20 | 21.26 | 34.779 | 7.85 | -170.6 | 31.03 |
| rc-matched-noact-40 | 37.95 | 37.013 | 10.72 | -253.9 | 32.81 |
| rc-matched-noact-80 | 57.85 | 38.259 | 12.75 | -353.5 | 33.48 |
| rc-matched-noact-150 | 67.24 | 38.572 | 13.32 | -404.7 | 33.63 |
| rc-nofov-80 | 62.98 | 44.405 | 31.67 | -98.8 | 44.14 |
| rc-spatial-20 | 20.29 | 34.846 | 7.93 | -155.9 | 30.71 |
| rc-spatial-40 | 37.06 | 36.315 | 9.73 | -281.0 | 31.68 |
| rc-spatial-80 | 57.80 | 36.886 | 10.53 | -448.7 | 32.01 |
| rc-spatial-150 | 66.00 | 37.093 | 10.84 | -508.7 | 32.13 |
| x265-p-refresh-crf38 | 8.97 | 38.371 | 12.96 | 30.8 | 36.06 |
| x265-p-refresh-crf32 | 15.65 | 42.022 | 22.73 | 31.2 | 39.91 |
| x265-p-refresh-crf26 | 28.14 | 45.775 | 37.53 | 25.0 | 43.77 |
| x265-p-refresh-crf20 | 52.04 | 49.731 | 61.24 | 15.0 | 47.72 |
| x265-p-refresh-crf16 | 77.74 | 52.337 | 84.48 | 8.0 | 50.36 |
| x265-p-refresh-crf10 | 142.95 | 55.942 | -- | -- | 54.11 |

**Bits at equal `fvvdp` against the flat-QP curve**

| point | Mbit/s | fvvdp | flat Mbit/s at the same fvvdp | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-full-20 | 19.46 | 8.590 | 6.84 | -184.6 | 30.71 |
| rc-full-40 | 36.15 | 8.932 | 10.62 | -240.5 | 31.68 |
| rc-full-80 | 54.99 | 9.379 | 26.08 | -110.9 | 31.99 |
| rc-full-150 | 61.61 | 9.524 | 36.07 | -70.8 | 32.11 |
| rc-matched-20 | 20.93 | 8.870 | 9.80 | -113.5 | 31.33 |
| rc-matched-40 | 36.62 | 8.924 | 10.51 | -248.5 | 32.65 |
| rc-matched-80 | 57.24 | 9.335 | 23.39 | -144.8 | 33.42 |
| rc-matched-150 | 65.62 | 9.572 | 40.07 | -63.8 | 33.62 |
| rc-matched-noact-20 | 21.26 | 8.659 | 7.47 | -184.6 | 31.03 |
| rc-matched-noact-40 | 37.95 | 9.059 | 12.49 | -203.7 | 32.81 |
| rc-matched-noact-80 | 57.85 | 9.350 | 24.28 | -138.3 | 33.48 |
| rc-matched-noact-150 | 67.24 | 9.586 | 41.32 | -62.7 | 33.63 |
| rc-nofov-80 | 62.98 | 9.172 | 15.58 | -304.3 | 44.14 |
| rc-spatial-20 | 20.29 | 8.590 | 6.84 | -196.6 | 30.71 |
| rc-spatial-40 | 37.06 | 8.932 | 10.61 | -249.2 | 31.68 |
| rc-spatial-80 | 57.80 | 9.383 | 26.30 | -119.8 | 32.01 |
| rc-spatial-150 | 66.00 | 9.527 | 36.31 | -81.8 | 32.13 |
| x265-p-refresh-crf38 | 8.97 | 9.268 | 19.79 | 54.7 | 36.06 |
| x265-p-refresh-crf32 | 15.65 | 9.609 | 43.48 | 64.0 | 39.91 |
| x265-p-refresh-crf26 | 28.14 | 9.811 | 81.03 | 65.3 | 43.77 |
| x265-p-refresh-crf20 | 52.04 | 9.908 | -- | -- | 47.72 |
| x265-p-refresh-crf16 | 77.74 | 9.939 | -- | -- | 50.36 |
| x265-p-refresh-crf10 | 142.95 | 9.968 | -- | -- | 54.11 |

---

### vr-mixed-1024-v2.yuv420p  (2048x1024 yuv420p, 8 frames, sbs, 10.43 ppd on axis, rate scale 1)

**Bits at equal `fov_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov_psnr_y | flat Mbit/s at the same fov_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-full-40 | 33.89 | 33.429 | 6.22 | -444.6 | 31.72 |
| rc-full-80 | 46.76 | 33.764 | 6.50 | -618.9 | 32.00 |
| rc-matched-40 | 34.35 | 34.285 | 6.97 | -392.8 | 32.70 |
| rc-matched-80 | 49.99 | 35.166 | 7.84 | -537.9 | 33.43 |
| x265-p-refresh-crf26 | 28.22 | 44.390 | -- | -- | 43.77 |
| x265-p-refresh-crf32 | 15.67 | 40.677 | 16.70 | 6.2 | 39.99 |

**Bits at equal `fov2_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov2_psnr_y | flat Mbit/s at the same fov2_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-full-40 | 33.89 | 36.389 | 9.45 | -258.5 | 31.72 |
| rc-full-80 | 46.76 | 36.874 | 10.06 | -364.7 | 32.00 |
| rc-matched-40 | 34.35 | 36.851 | 10.03 | -242.4 | 32.70 |
| rc-matched-80 | 49.99 | 38.180 | 11.90 | -320.1 | 33.43 |
| x265-p-refresh-crf26 | 28.22 | 45.769 | -- | -- | 43.77 |
| x265-p-refresh-crf32 | 15.67 | 42.077 | 20.52 | 23.6 | 39.99 |

**Bits at equal `fvvdp` against the flat-QP curve**

| point | Mbit/s | fvvdp | flat Mbit/s at the same fvvdp | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-full-40 | 33.89 | 8.939 | 10.32 | -228.2 | 31.72 |
| rc-full-80 | 46.76 | 9.381 | 23.74 | -96.9 | 32.00 |
| rc-matched-40 | 34.35 | 8.921 | 10.10 | -240.1 | 32.70 |
| rc-matched-80 | 49.99 | 9.331 | 21.20 | -135.8 | 33.43 |
| x265-p-refresh-crf26 | 28.22 | 9.810 | -- | -- | 43.77 |
| x265-p-refresh-crf32 | 15.67 | 9.623 | -- | -- | 39.99 |

---

### vr-turn-256-v2.yuv444p  (512x256 yuv444p, 12 frames, sbs, 2.61 ppd on axis, rate scale 0.0625)

**Bits at equal `fov_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov_psnr_y | flat Mbit/s at the same fov_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 5.23 | 39.527 | 4.42 | -18.4 | 40.05 |
| rc-spatial-20 | 2.00 | 28.749 | 0.87 | -130.6 | 27.44 |
| rc-spatial-40 | 3.25 | 29.921 | 1.03 | -214.6 | 28.35 |
| rc-spatial-80 | 5.83 | 30.279 | 1.09 | -435.4 | 28.62 |
| rc-spatial-150 | 9.90 | 30.476 | 1.12 | -783.0 | 28.75 |
| rc-full-20 | 1.97 | 28.766 | 0.87 | -127.1 | 27.46 |
| rc-full-40 | 3.18 | 29.909 | 1.03 | -208.6 | 28.35 |
| rc-full-80 | 5.53 | 30.253 | 1.09 | -409.2 | 28.60 |
| rc-full-150 | 9.05 | 30.456 | 1.12 | -710.3 | 28.73 |
| rc-matched-20 | 2.15 | 28.780 | 0.87 | -147.7 | 27.63 |
| rc-matched-40 | 3.16 | 35.870 | 2.44 | -29.4 | 35.93 |
| rc-matched-80 | 5.37 | 39.780 | 4.60 | -16.7 | 40.09 |
| rc-matched-150 | 9.32 | 43.189 | 7.11 | -31.1 | 43.45 |
| rc-matched-noact-20 | 2.22 | 30.834 | 1.18 | -88.7 | 29.84 |
| rc-matched-noact-40 | 3.34 | 36.485 | 2.69 | -23.9 | 36.55 |
| rc-matched-noact-80 | 5.54 | 39.976 | 4.75 | -16.5 | 40.39 |
| rc-matched-noact-150 | 9.44 | 43.195 | 7.11 | -32.7 | 43.54 |
| x265-p-refresh-crf18 | 2.48 | 43.933 | 7.76 | 68.0 | 43.26 |
| x265-p-refresh-crf24 | 1.29 | 39.193 | 4.18 | 69.2 | 38.52 |
| x265-p-refresh-crf30 | 0.72 | 34.947 | 2.11 | 65.7 | 34.21 |
| x265-p-refresh-crf36 | 0.42 | 31.367 | 1.27 | 66.9 | 30.55 |
| x265-p-refresh-crf42 | 0.30 | 27.774 | 0.75 | 60.0 | 26.97 |

**Bits at equal `fov2_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov2_psnr_y | flat Mbit/s at the same fov2_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 5.23 | 38.707 | 4.36 | -19.9 | 40.05 |
| rc-spatial-20 | 2.00 | 30.692 | 1.35 | -47.7 | 27.44 |
| rc-spatial-40 | 3.25 | 32.554 | 1.78 | -82.7 | 28.35 |
| rc-spatial-80 | 5.83 | 33.138 | 1.94 | -201.1 | 28.62 |
| rc-spatial-150 | 9.90 | 33.581 | 2.07 | -378.5 | 28.75 |
| rc-full-20 | 1.97 | 30.689 | 1.35 | -46.0 | 27.46 |
| rc-full-40 | 3.18 | 32.518 | 1.77 | -79.9 | 28.35 |
| rc-full-80 | 5.53 | 33.093 | 1.92 | -187.2 | 28.60 |
| rc-full-150 | 9.05 | 33.553 | 2.06 | -339.6 | 28.73 |
| rc-matched-20 | 2.15 | 29.964 | 1.21 | -77.5 | 27.63 |
| rc-matched-40 | 3.16 | 35.622 | 2.78 | -13.3 | 35.93 |
| rc-matched-80 | 5.37 | 39.210 | 4.69 | -14.5 | 40.09 |
| rc-matched-150 | 9.32 | 42.633 | 7.05 | -32.2 | 43.45 |
| rc-matched-noact-20 | 2.22 | 32.061 | 1.65 | -34.6 | 29.84 |
| rc-matched-noact-40 | 3.34 | 36.285 | 3.07 | -8.9 | 36.55 |
| rc-matched-noact-80 | 5.54 | 39.242 | 4.71 | -17.5 | 40.39 |
| rc-matched-noact-150 | 9.44 | 42.522 | 6.96 | -35.6 | 43.54 |
| x265-p-refresh-crf18 | 2.48 | 45.043 | 9.23 | 73.1 | 43.26 |
| x265-p-refresh-crf24 | 1.29 | 40.122 | 5.33 | 75.8 | 38.52 |
| x265-p-refresh-crf30 | 0.72 | 35.875 | 2.89 | 75.0 | 34.21 |
| x265-p-refresh-crf36 | 0.42 | 32.491 | 1.76 | 76.1 | 30.55 |
| x265-p-refresh-crf42 | 0.30 | 28.718 | 1.00 | 70.1 | 26.97 |

**Bits at equal `fvvdp` against the flat-QP curve**

| point | Mbit/s | fvvdp | flat Mbit/s at the same fvvdp | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 5.23 | 8.765 | 3.39 | -54.2 | 40.05 |
| rc-spatial-20 | 2.00 | 8.041 | 1.12 | -78.4 | 27.44 |
| rc-spatial-40 | 3.25 | 8.388 | 1.80 | -80.1 | 28.35 |
| rc-spatial-80 | 5.83 | 8.692 | 2.96 | -97.0 | 28.62 |
| rc-spatial-150 | 9.90 | 8.864 | 4.06 | -143.6 | 28.75 |
| rc-full-20 | 1.97 | 8.041 | 1.12 | -76.3 | 27.46 |
| rc-full-40 | 3.18 | 8.380 | 1.78 | -78.4 | 28.35 |
| rc-full-80 | 5.53 | 8.680 | 2.90 | -90.5 | 28.60 |
| rc-full-150 | 9.05 | 8.858 | 4.02 | -125.5 | 28.73 |
| rc-matched-20 | 2.15 | 8.114 | 1.24 | -74.2 | 27.63 |
| rc-matched-40 | 3.16 | 8.372 | 1.76 | -78.8 | 35.93 |
| rc-matched-80 | 5.37 | 8.798 | 3.60 | -49.3 | 40.09 |
| rc-matched-150 | 9.32 | 9.181 | 6.99 | -33.3 | 43.45 |
| rc-matched-noact-20 | 2.22 | 8.070 | 1.16 | -91.4 | 29.84 |
| rc-matched-noact-40 | 3.34 | 8.437 | 1.93 | -73.2 | 36.55 |
| rc-matched-noact-80 | 5.54 | 8.883 | 4.20 | -31.7 | 40.39 |
| rc-matched-noact-150 | 9.44 | 9.217 | 7.41 | -27.4 | 43.54 |
| x265-p-refresh-crf18 | 2.48 | 9.658 | -- | -- | 43.26 |
| x265-p-refresh-crf24 | 1.29 | 9.398 | -- | -- | 38.52 |
| x265-p-refresh-crf30 | 0.72 | 9.041 | 5.57 | 87.0 | 34.21 |
| x265-p-refresh-crf36 | 0.42 | 8.645 | 2.72 | 84.5 | 30.55 |
| x265-p-refresh-crf42 | 0.30 | 8.191 | 1.37 | 78.3 | 26.97 |

---

### panel-static-256-v2.yuv444p  (512x256 yuv444p, 6 frames, sbs, 2.61 ppd on axis, rate scale 0.0625)

**Bits at equal `fov_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov_psnr_y | flat Mbit/s at the same fov_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 2.80 | 42.786 | 1.10 | -154.6 | 42.83 |
| rc-spatial-20 | 1.27 | 34.627 | 0.43 | -193.9 | 34.02 |
| rc-spatial-40 | 2.11 | 36.764 | 0.53 | -297.5 | 36.08 |
| rc-spatial-80 | 4.06 | 38.229 | 0.63 | -547.2 | 37.55 |
| rc-spatial-150 | 5.28 | 38.609 | 0.65 | -705.8 | 37.90 |
| rc-full-20 | 1.25 | 34.629 | 0.43 | -191.1 | 34.02 |
| rc-full-40 | 2.07 | 36.745 | 0.53 | -289.4 | 36.05 |
| rc-full-80 | 3.34 | 38.223 | 0.63 | -433.0 | 37.54 |
| rc-full-150 | 4.18 | 38.606 | 0.65 | -538.1 | 37.89 |
| rc-matched-20 | 1.59 | 34.867 | 0.44 | -265.3 | 34.53 |
| rc-matched-40 | 1.82 | 38.531 | 0.65 | -181.0 | 38.42 |
| rc-matched-80 | 2.88 | 42.820 | 1.10 | -161.6 | 42.71 |
| rc-matched-150 | 3.01 | 45.493 | 1.86 | -61.9 | 45.41 |
| rc-matched-noact-20 | 1.12 | 35.948 | 0.49 | -130.0 | 36.13 |
| rc-matched-noact-40 | 1.72 | 39.660 | 0.74 | -132.1 | 39.54 |
| rc-matched-noact-80 | 2.35 | 43.748 | 1.30 | -81.3 | 43.56 |
| rc-matched-noact-150 | 2.84 | 46.175 | 2.14 | -32.7 | 45.95 |
| x265-p-refresh-crf18 | 1.33 | 50.816 | -- | -- | 50.29 |
| x265-p-refresh-crf24 | 0.86 | 47.957 | -- | -- | 47.47 |
| x265-p-refresh-crf30 | 0.61 | 44.691 | 1.58 | 61.2 | 44.20 |
| x265-p-refresh-crf36 | 0.47 | 41.235 | 0.90 | 47.6 | 40.74 |
| x265-p-refresh-crf42 | 0.41 | 37.173 | 0.56 | 26.4 | 36.62 |

**Bits at equal `fov2_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov2_psnr_y | flat Mbit/s at the same fov2_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 2.80 | 43.445 | 1.08 | -158.7 | 42.83 |
| rc-spatial-20 | 1.27 | 36.347 | 0.47 | -169.4 | 34.02 |
| rc-spatial-40 | 2.11 | 38.652 | 0.60 | -250.6 | 36.08 |
| rc-spatial-80 | 4.06 | 40.174 | 0.71 | -470.0 | 37.55 |
| rc-spatial-150 | 5.28 | 40.631 | 0.76 | -598.8 | 37.90 |
| rc-full-20 | 1.25 | 36.353 | 0.47 | -166.6 | 34.02 |
| rc-full-40 | 2.07 | 38.635 | 0.60 | -243.4 | 36.05 |
| rc-full-80 | 3.34 | 40.172 | 0.71 | -369.3 | 37.54 |
| rc-full-150 | 4.18 | 40.634 | 0.76 | -453.0 | 37.89 |
| rc-matched-20 | 1.59 | 36.285 | 0.47 | -240.9 | 34.53 |
| rc-matched-40 | 1.82 | 39.467 | 0.66 | -176.9 | 38.42 |
| rc-matched-80 | 2.88 | 43.657 | 1.11 | -159.8 | 42.71 |
| rc-matched-150 | 3.01 | 46.287 | 1.89 | -59.3 | 45.41 |
| rc-matched-noact-20 | 1.12 | 36.612 | 0.48 | -130.8 | 36.13 |
| rc-matched-noact-40 | 1.72 | 40.580 | 0.75 | -129.3 | 39.54 |
| rc-matched-noact-80 | 2.35 | 44.637 | 1.33 | -76.5 | 43.56 |
| rc-matched-noact-150 | 2.84 | 47.069 | 2.23 | -27.2 | 45.95 |
| x265-p-refresh-crf18 | 1.33 | 52.200 | -- | -- | 50.29 |
| x265-p-refresh-crf24 | 0.86 | 49.282 | -- | -- | 47.47 |
| x265-p-refresh-crf30 | 0.61 | 46.055 | 1.80 | 66.1 | 44.20 |
| x265-p-refresh-crf36 | 0.47 | 42.666 | 0.98 | 51.6 | 40.74 |
| x265-p-refresh-crf42 | 0.41 | 38.727 | 0.61 | 32.5 | 36.62 |

**Bits at equal `fvvdp` against the flat-QP curve**

| point | Mbit/s | fvvdp | flat Mbit/s at the same fvvdp | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 2.80 | 8.762 | 0.82 | -239.8 | 42.83 |
| rc-spatial-20 | 1.27 | 8.141 | 0.40 | -217.8 | 34.02 |
| rc-spatial-40 | 2.11 | 8.506 | 0.59 | -259.9 | 36.08 |
| rc-spatial-80 | 4.06 | 8.848 | 0.93 | -338.2 | 37.55 |
| rc-spatial-150 | 5.28 | 9.093 | 1.49 | -253.6 | 37.90 |
| rc-full-20 | 1.25 | 8.140 | 0.40 | -215.0 | 34.02 |
| rc-full-40 | 2.07 | 8.519 | 0.60 | -246.1 | 36.05 |
| rc-full-80 | 3.34 | 8.856 | 0.94 | -256.4 | 37.54 |
| rc-full-150 | 4.18 | 9.101 | 1.53 | -173.4 | 37.89 |
| rc-matched-20 | 1.59 | 8.302 | 0.45 | -249.8 | 34.53 |
| rc-matched-40 | 1.82 | 8.394 | 0.51 | -257.3 | 38.42 |
| rc-matched-80 | 2.88 | 8.758 | 0.82 | -252.5 | 42.71 |
| rc-matched-150 | 3.01 | 9.078 | 1.43 | -111.1 | 45.41 |
| rc-matched-noact-20 | 1.12 | 8.138 | 0.40 | -181.0 | 36.13 |
| rc-matched-noact-40 | 1.72 | 8.481 | 0.57 | -202.0 | 39.54 |
| rc-matched-noact-80 | 2.35 | 8.854 | 0.93 | -151.7 | 43.56 |
| rc-matched-noact-150 | 2.84 | 9.139 | 1.71 | -66.2 | 45.95 |
| x265-p-refresh-crf18 | 1.33 | 9.715 | -- | -- | 50.29 |
| x265-p-refresh-crf24 | 0.86 | 9.518 | -- | -- | 47.47 |
| x265-p-refresh-crf30 | 0.61 | 9.216 | 2.15 | 71.6 | 44.20 |
| x265-p-refresh-crf36 | 0.47 | 8.914 | 1.02 | 53.4 | 40.74 |
| x265-p-refresh-crf42 | 0.41 | 8.570 | 0.64 | 35.6 | 36.62 |

---

### mono-mixed-256-v2.yuv444p  (256x256 yuv444p, 12 frames, mono, 2.61 ppd on axis, rate scale 0.03125)

**Bits at equal `fov_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov_psnr_y | flat Mbit/s at the same fov_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 2.83 | 40.164 | 2.12 | -33.1 | 40.74 |
| rc-spatial-20 | 1.03 | 28.040 | 0.45 | -131.4 | 26.91 |
| rc-spatial-40 | 1.85 | 29.974 | 0.56 | -233.1 | 28.43 |
| rc-spatial-80 | 3.17 | 30.367 | 0.58 | -445.2 | 28.72 |
| rc-spatial-150 | 5.05 | 30.537 | 0.59 | -751.6 | 28.84 |
| rc-full-20 | 1.01 | 28.080 | 0.45 | -126.4 | 26.94 |
| rc-full-40 | 1.83 | 29.952 | 0.55 | -228.9 | 28.41 |
| rc-full-80 | 3.15 | 30.357 | 0.58 | -441.4 | 28.71 |
| rc-full-150 | 4.43 | 30.530 | 0.59 | -647.3 | 28.83 |
| rc-matched-20 | 1.34 | 30.498 | 0.59 | -126.5 | 29.33 |
| rc-matched-40 | 1.73 | 36.641 | 1.26 | -37.4 | 36.83 |
| rc-matched-80 | 2.85 | 40.634 | 2.28 | -24.9 | 41.00 |
| rc-matched-150 | 4.88 | 44.440 | 3.66 | -33.2 | 44.62 |
| rc-matched-noact-20 | 1.25 | 32.587 | 0.75 | -65.1 | 32.01 |
| rc-matched-noact-40 | 1.74 | 37.227 | 1.37 | -26.9 | 37.38 |
| rc-matched-noact-80 | 2.84 | 41.052 | 2.40 | -18.4 | 41.39 |
| rc-matched-noact-150 | 4.80 | 44.361 | 3.63 | -32.5 | 44.69 |
| x265-p-refresh-crf18 | 1.22 | 44.362 | 3.63 | 66.3 | 43.69 |
| x265-p-refresh-crf24 | 0.70 | 40.307 | 2.17 | 67.9 | 39.63 |
| x265-p-refresh-crf30 | 0.42 | 35.844 | 1.12 | 62.1 | 35.09 |
| x265-p-refresh-crf36 | 0.28 | 32.248 | 0.72 | 60.8 | 31.48 |
| x265-p-refresh-crf42 | 0.22 | 29.171 | 0.51 | 56.2 | 28.52 |

**Bits at equal `fov2_psnr_y` against the flat-QP curve**

| point | Mbit/s | fov2_psnr_y | flat Mbit/s at the same fov2_psnr_y | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 2.83 | 39.560 | 2.04 | -38.7 | 40.74 |
| rc-spatial-20 | 1.03 | 29.685 | 0.57 | -79.9 | 26.91 |
| rc-spatial-40 | 1.85 | 32.634 | 0.81 | -127.8 | 28.43 |
| rc-spatial-80 | 3.17 | 33.271 | 0.88 | -262.0 | 28.72 |
| rc-spatial-150 | 5.05 | 33.608 | 0.91 | -454.3 | 28.84 |
| rc-full-20 | 1.01 | 29.760 | 0.58 | -75.1 | 26.94 |
| rc-full-40 | 1.83 | 32.618 | 0.81 | -124.8 | 28.41 |
| rc-full-80 | 3.15 | 33.255 | 0.87 | -259.8 | 28.71 |
| rc-full-150 | 4.43 | 33.599 | 0.91 | -386.5 | 28.83 |
| rc-matched-20 | 1.34 | 32.231 | 0.78 | -72.4 | 29.33 |
| rc-matched-40 | 1.73 | 36.441 | 1.32 | -31.5 | 36.83 |
| rc-matched-80 | 2.85 | 40.245 | 2.24 | -26.9 | 41.00 |
| rc-matched-150 | 4.88 | 44.333 | 3.68 | -32.5 | 44.62 |
| rc-matched-noact-20 | 1.25 | 33.333 | 0.88 | -41.1 | 32.01 |
| rc-matched-noact-40 | 1.74 | 37.159 | 1.46 | -19.8 | 37.38 |
| rc-matched-noact-80 | 2.84 | 40.742 | 2.39 | -19.0 | 41.39 |
| rc-matched-noact-150 | 4.80 | 43.979 | 3.53 | -36.2 | 44.69 |
| x265-p-refresh-crf18 | 1.22 | 45.633 | 4.30 | 71.6 | 43.69 |
| x265-p-refresh-crf24 | 0.70 | 41.441 | 2.60 | 73.2 | 39.63 |
| x265-p-refresh-crf30 | 0.42 | 37.003 | 1.42 | 70.2 | 35.09 |
| x265-p-refresh-crf36 | 0.28 | 33.406 | 0.89 | 68.1 | 31.48 |
| x265-p-refresh-crf42 | 0.22 | 30.047 | 0.60 | 62.9 | 28.52 |

**Bits at equal `fvvdp` against the flat-QP curve**

| point | Mbit/s | fvvdp | flat Mbit/s at the same fvvdp | saving % | PSNR-Y |
|---|---|---|---|---|---|
| rc-nofov-80 | 2.83 | 8.704 | 1.58 | -79.5 | 40.74 |
| rc-spatial-20 | 1.03 | 7.920 | 0.57 | -80.7 | 26.91 |
| rc-spatial-40 | 1.85 | 8.344 | 0.92 | -101.8 | 28.43 |
| rc-spatial-80 | 3.17 | 8.665 | 1.48 | -114.8 | 28.72 |
| rc-spatial-150 | 5.05 | 8.845 | 2.00 | -152.8 | 28.84 |
| rc-full-20 | 1.01 | 7.919 | 0.57 | -77.7 | 26.94 |
| rc-full-40 | 1.83 | 8.347 | 0.92 | -98.1 | 28.41 |
| rc-full-80 | 3.15 | 8.662 | 1.47 | -114.1 | 28.71 |
| rc-full-150 | 4.43 | 8.842 | 1.99 | -122.9 | 28.83 |
| rc-matched-20 | 1.34 | 8.097 | 0.69 | -93.6 | 29.33 |
| rc-matched-40 | 1.73 | 8.350 | 0.92 | -87.3 | 36.83 |
| rc-matched-80 | 2.85 | 8.765 | 1.75 | -62.9 | 41.00 |
| rc-matched-150 | 4.88 | 9.124 | 3.18 | -53.2 | 44.62 |
| rc-matched-noact-20 | 1.25 | 8.029 | 0.64 | -94.8 | 32.01 |
| rc-matched-noact-40 | 1.74 | 8.407 | 0.99 | -76.8 | 37.38 |
| rc-matched-noact-80 | 2.84 | 8.856 | 2.04 | -39.7 | 41.39 |
| rc-matched-noact-150 | 4.80 | 9.182 | 3.51 | -36.9 | 44.69 |
| x265-p-refresh-crf18 | 1.22 | 9.632 | -- | -- | 43.69 |
| x265-p-refresh-crf24 | 0.70 | 9.380 | -- | -- | 39.63 |
| x265-p-refresh-crf30 | 0.42 | 9.016 | 2.66 | 84.1 | 35.09 |
| x265-p-refresh-crf36 | 0.28 | 8.623 | 1.38 | 79.4 | 31.48 |
| x265-p-refresh-crf42 | 0.22 | 8.160 | 0.74 | 70.0 | 28.52 |

---

## 5. Where the bits went: coded-tile fraction per ring

Rings are eccentricity after the fixed-foveation eye box (`ecc_deg`), so the
"fovea" ring is the eye box plus the 8-degree pad. Frame 0 is excluded: it is
the intra frame, where every tile is coded by definition.

Two invariants are visible in every table and hold on every sequence:
**`forced-skip fraction` is exactly 0.000 in the fovea ring**, at every rate
and on every clip, and `mean res_level` is 0.00 there. Neither ladder is
allowed to touch the middle of the picture, and neither does.


### vr-mixed-1024-v2.yuv444p  (2048x1024 yuv444p, 8 frames, sbs, 10.43 ppd on axis, rate scale 1)

**Rings, `rc-full-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 840 | 0.487 | 0.000 | 15.0 | 0.00 |
| 8-20 | 1344 | 0.408 | 0.378 | 17.1 | 0.88 |
| 20-35 | 1400 | 0.291 | 0.488 | 18.3 | 1.28 |
| >35 | 0 | -- | -- | -- | -- |

**Rings, `rc-matched-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 840 | 0.542 | 0.000 | 16.2 | 0.16 |
| 8-20 | 1344 | 0.318 | 0.501 | 17.7 | 0.24 |
| 20-35 | 1400 | 0.303 | 0.499 | 20.1 | 1.02 |
| >35 | 0 | -- | -- | -- | -- |

---

### vr-mixed-1024-v2.yuv420p  (2048x1024 yuv420p, 8 frames, sbs, 10.43 ppd on axis, rate scale 1)

**Rings, `rc-full-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 840 | 0.500 | 0.000 | 14.4 | 0.00 |
| 8-20 | 1344 | 0.443 | 0.391 | 16.1 | 0.88 |
| 20-35 | 1400 | 0.304 | 0.511 | 17.5 | 1.28 |
| >35 | 0 | -- | -- | -- | -- |

**Rings, `rc-matched-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 840 | 0.560 | 0.000 | 15.5 | 0.16 |
| 8-20 | 1344 | 0.316 | 0.510 | 17.3 | 0.24 |
| 20-35 | 1400 | 0.319 | 0.510 | 18.7 | 1.02 |
| >35 | 0 | -- | -- | -- | -- |

---

### vr-turn-256-v2.yuv444p  (512x256 yuv444p, 12 frames, sbs, 2.61 ppd on axis, rate scale 0.0625)

**Rings, `rc-full-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 88 | 0.648 | 0.000 | 19.6 | 0.00 |
| 8-20 | 176 | 0.523 | 0.244 | 22.0 | 1.00 |
| 20-35 | 88 | 0.409 | 0.432 | 18.7 | 1.00 |
| >35 | 0 | -- | -- | -- | -- |

**Rings, `rc-matched-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 88 | 0.500 | 0.000 | 24.1 | 0.00 |
| 8-20 | 176 | 0.364 | 0.273 | 24.4 | 0.00 |
| 20-35 | 88 | 0.318 | 0.432 | 22.6 | 0.00 |
| >35 | 0 | -- | -- | -- | -- |

---

### panel-static-256-v2.yuv444p  (512x256 yuv444p, 6 frames, sbs, 2.61 ppd on axis, rate scale 0.0625)

**Rings, `rc-full-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 40 | 0.225 | 0.000 | 17.7 | 0.00 |
| 8-20 | 80 | 0.562 | 0.300 | 19.6 | 1.00 |
| 20-35 | 40 | 0.300 | 0.500 | 18.1 | 1.00 |
| >35 | 0 | -- | -- | -- | -- |

**Rings, `rc-matched-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 40 | 0.300 | 0.000 | 23.1 | 0.00 |
| 8-20 | 80 | 0.338 | 0.250 | 23.6 | 0.00 |
| 20-35 | 40 | 0.100 | 0.575 | 22.4 | 0.00 |
| >35 | 0 | -- | -- | -- | -- |

---

### mono-mixed-256-v2.yuv444p  (256x256 yuv444p, 12 frames, mono, 2.61 ppd on axis, rate scale 0.03125)

**Rings, `rc-full-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 44 | 0.659 | 0.000 | 19.2 | 0.00 |
| 8-20 | 88 | 0.500 | 0.250 | 21.8 | 1.00 |
| 20-35 | 44 | 0.409 | 0.432 | 17.8 | 1.00 |
| >35 | 0 | -- | -- | -- | -- |

**Rings, `rc-matched-40`**

| ring (deg) | tiles | coded fraction | forced-skip fraction | mean QP | mean res_level |
|---|---|---|---|---|---|
| fovea<8 | 44 | 0.545 | 0.000 | 24.0 | 0.00 |
| 8-20 | 88 | 0.284 | 0.364 | 23.6 | 0.00 |
| 20-35 | 44 | 0.295 | 0.500 | 23.3 | 0.00 |
| >35 | 0 | -- | -- | -- | -- |

![per-tile decisions](/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/percept/rc-map-mixed444-40mbit-frame4.png)

`vr-mixed-1024-v2` 444 at 40 Mbit/s, frame 4, both eyes, 16x16 tiles per eye
(`tools/quality/percept_map_png.py`; the PNG lives under `nx-scratch` and is
regenerated by the command in section 8).

The `res_level` panel is the foveation map made visible: a full-resolution
disc on each lens axis, a half-resolution ring, and quarter resolution in the
four corners. The `skip` panel is the temporal ladder, which fires only
outside that disc -- the red band across the top and bottom is the high-
eccentricity rows -- with the encoder's own rate-distortion `WARP_SKIP`
decisions in grey underneath it. The `class` panel shows why the QP map is not
simply a radial ramp: the amber band is the horizon edge, which `dQ_class`
gives -2 QP wherever it lands.

---

## 6. Against the foveated hardware opponent

`x265-p-refresh` is the opponent `tools/quality/README.md` 3 exists to keep
honest: libx265, P-only, no IDR after the first frame, periodic intra refresh,
and a **foveated per-CTU delta-QP map** driven through `addroi` -- centre
QP-6, mid QP, periphery QP+6, one pattern per eye, centre fixation, which is
the same fixation the metrics use. It is what a competitor could build today
out of `VK_KHR_video_encode_intra_refresh` and
`VK_KHR_video_encode_quantization_map` without inventing a codec. It runs in
CRF mode because the offsets are silently discarded in constant-QP mode
(`nxq/qpmap.py`).

At the two rates the comparison was asked for, on `vr-mixed-1024-v2` 444. The
flat-QP and anchor curves are interpolated in log-rate at exactly 40 and
80 Mbit/s (both curves span those rates -- `flat-q8` reaches 95 Mbit/s and
`x265-p-refresh-crf10` reaches 143). The rate-controlled arms are quoted at
the rate they **delivered**, which is what they are scored on; both undershoot
their target, by about 10 % at 40 Mbit and 30 % at 80.

| at | metric | `x265-p-refresh` | `nxv-enc` flat QP | `--rc` `rc-full` | `--rc` `rc-matched` |
|---|---|---|---|---|---|
| 40 Mbit/s | fovPSNR | **46.64** | 46.21 | 33.37 *(at 36.1)* | 34.22 *(at 36.6)* |
| 40 Mbit/s | fov2PSNR | **48.04** | 46.29 | 36.31 | 36.76 |
| 40 Mbit/s | JOD | **9.866** | 9.571 | 8.932 | 8.924 |
| 80 Mbit/s | fovPSNR | 51.10 | **51.64** | 33.75 *(at 55.0)* | 35.15 *(at 57.2)* |
| 80 Mbit/s | fov2PSNR | **52.51** | 51.90 | 36.86 | 38.15 |
| 80 Mbit/s | JOD | **9.940** | 9.808 | 9.379 | 9.335 |

Three separate things are in that table and they should not be run together.

**The rate-controlled arms are 13 to 16 dB of foveated PSNR and 0.6 to 0.9 JOD
behind the anchor.** That is sections 3 to 5 restated against a different
reference; nothing about the anchor causes it.

**The reference codec's own flat-QP arm is competitive with the anchor.** It
is 0.4 dB of fovPSNR and 0.30 JOD behind at 40 Mbit and 0.5 dB *ahead* on
fovPSNR at 80 (0.13 JOD behind). So the 13 to 16 dB above is not "NX Warp is
13 dB behind HEVC"; it is the rate controller, and the flat encoder it is
driving is roughly where the anchor is on this material.

**Half of the loss is the rate controller and half is the foveation.** The
`rc-nofov` arm -- the allocator running with the foveation map and the
temporal ladder both off, so nothing but per-tile bit allocation -- delivers
63.0 Mbit/s at 44.18 dB fovPSNR and JOD 9.172, against 49.77 dB and 9.745 for
flat QP at the same rate. **5.6 dB and 0.57 JOD of the gap is the allocator
alone, before a single foveation decision is made**, and the remaining 8 dB
(to `rc-matched-80`'s 35.15 at a comparable rate) is the ladder. Section 7
splits both further.

Worth noting against the foveation claim specifically: the anchor's own
fovea/periphery split at 40 Mbit is 48.2 / 43.7 dB, a **4.5 dB** gap, so the
delta-QP map really is foveating, with the quantiser alone. `rc-full-40`'s
split is 45.0 / 31.6 -- a **13.4 dB** gap, three times as steep, bought with
resolution rather than QP. The steeper gradient is the right shape. It is
priced wrong.

On the small clips the same picture holds with a larger margin. On
`vr-turn-256-v2` the anchor saves 60 to 69 % of the bits against the `nxv-enc`
flat-QP curve at equal fovPSNR at every CRF point, which is the reference
codec's standing position on that material and not this branch's to move.

---

## 7. Why it loses, and what to change

Three things are wrong, in decreasing order of how much they cost, and none of
them is the wiring.

### 7.1 The bit model budgeted for tiles the encoder was going to skip (fixed)

The allocator and the encoder's mode decision are two independent optimisers,
and until this branch they had never met. `RateController::update_model()`
ignored any tile whose measured bits came back under one, on the reasonable-
looking grounds that a tile with no bits carries no information about how many
bits it would need. That is exactly backwards: the tiles with no bits are the
ones the encoder's rate-distortion search chose to code as `WARP_SKIP`, and on
`vr-turn-256-v2` that is **22 to 28 tiles out of 32, every frame** -- for the
flat-QP arm just as much as the rate-controlled one. Their `a_t` therefore
never left `a_init`, `predicted_total` kept budgeting bits nobody would emit,
and the pressure search met its budget at a QP coarser than it needed. The
loop could not recover, because the same tiles are skipped again next frame.

The controlled measurement, with foveation, the temporal ladder and `dQ_act`
all switched off so that the rate controller is the only thing under test
(`--rc-bitrate 2.17 --rc-fov off --rc-temporal off --rc-act 0` against
`--qp 26`, which delivers the same 2.17 Mbit/s):

| frame | budget (bits) | predicted | actual, before | actual, after |
|---|---|---|---|---|
| 0 | 33997 | 33958 | 39200 | 39200 |
| 1 | 22113 | 22098 | 18480 | 18480 |
| 2 | 22113 | 22151 | **7200** | 16880 |
| 3 | 22113 | 22120 | **16272** | 17920 |
| 4 | 22113 | 21814 | **10176** | 37248 |
| 5 | 22113 | 22107 | **11328** | 20400 |
| 6 | 22113 | 22199 | **15152** | 32832 |
| 7 | 22113 | 22475 | **10016** | 38240 |

Before: `actual / predicted` sat between 0.33 and 0.67 for eleven consecutive
frames, never once above 1. The median coded QP was 30 against the flat 26
that hits the same rate, and the encode landed **4.3 dB of PSNR-Y below flat
QP at equal rate**. After: the bias is gone and the gap is **1.6 dB**.

The fix is one line -- floor the evidence at one bit instead of discarding it
-- and it is in `rc/src/allocate.cpp` with the reasoning next to it.

**It is not a free win, and the whole-sequence numbers say so.** Mean bit
saving at equal fovPSNR, averaged over the four target rates, before and after
(all three 256-px sequences were measured twice, once with each build; the
result JSONs are kept as `...-beforefix.json`):

| sequence | `rc-nofov` | `rc-matched` |
|---|---|---|
| `vr-turn-256-v2` (head turn) | -57.7 % -> **-18.4 %** | -63.9 % -> **-56.2 %** |
| `mono-mixed-256-v2` (mono, mixed) | -45.3 % -> **-33.1 %** | -45.0 % -> -55.5 % |
| `panel-static-256-v2` (static panel) | -110.5 % -> -154.6 % | -81.1 % -> -167.4 % |

(These are means over four rate points whose *delivered* rates moved when the
model changed, so they are an indication rather than a controlled comparison;
the frame table above is the controlled one.)

On moving content the bias fix is worth 12 to 39 points. On the **static**
clip it makes things worse, and for a reason that follows directly from what
was changed: on static content the encoder's skipped tiles really are cheap,
the old behaviour's optimism was accidentally correct, and letting `a_t` fall
now makes the controller overshoot instead. `a_ratio_clamp` lets `a_t` move
2.3x per frame, so a tile that alternates skip and code oscillates, and on a
six-frame clip there is no time to settle.

Symmetric error is better than a permanent one-directional bias, so the change
stays; but neither behaviour is good rate control. **The principled fix is for
the allocator to know the encoder's skip decision before it allocates**,
rather than to learn it one frame late through the bit model:
`nxvc_encoder_tiles()` already reports `skipped`, and `FrameInputs` already
has a `force_warp_skip` span the allocator treats as weight zero. That is a
`feedback()` line and a span, and it is the first item in 7.5.

### 7.2 The foveation ladder is calibrated for a panel we cannot measure on

Section 2 states the problem and it is worth restating as a result: on the
`rc-spatial` and `rc-full` arms the eccentricity-weighted PSNR **saturates**.
On `vr-mixed-1024-v2` it stops improving at about 33.9 dB no matter how many
bits are supplied -- 61.6 Mbit/s buys 0.14 dB over 55.0 -- because three
quarters of the picture is being coded at half or quarter resolution and no
quantiser can put back what the resampling took out. That ceiling is the
correct behaviour *for a 2160-px-per-eye panel*, where quarter resolution at
30 degrees still leaves 5.5 ppd against the 4.3 ppd the acuity model asks for.
It is one full ladder step too aggressive for a 1024-px-per-eye clip, and the
metrics score the clip.

`rc-matched` -- the identical ladder told the truth about the clip's own
density -- has no ceiling and tracks the flat-QP curve. The difference between
the two arms is the whole of the saturation. **This is not a result about
whether foveation works; it is a result about not having a 2160-px sequence.**
Generating one is the single measurement that would settle it.

### 7.3 `dQ_act` out-pulls `dQ_ecc`, and on VR content it reverses it

`RateConfig::act_strength` is 1.0 with a clamp of 4, so the activity term
spans +/-4 QP. `dQ_ecc` spans 0 to +6 across the whole visual field. On
content whose centre is busier than its periphery -- which is what a rendered
VR scene looks like, and what every sequence here is -- the two cancel, and on
`vr-turn-256-v2` they reverse: at `rc-matched-150` the mean coded QP is
**17.3 in the fovea and 13.3 in the 20-35 degree ring**, and the delivered
`psnr_fovea` (38.8 dB) is *below* `psnr_periphery` (42.6 dB). A foveated rate
controller that gives the periphery the finer quantiser is not foveating.

`docs/RATECONTROL.md` appendix A.2 already flags the sign of this term as a
judgement call taken from x264's prose rather than from PAPER.md 5.2's
formula. `--rc-act` is exposed so the two can be separated, and the
`rc-matched-noact` arm is that separation.

**And switching it off recovers almost nothing.** On `vr-mixed-1024-v2` 444,
`rc-matched-noact` beats `rc-matched` by 0.02 to 0.19 dB of fovPSNR and 0.01
to 0.14 JOD -- inside the noise of the arm-to-arm rate difference. On
`vr-turn-256-v2` it is worth more, 0.6 to 2.1 dB at the low rates, and on
`panel-static-256-v2` about 1 dB throughout. So the QP inversion is real and
worth fixing, but on this evidence it is a **third-order** effect next to 7.1
and 7.2, and anyone reading section 3 hoping the activity term explains the
gap should stop here: it does not.

### 7.4 A fourth thing, which the clips are too short to separate

Every sequence here is 6 to 12 frames. `RateConfig` repays a scene-cut burst
over `scene_cut_recovery = 30` frames and caps it at `scene_cut_cap = 1.5`
frame budgets, so the all-intra first frame is given at most 1.5 frames' worth
of bits and the debt is amortised over a third of a second -- neither of which
fits inside an eight-frame clip. On `vr-mixed-1024-v2` at the 20 Mbit target
the intra frame is allocated 224 kbit, a tenth of a bit per pixel, and the
seven inter frames that follow all predict from it. The flat-QP arm has no
such constraint and spends whatever intra costs.

That is correct behaviour on a 90 Hz stream and a measurement artefact here,
and this branch cannot tell how much of section 3's gap it accounts for. The
honest thing is to re-run on the full 36-frame `vr-mixed-1024-v2` once the
wall clock allows, and to say until then that **every rate-controlled number
in this file carries an unquantified penalty from the starved first frame that
the flat-QP numbers do not.**

### 7.5 What to change, in order

1. Give the allocator the encoder's skip decision before it allocates, instead
   of after (7.1). This is a `FrameInputs` field and a `feedback()` line.
2. Generate a 2160-px-per-eye sequence, or accept that `rc-spatial` and
   `rc-full` cannot be scored on this corpus (7.2).
3. Re-derive `act_strength` and `act_clamp` against `dq_ecc_*` so that
   eccentricity dominates, and re-open appendix A.2's sign question with the
   measurement rather than with the prose (7.3).
4. Re-run on the full 36-frame sequences so the scene-cut recovery horizon
   fits inside the measurement (7.4).
5. Only then re-run this file. Nothing above needs a bitstream change, a
   decoder change, or a change to the wire this branch built.

---

## 8. Reproducing

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export NXQ_CPUS=16-19 NXQ_THREADS=2
PATH=<worktree>/build-ref/bin:$PATH
cd <worktree>/tools/quality
$NXQ_SCRATCH/venv/bin/python percept_run.py \
    --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.json \
    --work $NXQ_SCRATCH/percept/w-mixed444 \
    --out $NXQ_SCRATCH/results/tourney-percept-mixed444.json \
    --frames 8 --qp 20,26,32,38,44 --anchor-crf 20,26,32,38 \
    --arms rc-full,rc-matched,rc-matched-noact
$NXQ_SCRATCH/venv/bin/python percept_report.py \
    $NXQ_SCRATCH/results/tourney-percept-*.json
```

The per-tile decision picture:

```sh
$NXQ_SCRATCH/venv/bin/python percept_map_png.py \
    --csv $NXQ_SCRATCH/percept/w-mixed444/rc-full-40.csv --frame 4 \
    --out $NXQ_SCRATCH/percept/rc-map-mixed444-40mbit-frame4.png \
    --title "vr-mixed-1024-v2 444, nxv-enc --rc at 40 Mbit/s, frame 4 (both eyes, 16x16 tiles per eye)"
```

Results JSON: `$NXQ_SCRATCH/results/tourney-percept-{mixed444,mixed420,
turn444,panel444,mono444}.json`, plus `...-beforefix.json` for the three
256-px sequences measured against the pre-7.1 build. Everything ran as
`chrt -i 0 taskset -c 16-19 nice -n 19`, `-j2`, `ffmpeg -threads 2`, one
encode at a time, on cores 16-19 only; FovVideoVDP ran on the 7900 XTX
through ROCm PyTorch 2.9.1.

**Tests** (`tests/rcenc/test_encdrive.cpp`, ctest `rcenc.encdrive`, 1532
assertions), all green, along with the 53 pre-existing ctests including the
conformance vectors:

1. **determinism** -- two rc-driven six-frame encodes of the same input are
   byte-identical, and the arms that appear in both runs of section 3 produced
   identical rates and metrics across separate processes;
2. **a text panel is never resampled and never withheld while it changes** --
   a glyph tile redrawn every frame keeps `res_level == 0` and
   `force_warp_skip == 0` at a budget low enough that the ladder is engaged,
   and the encoder confirms `res_level == 0` in the tile header. (On the
   corpus itself this invariant is untested rather than confirmed: the
   classifier calls no tile of `panel-static-256-v2` `Text`, so the synthetic
   fixture is the only place it fires.)
3. **no fovea tile is ever skipped by the temporal ladder** -- checked per
   frame against `RefreshConfig::fovea_full_deg`, and visible in every ring
   table of section 5 as `forced-skip fraction 0.000`;
4. **the maps reach the bitstream** -- every coded tile's `qp`, `res_level`
   and `wm_id` read back from `nxvc_encoder_tiles()` equal what the allocator
   asked for, with a guard that some `res_level` is non-zero so the check is
   not vacuous;
5. **`warp_mad_q8` is populated** -- unmeasured on the first frame, measured
   afterwards, so the complexity input cannot silently be zero.
