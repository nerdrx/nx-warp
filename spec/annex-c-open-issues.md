# Annex C — Open issues

Every gap, ambiguity and inter-document conflict found while assembling this
specification. This annex is a deliverable in its own right: the value of
writing a spec against documents that are still being written is exactly this
list.

Each issue names the documents that disagree, states what an implementer cannot
do today, and says what would close it. Nothing here is a complaint about
quality — a design this size producing 29 open issues at this stage is normal.
What matters is that none of them is *silent*.

**Severity**

| | Meaning |
|---|---|
| **BLOCKING** | A conforming decoder cannot be written, or two conforming decoders would produce different pixels |
| **MAJOR** | A conforming decoder can be written for the affected feature only by guessing |
| **MINOR** | Ambiguous or redundant, but an implementer would reach the right answer |

## C.0 Summary

| ID | Severity | One line | Clause |
|---|---|---|---|
| C-4 | BLOCKING | No syntax element carries the homography | 4.4, 6.7 |
| C-20 | BLOCKING | `ERRATA` says inverse-transform shifts 8/12, `SYNTAX` says 7/13 | 6.4.3 |
| C-3 | BLOCKING | 64-bit `skip_bitmap` caps 64 tile columns; transport uses 68 | 4.2.1 |
| C-21 | BLOCKING | STEREO disparity needs ±512 samples and Exp-Golomb; syntax gives two `s(8)` bytes | 5.4, 6.8.4 |
| C-2 | BLOCKING | Three incompatible fixed-point formats for the homography | 3.7, 6.7.2 |
| C-7 | BLOCKING | Interpolation filter is selected only by `profile`, which is informative | 8.2, 6.7.5 |
| C-6 | MAJOR | Two different 26-byte pose layouts | 5.3, 4.9 |
| C-5 | MAJOR | `res_level == 3` reserved in the syntax, "DC-plane" in the transport | 5.4 |
| C-14 | MAJOR | `bit_depth == 10` is legal but 10-bit is specified nowhere | 6.2 |
| C-10 | MAJOR | `wgt`: four weights or five, and no blend formula | 5.4, 6.9 |
| C-15 | MAJOR | Three incompatible per-tile state definitions | 6.10 |
| C-11 | MAJOR | Motion vector: delta or absolute, unstated normatively | 5.4 |
| C-8 | MAJOR | `ref_slots` semantics undefined | 5.3 |
| C-19 | MAJOR | No inter, rejection, custom-table or 10-bit conformance vectors | 9.3.1 |
| C-22 | MAJOR | `ref_sel` "not coded" for STEREO, but the bits are always present | 5.4, 6.8.4 |
| C-24 | MAJOR | STEREO's zero-vertical-disparity rule has no syntax constraint | 6.8.1 |
| C-9 | MAJOR | Three disagreeing maximum tile sizes | 8.3.1 |
| C-30 | MAJOR | Hybrid path unspecified but `layer_type` 1 and 2 are legal | 6.9 |
| C-16 | MINOR | `ref_sel` versus `ref_delta`: which is authoritative | 7.3 |
| C-13 | MINOR | `dir_qp` absolute versus `qp_delta` relative | 5.6 |
| C-12 | MINOR | `frame_id` and `frame_number` never related | 5.6 |
| C-17 | MINOR | Tool bits 15–19 declared, never specified | 8.4 |
| C-18 | MINOR | No relation defined between `caps` bits and tool bits | 8.6 |
| C-23 | MINOR | `STEREO` called "mode 3" ordinally, is mode 4 by value | 5.4 |
| C-25 | MINOR | Two skip signalling mechanisms | 5.4, 7.1 |
| C-26 | MINOR | STEREO and foveation interaction undesigned | 6.8 |
| C-27 | MINOR | No level limits defined at all | 8.3 |
| C-28 | MINOR | "No tile depends on another" is false once STEREO exists | 6.1 |
| C-29 | MINOR | `warp/` cites a `docs/WARP.md` that does not exist | 6.7 |

Counts: **6 blocking, 12 major, 11 minor — 29 open issues.**

Closed during drafting, kept for the record:

| ID | Closed by | Resolution |
|---|---|---|
| C-1 | `docs/SYNTAX.md` 2.2 | `color_space` landed as a *descriptive* element at offset 42, tied to `color_transform` by "`color_space == 3` if and only if `color_transform == 1`". The two do not duplicate: one says what the codec does, the other what the result means |
| C-31 | `docs/RATECONTROL.md` | Landed. Informative, as expected; adds no normative decoder behaviour |

