# Audit: the 8 dB between `warp/RESULTS.md` and `ref/RESULTS-inter.md`

**2026-09-04.** Two measurements of the same predictor disagreed, and the
codec's core bet hangs on which one is right.

- `warp/RESULTS.md` (a): single-step warp prediction, centre crop, **32.4 to
  32.7 dB flat from 0 to 313 deg/s**.
- `ref/RESULTS-inter.md` section 3: the warp-only chain on the real codec
  starts at **24.40 dB on the first warped frame** of `vr-mixed-1024`, before
  any chaining has happened at all.

Either the reference codec's pose ingestion has a convention wrong — a
quaternion read backwards, a rotation applied in the wrong direction, a sign
on a FOV tangent — in which case the codec is 8 dB better than it has been
reporting and the Phase 2 kill test deserves a re-run; or 24.40 dB is what the
predictor really achieves on that material, in which case the number stands and
so does the verdict.

## The answer, first

**It is not a convention mismatch. Every convention checks out, and the
reference codec's integer predictor is within 0.02 dB of the exact float
homography on the exact frames where it scores 24.40 dB.**

The 8 dB is a difference between the two harnesses' **ground truth**, not
between their predictors. `nxvc-warpsim` renders band-limited reference frames
(panorama at 4.2x the eye's angular resolution, 5x5 box supersampling per
output sample, and it says so in its own caveats). `gen_synthetic.py` takes a
single bilinear tap from a panorama at 2.1x. Its output is aliased, the aliasing
is not a geometric function of the pose, and no warp of any precision can
predict it.

So the number in `ref/RESULTS-inter.md` is correct as a measurement and
misleading as a statement about the codec. It is mostly a measurement of the
test harness. The Phase 2 kill test is **not** re-run below, because nothing
that feeds it changed; what changed is what the number is allowed to be used
to argue. Section 6 says what that is.

Two real defects were found on the way and are fixed. Neither is the 8 dB.

---

## 1. Method

Three predictors are measured on identical frame pairs, so that the
generator's error and the predictor's error separate:

| | what it is | why |
|---|---|---|
| **ideal** | the exact float homography, evaluated per pixel in double precision, bilinear resample | the ceiling any integer warp can reach *on this material*. Contains the generator's aliasing and nothing else. |
| **ref** | `nxv-enc --inter on` with a skip map forcing `WARP_SKIP` on every tile, `--lossless` so frame 0 is a bit-exact reference, decoded | the reference codec's real predictor, end to end, including `derive_homography()`, the Q10.21/Q2.29 quantisation, the corner divide, the 64x64 corner interpolation and the bilinear taps |
| **warpsim** | `warp/RESULTS.md` as published | the second implementation |

`ideal` is written from the generator's own projection maths
(`synth.py` `_ray_grid`, `render_view`, `rot_matrix`) rather than from
`homography.cpp`, so that agreement between `ideal` and `ref` is evidence about
conventions and not a shared assumption.

All measurement under `chrt -i 0 taskset -c 28-31 nice -n 19`.

## 2. The decisive measurement

`vr-mixed-1024`, 4:4:4, the actual sequence `ref/RESULTS-inter.md` reports on,
the actual pose log, both eyes, full-frame PSNR-Y — i.e. exactly the quantity
that document publishes. `ideal` warps the **true** frame N-1; `ref` is the
codec chaining as it really does.

| frame | pose delta | ideal, full | ideal, centre | ref, full | ref, centre | ref - ideal (centre) |
|---|---|---|---|---|---|---|
| 1 | 0.0632 deg | **24.43** | 24.13 | **24.40** | 24.11 | **-0.02** |
| 2 | 0.0475 deg | 25.71 | 24.49 | 24.35 | 23.68 | -0.81 |
| 3 | 0.0326 deg | 29.03 | 28.57 | 23.68 | 23.02 | -5.55 |
| 4 | 0.0363 deg | 27.20 | 26.59 | 22.88 | 22.32 | -4.27 |
| 5 | 0.1482 deg | 23.97 | 24.60 | 21.52 | 21.93 | -2.67 |

