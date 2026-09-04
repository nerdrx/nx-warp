# The PAPER.md 5.3 perceptual metrics, measured

*2026-09-04. Produced by `tools/quality/compare.py --metric fvvdp,fov-ssim,popin
--foveated-psnr` on the **v2 (band-limited)** sequences, driver
`$NXQ_SCRATCH/run-perceptual.sh`, result files under
`$NXQ_SCRATCH/results/perceptual/`. Every external process under
`chrt -i 0 taskset -c 16-19 nice -n 19`, ffmpeg `-threads 4`. FovVideoVDP is the
authors' `pyfvvdp` 1.2.2 on PyTorch 2.9.1+rocm6.4, running on the Radeon
7900 XTX.*

---

## The five things to read first

1. **PSNR and FovVideoVDP rank the two HEVC anchors in opposite directions, and
   it is not close.** Against flat `x265-p`, the foveated intra-refresh anchor
   `x265-p-refresh` is **+21 to +25 % BD-rate on PSNR-Y** — worse — and
   **−37.6 to −39.1 % BD-rate on JOD** — much better. The same two bitstreams,
   the same frames, a sixty-point swing, and the JOD figure agrees to within
   1.5 points across four sequence-and-format combinations spanning 16x in
   pixel count. Section 3.
2. **PSNR is flattering `nxv`, and flattering it most against the anchor it
   most needs to beat.** Every deficit is larger on JOD than on PSNR, by 9 to
   1297 points, and the gap widens monotonically as the anchor gets more
   foveated: +9 to +33 points against `x264-intra`, +138 to +411 against
   `x265-p`, **+499 to +1297 against `x265-p-refresh`**. Section 2.
3. **Eccentricity-weighted PSNR does not recover this; eccentricity-weighted
   SSIM partly does.** `fov-PSNR` moves the anchor comparison by 15 to 18
   points (from +25 % to +7 %) and never changes a sign. `fov-SSIM` does change
   the sign and lands at −20 to −30 %, still short of JOD's −38 %. The cheap
   secondary metric of 5.3 is a real instrument; it is not a substitute for the
   primary one. Section 3.
4. **A forced temporal ladder at Floeter et al.'s own tolerated operating
   point is not survivable on this codec**, and the pop-in metric is what says
   so: 95th-percentile `C_M` of 11.3 to 12.5 JND against 0.09 to 2.34 for the
   anchors on the same refresh events, with 13 to 17 % of events above one JND.
   The cause is not quantisation — the distribution barely moves over a 10x rate
   range — it is the pose warp failing to hold the tile, which
   `ref/RESULTS-inter.md` section 3 measures independently. Section 5b.
5. **The pop-in metric says the fovea is not where the pops are.** Across every
   rate-distortion run, the 95th-percentile `C_M` inside the 8-degree fovea disc
   is 0.000 to 0.014 JND while the whole-frame figure is 0.05 to 2.1. That is the
   Tursun-Didyk direction (`docs/RATECONTROL.md` 8.3: peripheral vision is
   *more* sensitive to temporal change, not less) reproduced on real
   bitstreams. Section 5.

---

## 1. What was measured, and how

| | |
|---|---|
| sequences | `vr-mixed-1024-v2` 4:4:4 **and** 4:2:0 (2048x1024, 36 frames), `vr-mixed-512-v2` 4:2:0 (1024x512, 12), `vr-turn-256-v2` 4:4:4 (512x256, 12), all 90 fps, `sbs` |
| codec | `nxv` intra (`--codec-cmd nxv`) and inter (`--eyes 2 --inter on --poses <seq>.poses.json`), branch `tourney/metric`, `build-ref/` |
| anchors | `x264-intra`, `x265-p`, `x265-p-refresh` (foveated delta-QP map plus periodic intra refresh, forced to CRF — see `README.md` and the note below) |
| rate ladder | `nxv` QP 0/6/12/18/24, anchors QP or CRF 8/16/24/32/40 |
| temporal ladder | `--write-skip-map --ladder 11223` and `12345` into `nxv-enc --skip-map`, scored back through `--popin-skip-map` (section 5b) |
| runs | 8 rate-distortion runs plus 2 temporal-ladder runs, 10 result files |
| metrics | PSNR-Y, SSIM, MS-SSIM, eccentricity-weighted PSNR and SSIM, FovVideoVDP JOD, pop-in `C_M`, motion-to-photon |

`x265-p-refresh` is driven in CRF because both x264 and x265 discard the
`addroi` offsets under constant QP; the harness forces it and records
`rate_control_note`. Its "QP" column below is therefore a CRF value. This is
the documented behaviour, not a defect of this run.

### The display model

FovVideoVDP is run against a **headset**, not a monitor: `pico4`, 2160x2160 per
eye, 100 degrees horizontal, 90 Hz, 100 nit sRGB, the equivalent flat display
at the reference implementation's 3 m VR viewing distance (7.15 m wide, which
subtends the 100 degrees exactly). Fixation is the view centre, which is where
the foveated anchor puts its low-QP box, so that anchor is scored on the metric
it optimises.

