# Annex C — Open issues

Every gap, ambiguity and inter-document conflict found while assembling this
specification. This annex is a deliverable in its own right: the value of
writing a spec against documents that are still being written is exactly this
list.

Each issue names the documents that disagree, states what an implementer cannot
do today, and says what would close it. Nothing here is a complaint about
quality — a design this size producing 29 open issues at this stage is normal.
What matters is that none of them is *silent*.

**Status.** Twenty-four of the twenty-nine are now closed by
[Annex D](annex-d-inter-decisions.md), which was written to unblock Phase 2:
every blocking issue, every issue on the inter path, and every boundary dispute
between `docs/SYNTAX.md` and `docs/TRANSPORT.md`. Each closed issue below
carries **CLOSED** and a pointer to the decision that closed it. The change
list the `docs/SYNTAX.md` owner needs to apply them is
`docs/SYNTAX-CHANGES-PHASE2.md`.

**Severity**

| | Meaning |
|---|---|
| **BLOCKING** | A conforming decoder cannot be written, or two conforming decoders would produce different pixels |
| **MAJOR** | A conforming decoder can be written for the affected feature only by guessing |
| **MINOR** | Ambiguous or redundant, but an implementer would reach the right answer |

## C.0 Summary

| ID | Severity | One line | Clause |
|---|---|---|---|
| C-4 | ~~BLOCKING~~ **CLOSED** | No syntax element carries the homography | Annex D **D-1** |
| C-20 | ~~BLOCKING~~ **CLOSED** | `ERRATA` says inverse-transform shifts 8/12, `SYNTAX` says 7/13 | Annex D **D-18** |
| C-3 | ~~BLOCKING~~ **CLOSED** | 64-bit `skip_bitmap` caps 64 tile columns; transport uses 68 | Annex D **D-3** |
| C-21 | ~~BLOCKING~~ **CLOSED** | STEREO disparity needs ±512 samples; syntax gives two `s(8)` bytes | Annex D **D-4** |
| C-2 | ~~BLOCKING~~ **CLOSED** | Three incompatible fixed-point formats for the homography | Annex D **D-1** |
| C-7 | ~~BLOCKING~~ **CLOSED** | Interpolation filter is selected only by `profile`, which is informative | Annex D **D-5** |
| C-6 | ~~MAJOR~~ **CLOSED** | Two different 26-byte pose layouts | Annex D **D-2** |
| C-5 | ~~MAJOR~~ **CLOSED** | `res_level == 3` reserved in the syntax, "DC-plane" in the transport | Annex D **D-6** |
| C-14 | ~~MAJOR~~ **CLOSED** | `bit_depth == 10` is legal but 10-bit is specified nowhere | Annex D **D-16** |
| C-10 | MAJOR | `wgt`: four weights or five, and no blend formula | 5.4, 6.9 |
| C-15 | ~~MAJOR~~ **CLOSED** | Three incompatible per-tile state definitions | Annex D **D-9**, **D-7** |
| C-11 | ~~MAJOR~~ **CLOSED** | Motion vector: delta or absolute, unstated normatively | Annex D **D-8** |
| C-8 | ~~MAJOR~~ **CLOSED** | `ref_slots` semantics undefined | Annex D **D-10** |
| C-19 | MAJOR | No inter, rejection, custom-table or 10-bit conformance vectors | 9.3.1 |
| C-22 | ~~MAJOR~~ **CLOSED** | `ref_sel` "not coded" for STEREO, but the bits are always present | Annex D **D-12** |
| C-24 | ~~MAJOR~~ **CLOSED** | STEREO's zero-vertical-disparity rule has no syntax constraint | Annex D **D-4** |
| C-9 | ~~MAJOR~~ **CLOSED** | Three disagreeing maximum tile sizes | Annex D **D-15** |
| C-30 | MAJOR | Hybrid path unspecified but `layer_type` 1 and 2 are legal | 6.9 |
| C-16 | ~~MINOR~~ **CLOSED** | `ref_sel` versus `ref_delta`: which is authoritative | Annex D **D-12** |
| C-13 | ~~MINOR~~ **CLOSED** | `dir_qp` absolute versus `qp_delta` relative | Annex D **D-13** |
| C-12 | ~~MINOR~~ **CLOSED** | `frame_id` and `frame_number` never related | Annex D **D-11** |
| C-17 | MINOR | Tool bits 15–19 declared, never specified | 8.4 |
| C-18 | ~~MINOR~~ **CLOSED** | No relation defined between `caps` bits and tool bits | Annex D **D-19** |
| C-23 | ~~MINOR~~ **CLOSED** | `STEREO` called "mode 3" ordinally, is mode 4 by value | Annex D **D-22** |
| C-25 | ~~MINOR~~ **CLOSED** | Two skip signalling mechanisms | Annex D **D-14** |
| C-26 | ~~MINOR~~ **CLOSED for v1** | STEREO and foveation interaction undesigned | Annex D **D-17** |
| C-27 | MINOR | No level limits defined at all | 8.3 |
| C-28 | ~~MINOR~~ **CLOSED** | "No tile depends on another" is false once STEREO exists | Annex D **D-22**, change list |
| C-29 | ~~MINOR~~ **CLOSED** | `warp/` cites a `docs/WARP.md` that does not exist | `docs/WARP.md` landed |

