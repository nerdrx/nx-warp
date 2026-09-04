# NX Warp quality and comparison harness

Test material, anchor encoders, quality metrics, BD-rate and reports for the
NX Warp codec.

This harness exists to answer one question first — the **Phase 1 exit
criterion** from `docs/PAPER.md` 3.11:

> within 1.0 dB PSNR of x264 intra (`--keyint 1`, zerolatency) at 100 to 400
> Mbit on VR captures

and then the Phase 2, 3 and 4 criteria that follow it. It is written so that
every one of those criteria is *evaluated by code*, not eyeballed off a chart:
`compare.py` prints PASS or FAIL against the paper's own numbers, and says so
explicitly when the measurement does not support a verdict.

Everything is Python 3 with numpy. There is no C++ here.

---

## Contents

| Path | What it is |
|---|---|
| `nxq/` | the library: YUV I/O, metrics, BD-rate, ffmpeg driving, codec driving |
| `capture/` | test material: synthetic generation, media import, WiVRn grabs |
| `compare.py` | run the anchors and the codec, measure, analyse |
| `report.py` | Markdown + SVG rate-distortion report |
| `foveated_metrics.py` | eccentricity-weighted PSNR/SSIM (PAPER.md 5.1.2) |
| `dummy_codec.py` | a mock `nxv-enc`/`nxv-dec` for proving the harness |
| `tests/` | pytest suite, registered with ctest by `tools/CMakeLists.txt` |
| `reports/` | generated reports land here (git-ignored) |

---

## Setup

numpy is the only hard requirement. `pytest` is needed for the tests,
`matplotlib` gives nicer plots (there is a hand-written SVG fallback if it is
absent), `Pillow` is needed only to import PNG/JPEG sequences, and `scipy` only
for the optional `pchip` BD-rate variant.

```sh
python3 -c 'import numpy, matplotlib, PIL'          # check what you have
python3 -m venv --system-site-packages /run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/venv
/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/venv/bin/pip install pytest scipy
```

Check what this machine can do:

```sh
python3 compare.py --probe
```

which reports the ffmpeg version, whether `libx264`/`libx265` and the `libvmaf`
filter are present, the CPU-discipline prefix in use, and whether the codec
CLIs are on `PATH`.

### CPU discipline

Every external process — ffmpeg, x264, x265, the codec CLIs — is launched
through `nxq/cpu.py` as:

```
chrt -i 0 taskset -c 28-31 nice -n 19 <cmd>
```

with ffmpeg additionally capped at `-threads 4`. Override the core slice with
`NXQ_CPUS`, the thread count with `NXQ_THREADS`, or disable the prefix entirely
with `NXQ_NO_CPU_LIMIT=1` (for CI containers without `chrt`/`taskset`).

Run the harness itself under the same prefix.

### Where files go

Generated material is large: one second of 2048x2048 stereo 4:4:4 at 90 Hz is
about 2 GB. **Nothing large goes in the repository or in `/tmp`** (which is
tmpfs here). Use the scratch volume:

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
mkdir -p $NXQ_SCRATCH
```

---

## 1. Getting test material

### 1a. Synthetic VR sequences

`capture/gen_synthetic.py` renders stereo views out of a large equirectangular
panorama using a per-frame head pose, so a rotation sequence is geometrically
exactly what the codec's rotation-only reprojection (PAPER.md 2.2) is supposed
to predict.

```sh
python3 capture/gen_synthetic.py --out $NXQ_SCRATCH/seq --name vr-mixed-512 \
    --frames 10 --eye-width 512 --eye-height 512 --motion mixed --preview