**The pixels per degree follow the sequence, not the panel**, because that is
what the compared pixels actually subtend:

| view width | on-axis ppd | which sequence |
|---:|---:|---|
| 2160 px | 15.82 | the Pico 4 panel itself — not measured here |
| 1024 px | 7.50 | `vr-mixed-1024-v2`, 4:4:4 and 4:2:0 |
| 512 px | 3.75 | `vr-mixed-512-v2` |
| 256 px | 1.87 | `vr-turn-256-v2` |

This is a real limitation of the material and it is stated rather than hidden.
At 1.87 ppd the Nyquist frequency is 0.94 cycles/degree, so on `vr-turn-256-v2`
FovVideoVDP has only its coarsest bands to work with and the absolute JODs there
are compressed (8.4 to 9.9 across a 100x rate range). The **orderings** are what
this report reads, and they are the same on all four sequence-and-format
combinations — including on
`vr-mixed-1024-v2`, which at 7.50 ppd is within a factor of two of the panel.
Generating a 2160 px sequence would remove the caveat and is the obvious next
measurement.

### Three caveats carried into every number below

1. **This is not the paper's display space.** 5.3 asks for the metric to be run
   on the output of the client reprojection shader, so that warped-reference
   concealment is charged for what the eye finally sees. There is no
   reprojection simulator in `tools/quality`, so what is compared is the decoded
   frame against the source frame. Everything here therefore *under*-charges the
   inter path, whose artefacts are exactly the ones reprojection would expose.
2. **The Tursun-Didyk reduction.** The pop-in metric inherits all five
   approximations of `docs/RATECONTROL.md` 8.2, the largest being the
   log-frequency reading of the De Lange polynomial. The orderings are robust to
   it; the absolute `C_M` values are not. `tools/quality/nxq/popin.py` is a
   numpy port of `rc/src/tvm.cpp` and reproduces its published sensitivity table
   to five significant figures, so the encoder's scheduler and this metric
   cannot disagree about what is visible.
3. **Motion to photon is a budget, not a measurement.** Section 6.

### Cost

FovVideoVDP is **not** the expensive part. On the 7900 XTX one operating point
of `vr-mixed-1024-v2` — 36 frames, two eyes at 1024x1024 — takes **4 to 12
seconds**. The 73-minute wall time of that run is dominated by SSIM and MS-SSIM
in numpy on 2048x1024 frames. The metric PAPER.md 5.3 describes as too slow for
the rate-control loop is, offline and on a GPU, cheaper than the SSIM it is
compared against. (On CPU PyTorch it is about 0.6 s per 512x512 frame, roughly
six times slower, which is still usable.)

---

## 2. Rate-distortion, PSNR beside JOD

### `vr-mixed-1024-v2` 4:4:4, `nxv` intra

| codec | QP/CRF | Mbit/s | PSNR-Y dB | fov-PSNR dB | SSIM | fov-SSIM | JOD | pop p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `x264-intra` | 8 | 215.0 | 59.61 | 59.67 | 0.9993 | 0.9993 | 9.966 | 0.092 |
| `x264-intra` | 16 | 103.5 | 54.81 | 54.74 | 0.9982 | 0.9983 | 9.887 | 0.222 |
| `x264-intra` | 24 | 53.2 | 48.87 | 48.94 | 0.9969 | 0.9970 | 9.723 | 0.337 |
| `x264-intra` | 32 | 28.0 | 44.24 | 43.96 | 0.9937 | 0.9935 | 9.521 | 0.720 |
| `x264-intra` | 40 | 15.0 | 38.56 | 38.22 | 0.9845 | 0.9830 | 9.102 | 1.868 |
| `x265-p` | 8 | 50.3 | 56.63 | 56.54 | 0.9988 | 0.9989 | 9.937 | 0.052 |
| `x265-p` | 16 | 19.4 | 52.76 | 52.39 | 0.9980 | 0.9980 | 9.862 | 0.092 |
| `x265-p` | 24 | 7.8 | 47.80 | 47.31 | 0.9961 | 0.9959 | 9.697 | 0.177 |
| `x265-p` | 32 | 3.0 | 42.11 | 41.84 | 0.9901 | 0.9899 | 9.377 | 0.406 |
| `x265-p` | 40 | 1.3 | 37.35 | 37.05 | 0.9786 | 0.9773 | 8.968 | 0.811 |
| `x265-p-refresh` | 8 | 33.3 | 54.23 | 54.65 | 0.9985 | 0.9986 | 9.959 | 0.088 |
| `x265-p-refresh` | 16 | 13.7 | 49.80 | 50.22 | 0.9972 | 0.9975 | 9.918 | 0.164 |
| `x265-p-refresh` | 24 | 5.4 | 44.06 | 44.58 | 0.9933 | 0.9940 | 9.716 | 0.376 |
| `x265-p-refresh` | 32 | 2.0 | 38.79 | 39.36 | 0.9847 | 0.9863 | 9.426 | 0.687 |
| `x265-p-refresh` | 40 | 0.8 | 33.49 | 34.10 | 0.9666 | 0.9693 | 8.941 | 1.208 |
| `nxv-intra` | 0 | 251.6 | 57.21 | 57.16 | 0.9989 | 0.9989 | 9.953 | 0.128 |
| `nxv-intra` | 6 | 164.1 | 54.37 | 54.27 | 0.9981 | 0.9982 | 9.885 | 0.207 |
| `nxv-intra` | 12 | 106.7 | 50.59 | 50.35 | 0.9966 | 0.9967 | 9.748 | 0.320 |
| `nxv-intra` | 18 | 69.1 | 46.08 | 45.76 | 0.9934 | 0.9933 | 9.513 | 0.682 |
| `nxv-intra` | 24 | 44.4 | 41.44 | 41.06 | 0.9866 | 0.9859 | 9.318 | 1.141 |

