# The reference encoder's tile decision

For a second encoder porting it. Everything here is encoder-side: **nothing in
this document changes how a stream decodes**, and a port that disagrees with it
produces a legal stream that is merely worse. The point of writing it down is
that "worse" has been hard to attribute.

Source of truth is `ref/src/codec_impl.inc`, the `decide` lambda inside
`nxvc_encoder_encode_frame`. Where this document and the code disagree, the
code is right and this document is a bug.

---

## 0. Read this first: the tile decision is not where the atlas is won

The reported symptom — *"the GPU encoder never skips under motion, so its atlas
is worth nothing at mid speed"* — is real, and the tile decision is **not** the
main cause. Measured on the vrroom corpus at QP 26, same content, same encoder,
only the mode policy differing:

| mid, 26.2 deg/s | skip % | bytes/frame | PSNR | seam |
|---|---|---|---|---|
| integer decision, **all-ATLAS** | 73.7 % | 11573 | 37.26 | 2.60 |
| integer decision, **D=8 mode switch** | 87.8 % | **6302** | **37.71** | 1.88 |

**The same tile decision, with 13.12.11's per-frame mode switch in front of it,
is +0.45 dB at 46 % fewer bytes.** No change to the tile decision comes close to
that. An encoder that is in `all-ATLAS` mode at 26 deg/s is answering the wrong
question well: at that velocity the atlas's own reference is contaminated (the
cross-tile gather of 13.12.4 across a mosaic of capture times), so *every* tile
decision made against it is made against a bad predictor, and coding more is the
correct local response to a bad predictor.

Port 13.12.11 first. The rest of this document is still worth having, but it is
the second-order term.

---

## 1. Where each measured quantity comes from

| symbol | what it is | where |
|---|---|---|
| `q` | quantiser step, **Q4**, `kQStep[qp]` | `ref/src/common.h:57` |
| `npix` | luma samples in the tile, `64*64 >> (2*res_level)` | geometry |
| `sse_skip` | squared error of the WARP_SKIP predictor against the **source**, luma only | `sse_pred_i64(probe.pl[0])` after `predict_tile` with the tile's stored vector |
| `sad_skip` | the same, absolute error | `sad_pred(probe.pl[0])` |
| `drift[t]` | mean squared error **per luma sample** of the encoder's *client shadow* against the source, measured on the frame just encoded | `tile_drift_shadow()`; the shadow is what the client displays, and `nxvc_encoder_set_received_tiles()` replays concealment into it, so loss is included |
| `age_since_intra[t]` | frames since this position last coded an `INTRA` | encoder state |
| `corner_disp` | max `|d|` at the tile's four corners under its composed `C`, **Q6 luma samples** | `atlas_corner_disp()`; the same function 13.12.11.1's mode trigger uses |
| `warp_mad_q8` | mean absolute residual of the warped predictor, Q8, reported for **every** tile with a reference | `sad_pred * 256 / npix` |
| `atlas_disp_*_q6` | corner displacement across the atlas, **1/64 sample**, over valid non-static entries after this frame's advance | `nxvc_encode_stats`; printed by `nxv-enc --stats` as `disp: max/mean` |

`probe` is built by `predict_tile()` through `wm_for(t, eye)`, which under
`ATLAS` is **the tile's own atlas entry's `C`**, not the frame matrix. That is
13.12.4 and it is the single most important difference from a picture-model
encoder: two tiles in the same frame predict through different matrices.

---

## 2. The decision, in evaluation order

Integers unless marked. `>>` is arithmetic. Every early return is final.