**Row 1 settles it.** On the first warped frame — the one the kill test quotes,
the one with no chaining in it — the reference codec's integer predictor is
**0.02 dB** from an exact double-precision evaluation of the same geometry.
There is no 8 dB of convention error in the codec, because there is no room for
8 dB of anything.

Rows 2 onward diverge for a stated reason and not a mysterious one: `ideal`
re-warps the true previous frame each time while `ref` warps its own previous
output, so the gap from row 2 down is chain accumulation, which is the effect
`ref/RESULTS-inter.md` section 3 is *about*.

Note the pose delta. The first warped frame of `vr-mixed-1024` is a **0.063
degree** rotation — 0.68 pixels at 1024 px per 95 degrees. A sub-pixel
rotation of a static panorama scores 24.4 dB. That is the whole story in one
line, and it is a story about the panorama.

## 3. The convention table

Each row is one convention deliberately broken, everything else held correct,
measured on a pure-rotation pair from the same generator (512 px eye, 95 deg,
centre-crop PSNR-Y). The pose is yaw + pitch + roll together, because a
pure-yaw pair cannot distinguish several of these.

| convention variant | 2 deg | 10 deg | verdict |
|---|---|---|---|
| **as specified (`WARP.md` 2.1 / 4)** | **32.59** | **30.04** | — |
| `R_rel` inverted (`R_cur^T R_prev`) | 14.54 | 10.79 | direction correct |
| quaternion conjugated (handedness) | 14.54 | 10.79 | handedness correct |
| components read as `(w,x,y,z)` | 25.56 | 14.64 | order correct |
| y-up (row 1 sign of `K` not flipped) | 17.14 | 14.29 | row direction correct |
| FOV tangents both positive (no left) | 8.92 | 8.53 | tangent signs correct |
| no `+0.5` pixel centre in `K` | 32.56 | 29.34 | centre convention correct (worth 0.03-0.7 dB) |
| origin at the corner (`ox=oy=0`) | 15.73 | 12.92 | centring correct |
| identity — no warp at all | 15.46 | 12.82 | *(the floor: what "broken" looks like)* |

Read the last row first. **A broken convention scores about what applying no
warp at all scores** — 15 dB — because a wrongly-oriented warp is not a
degraded prediction, it is a differently-wrong picture. Every variant lands
there. The correct convention is 17 dB clear of all of them, and the codec is
on the correct one.

Two conventions were checked directly rather than by ablation, because they
admit an exact answer:

| check | measured |
|---|---|
| `quat_to_mat(_quat_from_ypr(y,p,r))` vs the generator's `rot_matrix(y,p,r)`, 200 random triples | max abs difference **7.8e-16** |
| `homography.cpp` `make_K()` vs the generator's `_ray_grid` inverse, 200 random rays | max **1.1e-13 px** |

And two by construction:

- **Frame pairing (N-1 to N).** Section 2's `ideal` column was computed from
  `poses[i-1] -> poses[i]` and agrees with the codec to 0.02 dB on frame 1. An
  off-by-one would not agree. `ref.warp_convention` additionally requires an
  identity pose to decode bit-exactly, which an off-by-one breaks.
- **Render pose vs predicted display pose.** The generator renders each frame
  with the pose it writes to the sidecar; there is no display-time
  reprojection anywhere in the harness, so the distinction does not yet exist
  in this material. It will exist for WiVRn captures, and `pose_kind` is now a
  field in the sidecar so that the file says which one it holds
  (`WARP.md` 2.1).

### Per-eye IPD

A rotation-only homography cannot model translation, so the audit priced the
stereo disparity the generator does produce. It is smaller than expected and
for a structural reason: the panorama background is sampled **by ray direction
only**, so it sits at infinity and carries **zero** disparity. Only the seven
near-field discs (0.4 to 3 m) have any, plus the +-3 cm positional sway the
pose log carries. Both eyes are warped with the same matrix and each eye is
predicted from its own previous frame, so the constant IPD offset cancels
exactly; what remains is only the *change* in parallax across one frame.