Read the two `x265` blocks against each other before reading the codec. At
CRF 24 the foveated anchor is **3.74 dB below** flat `x265-p` on PSNR-Y (44.06
against 47.80) and **0.019 JOD above it** (9.716 against 9.697) — while
spending 5.4 Mbit/s instead of 7.8. PSNR says it lost badly; FovVideoVDP says it
won slightly, for 31 % fewer bits. The `fov-PSNR` column shows where the
disagreement comes from: it is the only anchor whose eccentricity-weighted PSNR
is *higher* than its flat PSNR (+0.52 dB at CRF 24), because its error is
deliberately in the periphery. `x265-p` goes the other way (−0.49 dB).

### `vr-mixed-1024-v2` 4:4:4, `nxv` inter

The anchor rows are identical (same anchors, same sequence, same points), so
only the codec is repeated:

| codec | QP | Mbit/s | PSNR-Y dB | fov-PSNR dB | SSIM | fov-SSIM | JOD | pop p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `nxv-inter` | 0 | 204.0 | 57.03 | 56.98 | 0.9988 | 0.9989 | 9.941 | 0.108 |
| `nxv-inter` | 6 | 111.9 | 54.00 | 53.89 | 0.9980 | 0.9981 | 9.863 | 0.181 |
| `nxv-inter` | 12 | 61.1 | 49.84 | 49.60 | 0.9965 | 0.9965 | 9.707 | 0.292 |
| `nxv-inter` | 18 | 30.3 | 44.77 | 44.43 | 0.9927 | 0.9924 | 9.462 | 0.584 |
| `nxv-inter` | 24 | 13.1 | 39.80 | 39.40 | 0.9850 | 0.9838 | 9.165 | 0.837 |

### `vr-turn-256-v2` 4:4:4, `nxv` intra

| codec | QP/CRF | Mbit/s | PSNR-Y dB | fov-PSNR dB | SSIM | fov-SSIM | JOD | pop p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `x264-intra` | 8 | 28.9 | 57.81 | 57.81 | 0.9988 | 0.9989 | 9.823 | 0.068 |
| `x264-intra` | 24 | 8.2 | 46.57 | 46.60 | 0.9929 | 0.9932 | 9.452 | 0.354 |
| `x264-intra` | 40 | 2.3 | 35.60 | 35.29 | 0.9627 | 0.9616 | 8.624 | 2.103 |
| `x265-p` | 8 | 10.5 | 54.71 | 54.53 | 0.9980 | 0.9980 | 9.822 | 0.058 |
| `x265-p` | 24 | 2.3 | 44.55 | 43.93 | 0.9908 | 0.9907 | 9.419 | 0.277 |
| `x265-p` | 40 | 0.5 | 33.30 | 33.05 | 0.9478 | 0.9463 | 8.590 | 0.783 |
| `x265-p-refresh` | 8 | 6.8 | 50.51 | 51.10 | 0.9967 | 0.9970 | 9.858 | 0.152 |
| `x265-p-refresh` | 24 | 1.3 | 38.84 | 39.49 | 0.9816 | 0.9835 | 9.413 | 0.634 |
| `x265-p-refresh` | 40 | 0.3 | 28.83 | 29.53 | 0.9133 | 0.9199 | 8.435 | 1.149 |
| `nxv-intra` | 0 | 34.7 | 54.98 | 54.89 | 0.9981 | 0.9981 | 9.735 | 0.161 |
| `nxv-intra` | 12 | 15.2 | 47.95 | 47.76 | 0.9928 | 0.9929 | 9.434 | 0.475 |
| `nxv-intra` | 24 | 6.0 | 38.47 | 38.10 | 0.9695 | 0.9682 | 8.749 | 1.450 |

The clearest single pair in the whole report is here. `x265-p-refresh` at
CRF 8 is **4.20 dB below** `x265-p` at QP 8 (50.51 against 54.71) and **0.036
JOD above** it (9.858 against 9.822), at **35 % fewer bits** (6.8 against 10.5
Mbit/s). Four decibels of PSNR are being spent on something that, at a centre
fixation, is not visible.

