# Annex D — Inter-path decisions

**Normative.** This annex closes the open issues of Annex C that block Phase 2
(inter prediction) or that sit on the `docs/SYNTAX.md` / `docs/TRANSPORT.md`
boundary. Every decision here is binding on the clauses of this specification
and on the component documents named in D-20.

Annex C states the problems. This annex states the answers, with the byte and
bit layouts an implementer needs and the reason each alternative was rejected.
Where a decision changes a component document, the change is listed in D-22 and
was applied in the same pass that wrote this annex.

**Scope.** D-1 to D-9 are the substance: what the inter path carries and how it
is coded. D-10 to D-19 close the smaller conflicts that would otherwise force a
Phase 2 implementer to guess. D-20 assigns every disputed field to exactly one
owning document. D-21 lists the conformance vectors Phase 2 must add.

## D.0 Summary

| # | Decision | Closes |
|---|---|---|
| D-1 | The homography travels in a fixed 36-byte-per-eye `warp_ext()` immediately after the frame header, gated by `warp_present` | C-4, C-2 |
| D-2 | One 26-byte pose layout, the integer one, owned by `docs/TRANSPORT.md` | C-6 |
| D-3 | One picture per eye; the transport's tile grid is the eye pair; `skip_bitmap` stays 64 bits | C-3 |
| D-4 | The STEREO disparity is a 16-bit unsigned field in the tile's optional area, 12 bits used | C-21, C-24 |
| D-5 | Version 1 is **bilinear only**. Catmull-Rom becomes tool bit 23, undefined in v1 | C-7 |
| D-6 | `res_level == 3` is reserved and rejected everywhere, transport included | C-5 |
| D-7 | Tile corners are **derived** from the frame matrix; no corner record is transmitted | C-15 (part) |
| D-8 | Motion vectors are **absolute**, not deltas | C-11 |
| D-9 | The decoder-side per-tile prediction state is defined; it is a different object from the transport's receiver record | C-15 |
| D-10 | `ref_slots` is a bitmask and MUST equal `1 << (frame_number mod 4)` | C-8 |
| D-11 | `frame_id == frame_number` | C-12 |
| D-12 | `ref_sel` is authoritative; `ref_delta` is an advisory copy; both are constrained for INTRA and STEREO | C-16, C-22 |
| D-13 | `qp_delta` is authoritative; `dir_qp` is an advisory copy | C-13 |
| D-14 | `skip_bitmap` is authoritative; the directory's skip entry is an advisory copy | C-25 |
| D-15 | `dir_len` bounds a **fragment**, not a tile; the level asserts bytes per tile | C-9 |
| D-16 | Version 1 is 8-bit only; tool bit 14 is undefined in v1 and MUST be rejected | C-14 |
| D-17 | Version 1 has no foveation syntax, so the STEREO/foveation interaction cannot arise | C-26 |
| D-18 | The inverse-transform shifts are **7 then 13** | C-20 |
| D-19 | Capability bits and tool bits are orthogonal; one coupling is defined | C-18 |
| D-20 | Field ownership map | C-1..C-30 boundary set |
| D-21 | Phase 2 conformance vectors | C-19 (part) |

Issues left open on purpose: C-10 and C-30 (hybrid, `[pending HYBRID.md]`),
C-17 (tool bits 15–19, correctly refused by a v1 decoder), C-19 in full (the
vectors must be *generated*, not merely listed), C-27 (level limits), C-28
(an editorial correction owned by `docs/SYNTAX.md`, listed in the change list).

---


> Editorial correction (2026-09-04, after implementation): `warp_present` is
> `frame_flags` bit 3, not 2 (bit 2 is layered directional intra, shipped in
> minor 3), and `FILTER_CATMULL_ROM` is tool bit 23, not 20 (bit 20 is `WM_ID`).
> The text below has been updated; SYNTAX.md appendix A item 52 is authoritative.

## D-1 — The homography travels in a frame-header extension

*Closes C-4 (blocking) and C-2 (blocking).*

### The decision

A new syntax structure `warp_ext()` carries the quantised per-eye homography.
It sits **immediately after the 40-byte frame header and before
`quant_matrices()`**, and is present if and only if the new `frame_flags` bit
`warp_present` is set.

```
frame unit:
    frame_header()          40 bytes, unchanged
    warp_ext()              36 * eyes bytes, iff warp_present
    quant_matrices()        128 bytes, iff quant_matrix == 255
    table_set(k)            120 bytes each, per tables_present
    tile rows ...
```

### Why not the alternatives

* **Widening the frame header.** The header is a fixed 40 bytes with every byte
  assigned and `frame_reserved` is one byte. Growing it to 112 bytes for a
  quantity that is absent from every Phase 1 and every all-intra frame costs 72
  bytes on frames that cannot use them, and it would move `frame_bytes` and
  every offset in `docs/SYNTAX.md` 3.1. A conditional structure *after* the
  header leaves Phase 1 frames byte-identical to today.
* **A TLV in the extension area.** Clause 4.3 places TLV records after the
  **stream** header, once per stream. The homography is per frame and per eye,
  so the extension area cannot carry it at all. Clause 4.3 additionally states
  that version 1 defines no mandatory TLV type — anything a decoder must
  understand is gated by `tools`. A mandatory per-frame TLV would contradict
  both statements.
* **The tile-row header (band header).** The matrix is constant over the frame.
  Putting it in the row header replicates it `eyes * rows` times — 68 copies,
  4896 bytes per frame at the v1 configuration — to buy loss tolerance that the
  transport already provides by replicating the frame header into the first
  datagram of every band [PAPER 6.7, TRANSPORT 3.3].
* **Deriving it from the pose bytes.** Rejected by clause 3.4: the derivation of
  `docs/WARP.md` 4 needs `tan` and a matrix inverse. The decoder performs no
  floating-point arithmetic. This is why the quantised matrix, and not the pose,
  is the thing on the wire.