---

## C.1 Blocking issues

### C-4 — No syntax element carries the homography

*Documents:* `docs/SYNTAX.md` 3.1, 3.2 versus `docs/PAPER.md` 2.2 versus
`warp/include/nxvc/warp.h`.

The inter predictor is a per-eye projective transform. The paper puts nine
`int32` — 36 bytes per eye — in the frame header. The frame header as specified
is exactly 40 bytes with no room, and the 26 pose bytes are explicitly opaque
and explicitly never interpreted by the decoding process. **A Phase 2 stream
therefore cannot be decoded from the syntax as it stands.**

*To close:* extend the frame header, or add a mandatory TLV, or define a
derivation of the homography from the opaque pose bytes — which the integer-only
rule forbids, since deriving it needs trigonometry. The first option is the only
one consistent with clause 3.4.

### C-20 — Contradictory inverse-transform shifts

*Documents:* `docs/ERRATA.md` (2026-09-04, section 1.4) versus
`docs/SYNTAX.md` 6.3.

The errata corrects the paper's "7 and 12" to **8 and 12**; `docs/SYNTAX.md`
corrects the same error to **7 and 13**. Both total 20 and both give unity gain,
but they place the pass-1 intermediate one bit apart, so they round differently
and produce **different decoded samples from the same bitstream**. The errata
itself declares `docs/SYNTAX.md` authoritative while stating the other numbers.

This specification follows 7 and 13, because that is what the conformance
vectors were generated against.

*To close:* one of the two documents changes. If it is `docs/SYNTAX.md`, every
digest in `tests/vectors/vectors.md5` must be regenerated.

### C-3 — The skip bitmap caps a picture at 64 tile columns

*Documents:* `docs/SYNTAX.md` 2 versus `docs/TRANSPORT.md` 1.

`skip_bitmap` is 64 bits and the syntax requires `ceil(width / 64) <= 64`. The
transport's v1 default configuration is `cols = 68` for a 4320-wide stereo
picture. **The transport's own headline configuration is not a legal
bitstream.**

Note the two documents are measuring differently: the syntax's `width` is per
eye (2160 → 34 columns, legal), while the transport's 4320 is the stereo pair
side by side. If the two eyes are one picture on the wire but two pictures in
the bitstream, that mapping is stated nowhere, and it is what makes the numbers
appear to conflict.

*To close:* state the eye-to-picture mapping normatively; then either confirm
34 columns per eye, or widen `skip_bitmap` and the constraint.

### C-21 — The STEREO disparity does not fit the vector field

*Documents:* `docs/STEREO.md` 2.3 versus `docs/SYNTAX.md` 4.1.

`docs/STEREO.md` decides the STEREO vector is an **unsigned disparity in quarter
samples, Exp-Golomb coded, with no fixed upper bound** (practically ±512
samples), because `f * IPD / z` is 60 samples at 1 m and about 200 at 30 cm, and
37.6% of tiles in the experiment exceeded ±16 samples. The syntax codes the
vector as two `s(8)` bytes in the tile header: ±31.75 samples, fixed width,
byte-aligned, outside the entropy payload.

**No near-field STEREO tile is representable.** The decision and the syntax were
written independently and neither has been reconciled.

*To close:* either widen the header field, or move the vector into the payload
for STEREO tiles — which changes the coding-unit list of clause 6.6.3 and
therefore the lane schedule.

### C-2 — Three fixed-point formats for the homography

*Documents:* `docs/PAPER.md` 2.2 (Q8.24, `h22 = 2^24`) versus
`docs/STEREO.md` 5 (centred coordinates, common Q10.21, `h22 = 2^21`) versus
`warp/include/nxvc/warp.h` (Q10.21 rows 0–1, Q2.29 row 2, `h22 = 2^29`).

`docs/STEREO.md` demonstrates the paper's Q8.24 **overflows `int32`** by about
seven bits: with centred coordinates the largest coefficient is of order `f`
(940 at the streamed width) and `940 * 2^24 > 2^31`. So the paper's format is
not merely unratified, it is wrong. The remaining two disagree about whether the
scale is common or split by row.