(Full tables for `vr-mixed-1024-v2` 4:2:0, `vr-mixed-512-v2` and the remaining
rows of `vr-turn-256-v2` are in the result JSONs; they say the same thing and are
summarised next. The 4:2:0 copy of `vr-mixed-1024-v2` is worth one sentence on
its own: every conclusion in this report is the same there, and the
anchor-against-anchor JOD figure moves by 0.9 points, so **none of this is an
artefact of 4:4:4 chroma**.)

---

## 3. BD-rate: every run, PSNR beside JOD

Negative is better. `JOD − PSNR` is the disagreement, in percentage points.

| run | codec | anchor | BD PSNR-Y | BD fov-PSNR | BD fov-SSIM | BD JOD | JOD − PSNR |
|---|---|---|---:|---:|---:|---:|---:|
| `intra-turn256-444` | `nxv-intra` | `x264-intra` | +71.3 % | +73.6 % | +90.2 % | +104.4 % | +33.1 pt |
| `intra-turn256-444` | `nxv-intra` | `x265-p` | +336.4 % | +324.5 % | +573.8 % | +611.4 % | +274.9 pt |
| `intra-turn256-444` | `nxv-intra` | `x265-p-refresh` | +288.9 % | +332.9 % | +688.1 % | +1096.7 % | +807.8 pt |
| `intra-mixed512-420` | `nxv-intra` | `x264-intra` | +58.7 % | +60.8 % | +82.8 % | +80.0 % | +21.3 pt |
| `intra-mixed512-420` | `nxv-intra` | `x265-p` | +468.1 % | +457.0 % | +755.2 % | +828.6 % | +360.5 pt |
| `intra-mixed512-420` | `nxv-intra` | `x265-p-refresh` | +406.5 % | +460.9 % | +911.6 % | +1451.5 % | +1045.0 pt |
| `intra-mixed1024-444` | `nxv-intra` | `x264-intra` | +80.1 % | +83.5 % | +98.4 % | +103.3 % | +23.2 pt |
| `intra-mixed1024-444` | `nxv-intra` | `x265-p` | +819.5 % | +797.9 % | +1428.7 % | +1159.6 % | +340.1 pt |
| `intra-mixed1024-444` | `nxv-intra` | `x265-p-refresh` | +716.5 % | +810.0 % | +1616.9 % | +1967.2 % | +1250.8 pt |
| `intra-mixed1024-420` | `nxv-intra` | `x264-intra` | +60.4 % | +63.3 % | +85.1 % | +92.5 % | +32.1 pt |
| `intra-mixed1024-420` | `nxv-intra` | `x265-p` | +732.0 % | +712.5 % | +1327.9 % | +1142.8 % | +410.7 pt |
| `intra-mixed1024-420` | `nxv-intra` | `x265-p-refresh` | +642.5 % | +723.8 % | +1487.1 % | +1939.0 % | +1296.5 pt |
| `inter-turn256-444` | `nxv-inter` | `x264-intra` | +33.2 % | +34.2 % | +35.2 % | +45.9 % | +12.7 pt |
| `inter-turn256-444` | `nxv-inter` | `x265-p` | +242.6 % | +231.6 % | +377.5 % | +409.8 % | +167.2 pt |
| `inter-turn256-444` | `nxv-inter` | `x265-p-refresh` | +193.4 % | +227.4 % | +476.6 % | +752.8 % | +559.5 pt |
| `inter-mixed512-420` | `nxv-inter` | `x264-intra` | **−7.7 %** | **−6.8 %** | **−12.4 %** | **−8.3 %** | −0.6 pt |
| `inter-mixed512-420` | `nxv-inter` | `x265-p` | +237.3 % | +230.6 % | +302.4 % | +375.0 % | +137.7 pt |
| `inter-mixed512-420` | `nxv-inter` | `x265-p-refresh` | +182.2 % | +216.5 % | +391.5 % | +680.8 % | +498.6 pt |
| `inter-mixed1024-444` | `nxv-inter` | `x264-intra` | +4.2 % | +6.0 % | **−6.0 %** | +13.1 % | +8.9 pt |
| `inter-mixed1024-444` | `nxv-inter` | `x265-p` | +450.2 % | +438.9 % | +598.9 % | +619.1 % | +168.9 pt |
| `inter-mixed1024-444` | `nxv-inter` | `x265-p-refresh` | +365.1 % | +425.8 % | +766.2 % | +1089.1 % | +724.0 pt |
| `inter-mixed1024-420` | `nxv-inter` | `x264-intra` | **−10.9 %** | **−9.4 %** | **−18.9 %** | +2.9 % | +13.8 pt |
| `inter-mixed1024-420` | `nxv-inter` | `x265-p` | +376.6 % | +366.6 % | +500.8 % | +568.3 % | +191.8 pt |
| `inter-mixed1024-420` | `nxv-inter` | `x265-p-refresh` | +301.9 % | +353.3 % | +640.4 % | +989.3 % | +687.4 pt |