```

That writes, for each requested pixel format:

* `vr-mixed-512.yuv444p.yuv` — headerless planar YUV, 8 bit
* `vr-mixed-512.yuv444p.json` — the sequence sidecar (geometry, fps, source)
* `vr-mixed-512.poses.json` — one pose per frame
* `vr-mixed-512.preview.png` — with `--preview`, frame 0 as an image

The panorama deliberately contains the content classes the paper cares about:

| Content | Why it is there |
|---|---|
| textured noise terrain | real entropy for the coder |
| text panels at two scales | the 4:2:0 chroma-fringe case; 5.2 locks text to lossless |
| checkerboards at 3 frequencies | aliasing under any downsampling |
| a zone plate (radial chirp) | resampling blur, the 2.11 item 2 risk |
| a near-black gradient region | banding; 5.2's `dQ_lum = -2` below 16/255 |
| saturated colour bars | chroma fidelity |
| a star field | isolated impulses the warp must not smear |
| near-field moving discs | residual motion after pose warp (2.3); real stereo disparity |
| a head-locked HUD panel | the `STATIC_MV` class (2.11 item 6) |

**Motion profiles** are defined by angular *rate*, not amplitude, so a 10-frame
clip and a 600-frame clip exercise the same angular velocities:

| `--motion` | peak yaw rate | use |
|---|---|---|
| `static` | ~0 (human drift only) | at-rest coding |
| `pan` | 30 deg/s | sustained comfortable panning |
| `turn` | 150 deg/s | a brisk head turn |
| `mixed` | 120 deg/s | rest, then turn, then rest — **the Phase 2 kill-test profile** |

`mixed` is the important one: it contains both low- and high-velocity frames in
a single sequence, which is what PAPER.md 2.11 item 1 needs when it asks for
BD-rate "overall and on the 20 percent of frames with the highest angular
velocity". Every pose carries `angular_velocity_deg_s`, and `compare.py` uses it
to do that split automatically.

Use `--motion all` to generate one sequence per profile. Resolutions above
1 Mpix per eye require `--full`, which is a guard against accidentally filling
the disk — that is the documented "full" mode:

```sh
python3 capture/gen_synthetic.py --out $NXQ_SCRATCH/seq --name vr-mixed-2048 \
    --frames 60 --eye-width 2048 --eye-height 2048 --motion mixed --full
```

Generation is deterministic: the same arguments produce byte-identical output
on any machine, including the built-in 5x7 bitmap font (a system font would
make PSNR numbers incomparable between machines).

### 1b. PNG/JPEG sequences and video files

```sh
python3 capture/import_media.py images --in "$HOME/shots/*.png" \
    --out $NXQ_SCRATCH/seq --name vrchat-stills --pix yuv444p,yuv420p

python3 capture/import_media.py video --in capture.mp4 \
    --out $NXQ_SCRATCH/seq --name alyx-60s --frames 300 --fps 90
```

Images are sorted naturally (`f_2.png` before `f_10.png`) and converted with
the harness's own BT.709 limited-range conversion; video goes through ffmpeg.
Both produce the same sidecar as the synthetic generator, so the rest of the
harness takes them without extra flags. Use `--layout sbs` if the frames are
already side-by-side stereo.

### 1c. Real frames from WiVRn NX

**Finding: WiVRn NX can already produce raw frames and pose logs with no source
changes.** The investigation of `/run/media/nerdrx/Lex/claude/wivrn-nx` (read
only; nothing in that tree was modified) found three existing dump facilities
and, crucially, an uncompressed encoder.

#### What already exists

| Facility | Where | What it gives |
|---|---|---|
| `WIVRN_DUMP_VIDEO=<prefix>` | `server/encoder/video_encoder.cpp` (`video_encoder::create`, and the write in `SendData`) | writes the encoder's output bytes to `<prefix>-<stream>.yuv` |
| `encoder: "raw"` | `server/encoder/video_encoder_raw.{h,cpp}` | an *uncompressed* encoder: copies both image planes into a host-mapped buffer and returns them as the "encoded" data. 8 bit only. |
| `WIVRN_DUMP_TIMINGS=<file>` | `server/driver/wivrn_session.cpp` (`dump_time`) | CSV of `event, frame, time, stream`; includes `encode_begin`/`encode_end` keyed by the same `frame_index` as the video |
| `WIVRN_DUMP_HEAD=<file>` | `server/driver/pose_list.cpp` | CSV of the head pose track: timestamps, position, orientation quaternion, and their derivatives |
| `WIVRN_DUMP=list` | same | prints the device names that can be dumped, if `_HEAD` is not the right suffix on your build |

Combining `encoder: "raw"` with `WIVRN_DUMP_VIDEO` therefore yields genuine
uncompressed frames today. Print the full recipe with:

```sh
python3 capture/wivrn_capture.py plan
```

Then convert what you captured:

```sh
python3 capture/wivrn_capture.py convert --in dump-0.yuv --w 1440 --h 1440 \
    --out $NXQ_SCRATCH/seq --name vrchat-left --pix yuv420p,yuv444p

python3 capture/wivrn_capture.py poses --timings timings.csv --head head.csv \
    --out $NXQ_SCRATCH/seq/vrchat-left.poses.json