Counts as first written: **6 blocking, 12 major, 11 minor — 29 open issues.**
After Annex D: **0 blocking, 3 major, 2 minor — 5 open issues.**

Still open, and why each was left:

| ID | Severity | Why it stays open |
|---|---|---|
| C-10 | MAJOR | `wgt` blend weights and formula. Hybrid path, `[pending HYBRID.md]` |
| C-30 | MAJOR | Hybrid path unspecified. `[pending HYBRID.md]` |
| C-19 | MAJOR | Conformance vectors. Annex D **D-21** fixes *what* they must cover; they must still be generated, and a list is not a vector |
| C-17 | MINOR | Tool bits 15-19 named but undefined. A v1 decoder refuses them, so this is a documentation gap, not a decoding gap |
| C-27 | MINOR | No level limits. Not on the inter path; needs a decode-work unit nobody has measured |

C-26 is closed **for version 1 only** — no foveation syntax exists, so the
interaction cannot arise — and reopens with the foveation syntax, which must
state the STEREO interaction in the same change (Annex D **D-17**).

C-14's 10-bit tables and C-7's Catmull-Rom table are closed by *refusal*: the
tool bits exist, version 1 does not define them, and a version 1 decoder
rejects them. That is a decision, not a deferral — see Annex D **D-16** and
**D-5**.

Closed during drafting, kept for the record:

| ID | Closed by | Resolution |
|---|---|---|
| C-1 | `docs/SYNTAX.md` 2.2 | `color_space` landed as a *descriptive* element at offset 42, tied to `color_transform` by "`color_space == 3` if and only if `color_transform == 1`". The two do not duplicate: one says what the codec does, the other what the result means |
| C-31 | `docs/RATECONTROL.md` | Landed. Informative, as expected; adds no normative decoder behaviour |

---

## C.1 Blocking issues

### C-4 — No syntax element carries the homography

**CLOSED by Annex D [D-1](annex-d-inter-decisions.md).** The homography travels in a `warp_ext()` structure of `36 * eyes` bytes immediately after the frame header, gated by the new `warp_present` flag. The TLV and pose-derivation options were rejected for the reasons below; the frame-header route was taken as a *following* structure rather than a widened header so Phase 1 frames stay byte-identical.


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

**CLOSED by Annex D [D-18](annex-d-inter-decisions.md).** 7 and 13 are normative; `docs/ERRATA.md` 1.4 is corrected, not `docs/SYNTAX.md`, because the conformance digests were generated against 7/13.


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

**CLOSED by Annex D [D-3](annex-d-inter-decisions.md).** A picture is one eye; the transport's grid spans the pair; `cols = eyes * cols_per_eye`. 34 columns per eye is legal and `skip_bitmap` stays 64 bits. The issue was a missing sentence, not a conflict.


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

**CLOSED by Annex D [D-4](annex-d-inter-decisions.md).** A 16-bit unsigned `disparity` in the tile's optional area, 12 bits used, reaching 1023.75 samples — in the same two bytes `mv_x`/`mv_y` occupied. The Exp-Golomb-in-the-payload option was rejected because it changes the lane schedule for under a tenth of a percent of a tile.


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

**CLOSED by Annex D [D-1](annex-d-inter-decisions.md).** Rows 0-1 Q10.21, row 2 Q2.29, `h22 == 2^29`, per `docs/WARP.md` 3 — the only one of the three that holds both the translation and the perspective ends.


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

**CLOSED by Annex D [D-5](annex-d-inter-decisions.md).** Tool bit 20 `FILTER_CATMULL_ROM`, undefined in version 1, so every v1 stream is bilinear and `profile` acquires no normative role.


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

**CLOSED by Annex D [D-2](annex-d-inter-decisions.md).** One layout, the integer one, owned by `docs/TRANSPORT.md` 3.3. The bitstream's `pose` is byte-identical to it.