### Layout

`warp_ext()` is `36 * eyes` bytes: one 36-byte record per eye, in ascending eye
order (eye 0 first). Each record is nine little-endian signed 32-bit integers.

| off | size | element | format |
|---|---|---|---|
| 0 | 4 | `h00` | Q10.21 |
| 4 | 4 | `h01` | Q10.21 |
| 8 | 4 | `h02` | Q10.21 |
| 12 | 4 | `h10` | Q10.21 |
| 16 | 4 | `h11` | Q10.21 |
| 20 | 4 | `h12` | Q10.21 |
| 24 | 4 | `h20` | Q2.29 |
| 28 | 4 | `h21` | Q2.29 |
| 32 | 4 | `h22` | Q2.29, MUST be `0x20000000` |

Rows 0 and 1 are **Q10.21** (range ±1024.0, resolution 2^-21); row 2 is
**Q2.29** (range ±4.0, resolution 2^-29). This is the format of
`docs/WARP.md` 3 and it is the answer to C-2: the paper's Q8.24 overflows
`int32` by about seven bits at any streamed width, as `docs/WARP.md` 3 and
`docs/STEREO.md` 5 demonstrate independently, and `docs/STEREO.md`'s common
Q10.21 cannot resolve the perspective row, whose entries are of order 5e-5 and
would carry ten significant bits. The split-by-row format is the only one of the
three that holds both ends.

`h22` is normalised and not free; it is nevertheless transmitted so that the
record is the paper's nine `int32` and a decoder loads the matrix with one
uniform copy. A decoder MUST reject `h22 != 0x20000000`.

### Origin

The origin `(ox, oy)` is **not transmitted**. It is derived:

```
ox = width  >> 1
oy = height >> 1
```

both being luma sample counts per eye. The half-sample convention and the
centring are folded into the matrix by the encoder [WARP.md 2, 4], so the
decoder subtracts `ox`/`oy`, runs the matrix, and adds them back, and needs no
other geometric parameter. This resolves the "nine `int32` plus an origin"
signature of `warp/include/nxvc/warp.h` against the paper's nine `int32`: the
API takes an origin because it is a library entry point, the bitstream does not
carry one because it is derivable.

### Presence

`frame_flags` gains bit 3, `warp_present`, and `frame_flags_reserved` shrinks
from 6 bits to 5:

| bit | element |
|---|---|
| 0 | `tile_map_reset` |
| 1 | `stereo_enable` |
| 2 | `warp_present` |
| 3–7 | `frame_flags_reserved`, MUST be 0 |

* `warp_present == 1` requires the `WARP` tool bit (bit 11).
* A tile with `mode == WARP_SKIP` or `mode == WARP_MV` requires
  `warp_present == 1` in the frame that contains it. A decoder MUST reject a
  frame that violates this.
* `warp_present` MAY be 1 in a frame that contains no warped tile. An encoder
  that has the matrix is free to emit it unconditionally.
* `mode == STATIC_MV` and `mode == STEREO` do not read the matrix
  [WARP.md 1, 10; STEREO.md 4] and do not require `warp_present`.

### Constraints (a decoder MUST reject a stream that violates any of these)

1. `h22 == 0x20000000`.
2. Every entry `h` satisfies `-2^30 <= h <= 2^30` (`kEntryMax`, `docs/WARP.md` 3).
3. At each of the four picture corners `(cx, cy)` with
   `cx in {-ox, width - ox}` and `cy in {-oy, height - oy}`, the denominator
   `den = h20*cx + h21*cy + h22`, accumulated in 64 bits, fits `int32` and
   satisfies `2^28 <= den < 2^30`.
4. `warp_ext()` is present exactly when `warp_present` is set, and
   `frame_bytes` accounts for its `36 * eyes` bytes.

Constraint 3 bounds the whole picture because `den` is affine in `(cx, cy)`
[WARP.md 3]. It is what licenses the 32-iteration restoring divide of
clause 6.7 to use a `uint32` remainder. A decoder that accepts a matrix
violating it still produces defined, identical output on every implementation
(the saturation path of `docs/WARP.md` 6) — but it is a malformed stream and
rejection is the specified behaviour.

### Cost

72 bytes per stereo frame, 6.5 kB/s at 90 Hz, 52 kbit/s. Against the 150 Mbit/s
working point that is 0.00003% of the stream. The replication into the first
datagram of each band multiplies it by 6 and it remains negligible.

---

## D-2 — One pose layout, owned by the transport

*Closes C-6.*

The bitstream's 26-byte `pose` [SYNTAX 3.2] and the transport's 26-byte
frame/pose header [TRANSPORT 3.3] are **the same structure**. There is exactly
one 26-byte pose layout in the format, it is the integer one, and
**`docs/TRANSPORT.md` 3.3 owns it**. It is named `pose_header`.

| off | size | field | format |
|---|---|---|---|
| 0 | 2 | `pose_seq` | u16, index into the client's 2 s pose ring |
| 2 | 2 | `quat_x` | s16, Q15 |
| 4 | 2 | `quat_y` | s16, Q15 |
| 6 | 2 | `quat_z` | s16, Q15 |
| 8 | 2 | `quat_w` | s16, Q15 |
| 10 | 4 | `pos_x` | s32, millimetres × 256 (Q8) |
| 14 | 4 | `pos_y` | s32, millimetres × 256 (Q8) |
| 18 | 4 | `pos_z` | s32, millimetres × 256 (Q8) |
| 22 | 4 | `render_finish_ts` | u32, server clock, microseconds |

Total 26 bytes. Q15 encodes ±1.0 over `[-32768, 32767]`; a component of
magnitude 1.0 is encoded as 32767.

### Why the integer layout wins