```

`convert` de-interleaves NV12 into planar 4:2:0 (and replicates chroma for a
4:4:4 copy). `poses` joins the frame-indexed timing CSV to the pose track by
timestamp — `head.csv` is a track sampled at its own rate, not one row per
frame — and warns if the nearest pose sample for any frame is more than a frame
time away. It locates CSV columns by name and, if the format has changed, fails
with the list of columns it actually found rather than silently misreading.

#### Caveats you must know before trusting the numbers

1. **The dumped frame is post-foveation and post-colour-conversion.** In WiVRn
   the foveation compute pass *is* the RGB→YCbCr pass (`foveation.comp`), so
   there is no un-foveated NV12 anywhere in the server. The output is BT.709
   **full range** with an sRGB transfer already applied.
2. **You can degenerate the foveation to an identity.** `foveation::compute_params`
   emits a 1:1 LUT when the encode extent is at least the source view extent.
   So: set the headset's video scale so the stream extent ≥ the render extent,
   `render_scale` to 1.0, `foveation_strength` to 0 and adaptive foveation off.
   Then the pass is a pure colour conversion. This is the closest you get to
   linear material without a code change.
3. **Turn off FSR1 and motion smoothing.** Motion smoothing synthesises frames
   (`motion_warp_commit`), and a synthesised frame is not a render target; it
   must not enter a codec test.
4. **There is no headless or fake-driver mode.** Monado's simulated driver is
   explicitly disabled (`XRT_BUILD_DRIVER_SIMULATED OFF` in `server/CMakeLists.txt`)
   and the compositor path only runs when a real client is connected and an
   OpenXR app is submitting layers. A client on loopback works; a server-only
   capture mode does not exist.
5. **Raw frames are enormous** — roughly 3 MB per eye per frame at 1440x1440
   NV12, so about 17 GB per eye per minute at 90 Hz, and they also traverse the
   network. Capture short takes onto the scratch volume.

#### Where a dump hook belongs, if one is added later

Two tap points, in increasing order of effort. **This is a note for whoever
works on the WiVRn side; nothing here modifies that tree.**

*Post-foveation NV12, plus poses, in one place — the cheap option.* In
`server/encoder/video_encoder.cpp`, inside
`video_encoder::encode(wivrn_session&, const view_info_t&, uint64_t frame_index)`,
immediately after the backend `encode()` returns its data. Everything needed is
in scope at once: the frame bytes, `frame_index`, and the `view_info_t` holding
`display_time`, `pose[2]` and `fov[2]` (`common/wivrn_packets.h`). Writing one
CSV row per frame there closes the real gap — the existing pose CSV is a track
that has to be joined by timestamp, whereas `view_info` is already the pose the
frame was rendered (or warped) to. Gate it on a new `WIVRN_DUMP_FRAMES`
variable opened next to `video_dump` in `video_encoder::create`.

*Pre-foveation linear RGBA — the correct option.* In
`server/compositor/compositor.cpp`, in `layer_commit` just before
`foveation.foveate(...)`, where the composited source views, the source rect,
`flip_y`, `frame_index` and `view_info` are all live. The existing PipeWire
mirror (`server/compositor/pipewire_mirror.cpp`, `mirror_impl::capture`) already
does exactly this readback — resample plus `copyImageToBuffer` into a
host-mapped buffer, waiting on the compositor timeline semaphore — and its own
header states it taps "after layer composition but before foveation". Copying
that pattern is the shortest path to linear material.

*Or use the mirror as-is,* accepting its limits: left eye only, rate-limited
(default 30 fps) and scaled by 0.5 by default. Set `scale: 1.0` and the mirror
fps to the stream rate; the config is read only at session start.

---

## 2. Comparing against the anchors

```sh
python3 compare.py --seq $NXQ_SCRATCH/seq/vr-mixed-512.yuv444p.json \
    --codec-cmd nxv --qp 16,22,28,34 --anchor-qp 16,22,28,34 \
    --anchors x264-intra,x265-p --out $NXQ_SCRATCH/results/run.json