```
decide(t, eye, col, row):

  # ---- 0. no reference at all (13.12.4, per TILE under the atlas)
  if not tile_has_ref(t):                  return INTRA
      # tile_has_ref = the atlas entry at t is valid.  A position never coded,
      # or whose composed warp left the envelope, has no reference.

  q      = kQStep[qp]                      # Q4
  npix   = 64*64 >> (2*res_level)

  # ---- 1. the hard INTRA cap (loss recovery; PAPER 2.6).  Absolute: nothing
  #         below may defer it.
  if drift_refresh:
      if age_since_intra[t] >= intra_period:            return INTRA
  else:
      if ((tile*2654435761 >> 8) + frame) % intra_period == 0:  return INTRA

  # ---- 2. the DRIFT GATE.  Removes the free option; does not force INTRA.
  skip_forbidden = false
  if drift_refresh:
      # float form:    drift[t] > (q/16)^2/12 * (G_q8/256)
      # integer form:  drift_sse[t] * 786432 > q*q * G_q8 * npix
      skip_forbidden = drift_gate_exceeded(t)

  # ---- 3. the DISPLACEMENT BOUND (off by default: atlas_skip_margin == 0)
  if atlas and atlas_skip_margin != 0 and not skip_forbidden:
      if corner_disp(t) > (atlas_skip_margin << 6):
          skip_forbidden = true            # also takes NEAR_SKIP away

  # ---- 4. externally forced skips (rate control; loses to 1 and 2)
  force_skip = not alpha and not skip_forbidden and skip_map[t]

  # ---- 5. build the WARP_SKIP probe and measure it
  probe = predict_tile(t, mode=WARP_SKIP, mv=state[t].last_mv, through C_t)
  sse_skip = sse_pred(probe);  sad_skip = sad_pred(probe)
  warp_mad_q8 = (sad_skip*256 + npix/2) / npix

  if inter_int_decision:  return decide_integer(...)     # section 3
  else:                   return decide_float(...)       # section 4
```

**Order matters and is part of the decision.** A port that evaluates the drift
gate before the intra cap will refresh late; one that lets `skip_map` beat the
cap will lose a tile for good on a dropped frame.

---

## 3. `decide_integer` — the ADR-0028 path (what a GPU runs)

```
  # 3a. the early-out: skip when the predictor is already under the
  #     quantiser's own noise floor.  thr = skip_thresh or 256 (Q8).
  #     float:   sse_skip/npix <= (q/16)^2/12 * thr/256
  #     integer: sse_skip * 786432 <= q*q * thr * npix
  if force_skip or (not skip_forbidden and not alpha and
                    sse_skip * 786432 <= q*q*thr*npix):
      return WARP_SKIP

  # 3b. lambda, SAD domain, Q8
  lam_q8   = (int_lambda_q8 or 45) * q / 16
  bits_vec = 8 * (kTileHeaderBytes + 2)          # = 8 * (8+2) = 80

  # 3c. the coded vectors: STATIC_MV then WARP_MV, in that order.
  #     THE ORDER IS THE DECISION -- an exact tie goes to the mode searched
  #     first, on both sides.  Integer pel, SAD only, |d| <= 31 samples.
  for mode in [STATIC_MV, WARP_MV][:int_coded_vectors]:
      sad = best integer-pel SAD over the search pattern
      cost = sad + ((lam_q8 * bits_vec) >> 8)
      keep the minimum; remember best_coded_sad

  # 3d. the skip, charged for the error it LEAVES IN THE REFERENCE
  if not alpha and not skip_forbidden:
      excess    = max(0, sad_skip - best_coded_sad)     # 0 if no coded cand
      cost_skip = sad_skip + (kIntSkipPersist-1)*excess + ((lam_q8*1) >> 8)
      if cost_skip <= best_cost:  best = WARP_SKIP      # <= : ties go to skip

  # 3e. the INTRA fallback -- never costed against the others, because costing
  #     it needs a coded tile's rate, which is the quantity a GPU cannot
  #     reproduce.  The rolling refresh (step 1) is what bounds staleness.
  if best is INTRA or best_pred_sad*256 > int_intra_mad_q8 * npix:
      return INTRA
  return best
```

`kIntSkipPersist = 4`. The excess term is not decoration: without it, measured
on the ADR-0028 clip, the decision gave up **2.7 dB (36.62 -> 33.91)** to save
15 % of the rate, because skip won on tiles whose error then sat in the
reference for the rest of the sequence. The *excess* form is what keeps the
repeated-frame case free — a skip whose error merely repeats the error a coded
tile would have made costs nothing extra.