*To close:* `docs/WARP.md`. Note that `docs/STEREO.md` explicitly says the fix
belongs there.

### C-7 — The interpolation filter is unsignalled

*Documents:* `docs/PAPER.md` 2.2 and `docs/STEREO.md` 5 versus
`docs/SYNTAX.md` 2.

Bilinear (Lite) versus Catmull-Rom (Full) changes **every predicted sample**, so
it is normative in the strongest sense. The only thing selecting it is
`profile`, which `docs/SYNTAX.md` marks *informative*, and no tool bit
distinguishes the two. Two decoders reading the same bitstream can legitimately
produce different pictures.

*To close:* make `profile` normative for this purpose, or define a tool bit.

---

## C.2 Major issues

### C-6 — Two incompatible 26-byte pose layouts

`docs/SYNTAX.md` 3.2 defines the frame header's pose as 7 × `binary16` +
3 × `binary32`. `docs/TRANSPORT.md` 3.3 defines the replicated frame/pose header
as `pose_seq` + 4 × `s16` Q15 quaternion + 3 × `s32` millimetres Q8 +
`render_finish_ts`. Both are 26 bytes; the layouts share nothing. Since
`docs/PAPER.md` 6.7 says the frame header is replicated into the first datagram
of every band, an implementer will assume they are the same structure.

*To close:* say which one the replicated header carries, or rename one.

### C-5 — `res_level == 3`

Reserved and to be rejected by `docs/SYNTAX.md` 4.1; assigned the meaning
"DC-plane only" by `docs/TRANSPORT.md` 3.1, citing the degradation ladder of
`docs/PAPER.md` 4.6.1. The ladder's step 4 is a *coding* choice — code only the
DC plane — that needs no new `res_level` value, so the transport's reading looks
like an over-interpretation.

### C-14 — 10-bit is signalled but not specified