Measured on frame 0 to 1 of `vr-mixed-1024`, ideal warp, removing the discs and
the HUD together: **25.15 dB against 23.12 dB**, so all near-field content —
disc parallax, disc motion, and the HUD's changing frame counter — is worth
**2.0 dB**. Real, and not the 8 dB.

## 4. What the 8 dB actually is

Same pose pair (frame 0 to 1 of `vr-mixed-1024`, 0.063 deg), same ideal float
warp, varying only how the ground truth is rendered:

| ground truth | full dB | centre dB |
|---|---|---|
| as `gen_synthetic.py` renders it (panorama 8x eye, 1 tap, discs + HUD) | 23.12 | 22.80 |
| — discs and HUD removed | 25.15 | 23.13 |
| — and 4x4 render supersampling | 29.25 | 27.62 |
| — and panorama at 16x eye (`warpsim`'s ratio) | **32.38** | **37.52** |
| panorama at 16x eye, still 1 tap, no discs/HUD | 21.43 | 22.41 |

The last row is the control, and it is the one that proves the mechanism: a
*finer* panorama point-sampled once is **worse** (21.43 dB), not better. That
can only be aliasing. Oversampling the source and then not band-limiting the
render puts more energy above the eye's Nyquist, and energy above Nyquist is
not a function of the pose, so it is unpredictable by construction.

Adding it up: **7.2 dB full-frame / 14.4 dB centre** separates the two
harnesses' ground truth, of which 2.0 dB is content (discs, HUD) and the rest
is band-limiting. That is the disagreement, entirely, and none of it is in
either predictor.

The corroborating measurement is `ref.warp_convention`, which renders the same
projection over an analytically smooth world — band-limited by construction,
no panorama at all — and runs the **real reference codec** through it:

| pair | ref codec, PSNR-Y |
|---|---|
| yaw 0.25 deg (22 deg/s) | 57.55 |
| yaw 1 deg (90 deg/s) | 57.70 |
| yaw 2 deg (180 deg/s) | 55.89 |
| pitch 2 deg | 54.71 |
| roll 2 deg | 57.60 |
| yaw 2 / pitch 0.8 / roll 0.5 deg | 55.39 |
| yaw 5 deg (450 deg/s, past the envelope) | 50.02 |
| identity | bit-exact |

**The same code path that scores 24.40 dB scores 55.89 dB on band-limited
material at 180 deg/s.** The predictor is not the limit. And note the shape:
flat from 22 to 180 deg/s, down 6 dB by 450 deg/s where the 64x64 corner
interpolation of `WARP.md` 7 starts to cost half a pixel. That is the
speed-independence `warp/RESULTS.md` finding 1 claims, reproduced on the real
codec for the first time.

## 5. The defect that was found

The audit was looking for a convention mismatch and found one — not the one it
was looking for, and not live, but one flag away from live.

**`.poses.json` carried no field of view, and `nxv-enc` assumed 95x95
degrees.** `gen_synthetic.py` has `--hfov` and `--vfov`. Using them produced a
sequence whose warp was derived from the wrong `K`, silently.

| rendered FOV | encoder assumed | 2 deg centre PSNR |
|---|---|---|
| 95 | 95 | 32.59 |
| 110 | 110 | 31.01 |
| 110 | **95** (the old default) | **18.70** |
| 80 | 80 | 34.76 |
| 80 | **95** | **16.61** |

A 12 to 18 dB silent loss, available from a single command-line flag, with
nothing in the codebase to catch it: it produces a legal bitstream, it decodes,
the encoder-runs-the-decoder shadow check still matches bit for bit, and every
`warp.*` test still passes, because they all check the predictor against its own
arithmetic rather than against a picture. That is exactly the class of bug this
audit was commissioned to look for; it was simply in the sidecar rather than in
the maths.

### Fixed

1. **`tools/quality/capture/gen_synthetic.py`** writes a version 2 pose log
   carrying `fov_deg`, `fov_rad`, `eye`, `fps`, and a `convention` block
   naming all eleven conventions explicitly.
2. **`ref/tools/nxv-enc.cpp`** reads `fov_deg` from the sidecar; `--fov` still
   overrides it; a `convention.id` it does not implement is **refused** rather
   than assumed; and when it must fall back to 95x95 it now says so on stderr
   instead of silently proceeding.
3. **`docs/WARP.md` section 2.1** states the eleven conventions and the sidecar
   schema normatively. They were previously derivable from `homography.cpp` and
   from nothing else.
4. **`tests/ref/test_warp_convention.cpp`** (`ctest -R ref.warp_convention`)
   pins the first warped frame of a pure-rotation pair above a floor, on
   band-limited ground truth, end to end through the real encoder and decoder.
   Verified to catch a conjugated quaternion, a `(w,x,y,z)` component order and
   a swapped `fov_up`/`fov_down`, each of which drops at least one row from
   50-58 dB into 25-36 dB.

Version 1 pose logs still work. `corpus/` is unmodified, so `vr-turn-256` and
`vr-mixed-512` still carry version 1 sidecars and encode exactly as before,
with the assumption now printed.

### Not fixed, deliberately

`gen_synthetic.py` should supersample its render and use a panorama at 4x the
eye's angular resolution, as `warpsim` does — section 4 prices that at 7 to 14
dB of ground-truth fidelity. That is a change to what the corpus *is*, not a
convention fix, and every published number on that material would have to be
regenerated behind it. It is written down here as the recommendation and left
to whoever owns the corpus.

## 6. What this means for the paper's core bet

The bet is that pose warping replaces motion search. Three things follow.

1. **`ref/RESULTS-inter.md`'s 24.40 dB does not measure the predictor.** It
   measures `gen_synthetic.py`'s ground truth, to within 0.02 dB. The
   Phase 2 kill test verdict (`FAIL`, +160.70 % BD-rate) is unaffected — it is a
   BD-rate against x265 on the same frames, and x265 sees the same aliasing —
   but the *diagnosis* in section 4 of that document, "the predictor is not
   accurate enough for the rate at which the codec wants to lean on it", is
   now only partly supportable. On this material the predictor is exact and
   the material is not predictable. On band-limited material the same predictor
   is at 56 dB.

2. **The chain decay is still real and still unexplained by aliasing.** Section
   2's rows 2 to 5 show `ref` falling 5.5 dB below a re-warp of the true
   previous frame within four frames. Aliasing sets the *level*; repeated
   resampling sets the *slope*, and the slope is the predictor's. Paper 2.11
   item 2's conclusion — the refresh rate must rise — survives this audit
   intact. The finding here is only that the starting point was mismeasured.

3. **Neither harness can settle the bet, and the reason is now quantified.**
   `warp/RESULTS.md` says its absolute figures are a lower bound because its
   panorama is not perfectly band-limited; this audit says `gen_synthetic.py`'s
   are a much looser lower bound for the same reason, 7 to 14 dB looser.
   Real rendered VR content has engine antialiasing and TAA, which is to say it
   sits nearer the `ref.warp_convention` end of this range than the
   `vr-mixed-1024` end — and it also has translation, parallax, disocclusion and
   specular motion, which sit the other way and which neither harness has at
   all. The 56 dB is not a prediction for real captures either. It is the
   statement that **the predictor has headroom that this corpus has been
   hiding**, and that `corpus/README.md`'s empty `wivrn-capture` class is now
   the only remaining way to find out how much of it survives contact with real
   content.

## 7. Reproducing

```sh
cmake -S . -B build-ref && cmake --build build-ref -j4
ctest --test-dir build-ref -R 'ref\.warp_convention' -V
```

The section 2 and 4 tables come from a scratch harness rather than a
checked-in tool; the two things worth keeping from it are in the ctest above
and in section 5's fixes. To reproduce section 2 directly: encode
`vr-mixed-1024` with `--lossless --inter on --intra-period 1000000
--skip-thresh 100000` and a `--skip-map` of all-zero for frame 0 and all-one
after, decode, and compare against an independent float evaluation of
`K R_{N-1}^T R_N K^-1` written from `synth.py` rather than from
`homography.cpp`.