* The float layout (7 × `binary16` + 3 × `binary32`) puts IEEE bit patterns in
  a document set whose clause 3.3 forbids floating point on every normative
  path. The field is opaque, so this is not a correctness violation — but it
  makes the one structure the two documents share the only place in the format
  where a float appears, for no gain.
* The integer layout is the one a shipped library already writes and reads
  (`transport/`), and it is the one PAPER 6.7's replication argument is about.
* It carries `pose_seq`, which the client needs to identify the pose in its own
  ring, and `render_finish_ts`, which the transport reads for telemetry
  [TRANSPORT 3.3]. The float layout carries neither, so adopting it would
  require re-adding both and the structure would no longer be 26 bytes.

### What is lost, and why that is acceptable

The float layout carried three `binary16` angular-velocity components. They are
dropped. Angular velocity is a **client-side** quantity: the client holds its
own pose ring at full rate, `pose_seq` identifies the entry, and the client
differentiates its own ring more accurately than three half-precision values on
the wire ever could. Nothing in the decoding process reads it.

### Standing

The pose remains **opaque to the decoding process**: carried, hashed and
compared byte-wise, never interpreted [SYNTAX 3.2 decision 5, clause 3.4]. The
decision here is only that there is one layout rather than two.

`docs/SYNTAX.md` 3.2 references this layout; it does not restate it. The
frame/pose header replicated into the first datagram of each band and the
`pose` field of a stored frame header are byte-identical, which is what makes
PAPER 6.7's replication argument true rather than merely plausible.

---

## D-3 — One picture per eye; the transport grid is the pair

*Closes C-3 (blocking).*

### The mapping

The two documents were measuring different things, and the missing statement is
this one:

* **A picture is one eye.** `width` and `height` in the stream header are per
  eye. A stereo frame contains `eyes` pictures, not one picture of double
  width.
* **The transport's tile grid spans the eye pair.** `cols` in
  `nxt::StreamConfig` counts the columns of both eyes side by side.

```
cols_per_eye = ceil(width  / 64)
rows         = ceil(height / 64)
cols         = eyes * cols_per_eye          // the transport's cols
```

A decoder MUST reject a configuration in which the transport's `cols` is not
`eyes * cols_per_eye` or its `rows` is not `ceil(height / 64)`.

At the v1 target configuration `width = height = 2160`, `eyes = 2`:
`cols_per_eye = 34`, `cols = 68`, `rows = 34`, `tiles_per_frame = 2312` —
exactly the table of [TRANSPORT 1]. **The transport's headline configuration is
legal.** Nothing needs widening; what was missing was the sentence above.

### Linear tile index

```
tile_first = row * cols + eye * cols_per_eye + tile_index
```

and inversely, from a linear index `n`:

```
row        = n / cols
col        = n % cols
eye        = col / cols_per_eye
tile_index = col % cols_per_eye
```

`tile_index` is the bitstream's in-row index [SYNTAX 4.1] and `tile_first` is
the transport's linear index [TRANSPORT 1]. The `eye` bits of the tile header
MUST agree with the `eye` derived above; disagreement makes the tile
UNDECODABLE.

### `skip_bitmap` stays 64 bits

`skip_bitmap` is `f(64)` and covers one tile row **of one eye**. The
constraint `ceil(width / 64) <= 64` therefore binds `cols_per_eye`, not `cols`,
and is satisfied for every legal `width` up to the syntax maximum of 4096. It
never binds in practice and is retained as a structural check.

Two `u64` per row, or a per-eye-pair bitmap, were both considered and rejected:
they would make the tile-row header 20 bytes instead of 12 to encode a
constraint that is already unreachable, and they would break the one-row-header-
per-picture-row correspondence that the band pipeline relies on.

### Tile-row header order

A frame contains `eyes * rows` tile-row headers, ordered **row-major,
eye-minor**:

```
for (row = 0; row < rows; row++)
    for (eye = 0; eye < eyes; eye++)
        tile_row_header()   then its tiles
```

`row_index` MUST equal `row`; the eye is positional and is not a field of the
row header. This order is load-bearing in two places:

1. It matches the transport's linear index, so a tile run is a contiguous range
   of linear indices *and* a contiguous range of bitstream bytes.
2. It puts the whole left-eye row ahead of the right-eye row of the same index,
   which is precisely the ordering `docs/STEREO.md` 6.1 requires for a STEREO
   tile's three-left-tile dependency to be satisfiable. The dispatch rule and
   the bitstream order are the same rule.

---

## D-4 — The STEREO disparity is a 16-bit field in the tile's optional area

*Closes C-21 (blocking) and C-24.*

### The decision

For `mode == STEREO` the tile's optional area carries one unsigned `disparity`
field in place of `mv_x`/`mv_y`:

```
tile_optional()                                        Descriptor
    if (mv_present) {
        if (mode == STEREO)
            disparity                                  f(16)
        else {
            mv_x                                       s(8)
            mv_y                                       s(8)
        }
    }
    if (alpha_mode == 1)
        alpha_value                                    f(8)
```

`disparity` is one little-endian `u16`:

| bits | meaning |
|---|---|
| 11:0 | unsigned horizontal disparity in quarter samples, 0 to 4095 |
| 15:12 | reserved, MUST be 0 |

Range 0 to 1023.75 samples, resolution 1/4 sample. The source position in the
decoded first eye is

```
x_src_q6 = ((x << 2) + disparity) << 4          // Q.6, quarter-pel promoted
y_src_q6 = y << 6
```

that is, the left-eye source lies **to the right** of the right-eye position by
`disparity` quarter samples [STEREO 1, 2.1].

Constraints: `mode == STEREO` requires `mv_present == 1`; `disparity` bits
15:12 MUST be 0; `mode == STEREO` requires the `STEREO` tool bit and
`stereo_enable`; a left-eye tile (`eye == 0`) MUST NOT carry `mode == STEREO`.

