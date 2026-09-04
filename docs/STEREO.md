# STEREO mode: inter-view prediction of the right eye

Status: design, backed by a CPU experiment. Phase 4 in the roadmap; off in the Lite profile.
Specifies PAPER 2.5 in enough detail to implement, and reconciles it with 2.3 (per-tile
vectors), 2.6 (references), 4.2 (row-band pipelining) and 6.6 (reference epoch).

Companion documents: `stereo/RESULTS.md` (what the experiment measured),
`stereo/PATENT_NOTES.md` (how this differs from 3D-HEVC VSP and MV-HEVC, an FTO scoping note),
`docs/XR_EXT_NXWARP.md` (where the depth that seeds the disparity comes from).

The sampler and the fixed-point warp are shared with the temporal predictor. Where this document
describes them it is describing PAPER 2.2; if `docs/WARP.md` lands with a normative filter table
or a different fixed-point layout, that document wins and this one follows it. The one place this
document knowingly departs from PAPER 2.2 is the scale of the quantised homography, and the reason
is in [Fixed point](#fixed-point) — it is a bug in the paper, not a difference of opinion.

---

## 1. The mode in one paragraph

The two eyes are rendered from the same head pose at the same instant, separated by the IPD along
the head's x axis. For a surface at eye-space depth `z`, the same surface point lands in the left
image exactly `D = f * IPD / z` pixels to the right of where it lands in the right image, and at
the same row. So a right-eye tile can be predicted from the decoded left eye of the same frame by
a **pure horizontal shift**, one value per tile, taken from the tile's plane depth. That shift is
carried in the bitstream in the same field as a motion vector, sampled with the same 1/16-pel
filter, and refined by the same search. STEREO adds no new predictor machinery to the decoder: it
is `WARP_MV` with the reference swapped from `right(N-1)` to `left(N)` and the pose warp switched
off.

---

## 2. Geometry

### 2.1 Why a shift and not a homography

Both eye images at frame N share one rotation `R_N`; only the eye positions differ, by
`t = (IPD, 0, 0)` in head space. The mapping from the right image to the left image induced by a
plane at distance `d` with normal `n` is

    H = K ( R - t n^T / d ) K^-1     with R = I because the eyes share a rotation
      = K ( I - t n^T / d ) K^-1

For a fronto-parallel plane (`n = (0,0,1)`, `d = z`) this collapses exactly to a translation of
`f * IPD / z` in x and zero in y. The residual term for a plane tilted by angle `θ` within the
tile is bounded by `D * (w/2) * tan θ / f` pixels at the tile edge, where `w` is the tile width;
at `D = 30 px` (z = 1 m), `w = 64`, `f = 940` (2048 px over 95 degrees) and a 60-degree tilt that
is 1.7 px — real, but the same magnitude as the per-tile plane approximation the codec already
accepts for the positional warp (PAPER 2.1), and the refinement vector absorbs the mean of it.

**Decision: one horizontal disparity per tile, no per-tile plane parameters, no vertical
component in v1.** This is the same call PAPER 2.1 made for translation-induced parallax, made for
the same reason, and it is what keeps the decoder's STEREO path bit-identical to its `STATIC_MV`
path.

### 2.2 What the projection has to satisfy

`D = f * IPD / z` assumes the two eye projections are **rectified**: identical focal length,
identical principal-point offset relative to the eye, coplanar image planes, no relative rotation.
Streamed VR frames are usually rectified by construction — the runtime renders both eyes with the
same projection and applies the IPD as a pure translation — but three things break it:

- **Asymmetric frusta.** Almost every headset uses per-eye asymmetric projections (the left eye's
  frustum extends further left). That is a principal-point difference, not a rotation: it adds a
  *constant* offset to `D` for every tile of the frame, `Δc = (cx_L - cx_R)` in streamed-pixel
  units, and it is known from the projection matrices. Fold it into the seed:
  `D = f * IPD / z + (cx_L - cx_R)`.
- **Canted displays** (Pico 4 is parallel; some headsets cant the panels by a few degrees). A
  relative rotation between the eyes makes the true mapping a homography and introduces a
  row-dependent vertical term. If `|cant| > 0` the encoder must either rectify before encoding or
  disable STEREO. **v1: STEREO is enabled only when the two eye poses differ by a pure translation
  along the head x axis, within a tolerance of 0.1 degrees of relative rotation.** The encoder
  computes this from the two `XrView` poses each frame and clears a per-frame `stereo_enable` bit
  otherwise. The decoder never has to know why.
- **Per-eye foveated remap.** If the two eyes get different foveation grids (they do, under
  eye tracking — the gaze point is not symmetric), the streamed images are not rectified in
  streamed-pixel space even when the render is rectified in display space. STEREO must therefore
  be computed **in the foveated (streamed) domain with the disparity mapped through both eyes'
  grids**, or restricted to frames where both eyes share a grid. v1 takes the cheap option: the
  encoder disables STEREO for any tile whose left-eye source region crosses a foveation-grid cell
  boundary where the two grids disagree by more than one sample. See [open issues](#10-open-issues).

### 2.3 Per-tile plane depth

Given a depth source (Section 3), the tile's disparity seed is

    D_tile = median over the tile's pixels of ( f * IPD / z(x,y) )   +  (cx_L - cx_R)

rounded to quarter-pel. Median, not mean, and taken over disparity rather than over depth: a tile
straddling a depth edge has a bimodal disparity distribution and the median picks the dominant
surface instead of an average that matches neither. The encoder may subsample to 8x8 of the 64x64
pixels; the experiment used every pixel and the difference is under a quarter-pel on all tested
scenes.

Range: `f * IPD / z` for `f = 940`, `IPD = 64 mm` is 60 px at 1 m, 200 px at 30 cm, 6 px at 10 m.
**The disparity is routinely larger than the codec's ±16 px coarse search range** (PAPER 2.3 step
2) — in the experiment, 37.6 percent of tiles exceeded 16 px and the maximum was 102 px at 1024 px
width, which scales to about 200 px at 2048. Consequences, both required:

1. The disparity seed must be a **seed**, never a search origin of zero. Without depth or a
   coarse disparity search, STEREO simply does not find its reference.
2. The MV field width must accommodate it. PAPER 2.3 gives the vector a ±64 px range at quarter
   pel. That is not enough for near-field content at 2048 px per eye. The STEREO vector must
   therefore be an unsigned disparity in quarter-pel, wide enough for the near field, carried
   separately from the ±64 px signed `WARP_MV` vector. The two never coexist in a tile, so this is
   a reinterpretation of the same bytes by mode, not a new field.

   **Superseded (spec reconciliation, `spec/annex-d-inter-decisions.md` D-4).** This document
   originally decided on an **Exp-Golomb code with no fixed upper bound, in the tile's entropy
   payload**. That is not what the format carries. The disparity is one unsigned little-endian
   `u16` in the tile's byte-aligned optional area, in place of `mv_x`/`mv_y`: bits 11:0 are the
   disparity in quarter samples (0 to 4095, i.e. 0 to 1023.75 samples) and bits 15:12 MUST be 0.
   Five times the measured worst case, the same two bytes the motion vector occupied, and no
   change to the coding-unit list of the lane schedule — which is what the Exp-Golomb form would
   have cost, for under a tenth of a percent of a tile's bits. Bits 15:12 are reserved for the v2
   signed non-positive `dy` of section 6.1.

---

## 3. Where the disparity comes from

Three sources, in preference order. All three are encoder-side; the decoder sees only the final
vector.

1. **Application depth** (`XR_NX_render_hints`, or the existing
   `XR_KHR_composition_layer_depth`), reduced to one plane depth per tile as in 2.3. Cost: a
   reduction pass over the depth buffer, part of the encoder's E0 analysis dispatch, about 0.05 ms
   per eye at 4 Mpix on an RX 580. This is the intended path and the reason
   `docs/XR_EXT_NXWARP.md` exists.
2. **Encoder coarse disparity search**, when no depth is supplied: for each tile, SAD over a
   4x-downsampled tile against the left eye at horizontal offsets 0 to 192 px in steps of 2, then
   the normal refinement. Cost measured in the experiment: 97 candidate positions x 256 samples =
   25 k SAD ops per tile, 50 M per eye-frame at 2048 tiles, under 0.2 ms on a mid-range GPU. In
   the experiment this recovered nearly all of the depth-seeded gain (14.0 percent versus 14.8
   percent of right-eye bits) but was wrong by more than 16 px on 5.3 percent of tiles, all of
   them on repetitive texture (brick, stripes) where the search locks onto the wrong period.
3. **Temporal reuse**: the tile's disparity from frame N-1, held in the per-tile decoder state
   (PAPER 2.6 `last_mv`). The vector is coded as a delta from it, exactly as `WARP_MV` is, so a
   static scene at constant depth costs zero bits for the vector. This is what makes STEREO's side
   information negligible in practice.

The encoder evaluates all available seeds by SATD and takes the best; it never trusts a seed
blindly. Same rule as the engine velocity buffer in PAPER 2.3.

---

## 4. Composition with the pose warp and the per-tile MV

The three predictors compose as follows, and the important word is *not*:

| Mode | Reference | Pose warp applied | Vector means |
|---|---|---|---|
| `WARP_SKIP` / `WARP_MV` | `right(N-1)` | yes | residual motion in pixels |
| `STATIC_MV` | `right(N-1)` | no | motion of head-locked content |
| `STEREO` | `left(N)` | **no** | disparity in pixels |

STEREO does **not** apply the pose homography. The pose warp compensates the *change of view
between two instants*; the left and right eyes of frame N are the same instant, so there is nothing
to compensate. Applying the warp to the inter-view reference would be a bug, not a refinement.

The sampling chain is therefore the shorter one:

    x_src_q6 = ((x + D_int) << 6) + D_frac_q6      (no homography, no corner interpolation)
    x_q4     = (x_src_q6 + 2) >> 2
    pred     = filter16(left_recon, x_q4, y << 4)

with `D` split into integer and quarter-pel parts by the vector decode. The rounding is the same
"add half, shift" used by the warp path, and the filter is the same 16-phase table, so a decoder
implements STEREO by selecting a different reference image and skipping the corner-division step.
Measured decoder cost: strictly less than `WARP_MV` (four divisions and the interior bilinear are
skipped), about 15 ops per pixel in the Full profile against 50.

### 4.1 The disparity vector is not a motion vector

PAPER 2.8 retires WiVRn NX's block matcher and feeds the codec's per-tile MV field into
client-side motion smoothing. **STEREO tiles must be excluded from that field**, the same way
`STATIC_MV` tiles already are. A disparity of 60 px extrapolated as if it were object motion would
tear the right eye apart on the synthesised frame. The per-tile state already carries `mode`; the
extrapolation shader reads it and treats STEREO tiles as "no residual motion known", falling back
to the co-located left-eye tile's vector, which is a real motion vector for the same content.

Similarly, the encoder's smoothness penalty `lambda_s` (PAPER 2.8) must be computed against
neighbouring tiles' *motion* vectors, and a STEREO neighbour contributes nothing to it. Mixing
disparities into that term would bias the field toward physically wrong values.

---

## 5. Fixed point

STEREO reuses, unchanged:

- the 16-phase 1/16-pel filter, whichever `docs/WARP.md` section 9 selects. *(Spec reconciliation,
  `spec/annex-d-inter-decisions.md` D-5: the filter is chosen by tool bit 20 `FILTER_CATMULL_ROM`,
  which **version 1 does not define**, so a v1 stream is bilinear in every profile — not
  Catmull-Rom in Full and bilinear in Lite as written below. `profile` selects nothing.)* The
  4-tap Catmull-Rom table, with integer coefficients over 64 and each row summing to exactly 64 so
  a flat region reproduces exactly, is normative for the v2 bit;
- the quarter-pel vector representation, Q.2;
- the "sum in Q.6, round to Q.4, add half and shift" rounding;
- clamp-to-edge for out-of-frame reads.

It does not use the quantised homography at all. One finding from implementing that homography for
the experiment's temporal predictor belongs here because it is a defect in the current text:

<a name="fixed-point"></a>
**PAPER 2.2 specifies the nine homography coefficients as int32 in Q8.24 with `h22 = 2^24`. That
overflows.** With coordinates measured from the image corner, `h02` is of order `cx` (1024 at 2048
px per eye) and `1024 * 2^24 = 2^34`. Even with coordinates measured from the image centre, `h02`
is of order `f * tan(yaw)`, and `h00` of order 1, so the largest coefficient is about `f` — 940 at
the Pico 4's streamed width — and `940 * 2^24` still exceeds int32 by seven bits. The experiment
uses **centred coordinates and a common Q10.21 scale with `h22 = 2^21`**, which bounds every
coefficient by `2^31 / 2^21 = 1024 > f` and leaves 21 fractional bits (a rotation entry resolved to
5e-7, i.e. 4e-4 px after multiplication by `f` — two orders below the 1/64-pel output). The
alternative is a per-column scale, which costs a shift in the decoder's inner loop for no accuracy
benefit. **This needs to be fixed in PAPER 2.2 / `docs/WARP.md` and is flagged there, not here.**

---

## 6. Dispatch ordering, dependencies and latency

### 6.1 The dependency is one tile row wide, and that is not an accident

Because the disparity is horizontal-only, a right-eye tile at `(tx, ty)` reads a source region
spanning rows `[ty*64 - 2, ty*64 + 65]` — the tile's own rows plus the 4-tap filter's 2-pixel
margin — and columns `[x0 + D - 2, x0 + 63 + D + 2]`. The row extent never leaves the tile row
(the ±2 margin is absorbed because the left eye's *whole* rows 0..ty are already decoded under the
ordering below). The column extent covers **at most three left-eye tiles in the same row**, at
indices `floor((x0 + D - 2)/64)` through `floor((x0 + 65 + D)/64)`.

This is what makes the interleaved dispatch of PAPER 2.5 sufficient rather than merely convenient.
Two rules preserve it:

- **The vertical component of the STEREO vector is zero in v1.** The experiment's unconstrained
  search chose a non-zero vertical component on 22.1 percent of tiles, but the entire refinement
  (both axes) is worth only 1.8 percent of the tile's bits on average, so the constraint is nearly
  free — and 69 percent of those choices were *upward*, into rows the left eye has already
  finished, so a v2 that allows `dy <= 0` recovers most of it without breaking the ordering. A
  downward vertical component would make a right tile depend on a left tile row that has not been
  dispatched, and is forbidden.
- **A right tile may not reference left tiles it cannot prove are present.** See 6.3.

### 6.2 Ordering and its cost

Dispatch order per frame is `L row 0, R row 0, L row 1, R row 1, ...`, which is what PAPER 2.5
already says. Under the row-band pipelining of PAPER 4.2:

- The right eye's row `r` starts one left-row decode after the left eye's row `r`. At 2048x2048
  per eye, 64x64 tiles, 32 rows and a 2.2 ms whole-frame decode budget, one row is about 35 to 40
  us on the target hardware.
- **Total decode time is unchanged**: the added lag is one row at the head of the pipeline, and
  the two eyes were always going to be decoded serially in some order on a single queue. The cost
  is 40 us of *tail* latency for the right eye's last row, against a 12 to 23 ms render-to-photon
  budget: 0.3 percent.
- Transport must not undo it. Left and right tiles of the same row go on the **same path** under
  multipath striping (PAPER 4.8), and the right eye's row `r` datagrams are given a scheduling
  priority no higher than the left eye's row `r`, so a reordering does not park a right row ahead
  of the left row it needs.

If the compositor requires both eyes complete before present (it does), this ordering costs
nothing at all; the right eye finishing 40 us later than it otherwise would is invisible behind
the phase wait.

### 6.3 Loss, and the reference epoch

PAPER 2.5 says a STEREO tile whose left reference has not arrived is treated as lost. That is
right but incomplete, because 6.6 already established that a tile may only reference a *fully
acknowledged neighbourhood*. The rule for STEREO:

- The encoder may choose STEREO for right tile `(tx, ty)` only if all left tiles in its column
  span (6.1) are being sent in the **same frame** and are not themselves concealed in the shadow
  model. Since both eyes' tiles of frame N are sent together, this is a statement about the
  encoder's own optimism, not about acknowledgements: it is choosing to make one tile depend on
  two or three others arriving.
- The client, at the deadline, checks the presence bits of those left tiles. If any is missing,
  the right tile is concealed exactly like any other lost tile — `WARP_SKIP` from `right(N-1)`
  with `last_mv` — and reported missing. No new concealment path.
- The encoder's shadow model replays that concealment deterministically, as for any loss. The one
  new obligation is that the shadow must apply the *left* tiles' loss to the *right* tile's state,
  which means the replay has to process the left eye of a frame before the right eye. It already
  does, because it replays in dispatch order.
- **Loss amplification is the real cost**: one lost left tile can invalidate up to three right
  tiles in the same row (a right tile's span covers three left tiles, and symmetrically a left
  tile is covered by up to three right tiles' spans). At a 1 percent tile loss rate the right
  eye's effective loss rate rises toward 3 percent on STEREO tiles. Mitigation, and the reason the
  mode decision has a rate term for it: the encoder adds `lambda * P_loss * cost_of_concealment` to
  the STEREO RD cost, so under measured loss the mode switches itself off. Below about 0.1 percent
  loss the term is negligible; above 2 percent STEREO stops being chosen. That is the intended
  behaviour and it needs no signalling.

---

## 7. Encoder mode decision

Per right-eye tile, after the normal `WARP_*` / `STATIC_MV` / `INTRA` candidates:

1. **Gate.** Skip STEREO entirely if any of: the frame's `stereo_enable` bit is clear (2.2);
   `P_loss` over the last 8 frames exceeds the threshold (6.3); the tile is in the Lite profile;
   the layer is monoscopic (quad layers, most UI — a head-locked quad has *zero* disparity and
   `STATIC_MV` against `right(N-1)` is strictly better).
2. **Edge guard.** If the source span `[x0 + D - 2, x0 + 65 + D]` leaves the left image, the tile
   is in the **monocular border strip** (Section 8) and STEREO is not offered. Cheap test, and it
   removes the mode's worst failure case before any SAD is computed.
3. **Disocclusion guard.** If depth is available, compute the fraction of the tile's pixels whose
   projected left-eye position lands on a surface at a different depth (the test the experiment
   uses as ground truth: `|z_L(u + D) - z| > max(0.02 z, 1 cm)`). If that fraction exceeds 5
   percent, do not offer STEREO. The experiment's justification: on tiles with more than 20 percent
   disocclusion STEREO won **zero** times out of 18 and cost 43 percent *more* bits than the best
   non-STEREO mode; between 5 and 20 percent it won 14 percent of the time and still cost 22
   percent more. Below 1 percent it won 29 percent of the time and saved bits. Without depth, this
   guard is unavailable and the encoder relies on the RD comparison alone, which is correct but
   spends the search time to find out.
4. **Seed and refine.** Seeds: depth disparity, previous frame's disparity for this tile, coarse
   search result if no depth. Refinement: ±4 px integer (the experiment's range; ±2 loses under
   0.2 percent) then the eight quarter-pel neighbours, horizontal-only in v1.
5. **Decide** by `D + lambda*R` with SATD, against all other modes, exactly as PAPER 2.3 does. No
   special-casing: if STEREO wins it wins.

The mode decision costs the encoder one extra candidate evaluation per right-eye tile, roughly 10
percent of the right eye's search time, and nothing on the left eye.

---

## 8. Image edges and disocclusions

Two distinct failure modes, with two different answers.

### 8.1 The monocular border strip

Because `D > 0` always (the left eye sees everything shifted right), the right eye's **right-hand
border strip of width `D_max`** has no left-eye source at all: that content is outside the left
image. This is not a hole in the prediction — clamp-to-edge still produces samples — it is a region
where the prediction is *meaningless*. Its width is content-dependent (it is the maximum disparity
present at the border, so a near hand at the right edge widens it to 200 px) and it is entirely
predictable from the depth buffer.

Answer: the edge guard in 7.2. These tiles are coded `WARP_*` or `INTRA` like any other. There is
no attempt to extrapolate. Symmetrically, the left eye's left border is monocular, but the left eye
never uses inter-view prediction, so it does not care.

Note the asymmetry with the pose warp, which has the same problem on the leading edge of a head
turn (PAPER 2.2) and answers it the same way: clamp, and let the mode decision pick intra.

### 8.2 Disocclusions at depth edges

Where a near surface occludes a far one, the right eye sees a sliver of the far surface that the
left eye does not — width `D_near - D_far` pixels, which is large exactly where it hurts (a hand at
30 cm in front of a wall at 3 m disoccludes 180 px at 2048 width). Inside such a tile, the
prediction is right for the near surface, right for the far surface outside the sliver, and
completely wrong inside it.

Answers, in order of what v1 does:

- **v1: the disocclusion guard (7.3) plus RD.** Refuse STEREO where the depth buffer says more
  than 5 percent of the tile is disoccluded; below that, let the residual pay for it. This is
  cheap, needs no new decoder behaviour, and the experiment says the crossover is in the right
  place.
- **Rejected: hole detection and fill on the decoder.** This is the 3D-HEVC view-synthesis
  approach (`stereo/PATENT_NOTES.md`), it needs depth at the decoder, it is not
  determinism-friendly, and it costs a second pass. PAPER 2.1 already rejected depth on the decoder
  for the temporal path; there is no reason to reintroduce it for a mode worth 5 percent.
- **v2 candidate: sub-tile disparity segmentation** — two disparities per tile with a coded 1-bit
  mask at 8x8 granularity, which is where MV-HEVC-style partitioning would take this. It doubles
  the side information and it is a bitstream change. Not in v1, and only worth revisiting if the
  measured share of guarded-off tiles is large on real content.

---

## 9. Bitstream and profile impact

- **Minimal new syntax.** `STEREO` is the fifth mode of PAPER 2.3's mode table and its field
  **value is 4**, not 3; "mode 3" above counts ordinally. *(Spec reconciliation,
  `spec/annex-d-inter-decisions.md` D-22 / Annex C C-23.)* The vector bytes are reinterpreted as
  an unsigned `disparity` (Section 2.3); the residual, transform and entropy coding are
  unchanged.
- **Reference selection.** PAPER 6.6 gives every tile a 2-bit `ref_delta` selecting among N-1, N-2,
  N-3 and intra. STEREO consumes no reference value: the mode itself names the reference
  (`left(N)`). *(Spec reconciliation, `spec/annex-d-inter-decisions.md` D-12: the tile header's
  `ref_sel` bits are **present, MUST be 0 and are ignored** — "not coded" is not expressible in a
  fixed-width word — and the transport directory's `ref_delta` MUST be 3, "no temporal
  reference".)* Left-eye tiles never carry mode `STEREO`; an encoder that emits one produces a
  stream a decoder must reject.
- **Per-tile state.** `last_mv` holds the disparity for STEREO tiles. Because a tile can alternate
  between STEREO and `WARP_MV` across frames, and the two vectors mean different things, the
  temporal delta predictor must be **per-mode-class**: predict a disparity from the last disparity,
  a motion vector from the last motion vector. That is one extra 2x16-bit field in the per-tile
  state (16 bytes becomes 20, 160 kB at 8192 tiles — still nothing) and it is required, otherwise
  the first STEREO tile after a `WARP_MV` tile codes a 60 px delta.
- **Profiles.** Lite: off. Full: on, gated by a capability bit and by `stereo_enable` per frame.
  Hybrid (HEVC base): off — the base layer already carries both eyes and the enhancement residual
  is against the base, so there is no inter-view redundancy left to exploit.
- **Foveation.** A STEREO tile's source region must be read through the left eye's foveation grid.
  In v1 the encoder disables STEREO where the two grids disagree (2.2); this is the least
  satisfying part of the design and the largest open issue.

---

## 10. Open issues

1. **Foveation interaction.** Under eye tracking the two eyes have different foveation grids, and
   the naive answer (disable STEREO where they differ) may disable it exactly in the fovea, where
   the bits are. The right answer is probably to compute the disparity in display-pixel units and
   map it through both grids, which makes the decoder's source coordinate a two-step lookup. Not
   designed. **This is the item most likely to reduce the measured gain on a real system.**
2. **Chroma.** The experiment is luma-only. Disparity is a geometric quantity so the chroma
   disparity is the luma disparity halved in 4:2:0, exactly as for motion vectors, and no separate
   search is needed — but this is asserted, not measured.
3. **Alpha and transparency.** A transparent surface has two depths and one disparity. STEREO
   should be gated off by the same stencil bit that raises lambda for alpha regions (PAPER 2.3).
   Untested.
4. **The gain depends on how good the temporal predictor is, and the experiment's temporal
   predictor may be pessimistic.** See `stereo/RESULTS.md` section "Threats to validity". If the
   real encoder's `WARP_MV` is sharper than the experiment's, STEREO's share falls.
5. **Left-eye reconstruction quality.** STEREO predicts from a decoded left eye that has itself
   been through a resample and a quantiser. The experiment measures this (it is the difference
   between the 14.8 percent and 11.0 percent figures) but only for a single-frame chain; a long
   warp chain degrades the left eye further and STEREO with it.
6. **Symmetry.** Nothing forces the *left* eye to be the independent view. Alternating which eye
   is independent per frame would equalise quality and halve the worst-case loss amplification on
   any one eye. It also doubles the state and complicates the shadow replay. Not evaluated.

---

## 11. Verification

`stereo/sim/nxvc-stereosim` is the CPU experiment behind every number above; `stereo/RESULTS.md`
reports it. Tests in `tests/stereo/` (ctest: `stereo.raster`, `stereo.disparity`,
`stereo.determinism`, `stereo.sim_smoke`) cover the renderer's geometry, the `D * z = f * IPD`
relation recovered by search from rendered pairs, the filter table's invariants, the identity and
pure-yaw behaviour of the quantised warp, and bit-exact repeatability of the whole measurement.

When the mode is implemented for real, the conformance obligations are:

- a decoder's STEREO path and its `STATIC_MV` path must produce identical output for the same
  vector and reference (they are the same code with a different reference image);
- the CPU reference decoder and the Vulkan decoder must be bit-exact on a stream containing STEREO
  tiles with disparities spanning 0 to 512 quarter-pels, including tiles whose source span is
  clamped at both borders;
- an encoder must never emit STEREO on a left-eye tile, on a tile whose source span leaves the
  image, or with a non-zero vertical component in v1.