**This path never emits `NEAR_SKIP` and rarely emits `INTRA`** (measured below:
0 near-skips a frame against the float path's 22-32, and 0.0-2.4 intra tiles
against 2.4-18.4). It compensates with `STATIC_MV`, which is cheap in the SAD
domain — 119 tiles a frame at mid in `all-ATLAS` against the float path's 15.

---

## 4. `decide_float` — the reference path

Same shape, four differences, all of which need a rate that only exists after a
candidate has been quantised and its symbol histogram priced with `log2`:

1. Candidates are scored `D + lambda*R` in the **SSE** domain with the real
   rate, not SAD + lambda*bits.
2. `NEAR_SKIP` is a candidate (13.9): a skipped tile with a nine-byte
   block-mean correction.
3. `INTRA` **is** costed against the others.
4. The skip's charge is `d_skip + (kSkipPersist-1)*excess + lambda` with
   `kSkipPersist = 4.0` and `excess = max(0, d_skip - d_ref)` in SSE.

`kSkipPersist` and `kIntSkipPersist` are the same constant (4) in two domains.

---

## 5. Floating point, and the integer form I would accept

Only three quantities in the *gates* are `double`, and all three are the same
shape: a mean error against a multiple of the quantiser noise floor
`qstep^2/12`, `qstep = q/16`. Cleared of divisions each becomes an exact i64
comparison, and **786432 = 256 * 12 * 256** in every one:

| gate | float form | integer form I accept |
|---|---|---|
| skip early-out | `sse_skip/npix <= (q/16)^2/12 * thr/256` | `sse_skip * 786432 <= q*q*thr*npix` |
| drift gate | `drift[t] > (q/16)^2/12 * G_q8/256` | `drift_sse[t] * 786432 > q*q*G_q8*npix` |
| intra fallback | `sad/npix > mad_q8/256` | `sad * 256 > mad_q8 * npix` |

Ranges, so a port can pick its width: the left side of the first reaches
`2.66e8 * 786432 = 2.1e14`, the right `3200^2 * 65535 * 4096`; both fit i64,
neither fits i32.

`G_q8` is `drift_gate_q8` or **1024** (the default multiplier 4.0 in Q8).
`thr` is `skip_thresh` or **256** (1.0 in Q8). `mad_q8` is `int_intra_mad_q8`
or **2304** (a MAD of 9). Each of those three is a `0 means use the built-in`
field, so a zeroed config gets the documented default — **except
`int_coded_vectors`, where 0 means "search nothing" and is a real setting.**
`nxv-enc` initialises it to 2; `nxvc_config_default` does not.

**The scoring is a different matter.** `decide_float`'s `D + lambda*R` cannot
be made integer, because `R` is a `log2` of a symbol histogram; that is the
whole content of ADR-0028 and why `decide_integer` exists as a separate rule
rather than as a fixed-point transcription of the same one. Do not try to
integerise `decide_float`; port `decide_integer`.

**The measured difference between the two decisions**, vrroom, QP 26, D=8:

| fixture | float PSNR / bytes | integer PSNR / bytes | delta |
|---|---|---|---|
| still | 41.28 / 150 | 41.28 / **125** | **0.00 dB, -17 % bytes** |
| objmotion-still | 40.27 / 2800 | 39.77 / 3205 | -0.50 dB, +14 % bytes |
| rest | 38.92 / 1777 | 39.17 / 2408 | +0.25 dB, +36 % bytes |
| mid | 37.71 / 7366 | 37.71 / 6302 | **0.00 dB, -14 % bytes** |
| fast | 38.21 / 6566 | 37.95 / 5753 | -0.26 dB, -12 % bytes |
| objmotion | 38.74 / 3693 | 38.85 / 4483 | +0.11 dB, +21 % bytes |

**Under the mode switch the integer decision is not the weaker one.** At 26
deg/s it reaches the float path's PSNR to two decimals for 14 % fewer bytes.
The gap the report describes is a mode-policy gap, not a decision gap.

---

## 6. Constants