Two structural facts fall out of this table and neither is visible on PSNR
alone.

**(a) The disagreement is not noise; it scales with how foveated the anchor
is.** Averaged over the eight runs, `JOD − PSNR` is +18 points against
`x264-intra`, +256 against `x265-p` and **+871** against `x265-p-refresh`. A
metric that ignores where the error lands cannot see an encoder that chooses
where to put it, and the codec's own reported deficit is smallest exactly
against the opponent `docs/RESEARCH-INDUSTRY.md` 2.2 says is the real one.

**(b) On the inter path the codec is at parity with x264 intra on three metrics
and ahead on one.** `inter-mixed512-420` is −7.7 % on PSNR-Y and −8.3 % on JOD, and
`inter-mixed1024-420` is −10.9 % on PSNR-Y and −18.9 % on fov-SSIM. On
`inter-mixed1024-444` the codec is +4.2 % on PSNR-Y but **−6.0 % on
eccentricity-weighted SSIM**. Those fov-SSIM wins are the one place in this
report where `nxv` beats an anchor on a metric it was not already beating — and
they do not survive the move to JOD (+13.1 % and +2.9 %), and `x264-intra` is not
the Phase 2 opponent.

### The anchor-against-anchor table, which is the actual headline

BD-rate of `x265-p-refresh` against `x265-p`, on the same bitstreams:

| sequence | PSNR-Y | fov-PSNR | SSIM | fov-SSIM | JOD |
|---|---:|---:|---:|---:|---:|
| `vr-mixed-1024-v2` 4:4:4 | **+24.4 %** | +6.7 % | −16.2 % | −24.7 % | **−38.5 %** |
| `vr-mixed-1024-v2` 4:2:0 | **+25.4 %** | +7.5 % | −17.4 % | −25.4 % | **−37.6 %** |
| `vr-mixed-512-v2` 4:2:0 | **+22.1 %** | +6.4 % | −15.3 % | −20.1 % | **−38.7 %** |
| `vr-turn-256-v2` 4:4:4 | **+21.2 %** | +4.1 % | −23.5 % | −29.8 % | **−39.1 %** |

Five metrics, three sequences, one pair of encoders:

* **PSNR-Y** says foveation costs 21 to 24 % rate. Sign: against.
* **fov-PSNR** — the acuity weighting of PAPER.md 5.1.2 applied to squared
  error — recovers 15 to 17 points but **does not change the sign**. Weighting
  the squared error by `1/(1 + e/2.3)` is a gentle instrument: the weight at 40
  degrees is still 0.054, not zero, and squared error in the far periphery is
  large enough that a 5 % weight still dominates the sum.
* **SSIM** changes the sign on its own (−15 to −24 %), before any foveation is
  applied at all, because the periphery of a foveated encode is *blurred*, and
  structural similarity charges blur far less than squared error does.
* **fov-SSIM** adds 5 to 8 more points on top of SSIM.
* **JOD** lands at −38.5 to −39.1 %, with a spread of 0.6 points across three
  sequences that differ by 16x in pixel count.

The ordering `PSNR < fov-PSNR < SSIM < fov-SSIM < JOD` holds on all three
sequences without exception. It decomposes the disagreement cleanly: about a
third of the correction is "stop measuring squared error" (SSIM), about a sixth
is "weight by eccentricity" (the fov- prefix, worth 15 to 17 points on PSNR and
5 to 8 on SSIM), and the remaining half is everything the VDP does that neither
does — the contrast sensitivity function, the temporal channels, luminance
adaptation and the foveated pooling.

**What this means for the codec's own numbers.** `ref/RESULTS-inter.md` reports
the Phase 2 kill test against `x265-p` on PSNR-Y. That verdict does not change
here — it fails on JOD too, by more. But the *choice of anchor* does change: if
the real opponent is a foveated, IDR-free hardware HEVC streamer, then measuring
it on PSNR understates it by about 60 points of BD-rate, and every comparison in
`ref/` that uses `x265-p-refresh` as the "hardware-class baseline" is reading
that baseline through a metric that cannot see what it is doing.

---

## 4. Where FovVideoVDP and PSNR disagree within a single curve

The BD-rate figures above are integrals. The per-point behaviour is worth one
paragraph because it explains them.

JOD compresses hard at the top. On `vr-mixed-1024-v2`, PSNR-Y spans 37.35 to
59.61 dB — 22 dB — while JOD spans 8.94 to 9.97, about **one JOD unit**, and
one JOD is a 75 % preference in a pairwise comparison. Everything above about
50 dB PSNR is within a quarter of a JOD of everything else: at these
resolutions and this luminance the top of the ladder really is near-invisible
difference, which is what the metric is for and what PSNR's unbounded scale
cannot express. This is also why BD-rate on JOD produces such large percentages
— the quality axis is short, so a small vertical gap maps to a large horizontal
one — and why the *sign and ordering* of a BD-JOD figure are far more meaningful
than its magnitude. A "+1967 % BD-rate on JOD" should be read as "this curve is
below that one everywhere in the overlap", not as a rate ratio.