### Why 12 bits and not Exp-Golomb in the payload

`docs/STEREO.md` 2.3 decided on an unsigned Exp-Golomb code in the tile
substream. That decision is superseded here, for three reasons.

1. **It changes the coding-unit list of clause 6.6.3 and therefore the lane
   schedule.** The interleaved rANS schedule is the single most delicate part
   of the format and the only part with a lane-order dependency. Adding a
   mode-conditional bypass symbol at the head of a tile's unit list, for one
   value per tile in one mode of five, is a large change to the most fragile
   clause for the smallest possible payload.
2. **It would make parsing depend on prediction mode inside the entropy
   stream.** The tile's optional area is already byte-aligned, already
   mode-independent in position, and already where the vector lives. Keeping
   the disparity there preserves the property that a tile header can be parsed
   without starting the entropy decoder.
3. **The saving is not there.** Exp-Golomb on a typical 60-sample (240-count)
   disparity costs 15 bits against this field's 16. `docs/STEREO.md` 6.1
   measures the whole refinement — both axes — at 1.8% of a tile's bits, so the
   difference between the two codings is under a tenth of a percent of the
   tile.

The 12-bit width is chosen against `docs/STEREO.md` 2.3's own numbers:
`f * IPD / z` is 60 samples at 1 m, about 200 at 30 cm and about 200 at the
2048-px extrapolation of the experiment's 102-sample maximum. 1023.75 samples
is five times the measured worst case and covers `z` down to about 6 cm, which
is inside the near clip plane of every headset. The field cannot overflow on
real content.

### The vertical component

There is no vertical field. `docs/STEREO.md` 6.1 makes the horizontal-only
constraint load-bearing — it is what bounds a right-eye tile's dependency to
three left-eye tiles of the same row and hence what makes the interleaved
dispatch correct — and calls a downward component "forbidden". C-24 observes
that the syntax imposed no such constraint.