| name | value | config override |
|---|---|---|
| `intra_period` | 180 (2 s at 90 Hz) | `intra_period` |
| drift-gate multiplier | 4.0, i.e. `G_q8` 1024 | `drift_gate_q8` |
| `skip_thresh` | 256 (Q8, = 1.0) | `skip_thresh` |
| `kSkipPersist` / `kIntSkipPersist` | 4 | — |
| `int_lambda_q8` | 45 | `int_lambda_q8` |
| `bits_vec` | 80 bits = `8*(8+2)` | — |
| `int_intra_mad_q8` | 2304 (MAD 9) | `int_intra_mad_q8` |
| `int_coded_vectors` | 2 (STATIC_MV, WARP_MV) — **`nxvc_config_default` leaves this 0**; `nxv-enc` sets 2. A port that zero-initialises the config searches NO coded vectors and can only skip or go intra | `int_coded_vectors` |
| integer-pel search range | `|d| <= 31` samples (s8 quarter-pel) | `mv_range` |
| `atlas_skip_margin` | 0 (off) | `atlas_skip_margin` |
| refresh stagger | `(tile * 2654435761) >> 8` | — |

---

## 7. Expected output, vrroom, QP 26, D=8 mode switch

`--eyes 2 --atlas on --row-present on --atlas-picture-disp 8`, 578 tiles
(17x17 per eye, two eyes), 32 frames, averaged over frames 1..31. Frame 0 is
all-intra and excluded. "decoder warps" is what the *decoder* warps: on an
`ATLAS` frame only the coded inter tiles, on a `PICTURE` frame every tile that
is not `INTRA`.

**Float decision (the reference):**

| fixture | skip % | near | intra | warp_mv | static | warps/f | PICTURE % | B/f | PSNR | seam |
|---|---|---|---|---|---|---|---|---|---|---|
| **still** | **100.0** | 1.1 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | **150** | 41.28 | 3.266 |
| **objmotion-still** | 95.0 | 4.5 | 4.1 | 21.5 | 3.1 | 24.6 | 0.0 | 2800 | 40.27 | 3.362 |
| rest | 98.5 | 31.2 | 2.4 | 3.9 | 2.5 | 41.7 | 6.5 | 1777 | 38.92 | **1.891** |
| mid | 89.0 | 31.5 | 17.0 | 30.5 | 15.8 | 294.4 | 48.4 | 7366 | 37.71 | 1.990 |
| fast | 91.0 | 32.1 | 18.4 | 19.8 | 13.9 | 559.6 | 100.0 | 6566 | 38.21 | 2.071 |
| objmotion | 93.8 | 21.9 | 5.7 | 18.8 | 11.5 | 64.8 | 6.5 | 3693 | 38.74 | **1.972** |

**Integer decision (`--int-decision on`), same everything else:**

| fixture | skip % | near | intra | warp_mv | static | warps/f | PICTURE % | B/f | PSNR | seam |
|---|---|---|---|---|---|---|---|---|---|---|
| **still** | **100.0** | 0 | 0.0 | 0.0 | 0.0 | **0.0** | 0.0 | **125** | 41.28 | 3.266 |
| **objmotion-still** | 95.2 | 0 | 0.7 | 0.1 | 26.9 | 27.0 | 0.0 | 3205 | 39.77 | 3.322 |
| rest | 95.1 | 0 | 0.0 | 5.2 | 23.4 | 63.0 | 6.5 | 2408 | 39.17 | 1.970 |
| mid | 87.8 | 0 | 0.4 | 34.3 | 35.9 | 318.4 | 48.4 | 6302 | 37.71 | 1.879 |
| fast | 88.9 | 0 | 2.4 | 35.5 | 26.4 | 575.6 | 100.0 | 5753 | 37.95 | 1.902 |
| objmotion | 91.3 | 0 | 0.3 | 8.3 | 41.4 | 83.4 | 6.5 | 4483 | 38.85 | 1.950 |

On `still` the two decisions agree exactly where it matters — 100.0 % skip,
zero warps, 41.28 dB — and the integer path is **25 B/frame cheaper (125
against 150)** for the trivial reason that it has no `NEAR_SKIP` to spend: it
sits six bytes above the 119 B structural floor, the float path about thirty.
That is the one row where "the integer decision is worse" is not merely
unproven but false in both columns at once, and it is the row to port first.