---

## 5. Pop-in

`vr-mixed-1024-v2` 4:4:4, mode `all-frames` (no schedule: every frame scored at
`k = 2`, the fastest cadence a frame sequence can carry), 17 920 tile-frames per
point, `C_M` in JND units:

| codec | QP/CRF | Mbit/s | mean | p95 | max | > 1 JND | fovea p95 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `x264-intra` | 8 | 215.0 | 0.018 | 0.092 | 0.409 | 0.0 % | 0.000 |
| `x264-intra` | 32 | 28.0 | 0.199 | 0.720 | 3.046 | 2.1 % | 0.008 |
| `x264-intra` | 40 | 15.0 | 0.517 | 1.868 | 6.340 | 17.4 % | 0.014 |
| `x265-p` | 8 | 50.3 | 0.009 | 0.052 | 0.348 | 0.0 % | 0.000 |
| `x265-p` | 32 | 3.0 | 0.064 | 0.406 | 2.174 | 0.6 % | 0.001 |
| `x265-p` | 40 | 1.3 | 0.128 | 0.811 | 4.930 | 3.6 % | 0.003 |
| `x265-p-refresh` | 8 | 33.3 | 0.014 | 0.088 | 0.488 | 0.0 % | 0.000 |
| `x265-p-refresh` | 32 | 2.0 | 0.114 | 0.687 | 6.174 | 2.9 % | 0.001 |
| `x265-p-refresh` | 40 | 0.8 | 0.192 | 1.208 | **15.192** | 6.3 % | 0.003 |
| `nxv-intra` | 0 | 251.6 | 0.027 | 0.128 | 0.499 | 0.0 % | 0.001 |
| `nxv-intra` | 18 | 69.1 | 0.157 | 0.682 | 2.901 | 2.0 % | 0.005 |
| `nxv-intra` | 24 | 44.4 | 0.242 | 1.141 | 4.539 | 6.5 % | 0.007 |
| `nxv-inter` | 24 | 13.1 | 0.129 | 0.837 | 4.487 | 4.2 % | 0.004 |

Four readings.

1. **The fovea is quiet and the periphery is not.** Fovea p95 never exceeds
   0.014 JND while the whole-frame p95 reaches 1.9. This is the
   Tursun-Didyk `b4 · e^q` term doing what `docs/RATECONTROL.md` 8.3 says it
   does: sensitivity to a 12 Hz temporal change rises by a factor of 87 between
   1 and 30 degrees on a smooth tile. A pop-in metric that only looked at the
   fovea — which is how 5.3 words it, "in the fovea ring" — would measure
   nothing. **The metric as implemented reports both and the periphery is the
   interesting half.** This is a place where the measurement contradicts the
   paper's phrasing, and the model the paper's own rate-control section adopts
   is the reason.
2. **`x265-p-refresh` has the worst single pop in the whole run** (15.19 JND at
   CRF 40, against 4.93 for flat `x265-p` at a comparable rate) even though its
   *mean* and p95 are close to the others. That is the intra-refresh column
   sweeping the picture: one tile column per frame gets a full intra update, and
   at CRF 40 that update is a visible step. This is exactly the artefact class
   PAPER.md 5.3 asks for a distribution rather than a mean to catch, and a mean
   would have missed it — 0.192 looks unremarkable next to `x264-intra`'s 0.517.
3. **`nxv` sits between the two HEVC anchors** at matched rate and its pop
   distribution degrades smoothly with QP, with no tail spike. The inter path is
   quieter than the intra path at the same rate (0.837 against 1.141 p95),
   which is the pose-warped predictor doing its job on temporal stability even
   while it loses on rate.
4. **All of these are `all-frames` numbers**, i.e. frame-to-frame instability,
   not scheduled pop-in. The scheduled measurement is section 5b.

### 5b. The temporal ladder, driven end to end

This is the measurement the metric exists for. The harness wrote a
temporal-ladder skip map with `compare.py --write-skip-map --ladder`, handed it
to `nxv-enc --skip-map`, and scored the decoded result on the **same** map, so
the refresh events are known exactly rather than inferred:

```sh
compare.py --seq vr-mixed-1024-v2.yuv444p.json --write-skip-map ladder.skipmap --ladder 11223
compare.py --seq ... --codec-enc "nxv-enc --eyes 2 --inter on --poses ... --skip-map ladder.skipmap" \
           --metric fvvdp,fov-ssim,popin --popin-skip-map ladder.skipmap
```

Two schedules, both from `docs/RATECONTROL.md` 8 (Floeter et al., ETRA 2025):

| ladder | rule | tile-frames forced to `WARP_SKIP` |
|---|---|---:|
| `11223` | full rate inside 9.05 deg, `k=2` to 15.55, `k=3` beyond — *their most aggressive tolerated configuration* | 65.1 % |
| `12345` | `k=2` above 3.15 deg, 3 above 4.55, 4 above 9.05, 5 above 15.55 — *where their discomfort scores start to move* | 77.5 % |