Removing the field is a stronger fix than adding a constraint: a nonzero
vertical disparity is **not expressible**, so a stream cannot violate the rule,
and a row-pipelined decoder cannot be deadlocked by a malformed one. The four
reserved bits 15:12 are where a version 2 signed non-positive `dy` would go,
which is the extension `docs/STEREO.md` 6.1 identifies as recovering most of the
lost gain (69% of the experiment's non-zero vertical choices were upward).

### Size

Two bytes, exactly as `mv_x` + `mv_y`. No structure changes size, no offset in
any other document moves, and the transport's directory-balance arithmetic is
untouched.

---

## D-5 — Version 1 is bilinear only; Catmull-Rom is tool bit 23

*Closes C-7 (blocking).*

### The decision

1. **`profile` remains informative and acquires no normative role.** It never
   selects decoder behaviour.
2. **Tool bit 23 is `FILTER_CATMULL_ROM`.** When set, the interpolation filter
   of clause 6.7.5 is the 16-phase 4-tap Catmull-Rom table of Annex A.4. When
   clear, it is bilinear.
3. **Tool bit 23 is not defined for version 1.** A version 1 decoder MUST reject
   a stream that sets it, exactly as it rejects bits 15 to 19. **Every
   conforming version 1 stream is therefore bilinear**, and the filter choice is
   not a decoder decision at all.

Bit 20 is the first previously-reserved bit (clause 8.4 reserved 20 to 63), so
nothing is renumbered. Bits 21 to 63 remain reserved.

### Why bilinear for v1

* **The measurement.** `docs/ERRATA.md` (2026-09-04, section 2.2 / 2.11)
  records that Catmull-Rom is **within 0.05 dB of bilinear on a single step**
  and buys about 2 dB only on long warp chains. The same row records that the
  chains measure 28.9 / 26.7 / 25.0 dB, which means the per-tile refresh rate
  has to rise regardless — and a higher refresh rate is exactly what shortens
  the chains that were the only place Catmull-Rom paid.
* **The cost is not small.** Sixteen taps against four is 4× the reference
  fetches per predicted sample, on a target (Adreno, Vulkan 1.1) whose decode
  budget is 4 ms per frame [PAPER 2.11, 3.2.5]. Paying that for 0.05 dB on the
  common case is the wrong trade at v1.
* **It removes the last normative role of `profile`,** which is what made C-7 a
  blocking issue: two decoders reading the same bitstream could legitimately
  produce different pictures. With the filter fixed by a tool bit that v1
  refuses, there is exactly one legal predicted sample for every v1 bitstream.

### Standing of the Catmull-Rom table

Annex A.4's table is **normative for tool bit 23** and unused by version 1. It
stays in the specification, ratified by `docs/WARP.md` 9, so that the v2 bit has
a defined meaning the day it is enabled rather than a table to be re-derived.
The bilinear weights need no table: `16 - f` and `f` per axis, product over 256,
one rounding [WARP.md 9].

`docs/STEREO.md` 5 says STEREO "reuses the 4-tap Catmull-Rom table; bilinear in
the Lite profile". Under this decision STEREO reuses whatever clause 6.7.5
selects, which in v1 is bilinear in every profile. See D-22.

---

## D-6 — `res_level == 3` is reserved and rejected everywhere

*Closes C-5.*

**Reserved-and-reject wins, in both documents.**

* Tile header: `res_level == 3` is reserved and a decoder MUST reject it
  (unchanged from clause 4.7.1).
* Tile directory: `dir_res_level == 3` MUST NOT be sent. A receiver that finds
  it MUST treat the tile as UNDECODABLE (clause 7.4), not attempt a DC-plane
  reconstruction.

The transport's reading was an over-interpretation, and this specification says
so with a reason rather than a preference: **step 4 of the degradation ladder
[PAPER 4.6.1] needs no new `res_level` value.** "Code only the DC plane" is
`res_level == 2` with a coding-unit list in which only the DC unit has
`cbf == 1`. That is expressible today, it costs fewer bits than a 16×16
residual, and it reconstructs through the existing upsampling kernel of
clause 6.5 with no new path. A fourth resolution value would add a decoder
branch for something the encoder can already say.

The consequence for the rate controller is that ladder step 4 is a QP and
coefficient-budget decision, not a `res_level` decision. `docs/RATECONTROL.md`
is informative and its ladder description needs the same correction; it is
listed in the change list.

---

## D-7 — Tile corners are derived, never transmitted

*Closes the tile-record half of C-15; ratifies `docs/ERRATA.md` on PAPER 3.2.3 vs 2.10.*

**The bitstream carries no per-tile corner record.** The four source-coordinate
corners of a tile are derived on the decoder from the frame's `warp_ext()`
matrix by the process of clause 6.7.2 — two 64-bit accumulations and two
restoring divides per corner, eight divides per 64×64 tile [WARP.md 6].

Paper 3.2.3's Pass A alternative — four corner displacements per tile as Q4
`int16` — is rejected on arithmetic, not on taste: the corner coordinate is Q.6
with a clamp at ±8192 pel, which needs **20 bits**, and Q4 `int16` holds
neither the range nor the resolution [WARP.md 11 limitation 2, ERRATA
3.2.3 vs 2.10]. Widening it to Q6 `int32` would put 32 bytes per tile in the
bitstream — 74 kB per stereo frame at the v1 configuration, against 72 bytes for
the matrix — to save eight divides per tile on a path that already amortises
them over 4096 pixels.

A decoder MAY cache the derived corners per tile as an implementation matter.
If it does, each component needs at least 20 signed bits; `int32` is the natural
storage. This is not observable in the bitstream and is not a conformance
requirement.

---

## D-8 — Motion vectors are absolute

*Closes C-11.*

`mv_x` and `mv_y` are the tile's motion vector itself, in quarter samples (Q.2),
range `[-32, +31.75]` samples. They are **not** a delta from the tile's stored
previous vector.

[PAPER 2.3] describes a signed Exp-Golomb delta in the tile's own substream.
That reading is rejected:

* **It would couple parsing to reference state.** A delta must be added to the
  stored vector of the same tile position in a previous frame. A tile header
  could then not be parsed without the decoder's per-tile state being correct,
  which destroys the property the whole transport design rests on — that a tile
  is an independently decodable bitstream and a datagram is only a loss unit
  [PAPER 6.1, TRANSPORT 3]. After a concealed frame the stored vector may
  differ between encoder and decoder in exactly the cases where the format most
  needs to recover.
* **It would move the vector into the entropy payload**, with the same lane
  schedule cost argued in D-4.
* **The saving is 1.8% of a tile's bits** on `docs/STEREO.md` 6.1's measurement
  of the whole refinement cost, and less than that for the delta versus absolute
  difference alone.

The stored vector `last_mv` therefore exists only for `WARP_SKIP` and for
concealment (D-9), never for parsing.

`docs/WARP.md` 8 is consistent with this: the vector is "a *displacement in the
reference picture*, not a correction to the matrix", added after interpolation.

---

## D-9 — The decoder-side per-tile prediction state

*Closes C-15.*

There are **two** per-tile records and they are different objects with different
owners. C-15 exists because they were compared as if they were one.

### The transport's receiver record — 4 bytes, owned by `docs/TRANSPORT.md` 7.3

`pose_seq` (16 bits), `age` (8 bits, saturating), `state` (2 bits: EMPTY,
DECODED, CONCEALED, UNDECODABLE), `late`, `recovered`. Per tile position per
ring slot. It answers "what do I hold and how old is it". It is unchanged by
this annex.

### The decoder's prediction state — 6 bytes, owned by this specification

Per tile position **per eye**, not per ring slot, because it is a running
history rather than a property of a stored frame:

| off | size | field | meaning |
|---|---|---|---|
| 0 | 2 | `last_mv_x` | s16, quarter samples |
| 2 | 2 | `last_mv_y` | s16, quarter samples |
| 4 | 2 | `last_disp` | u16, quarter samples, 12 bits used |

6 bytes × 2312 tile positions = 13.9 kB at the v1 configuration.

`last_mv` and `last_disp` are separate fields because a tile may alternate
between `STEREO` and `WARP_MV` across frames and the two vectors mean different
things — a 60-sample disparity is not a motion vector and predicting one from
the other is a large error in the first frame after every switch
[STEREO 9]. This is the "per-mode-class" requirement, made concrete.

### Update rules, applied after a tile is reconstructed

| `mode` | `last_mv` | `last_disp` |
|---|---|---|
| `WARP_MV` | set to `(mv_x, mv_y)` | unchanged |
| `WARP_SKIP` | unchanged (it consumed it) | unchanged |
| `STATIC_MV` | unchanged | unchanged |
| `INTRA` | set to `(0, 0)` | set to 0 |
| `STEREO` | unchanged | set to `disparity` |

`STATIC_MV` does not update `last_mv` [PAPER 2.10]: its vector is a displacement
against an unwarped reference, and `WARP_SKIP` and concealment apply the stored
vector *after* the warp. Storing a `STATIC_MV` vector would conceal from the
wrong place. `INTRA` clears the state because after a refresh there is no motion
history and a stale vector is worse than zero.

The whole state is cleared to zero when `tile_map_reset` is set.

Concealment (clause 6.11) runs the `WARP_SKIP` predictor with `last_mv` and no
residual. It is deterministic, so the encoder replays it exactly on its shadow
[PAPER 2.7, TRANSPORT D10]. `last_disp` is never used for concealment: a
concealed right-eye tile conceals temporally, from its own eye's previous frame,
because the left eye of the *current* frame may itself be missing.

`docs/PAPER.md` 2.6's 16-byte record and `docs/STEREO.md` 9's 20-byte variant
are descriptions of an encoder-side structure. `held_frame_id`,
`age_since_intra` and `concealed_count` are encoder shadow bookkeeping; the
decoder's transport record already carries `age` and `state`. Neither is
normative for a decoder.

---

## D-10 — `ref_slots`

*Closes C-8.*

`ref_slots` is a **bitmask** of the four reference ring slots that this frame
overwrites, bit `s` for slot `s`.

In version 1 it MUST equal `1 << (frame_number mod 4)`. A decoder MUST reject
any other value.

The transport addresses the ring by `frame_id mod 4` [TRANSPORT 7.1] and the
ring has four slots [PAPER 6.6]. Making the bitstream field agree by
construction means a stored file is self-contained and self-checking without
inventing a second slot-allocation policy, and it makes the field a cheap
consistency check rather than dead weight. The mask form is retained rather than
collapsing the field to an index so that a version 2 which writes more than one
slot per frame — a frame that is simultaneously a reference and a long-term
anchor — does not need a new element.

---

## D-11 — `frame_id == frame_number`

*Closes C-12.*

The transport's `frame_id` [TRANSPORT 2] and the bitstream's `frame_number`
[SYNTAX 3.1] are the same 16-bit counter with the same wrap. They MUST be
equal. A receiver whose datagram header `frame_id` does not match the
`frame_number` of the frame header it carries, or of the frame the tile belongs
to, MUST treat the datagram as inconsistent and discard it (clause 7.2).

This also fixes the reference of D-10: the slot the frame overwrites is the same
slot under both names.

---

## D-12 — `ref_sel` is authoritative; `ref_delta` is an advisory copy

*Closes C-16 and C-22.*

* **`ref_sel`** (tile header, 2 bits) selects the reference frame for the
  decoding process. `ref_sel = d` selects frame `N - 1 - d` for `d` in 0..2.
  Value 3 is reserved and MUST be rejected.
* **`ref_delta`** (tile directory, 2 bits) is a copy of the same quantity for
  the receiver's reference bookkeeping, with the additional value 3 meaning
  **"no temporal reference"**.
* For `mode == INTRA` and `mode == STEREO`, `ref_sel` MUST be 0 and **is
  ignored by the decoding process**; the corresponding `ref_delta` MUST be 3.
* Otherwise `ref_delta` MUST equal `ref_sel`.
* If they disagree, the decoding process uses `ref_sel`, and the receiver MUST
  mark the tile UNDECODABLE (clause 7.4) because it cannot account for the tile
  in its reference model.

`docs/STEREO.md` 9's "`ref_delta` is not coded for a STEREO tile" is precise
about the transport's intent and imprecise about the bitstream: the two bits of
`word1` are always present. Constraining them to zero and declaring them ignored
says the same thing in a form the syntax can express, and gives a decoder a
check rather than a don't-care. That is C-22.

Value 3 exists in `ref_delta` and not in `ref_sel` because the directory is the
receiver's view, where "this tile needs no previous frame" is actionable
information, while the bitstream already says intra in `mode`.

---

## D-13 — `qp_delta` is authoritative; `dir_qp` is an advisory copy

*Closes C-13.*

The tile's quantiser is `clamp(base_qp + qp_delta, 0, 63)` from the bitstream.
`dir_qp` MUST equal that value. It exists so a receiver can act on a tile's
quality — retransmit priority, telemetry, concealment choice — before parsing
the tile. If they disagree the decoding process uses the bitstream value and the
receiver SHOULD count the datagram as an integrity fault; it is not required to
discard it, because `dir_qp` never affects decoded samples.

## D-14 — `skip_bitmap` is authoritative

*Closes C-25.*

Skip is stated twice by design: `skip_bitmap` in the tile-row header is the
bitstream's statement, and a directory entry with `dir_len == 0` and
`dir_mode == WARP_SKIP` is the transport's. They MUST agree:

* if bit `i` of `skip_bitmap` is set, no directory entry for that tile may carry
  `dir_len != 0`;
* if a directory entry has `dir_len == 0`, the tile's `skip_bitmap` bit MUST be
  set.

On disagreement the decoding process follows `skip_bitmap` and the receiver
marks the tile UNDECODABLE. The redundancy is worth keeping: the bitmap is what
makes a skipped tile cost one bit, and the directory entry is what lets a
receiver account for the tile without holding the row header.

## D-15 — `dir_len` bounds a fragment, not a tile

*Closes C-9.*

The three "maximum tile size" numbers are limits on three different things:

| Limit | Value | What it bounds |
|---|---|---|
| `payload_len` | 65535 | the tile's entropy payload in the **bitstream** |
| `dir_len` | 4095 | one **fragment** in one datagram |
| `max_tile_bytes` | `run_payload_budget - 4` | one **unfragmented** tile |

Normatively:

* An **unfragmented** tile MUST NOT exceed `max_tile_bytes`: 1356 bytes at a
  1400-byte MTU without FEC, 1312 with FEC [TRANSPORT 5, D5].
* A **fragmented** tile (legal only with `LOSSLESS` and `CAP_FRAGMENT`) is split
  into 2 to 4 fragments, each with its own directory entry whose `dir_len` is
  that fragment's length. Each fragment MUST satisfy
  `dir_len <= min(4095, max_tile_bytes)`.
* `payload_len` in the tile header is the whole tile and is unconstrained by
  the transport beyond the above; a stored file may carry tiles no live path can
  send.

The reachable maximum at a 1400-byte MTU is therefore `4 * 1312 = 5248` bytes.
**The ~12 kB worst-case lossless 64×64 4:4:4 tile of [PAPER 1.8] is not
transportable at a 1400-byte MTU under any fragmentation scheme.** It requires
`CAP_JUMBO`: at an 8900-byte MTU `max_tile_bytes` is 8844 and one fragment
carries it. This is a real constraint on the Pro profile and it belongs in a
level, which is what clause 8.3.1 item 4 was asking for.

## D-16 — Version 1 is 8-bit

*Closes C-14.*

Tool bit 14 (`BITDEPTH10`) is **not defined for version 1**. A version 1 decoder
MUST reject a stream that sets it, and MUST reject `bit_depth == 10`.

C-14's problem was that `bit_depth == 10` was legal while no 10-bit sample
domain, quantiser scaling or clamp existed. The alternative to defining all
three was to make the value unreachable, and that is the cheaper and more honest
answer for v1: the paper's "the same table with coefficients scaled by 4"
[PAPER 1.5] is an assertion no document has verified, and shipping a
signallable mode with an unverified table is how two implementations diverge.

The 10-bit path is a version 2 item and gets the tables then. Note that
`docs/WARP.md` 9 already states its intermediate bounds for 10-bit input
(`|acc| < 2^23`), so the warp is ready for it; the transform and quantiser are
not.

## D-17 — Foveation does not interact with STEREO in version 1

*Closes C-26.*

Version 1 defines no foveation syntax. `docs/RATECONTROL.md`'s foveation is an
encoder-side sample-density and QP choice that produces an ordinary v1
bitstream, and [PAPER 6.8] places the Phase 2 predictor at the coded resolution
against a full-resolution reference with no foveation map [WARP.md 11
limitation 3]. There is therefore no per-eye foveation grid in a conforming v1
stream and the two-step lookup `docs/STEREO.md` 10.1 describes cannot arise.

This closes C-26 for version 1 and **reopens with the foveation syntax**, which
must state the STEREO interaction in the same change that introduces it. It is
recorded here rather than deleted so that the requirement is attached to the
feature.

## D-18 — The inverse transform shifts by 7 then 13

*Closes C-20 (blocking).*

The pass-1 shift is **7** and the pass-2 shift is **13**, total 20, unity gain.
This is `docs/SYNTAX.md` 6.3 and `ref/`, it is what every digest in
`tests/vectors/vectors.md5` was generated against, and `docs/ERRATA.md` already
declares `docs/SYNTAX.md` authoritative while stating 8 and 12 in the same row.

The 8/12 split is not wrong in total — both sum to 20 and both give unity gain —
but it places the pass-1 intermediate one bit apart, so the two round
differently and produce different decoded samples from the same bitstream. Only
one can be normative. The one with conformance vectors behind it wins; changing
it would invalidate every digest in the suite for no gain.

`docs/ERRATA.md`'s 1.4 row must be corrected to state 7 and 13 as the
correction rather than as the alternative. It is listed in the change list;
`docs/ERRATA.md` is outside this annex's edit scope.

## D-19 — Capability bits and tool bits are orthogonal

*Closes C-18.*

A `tools` bit gates **bitstream syntax and decoder behaviour** and is enforced
by refusing the stream at the handshake. A `caps` bit gates **wire behaviour**
and is enforced per datagram [TRANSPORT 2.2]. They are independent by design:
the same bitstream must be carriable over a path with or without FEC,
multipath, jumbo frames or fragmentation, and the transport is codec agnostic
[TRANSPORT preamble].

Exactly one coupling is defined, and it is one-directional:

* `CAP_FRAGMENT` MAY be negotiated only for a stream whose `tools` sets
  `LOSSLESS` (bit 5). Fragmentation is legal only for lossless tiles
  [TRANSPORT 3.4], so on a stream with no lossless tiles the capability has no
  meaning.

No other pair is related, and in particular the Pro profile does not imply
`CAP_FRAGMENT`: `profile` is informative (D-5) and cannot gate a transport
negotiation.

---

## D-20 — Field ownership

Every field that appeared in a boundary dispute, and the single document that
owns its definition. "Owns" means: that document defines the layout and the
semantics, every other document references it and does not restate it, and a
change requires a change there first.

| Field | Owner | Referenced by | Note |
|---|---|---|---|
| `pose_header` (26 bytes) | `docs/TRANSPORT.md` 3.3 | `docs/SYNTAX.md` 3.2 as `pose` | D-2 |
| `warp_ext()` / `h00`..`h22` | this specification, clause 4.4.2 | `docs/SYNTAX.md`, `docs/WARP.md` 3 | D-1; formats owned by `docs/WARP.md` 3 |
| `ox`, `oy` | derived, clause 4.4.2 | `docs/WARP.md` 2 | never transmitted |
| interpolation filter selection | `tools` bit 20, clause 8.4 | `docs/WARP.md` 9, `docs/STEREO.md` 5 | D-5 |
| Catmull-Rom tap table | `docs/WARP.md` 9 | Annex A.4 | v2 only |
| `mv_x`, `mv_y` | `docs/SYNTAX.md` 4.1 | clause 5.4 | absolute, D-8 |
| `disparity` | `docs/SYNTAX.md` 4.1 | `docs/STEREO.md` 2.3 | D-4 |
| `skip_bitmap` | `docs/SYNTAX.md` 3.3 | directory skip entry is advisory | D-14 |
| `cols`, `rows`, `cols_per_eye`, linear index | `docs/TRANSPORT.md` 1 | clause 4.2.1 constraint | D-3 |
| `tile_index` (in-row) | `docs/SYNTAX.md` 4.1 | — | distinct from `tile_first` |
| `ref_sel` | `docs/SYNTAX.md` 4.1 | `ref_delta` is the advisory copy | D-12 |
| `ref_delta` | `docs/TRANSPORT.md` 3.1 | clause 7.3 | advisory, value 3 extra |
| `ref_slots` | `docs/SYNTAX.md` 3.1 | clause 5.3 | D-10 |
| `qp_delta` | `docs/SYNTAX.md` 4.1 | `dir_qp` is the advisory copy | D-13 |
| `dir_qp` | `docs/TRANSPORT.md` 3.1 | — | advisory |
| `res_level` | `docs/SYNTAX.md` 4.1 | `dir_res_level` mirrors it exactly | D-6 |
| `frame_number` / `frame_id` | `docs/SYNTAX.md` 3.1 | `docs/TRANSPORT.md` 2 | equal, D-11 |
| per-tile prediction state | this specification, clause 6.10 | — | D-9 |
| per-tile receiver record | `docs/TRANSPORT.md` 7.3 | clause 7.4 | unchanged |
| `payload_len` / `dir_len` / `max_tile_bytes` | see D-15 | — | three different quantities |
| `tools` mask | `docs/SYNTAX.md` 2.2 | clause 8.4 | normative gate |
| `caps` mask | `docs/TRANSPORT.md` 2.2 | clause 8.6 | orthogonal, D-19 |
| `profile`, `level` | `docs/SYNTAX.md` 2 | clause 8.2, 8.3 | informative, no normative role |

---

## D-21 — Conformance vectors Phase 2 must add

C-19 is closed only by generating vectors; this annex fixes what they must
cover, so that clause 9.3.1's "no inter vectors" has a definition to be measured
against. Each entry is one stream with a reference reconstruction digest.

| Vector | Fixes |
|---|---|
| `inter/identity` | identity matrix, `WARP_MV` with zero vector: a bit-exact copy of the reference [WARP.md 10 case 1] |
| `inter/static_mv` | `STATIC_MV` with an aggressive matrix present in `warp_ext()`: the matrix MUST NOT change one output bit [WARP.md 10 case 2] |
| `inter/integer_mv` | `mv = (4*px, 4*py)` under identity equals a shifted tile [WARP.md 10 case 3] |
| `inter/warp_sweep` | matrices across the accepted envelope, including corners at the `kCornerClamp` boundary and `den` near both `kDenMin` and `kDenMax` |
| `inter/warp_border` | tiles straddling all four picture edges, clamp-to-edge per tap |
| `inter/skip` | `skip_bitmap` with `WARP_SKIP` tiles consuming `last_mv`, and the D-9 update rules exercised across four frames |
| `inter/ref_sel` | all three `ref_sel` values against a four-slot ring |
| `inter/stereo` | `eyes == 2`, disparities spanning 0 to 4095 quarter samples, including tiles whose source span clamps at both borders [STEREO 11] |
| `inter/stereo_static_equiv` | a STEREO tile and a `STATIC_MV` tile with the same vector against the same reference produce identical output [STEREO 11] |
| `reject/homography` | `h22 != 2^29`; an entry beyond `kEntryMax`; `den` outside `[2^28, 2^30)` at a picture corner; `warp_present == 0` with a `WARP_MV` tile |
| `reject/inter_syntax` | `mode == 5..7`; `res_level == 3`; `disparity` bits 15:12 nonzero; `mode == STEREO` on `eye == 0`; `ref_sel != 0` on an INTRA tile; `ref_slots != 1 << (frame_number mod 4)` |
| `reject/tools` | tool bits 14, 15–20 set |

The rejection vectors matter more than the positive ones. Clause 4 and clause 5
impose roughly forty MUST-reject conditions and no vector exercises any of them,
so a decoder that accepts every malformed stream currently passes the suite
completely.

---

## D-22 — Spec-reconciliation edits to component documents

These decisions changed statements in documents this annex does not own. The
edits were applied in the same pass, marked *spec reconciliation*, and are
listed here so the owners can see exactly what moved. Nothing else in those
documents was touched.

| Document | Section | Edit | Decision |
|---|---|---|---|
| `docs/WARP.md` | 3 | Note that the nine coefficients travel in `warp_ext()` and that `(ox, oy)` is derived from `width`/`height`, never transmitted | D-1 |
| `docs/WARP.md` | 9 | Note that the filter is selected by tool bit 23 and that version 1 is bilinear only; the Catmull-Rom table is normative for v2 | D-5 |
| `docs/WARP.md` | 11 limitation 1 | Point at D-4: the disparity is a separate 12-bit unsigned field, not the shared MV field | D-4 |
| `docs/WARP.md` | 11 limitation 2 | Resolved: corners are derived; no tile record is transmitted | D-7 |
| `docs/TRANSPORT.md` | 1 | State `cols == eyes * cols_per_eye`, `cols_per_eye = ceil(width/64)`, and the eye-to-picture mapping | D-3 |
| `docs/TRANSPORT.md` | 3.1 | `dir_res_level == 3` MUST NOT be sent; `ref_delta` and `dir_qp` are advisory copies with a defined disagreement rule | D-6, D-12, D-13 |
| `docs/TRANSPORT.md` | 3.3 | Rename to `pose_header`, state that it is the same structure as the bitstream's `pose`, and that `docs/TRANSPORT.md` owns the layout | D-2 |
| `docs/TRANSPORT.md` | 3.4 | `dir_len` bounds a fragment; state the 4×`max_tile_bytes` ceiling and the jumbo requirement for a 12 kB tile | D-15 |
| `docs/STEREO.md` | 2.3 | Supersede the Exp-Golomb decision: the disparity is `f(16)` in the tile's optional area, 12 bits used | D-4 |
| `docs/STEREO.md` | 5 | The filter is bilinear in version 1 in every profile, selected by tool bit 23 | D-5 |
| `docs/STEREO.md` | 9 | `STEREO` is mode **4** by field value; `ref_sel` is present, constrained to 0 and ignored | D-12, C-23 |

Edits required in documents outside this annex's scope, listed for their owners
in `docs/SYNTAX-CHANGES-PHASE2.md`: `docs/SYNTAX.md` in full, `docs/ERRATA.md`
1.4 (state 7/13 as the correction, D-18), `docs/RATECONTROL.md` ladder step 4
(no `res_level == 3`, D-6).