`bit_depth == 10` is a legal stream-header value gated by tool bit 14, but the
sample-domain table of `docs/SYNTAX.md` 4.3 is 8-bit only, no 10-bit `qstep`
scaling is given (`docs/PAPER.md` 1.5 says "the same table with coefficients
scaled by 4", which is not in the normative document), and no 10-bit clamp is
defined. A decoder asked for 10 bits has nothing to implement.

### C-15 — Three per-tile state definitions

`docs/PAPER.md` 2.6: 16 bytes — `held_frame_id`, `last_mv`, `age_since_intra`,
`concealed_count`, mode/QP/flags. `docs/TRANSPORT.md` 7.3: 4 bytes —
`pose_seq`, `age`, `state`, `late`, `recovered`. `docs/STEREO.md` 9: add a
second vector field so disparities and motion vectors are predicted separately,
making it 20 bytes. Only the transport's is normative, and it does not contain
`last_mv`, which is the field prediction and concealment actually read.

### C-11 — Motion vector: delta or absolute

`docs/PAPER.md` 2.3 says the vector is coded as a delta from the same tile's
previous vector, with signed Exp-Golomb, in the tile's own substream.
`docs/SYNTAX.md` 4.1 puts two raw `s(8)` bytes in the header. Those are
different codings *and* different semantics, and the difference is invisible
until a stream decodes to the wrong pixels.

### C-8 — `ref_slots` semantics

Defined only as "reference slots this frame overwrites (Phase 2)". Bitmask or
index is unstated, as is its relationship to the transport's
`frame_id mod 4` slot addressing, which appears to make it redundant.

### C-19 — Conformance vector gaps

See clause 9.3.1. The most consequential: **no rejection vectors at all**, so a
decoder that accepts every malformed stream passes the suite completely; and no
custom-probability-table vector, so the normalisation of clause 6.6.2 — the one
place a decoder divides, and the easiest step to get subtly wrong — is pinned by
nothing.

### C-22 — `ref_sel` for STEREO tiles

`docs/STEREO.md` 9 states `ref_delta` is "not coded" for a STEREO tile because
the mode names the reference. The syntax has `ref_sel` as two fixed bits of
`word1`, always present. Presumably the intent is "ignored", not "absent", but
as written they contradict.

### C-24 — Zero vertical disparity is unenforced

`docs/STEREO.md` 6.1 makes the horizontal-only constraint load-bearing: it is
what bounds a right-eye tile's dependency to three left-eye tiles of the same
row, and hence what makes the interleaved dispatch correct. A downward component
is "forbidden". `docs/SYNTAX.md` imposes no constraint on `mv_y`, so a stream
violating it is syntactically legal and would deadlock or misdecode a
row-pipelined decoder.

### C-9 — Three maximum tile sizes

`payload_len` allows 65535; the transport directory's `dir_len` allows 4095; the
run budget yields `max_tile_bytes = 1400 - 24 - 4 = 1372`. A Pro-profile
lossless tile is up to 12 KB and is fragmented into at most four pieces. Which
limit a *level* asserts, and what a decoder must accept, is undefined.

### C-30 — Hybrid path unspecified

`layer_type` 1 (HEVC_NAL) and 2 (H264_NAL) are legal stream-header values, and
`num_layers` up to 4 is legal under tool bit 13, but no blend formula, no
lower-layer upsampling, no external-decoder colour conversion and no drift bound
is specified anywhere. Clause 6.9 is unimplementable. [pending HYBRID.md]

---

## C.3 Minor issues

**C-16** — `ref_sel` (bitstream) and `ref_delta` (datagram) are the same
quantity with different value sets (`ref_delta == 3` means intra; `ref_sel` has
no such value). Nothing says which wins if a tile header and its datagram
disagree, nor that they must agree.

**C-13** — `dir_qp` is the tile's absolute QP; `qp_delta` is relative to
`base_qp`. Neither is stated authoritative, and a receiver that acts on
`dir_qp` before parsing the tile could act on a different number than the
decoder uses.

**C-12** — The transport's `frame_id` and the bitstream's `frame_number` are
both 16-bit frame counters and are never related to each other in any document.

**C-17** — Tool bits 15 to 19 (`ENT_OFFSET_TABLE`, `ENT_BITPLANE`, `INTRA_DIR`,
`XFORM_WAVELET`, `XFORM_4X4_SPLIT`) are named but their behaviour is specified
nowhere. Refusing them is correct v1 behaviour, so this is only a documentation
gap — but the mask should not name what no document defines.

**C-18** — No relationship is defined between a transport capability bit and a
tool bit, even where they describe the same thing (`CAP_FRAGMENT` and the Pro
profile's fragmentation rule).

**C-23** — `docs/STEREO.md` 9 calls STEREO "mode 3", counting ordinally in the
paper's mode table; by field value it is mode 4. Harmless, but it is exactly the
kind of ordinal-versus-value slip that produced C-5.

**C-25** — Skip is signalled twice: by `skip_bitmap` in the tile-row header, and
by a transport directory entry with `dir_len == 0` and `dir_mode == WARP_SKIP`.
Which the receiver is to trust, and whether they may disagree, is unstated.

**C-26** — `docs/STEREO.md` 10.1 names the STEREO/foveation interaction as its
largest open issue and as "not designed". It is decoder-visible: the proposed
fix makes the source coordinate a two-step lookup through both eyes' foveation
grids.

**C-27** — No level limits exist anywhere; `level` is a byte with no meaning.
See clause 8.3.

**C-28** — `docs/SYNTAX.md` 10 states that nothing in reconstruction depends on
any other tile. That was true for Phase 1 and is false once STEREO exists: a
right-eye tile depends on up to three left-eye tiles of the same row. The claim
should be qualified rather than repeated.

**C-29** — `warp/include/nxvc/warp.h` says "See docs/WARP.md for the normative
prose". That file does not exist. [pending WARP.md]

---

## C.4 How to use this list

* The six blocking issues are all in the **inter path**. Phase 1 (intra-only)
  is specified end to end and has conformance vectors; nothing blocks a Phase 1
  decoder except C-20, which affects both phases and is the one issue that
  should be settled today because every conformance digest depends on it.
* C-4, C-2, C-7, C-11, C-21, C-22, C-24 and C-29 all close with one document:
  `docs/WARP.md`, written in coordination with `docs/STEREO.md`.
* C-1, C-3, C-5, C-6, C-8, C-9, C-12, C-13, C-14, C-16 and C-25 are all
  boundary disputes between `docs/SYNTAX.md` and `docs/TRANSPORT.md`. They would
  be cheaply closed by one joint pass over the two documents asking, per shared
  field, *which document owns this*.
* C-19 is the one issue that gets worse with time: every clause added without a
  vector is a clause nothing tests.
