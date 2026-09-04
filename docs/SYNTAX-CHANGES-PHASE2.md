# SYNTAX.md change list — Phase 2 inter path

**For the owner of `docs/SYNTAX.md`.** This is a field-by-field change list,
written so it can be applied in one pass without re-deriving anything. Every
item cites the decision that produced it in
[`spec/annex-d-inter-decisions.md`](../spec/annex-d-inter-decisions.md), which
carries the rationale, the rejected alternatives and the measurements.

Nothing here was applied to `docs/SYNTAX.md`: that document has an owner and was
being worked on (intra v2 tools) while these decisions were made. The spec
clauses under `spec/` already carry all of it, so the two will disagree until
this list lands.

**Nine of these changes are bitstream-visible.** They are marked **[WIRE]**. The
rest are wording, constraints on values already reserved, or ownership
statements.

---

## 1. Frame header — `warp_present` **[WIRE]**

*Annex D **D-1**. Closes Annex C C-4 (blocking).*

In §3.1's `frame_flags` byte, bit 2 becomes `warp_present` and the reserved
field shrinks from 6 bits to 5.

| bit | before | after |
|---|---|---|
| 0 | `tile_map_reset` | `tile_map_reset` |
| 1 | `stereo_enable` | `stereo_enable` |
| 2 | reserved | **`warp_present`** |
| 3–7 | reserved (6 bits from bit 2) | reserved (5 bits), MUST be 0 |

Semantics to add:

> `warp_present` set means the frame carries a `warp_ext()` structure
> immediately after the frame header. It requires tool bit 11 `WARP`. Every
> tile with `mode == WARP_SKIP` or `mode == WARP_MV` requires it; a frame MAY
> set it and contain no warped tile. `STATIC_MV` and `STEREO` do not read the
> matrix and do not require it.

The 40-byte frame header does **not** change size and no offset in §3.1 moves.

## 2. New structure `warp_ext()` **[WIRE]**

*Annex D **D-1**, with the fixed-point formats from `docs/WARP.md` 3. Closes
Annex C C-4 and C-2, both blocking.*

New sub-clause after §3.1, before the quantisation matrices. Present if and only
if `warp_present` is set. Size `36 * eyes` bytes: one 36-byte record per eye, in
ascending eye order, each nine little-endian `int32`.

| off | field | format |
|---|---|---|
| 0 | `h00` | Q10.21 |
| 4 | `h01` | Q10.21 |
| 8 | `h02` | Q10.21 |
| 12 | `h10` | Q10.21 |
| 16 | `h11` | Q10.21 |
| 20 | `h12` | Q10.21 |
| 24 | `h20` | Q2.29 |
| 28 | `h21` | Q2.29 |
| 32 | `h22` | Q2.29, MUST be `0x20000000` |

Frame-unit order becomes:

```
frame_header()      40 bytes
warp_ext()          36 * eyes,  iff warp_present
quant_matrices()    128 bytes,  iff quant_matrix == 255
table_set(k)        120 bytes each, per tables_present
tile rows ...
```

`frame_bytes` must account for `warp_ext()`.

**Rejection rules** (all four are new MUST-reject conditions):

1. `h22 != 0x20000000`;
2. any entry outside `[-2^30, +2^30]`;
3. at any of the four picture corners `(cx, cy)` with `cx in {-ox, width - ox}`,
   `cy in {-oy, height - oy}`, the denominator `den = h20*cx + h21*cy + h22`
   accumulated in 64 bits does not fit `int32` or falls outside `[2^28, 2^30)`;
4. a `WARP_SKIP` or `WARP_MV` tile in a frame with `warp_present == 0`.

**The origin is not a field.** `(ox, oy) = (width >> 1, height >> 1)`. The
`warp/` API takes an origin because it is a library entry point; the bitstream
derives it.

Note for the reader of §3.1: the TLV extension area cannot carry this. It sits
after the *stream* header, once per stream, and §2.1 states that version 1
defines no mandatory TLV type.

## 3. Frame header — `pose` layout **[WIRE]**

*Annex D **D-2**. Closes Annex C C-6.*

§3.2's pose layout — 7 × `binary16` (quaternion + angular velocity) then
3 × `binary32` position in metres — is **superseded**. The 26 bytes are the
transport's `pose_header`, which `docs/TRANSPORT.md` 3.3 now owns:

| off | size | field |
|---|---|---|
| 0 | 2 | `pose_seq`, u16 |
| 2 | 8 | orientation, 4 × s16 Q15, `(x, y, z, w)` |
| 10 | 12 | position, 3 × s32, millimetres × 256 (Q8) |
| 22 | 4 | `render_finish_ts`, u32 microseconds |

Replace §3.2's layout with a reference to `docs/TRANSPORT.md` 3.3 rather than
restating it. Keep decision 5's statement that the bytes are opaque to the
decoding process — that is unchanged and still correct.

Angular velocity is dropped: it is a client-side quantity, recovered from the
client's own pose ring, which `pose_seq` indexes. The float layout was the only
place in the format where an IEEE bit pattern appeared.

## 4. Frame header — `ref_slots`

*Annex D **D-10**. Closes Annex C C-8.*

Define it: a **bitmask** of the four reference-ring slots the frame overwrites,
bit `s` for slot `s`. In version 1 it MUST equal `1 << (frame_number mod 4)` and
any other value is malformed. This is the transport's `frame_id mod 4` ring
addressing restated so a stored file is self-contained; the mask form is kept so
a v2 frame that writes more than one slot needs no new element.

## 5. Frame header — `frame_number` **[WIRE-adjacent]**

*Annex D **D-11**. Closes Annex C C-12.*

Add: `frame_number` and the transport's `frame_id` are the same 16-bit counter
and MUST be equal. A datagram whose `frame_id` disagrees with the frame it
carries is inconsistent and MUST be discarded.

## 6. Tile-row header — `skip_bitmap` and the eye-to-picture mapping

*Annex D **D-3**. Closes Annex C C-3 (blocking).*

`skip_bitmap` stays 64 bits — no widening. What is missing is a normative
sentence, and it is the fix:

> A **picture is one eye**. `width` and `height` are per eye and a stereo frame
> contains `eyes` pictures. The transport's tile grid spans the eye pair:
> `cols_per_eye = ceil(width / 64)`, `cols = eyes * cols_per_eye`,
> `rows = ceil(height / 64)`. The linear transport index is
> `tile_first = row * cols + eye * cols_per_eye + tile_index`.
>
> `skip_bitmap` covers one tile row of one eye, so `ceil(width / 64) <= 64`
> binds `cols_per_eye`. The transport's `cols = 68` is 34 columns per eye and is
> a legal bitstream.
>
> A frame contains `eyes * rows` tile-row headers, ordered **row-major,
> eye-minor**: for each row, eye 0 then eye 1. The eye is positional and is not
> a field of the row header. This order is what puts a whole left-eye row ahead
> of the right-eye row of the same index, which is what `docs/STEREO.md` 6.1
> needs for a STEREO tile's three-left-tile dependency to be satisfiable.

Also add the tile-header check: a tile's `eye` MUST agree with the eye derived
from its linear index.

## 7. Tile header — `disparity` replaces `mv_x`/`mv_y` for STEREO **[WIRE]**

*Annex D **D-4**. Closes Annex C C-21 (blocking) and C-24.*

§4.1's optional area becomes:

```
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
| 11:0 | unsigned horizontal disparity, quarter samples, 0 to 4095 (0 to 1023.75 samples) |
| 15:12 | reserved, MUST be 0 (v2: a signed non-positive vertical component) |

The source position in the decoded first eye lies `disparity` quarter samples to
the **right** of the tile's own position.

Same two bytes as `mv_x` + `mv_y`, so no structure changes size.

New constraints: `mode == STEREO` requires `mv_present == 1`, `eye == 1` and
`stereo_enable`; `disparity` bits 15:12 MUST be 0.

There is deliberately **no vertical field**. `docs/STEREO.md` 6.1 calls a
downward component "forbidden" and C-24 observed that no constraint enforced it;
removing the field makes the rule structural, so a row-pipelined decoder cannot
be deadlocked by a malformed stream.

This supersedes `docs/STEREO.md` 2.3's Exp-Golomb-in-the-payload decision, whose
cost was a change to the coding-unit list and therefore the lane schedule, for a
saving under a tenth of a percent of a tile.

## 8. Tile header — `mv_x`, `mv_y` are absolute

*Annex D **D-8**. Closes Annex C C-11.*

State it normatively: the coded value is the vector itself, in quarter samples,
**not** a delta from the tile's stored previous vector. `docs/PAPER.md` 2.3's
delta is rejected because it would make parsing a tile header depend on the
decoder's per-tile state and destroy independent tile decodability — the
property the whole transport design rests on.

## 9. Tile header — `ref_sel` for INTRA and STEREO

*Annex D **D-12**. Closes Annex C C-16 and C-22.*

Add:

* `ref_sel == 3` is reserved and MUST be rejected.
* For `mode == INTRA` and `mode == STEREO`, `ref_sel` MUST be 0 and is
  **ignored** by the decoding process; the tile's transport `ref_delta` MUST be
  3 ("no temporal reference").
* Otherwise `ref_delta` MUST equal `ref_sel`.
* `ref_sel` is **authoritative**; `ref_delta` is an advisory copy. On
  disagreement the decoding process uses `ref_sel` and the receiver marks the
  tile UNDECODABLE.

This is what `docs/STEREO.md` 9's "`ref_delta` is not coded for a STEREO tile"
means in a syntax where the two bits of `word1` always exist.

## 10. Tile header — `res_level == 3` stays rejected, in both documents

*Annex D **D-6**. Closes Annex C C-5.*

No change to `docs/SYNTAX.md`'s own rule — it was right. Add the cross-reference
that `docs/TRANSPORT.md` 3.1 no longer assigns it "DC-plane only": the ladder's
step 4 is `res_level == 2` with only the DC unit coded, which needs no fourth
value. `docs/TRANSPORT.md` has been updated (D24).

## 11. Tile header — `qp_delta` is authoritative

*Annex D **D-13**. Closes Annex C C-13.*

Add: the tile QP is `clamp(base_qp + qp_delta, 0, 63)` from the bitstream. The
transport's `dir_qp` MUST equal it and is advisory; on disagreement the decoding
process uses `qp_delta` and the receiver counts an integrity fault without
discarding the tile, since `dir_qp` never affects decoded samples.

## 12. Tile-row header — `skip_bitmap` is authoritative

*Annex D **D-14**. Closes Annex C C-25.*

Add: a transport directory entry with `dir_len == 0` and
`dir_mode == WARP_SKIP` is the transport's statement of the same fact and MUST
agree. If a `skip_bitmap` bit is set, no directory entry for that tile may carry
`dir_len != 0`; if a directory entry has `dir_len == 0`, the bit MUST be set. On
disagreement the decoding process follows `skip_bitmap` and the receiver marks
the tile UNDECODABLE.

## 13. Tool mask — bit 20 `FILTER_CATMULL_ROM` **[WIRE]**

*Annex D **D-5**. Closes Annex C C-7 (blocking).*

Add to §2.2:

| bit | name | meaning |
|---|---|---|
| 20 | `FILTER_CATMULL_ROM` | Catmull-Rom interpolation in the warp instead of bilinear |

Bits 21 to 63 reserved (was 20 to 63), so nothing is renumbered.

**Bit 20 is not defined for version 1** and a version 1 decoder MUST reject a
stream that sets it. Consequently:

* **every conforming version 1 stream is bilinear**, in every profile;
* `profile` acquires **no** normative role and selects nothing — remove any
  statement tying the filter (or anything else) to it;
* the Catmull-Rom tap table stays normative for the v2 bit.

The evidence is `docs/ERRATA.md`'s measurement that Catmull-Rom is within
**0.05 dB** of bilinear on a single step and buys about 2 dB only on long warp
chains, which the same row shows must be shortened by a higher refresh rate
regardless — set against 16 taps per sample rather than 4 on a 4 ms decode
budget.

## 14. Tool mask — bit 14 `BITDEPTH10` becomes reject-in-v1 **[WIRE]**

*Annex D **D-16**. Closes Annex C C-14.*

Add: bit 14 is **not defined for version 1**. A version 1 decoder MUST reject a
stream that sets it and MUST reject `bit_depth == 10`. Version 1 is 8-bit.

This closes C-14 by refusal rather than by inventing a 10-bit sample domain,
`qstep` scaling and clamp that no document has verified. `docs/PAPER.md` 1.5's
"the same table with coefficients scaled by 4" is an assertion, and shipping a
signallable mode on an unverified table is how two implementations diverge.

## 15. §10 — the tile-independence claim

*Annex D **D-22**. Closes Annex C C-28.*

§10 states that nothing in reconstruction depends on any other tile. That was
true for Phase 1 and is false once `STEREO` exists: a right-eye tile depends on
up to three left-eye tiles of the same row. Qualify it:

> Within one eye and one frame, no tile's reconstruction depends on any other
> tile. The single exception is `mode == STEREO`, where a right-eye tile reads
> up to three left-eye tiles of the same row of the same frame
> (`docs/STEREO.md` 6.1). There is no other intra-frame tile dependency in the
> format.

## 16. Per-tile prediction state

*Annex D **D-9**. Closes Annex C C-15.*

If `docs/SYNTAX.md` describes decoder state at all, it should carry this rather
than `docs/PAPER.md` 2.6's 16 bytes. There are two records, not three, and they
are different objects: the transport's 4-byte receiver record
(`docs/TRANSPORT.md` 7.3) and the decoder's 6-byte prediction state per tile
position **per eye**:

| off | size | field |
|---|---|---|
| 0 | 2 | `last_mv_x`, s16 quarter samples |
| 2 | 2 | `last_mv_y`, s16 quarter samples |
| 4 | 2 | `last_disp`, u16 quarter samples, 12 bits used |

Update rules after reconstruction:

| `mode` | `last_mv` | `last_disp` |
|---|---|---|
| `WARP_MV` | `= (mv_x, mv_y)` | unchanged |
| `WARP_SKIP` | unchanged | unchanged |
| `STATIC_MV` | unchanged | unchanged |
| `INTRA` | `= (0, 0)` | `= 0` |
| `STEREO` | unchanged | `= disparity` |

Cleared entirely on `tile_map_reset`. `last_mv` and `last_disp` are separate
because a tile may alternate between `STEREO` and `WARP_MV` and a 60-sample
disparity is not a motion vector. `STATIC_MV` does not update `last_mv` because
its vector displaces an *unwarped* reference while concealment applies the
stored vector after the warp.

## 17. Mode numbering

*Annex D **D-22**. Closes Annex C C-23.*

No change to `docs/SYNTAX.md`, which is right: `STEREO` is field value **4**.
`docs/STEREO.md` 9 has been corrected — it was counting ordinally.

---

# Changes to other documents

## `docs/ERRATA.md` — the 1.4 row

*Annex D **D-18**. Closes Annex C C-20 (blocking).*

The 2026-09-04 / §1.4 row states the correction as **8 then 12** while declaring
`docs/SYNTAX.md` authoritative in the same sentence. `docs/SYNTAX.md` and `ref/`
use **7 then 13**. Both total 20 and both give unity gain, but they place the
pass-1 intermediate one bit apart, so they round differently and produce
different decoded samples from the same bitstream.

**7 and 13 are normative**, because that is what every digest in
`tests/vectors/vectors.md5` was generated against. Reword the row so the
correction *is* 7/13 and the bench's 8/12 is named as the alternative that was
not taken. Do not change `docs/SYNTAX.md`; changing it would invalidate the
whole conformance suite for no gain.

## `docs/RATECONTROL.md` — degradation ladder step 4

*Annex D **D-6**.*

Step 4 of the ladder must not be described as `res_level == 3`. It is
`res_level == 2` with a coefficient budget that codes only the DC unit. The
document is informative, so this is a wording fix with no normative
consequence — but it is the wording that produced C-5.

## Already applied

`docs/WARP.md`, `docs/TRANSPORT.md` and `docs/STEREO.md` were edited in the same
pass, minimally and marked *spec reconciliation*. The exact list is Annex D
**D-22**. In summary: WARP gains the carriage and filter-selection notes and its
limitations 1 and 2 are resolved; TRANSPORT gains the eye-to-picture mapping,
decision D24 (advisory copies, `res_level == 3`, `frame_id == frame_number`,
fragment bounds) and the `pose_header` ownership statement; STEREO's Exp-Golomb
disparity, its Catmull-Rom-in-Full filter statement and its "mode 3" are marked
superseded.