At 8.19 ppd and 64-pixel tiles one tile subtends 7.8 degrees, so on this
material `11223` gives every tile outside the innermost ring a divisor, which is
why the skipped fraction is as high as it is.

#### `11223`, `vr-mixed-1024-v2` 4:4:4

| codec | QP/CRF | Mbit/s | PSNR-Y dB | JOD | pop mean | pop p95 | pop max | > 1 JND |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `x265-p` | 8 | 50.3 | 56.63 | 9.937 | 0.015 | 0.088 | 0.591 | 0.0 % |
| `x265-p` | 24 | 7.8 | 47.80 | 9.697 | 0.056 | 0.328 | 1.304 | 0.3 % |
| `x265-p` | 40 | 1.3 | 37.35 | 8.968 | 0.224 | 1.460 | 6.769 | 7.9 % |
| `x265-p-refresh` | 8 | 33.3 | 54.23 | 9.959 | 0.027 | 0.171 | 0.832 | 0.0 % |
| `x265-p-refresh` | 24 | 5.4 | 44.06 | 9.716 | 0.110 | 0.684 | 4.008 | 2.9 % |
| `x265-p-refresh` | 40 | 0.8 | 33.49 | 8.941 | 0.369 | 2.344 | 26.819 | 10.5 % |
| `nxv-11223` | 0 | 78.5 | 36.84 | 9.240 | 1.873 | **11.269** | 153.1 | 13.4 % |
| `nxv-11223` | 12 | 26.7 | 35.03 | 9.149 | 2.001 | 12.207 | 153.2 | 15.0 % |
| `nxv-11223` | 24 | 8.1 | 31.74 | 8.822 | 2.076 | 12.449 | 154.0 | 16.3 % |

#### `12345`, same sequence

| codec | QP | Mbit/s | PSNR-Y dB | JOD | pop mean | pop p95 | pop max | > 1 JND | fovea p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `nxv-12345` | 0 | 53.1 | 32.22 | 8.860 | 3.641 | **23.166** | 203.8 | 17.5 % | 0.027 |
| `nxv-12345` | 12 | 19.6 | 31.08 | 8.749 | 4.127 | 26.343 | 204.2 | 19.2 % | 0.031 |
| `nxv-12345` | 24 | 6.7 | 28.95 | 8.492 | 5.045 | 30.017 | 363.3 | 19.5 % | 0.040 |

The anchors are the control: they code every tile every frame, so their rows are
what the *same* frames produce with no withheld residual. 5 640 refresh events
per point under `11223`, 3 632 under `12345`.

**Five readings.**

1. **The ladder fails on this material, and the pop-in metric is what says so.**
   `nxv-11223`'s 95th-percentile pop is **11.3 to 12.5 JND** against 0.09 to
   2.34 for the anchors on the same events — one to two orders of magnitude —
   and 13 to 17 % of all refresh events exceed one JND. One JND is a 75 %
   detection rate in a pairwise comparison. This is not a marginal call.
2. **The schedule is barely rate-dependent, which localises the cause.** The
   pop distribution moves by 10 % between QP 0 and QP 24 while the rate moves by
   10x. The pops are therefore not quantisation error: they are what the pose
   warp gets wrong over `k` frames, which is exactly the quantity
   `docs/RATECONTROL.md` 8.1 says the ladder is betting on. That bet is settled
   independently by `ref/RESULTS-inter.md` section 3, where the warp-only chain
   *starts* at 24.40 dB and decays to 18.44 — below the 35 dB the paper's own
   2.11 item 2 requires. **A temporal ladder cannot work on a predictor that
   does not hold the tile,** and this measurement is the same finding arriving
   from the perceptual side.
3. **`12345` is worse than `11223` by about the factor the model predicts.**
   Doubling the maximum divisor roughly doubles the pop (p95 11.3 to 23.2 at
   matched QP, mean 1.87 to 3.64), which is the linear drift accumulation of
   approximation 5 in `docs/RATECONTROL.md` 8.2 showing up in a real
   measurement rather than in a simulation.
4. **The fovea column is a self-check that passed.** Under `11223` the fovea
   disc reports **no tiles**: the schedule holds everything inside 9.05 degrees
   at full rate, so no fovea tile is ever a refresh event, and the metric
   correctly finds nothing to score. Under `12345`, which steps tiles from 3.15
   degrees outward, fovea events appear and their p95 is 0.027 to 0.138 — three
   orders of magnitude below the whole-frame figure. The map that was written,
   the map the encoder consumed and the map the metric scored are the same map.
