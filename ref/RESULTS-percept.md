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
mechanism works -- the fovea really does get 5 to 7 dB better than a flat-QP
encode at the same rate -- but the price charged in the periphery is larger
than any of the four perceptual metrics will forgive, and two of the three
causes are calibration errors rather than anything structural. Section 6 names
them. Nothing here is a reason to unpick the wiring; it is a list of numbers
to change and one measurement to redo on a real panel.

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

<!--TABLES-->

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

Results JSON: `$NXQ_SCRATCH/results/tourney-percept-{turn444,panel444,
mono444,mixed444,mixed420}.json`. Everything ran as
`chrt -i 0 taskset -c 16-19 nice -n 19`, `-j2`, `ffmpeg -threads 2`.