`objmotion-still` is where the substitution shows: the integer path answers the
walking meshes with **26.9 `STATIC_MV` tiles and 0.1 `WARP_MV`** where the float
path uses 21.5 `WARP_MV` and 3.1 `STATIC_MV`, and pays **0.50 dB for 14 % more
bytes** (39.77 / 3205 against 40.27 / 2800). Independent object motion is the
case the SAD-domain vector search handles worst, because a mesh crossing a tile
is not a translation of it and `STATIC_MV` is the cheapest wrong answer.

**And in `all-ATLAS` (`--atlas-picture-disp 0`), which is the reported case:**

| fixture | decision | skip % | static | B/f | PSNR | seam |
|---|---|---|---|---|---|---|
| mid | float | 83.6 | 14.7 | 9780 | 36.62 | 3.197 |
| mid | integer | 73.7 | 119.3 | 11573 | 37.26 | 2.600 |
| fast | float | 78.7 | 16.4 | 13843 | 32.65 | 4.571 |
| fast | integer | 75.7 | 76.1 | 12955 | 31.94 | 3.985 |

**`still` is the check a port should run first**, because it is the only row
with an unambiguous right answer: a seated head moves the atlas's tile corners
by at most **0.016 samples**, so every tile's prediction is exact, every tile
skips, and the decoder warps **nothing**. 100.0 % skip / 0.0 warps at 41.28 dB
for a stereo 1088x1088 pair, in **150 B a frame on the float decision and 125
on the integer one** — the whole difference being the float path's ~1.1
`NEAR_SKIP` tiles a frame, which buy nothing here. A port that codes anything here
has a bug in the skip early-out (section 3a) or in the drift gate, and the
failure will be invisible on the moving clips.

`objmotion-still` isolates what independent object motion costs on its own:
the same still head, two walking meshes, **2800 B/f against still's 150** --
so the meshes cost about 2650 B a frame and 21.5 `WARP_MV` tiles, and nothing
else in the frame is disturbed by them.

**The 119 B/frame floor.** At QP 34 and 40 the `still` clip converges to
119 B/f on all three modes, which is the structural minimum of a stereo frame
with `row_present`: 40 bytes of frame header, 72 of `warp_ext()` (36 an eye),
5 of `row_present` bitmap (34 row structures), and two bytes of slack. 85.7
kbit/s at 90 Hz for a 1088x1088 pair with nothing happening in it. That is the
number to compare an idle stream against, and it is set by tool bit 32 rather
than by the refresh policy.

The **PICTURE %** column is the one to check first: if a port reads 0 % at mid
and fast, it has not got 13.12.11 and everything else in the table is a
comparison of two encoders answering different questions.

`--stats` prints the per-frame mode histogram these averages come from, so a
port can be checked frame for frame rather than on a sequence average — which
hides a decision that is wrong on half the tiles in both directions:

```
  modes: skip 512 (near 25)  intra 18  warp_mv 33  static_mv 15  -> decoder warps 48  [ATLAS frame]
  disp: max 2.891  mean 1.641 samples  (46/26 sixteenths) over 578 entries
```

The `disp` line is the **accumulated** displacement of each atlas entry since
it last landed, not a per-frame delta, so on a clip that is not re-coding it
grows frame by frame. On `rest` it reaches 0.97, 1.94, 2.89, 3.83 samples over
the first four frames; on `still` it stays at **0.016**. That difference is the
whole reason `still` exists: `rest` was named for a head at rest and is not
one.

**Regenerate:**

```
python3 tools/quality/capture/gen_vrroom.py --out nx-scratch/fixtures/vrroom
python3 nx-scratch/atlasprice/encdec.py            # float, D=8
python3 nx-scratch/atlasprice/encdec_int.py        # integer, D=8
python3 nx-scratch/atlasprice/encdec_int_atlas.py  # integer, all-ATLAS
python3 nx-scratch/atlasprice/encdec_still.py      # float, D=8, still rows
python3 nx-scratch/atlasprice/encdec_still_int.py  # integer, D=8, still rows
```

Seam ratio is defined in docs/GALLERY.md and reported for every visual result;
`near` is a **subset** of `skip`, not a separate class, and does not enter the
total.