`docs/SYNTAX.md` 3.2 defines the frame header's pose as 7 × `binary16` +
3 × `binary32`. `docs/TRANSPORT.md` 3.3 defines the replicated frame/pose header
as `pose_seq` + 4 × `s16` Q15 quaternion + 3 × `s32` millimetres Q8 +
`render_finish_ts`. Both are 26 bytes; the layouts share nothing. Since
`docs/PAPER.md` 6.7 says the frame header is replicated into the first datagram
of every band, an implementer will assume they are the same structure.

*To close:* say which one the replicated header carries, or rename one.

### C-5 — `res_level == 3`

**CLOSED by Annex D [D-6](annex-d-inter-decisions.md).** Reserved and rejected in both documents. This annex's own reading was right: ladder step 4 is `res_level == 2` with only the DC unit coded.


Reserved and to be rejected by `docs/SYNTAX.md` 4.1; assigned the meaning
"DC-plane only" by `docs/TRANSPORT.md` 3.1, citing the degradation ladder of
`docs/PAPER.md` 4.6.1. The ladder's step 4 is a *coding* choice — code only the
DC plane — that needs no new `res_level` value, so the transport's reading looks
like an over-interpretation.

### C-14 — 10-bit is signalled but not specified

**CLOSED by Annex D [D-16](annex-d-inter-decisions.md).** Version 1 is 8-bit: tool bit 14 is undefined in v1 and `bit_depth == 10` is rejected. Closed by refusal rather than by inventing unverified tables.