5. **JOD is more forgiving of this than PSNR is, and pop-in is less forgiving
   than either.** Against `x265-p-refresh`, `nxv-11223` is +2553 % BD-rate on
   PSNR-Y but +1704 % on JOD — the ladder's damage is peripheral, so the VDP
   discounts it by 849 points. But the artefact is *temporal*, and FovVideoVDP
   pools two temporal channels over the whole frame rather than reporting the
   tail of a per-tile distribution. The pop-in metric is the instrument that
   resolves it: "16 % of refresh events above one JND, worst 154" is actionable
   in a way that "8.822 JOD" is not. That is the argument PAPER.md 5.3 makes for
   having a dedicated pop-in metric at all, and this run is the first evidence
   for it on real bitstreams.

Several BD-rate cells for these runs are `n/a`: forcing 65 to 78 % of
tile-frames to `WARP_SKIP` drops the codec's whole quality range below the
anchors' (`nxv-12345` spans 28.95 to 32.22 dB against `x265-p`'s 37.35 to
56.63), so the curves do not overlap and the Bjontegaard integral has nothing to
integrate over. The harness says so rather than extrapolating.

---

## 6. Motion to photon

PAPER.md 5.3: *"Latency is a quality metric: motion-to-photon measured with a
photodiode on the panel and an IMU on the headset, reported alongside every
quality number; a codec that gains 1 JOD by adding 8 ms has lost."*

**No motion-to-photon time was measured.** There is no photodiode and no IMU on
this machine and no headset in the loop. What follows is PAPER.md 4.2's budget
next to the one term this harness can observe, and `nxq/latency.py` marks it
`measured: false` in every results file.

The budget, WiFi 6: pose uplink 1.5 + render 11.0 + pipeline 6.8 + compositor
phase wait 5.5 + reprojection and scanout 5.0 = **29.8 ms**, against the paper's
own "25 to 35 ms floor". The frame period at 90 Hz is 11.1 ms.

Measured on `vr-mixed-1024-v2` 4:4:4, mean over the ladder, **reference CPU
implementations**:

| codec | encode ms/frame | decode ms/frame | x the budget's 6.8 ms pipeline |
|---|---:|---:|---:|
| `x264-intra` | 18.4 | 14.1 | 4.8 |
| `x265-p` | 62.8 | 12.2 | 11.0 |
| `x265-p-refresh` | 59.7 | 11.2 | 10.4 |
| `nxv-intra` | 2052.9 | 74.7 | **312.9** |
| `nxv-inter` | 1705.7 | 77.5 | **262.2** |

These numbers are **not** substituted into the budget, and they must not be:
4.2's 6.8 ms assumes the GPU encoder (3.0 ms on a 7900 XTX) and the GPU decoder
(4.0 ms on an XR2), and the reference codec is single-threaded C++ on four
nice'd cores. The ratio is the honest form. What it says is that the reference
implementation is three orders of magnitude away from its own latency budget,
which is expected of a reference implementation and is the reason the GPU
decoder is a Phase 0 gate — but it also means **no latency claim in this
repository is currently supported by a measurement**, and the paper's own rule
("a codec that gains 1 JOD by adding 8 ms has lost") cannot be evaluated either
way until the GPU path is timed on the target device.

---

## 7. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export PATH=<worktree>/build-ref/bin:$PATH
$NXQ_SCRATCH/venv/bin/pip install --index-url https://download.pytorch.org/whl/rocm6.4 torch
$NXQ_SCRATCH/venv/bin/pip install pyfvvdp
cd <worktree>/tools/quality
chrt -i 0 taskset -c 16-19 nice -n 19 $NXQ_SCRATCH/venv/bin/python compare.py \
    --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.json \
    --codec-cmd nxv --codec-name nxv-intra \
    --anchors x264-intra,x265-p,x265-p-refresh \
    --qp 0,6,12,18,24 --anchor-qp 8,16,24,32,40 \
    --metric fvvdp,fov-ssim,popin --foveated-psnr --no-vmaf \
    --out $NXQ_SCRATCH/results/perceptual/intra-mixed1024-444.json
```

CPU PyTorch (`--index-url .../whl/cpu`) gives the same JODs about six times
slower; `--fvvdp-device cpu` forces it on a GPU machine. The full driver,
including the temporal-ladder skip maps, is
`$NXQ_SCRATCH/run-perceptual.sh`.

## 8. What is still missing

1. **Display space.** The metric should be run on the reprojected pair, not the
   decoded pair (5.3). That needs the client reprojection shader in the loop and
   is a change to `ref/`, not to `tools/quality`.
2. **A 2160 px sequence.** Everything here is scored at 1.9 to 7.5 ppd against
   the Pico 4's 15.8. The orderings are stable across a 16x range of pixel
   counts, which is evidence they survive, but it is not the same as measuring
   at the panel's density.
3. **Gaze.** Every run here is a centre fixation. `--fvvdp-gaze` and
   `foveated_metrics.py --gaze-log` take a per-frame gaze log; no capture in
   `$NXQ_SCRATCH/seq` has one.
4. **ColorVideoVDP**, which 5.3 names as the colour-aware successor, and which
   would be the right instrument for the 4:4:4-fovea / 4:2:0-periphery decision
   of 5.2 that no metric in this report can see.
5. **A real motion-to-photon measurement**, per section 6.