```

### The anchors

| Name | Configuration | Used for |
|---|---|---|
| `x264-intra` | `--keyint 1 --tune zerolatency`, no B-frames | **the Phase 1 gate** |
| `x264-p` | zerolatency, P-only, `ref=1`, one IDR at the start | low-latency reference |
| `x265-p` | zerolatency, P-only, `ref=1`, one IDR at the start | **the Phase 2 BD-rate anchor** |
| `x265-intra` | `--keyint 1`, zerolatency | extra |

All are driven through ffmpeg and encode to an **elementary stream**, so the
measured rate has no container overhead. Rate is
`bytes * 8 / frames * fps`, reported in Mbit/s.

If `libx264` or `libx265` is missing, that anchor is skipped with a clear
message and the run continues.

On this machine (checked with `ffmpeg -encoders` / `-filters`): ffmpeg n9.0.1
with **libx264, libx265 and the libvmaf filter all present**.

### Driving the codec

The real CLIs are:

```
nxv-enc --in file.yuv --w W --h H --pix yuv444p|yuv420p --qp N --out out.nxv
nxv-dec --in out.nxv --out out.yuv
```

`--codec-cmd <prefix>` appends `-enc`/`-dec` (so `--codec-cmd nxv` runs
`nxv-enc`, `--codec-cmd build/nxv` runs `build/nxv-enc`). For anything else, use
`--codec-enc` and `--codec-dec` with full command lines. Nothing is hard-coded,
so the harness works before, during and after the codec exists.

### Metrics

| Metric | How |
|---|---|
| PSNR per plane, and `(6Y+Cb+Cr)/8` weighted | `nxq/metrics.py`, numpy; the weighted figure is formed in the MSE domain, the JVET convention |
| SSIM | Wang et al. 2004, Gaussian 11x11 σ=1.5, our own numpy |
| MS-SSIM | Wang et al. 2003, 5 scales, standard weights, our own numpy |
| VMAF | ffmpeg's `libvmaf` filter when present; inputs converted to 4:2:0, which is what the model is trained on |

The spatial filter is a sum of shifted slices rather than a big sliding-window
view, so peak memory stays at a few copies of the image even at 2048x2048, and
nothing here needs scipy.

### The analysis

**BD-rate / BD-PSNR** (`nxq/bdrate.py`) is the original VCEG-M33 method: a cubic
fit, integrated analytically over the overlapping interval. A monotone `pchip`
variant is available when scipy is installed.

Note that the two figures need overlap on *different* axes — BD-rate needs the
quality ranges to overlap, BD-PSNR needs the rate ranges to — so one can be
computable when the other is not. Each is attempted independently and reported
if it succeeds.

**The Phase 1 gate** is evaluated directly. Both curves are interpolated in
log-rate over the part of the 100–400 Mbit band that both actually cover, and
the worst PSNR deficit is compared against 1.0 dB:

```
  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    PASS: worst -0.412 dB at 168.3 Mbit/s, mean -0.298 dB over 105.0-380.0 Mbit/s
```

If the operating points do not reach the band, it says exactly that instead of
inventing a verdict:

```
    not evaluated: the 100-400 Mbit band is not covered by both curves
    (anchor spans 24.1-69.5, codec spans 14.6-43.4 Mbit/s); choose QP points
    that land in the band
```

That is a real and common situation — a small test clip at 90 fps simply does
not produce 100 Mbit — and it is why the band is a parameter (`--phase1-band`).

**The angular-velocity split** (PAPER.md 2.11 item 1) is computed whenever the
sequence has a pose log: frames are split at the `--velocity-pct` percentile of
`angular_velocity_deg_s` and PSNR is reported for each subset per operating
point.

---

## 3. Reports

```sh
python3 report.py --results $NXQ_SCRATCH/results/run.json
```

Writes Markdown plus an SVG rate-distortion plot into `reports/`. Several
result files can be passed at once to make one multi-sequence report. Plots use
matplotlib with the **Agg backend** (never a GUI window) when it is installed,
and a hand-written SVG writer otherwise, so the figure is never silently lost
on a machine without matplotlib. `--metric` selects what the plot shows
(`psnr_y`, `psnr_ycbcr`, `ssim_y`, `ms_ssim_y`, `vmaf`).

---

## 4. Foveated metrics

`foveated_metrics.py` implements the acuity model from PAPER.md 5.1.2,
`ppd_needed(e) = 60 / (1 + e/2.3)`, ready for the Phase 4 criterion.

```sh
python3 foveated_metrics.py --ref $NXQ_SCRATCH/seq/vr-mixed-512.yuv444p.json \
    --dis decoded.yuv --hfov 95 --fixation 300,240 --weighting acuity