`bit_depth == 10` is a legal stream-header value gated by tool bit 14, but the
sample-domain table of `docs/SYNTAX.md` 4.3 is 8-bit only, no 10-bit `qstep`
scaling is given (`docs/PAPER.md` 1.5 says "the same table with coefficients
scaled by 4", which is not in the normative document), and no 10-bit clamp is
defined. A decoder asked for 10 bits has nothing to implement.

### C-15 — Three per-tile state definitions

**CLOSED by Annex D [D-9](annex-d-inter-decisions.md) and [D-7](annex-d-inter-decisions.md).** There are two records, not three: the transport's 4-byte receiver record and a 6-byte decoder prediction state (`last_mv`, `last_disp`). The paper's and STEREO's are encoder-side.


`docs/PAPER.md` 2.6: 16 bytes — `held_frame_id`, `last_mv`, `age_since_intra`,
`concealed_count`, mode/QP/flags. `docs/TRANSPORT.md` 7.3: 4 bytes —
`pose_seq`, `age`, `state`, `late`, `recovered`. `docs/STEREO.md` 9: add a
second vector field so disparities and motion vectors are predicted separately,
making it 20 bytes. Only the transport's is normative, and it does not contain
`last_mv`, which is the field prediction and concealment actually read.

### C-11 — Motion vector: delta or absolute

**CLOSED by Annex D [D-8](annex-d-inter-decisions.md).** Absolute. A delta would make parsing depend on decoder state and break independent tile decodability.


`docs/PAPER.md` 2.3 says the vector is coded as a delta from the same tile's
previous vector, with signed Exp-Golomb, in the tile's own substream.
`docs/SYNTAX.md` 4.1 puts two raw `s(8)` bytes in the header. Those are
different codings *and* different semantics, and the difference is invisible
until a stream decodes to the wrong pixels.

### C-8 — `ref_slots` semantics

**CLOSED by Annex D [D-10](annex-d-inter-decisions.md).** A bitmask, constrained in v1 to `1 << (frame_number mod 4)` — the transport's ring rule restated as a check.


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

**CLOSED by Annex D [D-12](annex-d-inter-decisions.md).** Present, MUST be 0, ignored — which is what "not absent" means in a fixed-width word.


`docs/STEREO.md` 9 states `ref_delta` is "not coded" for a STEREO tile because
the mode names the reference. The syntax has `ref_sel` as two fixed bits of
`word1`, always present. Presumably the intent is "ignored", not "absent", but
as written they contradict.

### C-24 — Zero vertical disparity is unenforced

**CLOSED by Annex D [D-4](annex-d-inter-decisions.md).** There is no vertical field, so the rule is structural and no legal stream can violate it.


`docs/STEREO.md` 6.1 makes the horizontal-only constraint load-bearing: it is
what bounds a right-eye tile's dependency to three left-eye tiles of the same
row, and hence what makes the interleaved dispatch correct. A downward component
is "forbidden". `docs/SYNTAX.md` imposes no constraint on `mv_y`, so a stream
violating it is syntactically legal and would deadlock or misdecode a
row-pipelined decoder.

### C-9 — Three maximum tile sizes

**CLOSED by Annex D [D-15](annex-d-inter-decisions.md).** The three limits bound three different quantities; `dir_len` bounds a fragment. A 12 kB lossless tile needs `CAP_JUMBO`.


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

**C-16** — **CLOSED by Annex D D-12.** `ref_sel` (bitstream) and `ref_delta` (datagram) are the same
quantity with different value sets (`ref_delta == 3` means intra; `ref_sel` has
no such value). Nothing says which wins if a tile header and its datagram
disagree, nor that they must agree.

**C-13** — **CLOSED by Annex D D-13.** `dir_qp` is the tile's absolute QP; `qp_delta` is relative to
`base_qp`. Neither is stated authoritative, and a receiver that acts on
`dir_qp` before parsing the tile could act on a different number than the
decoder uses.

**C-12** — **CLOSED by Annex D D-11.** The transport's `frame_id` and the bitstream's `frame_number` are
both 16-bit frame counters and are never related to each other in any document.

**C-17** — Tool bits 15 to 19 (`ENT_OFFSET_TABLE`, `ENT_BITPLANE`, `INTRA_DIR`,
`XFORM_WAVELET`, `XFORM_4X4_SPLIT`) are named but their behaviour is specified
nowhere. Refusing them is correct v1 behaviour, so this is only a documentation
gap — but the mask should not name what no document defines.

**C-18** — **CLOSED by Annex D D-19.** No relationship was defined between a transport capability bit and a
tool bit, even where they describe the same thing (`CAP_FRAGMENT` and the Pro
profile's fragmentation rule).

**C-23** — **CLOSED by Annex D D-22.** `docs/STEREO.md` 9 calls STEREO "mode 3", counting ordinally in the
paper's mode table; by field value it is mode 4. Harmless, but it is exactly the
kind of ordinal-versus-value slip that produced C-5.

**C-25** — **CLOSED by Annex D D-14.** Skip is signalled twice: by `skip_bitmap` in the tile-row header, and
by a transport directory entry with `dir_len == 0` and `dir_mode == WARP_SKIP`.
Which the receiver is to trust, and whether they may disagree, is unstated.

**C-26** — **CLOSED for version 1 by Annex D D-17.** `docs/STEREO.md` 10.1 names the STEREO/foveation interaction as its
largest open issue and as "not designed". It is decoder-visible: the proposed
fix makes the source coordinate a two-step lookup through both eyes' foveation
grids.

**C-27** — No level limits exist anywhere; `level` is a byte with no meaning.
See clause 8.3.

**C-28** — **CLOSED by Annex D D-22** (the correction is in the change list). `docs/SYNTAX.md` 10 states that nothing in reconstruction depends on
any other tile. That was true for Phase 1 and is false once STEREO exists: a
right-eye tile depends on up to three left-eye tiles of the same row. The claim
should be qualified rather than repeated.

**C-29** — **CLOSED.** `docs/WARP.md` landed on 2026-09-04 and is the normative
predictor definition; clause 6.7 cites it and Annex D **D-1**, **D-5** and
**D-7** fix its bitstream boundary.

---

## C.4 How this list was used

The plan recorded here was carried out, and it is worth recording that it
worked as written.

* **The six blocking issues were all in the inter path.** All six are now
  closed by Annex D. Phase 2 can start from an unambiguous syntax.
* **C-4, C-2, C-7, C-11, C-21, C-22, C-24 and C-29 were to close with one
  document, `docs/WARP.md`.** `docs/WARP.md` landed and closed the physics;
  Annex D closed the bitstream boundary around it, in coordination with
  `docs/STEREO.md`, which is superseded in two places (D-4, D-5).
* **C-1, C-3, C-5, C-6, C-8, C-9, C-12, C-13, C-14, C-16 and C-25 were
  boundary disputes** to be closed by one joint pass asking, per shared field,
  *which document owns this*. That pass is Annex D **D-20**, and it closed all
  of them. The recurring answer is the same shape: the bitstream field is
  authoritative and the transport field is an advisory copy with a defined
  disagreement rule, because the transport is codec agnostic and must never be
  able to change a decoded sample.
* **C-19 is the one issue that gets worse with time**, and it is still open.
  Annex D **D-21** fixes what the Phase 2 vectors must cover — including the
  rejection vectors, without which a decoder that accepts every malformed
  stream passes the suite completely — but a list is not a vector.