```

It reports, per eye of an `sbs` frame: eccentricity-weighted PSNR and SSIM,
their unweighted counterparts for comparison, and a hard fovea/periphery PSNR
split at the paper's 8-degree `s=1` radius (5.1.4) — the region form the Phase 4
criterion is stated in.

Eccentricity is computed exactly: a VR render target is a rectilinear (tan)
projection, so a pixel `r` pixels off-axis is at `atan(r/f)`, and eccentricity
is the true angle between the ray to the pixel and the ray to the fixation.
That stays correct when the fixation is itself off-axis, which a scaled pixel
distance would not.

> **A trap worth stating.** In a tan projection the **centre has the lowest**
> angular pixel density, and density rises towards the edge as
> `ppd_render(θ) = ppd_center / cos²θ` — the paper's own formula, which the
> tests check against the exact pinhole derivative. So for a 2160 px, 100 degree
> view, `ppd_from_fov(2160, 100)` returns **15.8 ppd at the centre**, while the
> familiar "2160/100 = 21.6 ppd" is the *average across the width*. They are
> different numbers and the distinction matters when setting `--ppd`. Pass
> `mode="average"` if you want the second one. (PAPER.md 5.1.2's "about 21 ppd
> average, about 24 ppd in the center" is not reproducible from the tan
> projection alone; it appears to fold in the lens distortion mapping.)

---

## 5. Proving the harness without the codec

`dummy_codec.py` speaks the exact `nxv-enc`/`nxv-dec` command line and quantises
each plane on the H.264 QP ladder (`step = 2^(qp/6)`), then deflates. It is not
a real codec — it has no transform, no prediction and no entropy model — but it
produces a genuine monotone rate-distortion curve, which is all that is needed
to prove the plumbing.

```sh
python3 compare.py --seq $NXQ_SCRATCH/seq/vr-mixed-512.yuv444p.json \
    --codec-enc 'python3 dummy_codec.py enc' \
    --codec-dec 'python3 dummy_codec.py dec' \
    --codec-name nxv-dummy --qp 16,22,28,34 --anchor-qp 16,22,28,34 \
    --out $NXQ_SCRATCH/results/dummy.json
python3 report.py --results $NXQ_SCRATCH/results/dummy.json
```

Do not read anything into the numbers it produces.

---

## 6. Tests

```sh
python3 -m pytest tests/ -q          # about 2 seconds
python3 -m pytest tests/ -q -m slow  # just the end-to-end ffmpeg run
```

Or through ctest, which `tools/CMakeLists.txt` registers:

```sh
cmake -S . -B build -DNXWARP_QUALITY_PYTHON=$NXQ_SCRATCH/venv/bin/python
ctest --test-dir build -R nxwarp-quality
```

The registration is skipped with a status message, not a failure, when the
interpreter lacks pytest or numpy.

Test sequences are small on purpose (a few frames at 64–128 px); the one
end-to-end test generates a sequence, runs x264 and the mock codec over four QP
points, computes every metric, does the BD-rate and the gate, and renders a
report — in about a second.

BD-rate is pinned by analytic invariants that hold *exactly* for the cubic
method — scaling every rate by `k` must give `BD-rate = (k-1)·100`, shifting
every PSNR by `d` must give `BD-PSNR = d` — plus a cross-check against an
independently written integrator. That is stronger than a magic number copied
from a spreadsheet.

---

## The exact Phase 1 exit test

Once `nxv-enc` and `nxv-dec` exist, this is the command that decides Phase 1.
Capture a real VR sequence per section 1c (or generate one per 1a), then:

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp

python3 compare.py \
    --seq $NXQ_SCRATCH/seq/<capture>.yuv444p.json \
    --codec-cmd nxv \
    --anchors x264-intra \
    --qp 12,18,24,30 --anchor-qp 12,18,24,30 \
    --phase1-anchor x264-intra \
    --phase1-band 100,400 \
    --phase1-tolerance 1.0 \
    --out $NXQ_SCRATCH/results/phase1.json

python3 report.py --results $NXQ_SCRATCH/results/phase1.json \
    --title "NX Warp Phase 1 exit criterion"
```

Both commands under `chrt -i 0 taskset -c 28-31 nice -n 19`.

Choose the QP points so that **both** curves span 100–400 Mbit — the gate
refuses to give a verdict outside the band, by design. Check the coverage line
in the output and widen the ladder if it complains. The criterion is met when
the summary prints:

```
  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    PASS: worst -X.XXX dB at ... Mbit/s, ...
```

with `worst_delta_db >= -1.0` across the covered band, on VR captures rather
than on synthetic material.
