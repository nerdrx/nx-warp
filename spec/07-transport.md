# 7. Transport

`docs/TRANSPORT.md` [R-19] is normative for the wire format and for the
transport library. This clause states only what a **decoder** must know, and
cites the rest by reference. Nothing here overrides [R-19]; where this clause
and that document differ, that document wins and the difference is an Annex C
issue.

A decoder needs the transport for exactly three reasons: it must know **which
tiles it holds** (7.1, 7.2), **which reference frames it may predict from**
(7.3), and **which of its own samples are concealed** (7.4). Everything else —
pacing, FEC construction, multipath scheduling, telemetry — is invisible to the
decoding process.

## 7.1 What arrives, and how tiles are addressed

The datagram is a **tile run**: a contiguous, ascending sequence of tiles from
one tile row, packed to a payload budget (1400 bytes over Ethernet, more on a
jumbo USB path). The tile remains an independently decodable bitstream; the
datagram is only the loss unit [PAPER 6.1, TRANSPORT 3].

A run MUST be homogeneous in `stream_id`, `frame_id`, `layer_id`, the
`LOSSLESS` flag, `band` and **tile row**, and its tiles MUST be contiguous and
ascending in column [TRANSPORT 3.2]. In wire version 2 `ref_delta` and
`tile_class` live in the tile directory and may vary freely inside a run, which
is what lets a run pack to the payload budget instead of ending at a foveation
class boundary [TRANSPORT D19, D20].

Tiles are addressed **linearly** by `tile_first`, and the mapping between that
and the bitstream's per-eye, per-row addressing is Annex D decision **D-3**:

```
cols_per_eye = ceil(width  / 64)          // width is per eye
rows         = ceil(height / 64)
cols         = eyes * cols_per_eye        // the transport's cols
tile_first   = row * cols + eye * cols_per_eye + tile_index
```

**A picture is one eye**; the transport's tile grid spans the eye pair. At the
version 1 target — 2160x2160 per eye, two eyes — that is 34 columns per eye,
`cols = 68`, `rows = 34`, 2312 tiles per frame, exactly [TRANSPORT 1]. The
64-bit `skip_bitmap` binds `cols_per_eye`, not `cols`, so the transport's
headline configuration is a legal bitstream. This closes Annex C issue C-3,
which was a missing sentence rather than a conflict.

The band is `min(row / band_rows, bands - 1)` and the bit position in a
feedback bitmap is `(row - band * band_rows) * cols + col` [TRANSPORT 1]; both
are computed on the pair grid, so a band holds the same rows of both eyes.
Tile-row headers appear row-major, eye-minor (Annex D **D-3**), which is what
puts a whole left-eye row ahead of the right-eye row of the same index — the
ordering [STEREO 6.1] needs for a STEREO tile's dependency to be satisfiable.

The plaintext payload is an optional 26-byte frame/pose header, then
`dg_tile_count` four-byte directory entries, then the concatenated tile
bitstreams with no padding. **The directory must balance**: the sum of all
`dir_len`, plus `4 * dg_tile_count`, plus 26 if `pose_hdr`, MUST equal the
plaintext length. A receiver that finds otherwise MUST discard the whole
datagram and count it as lost, because it cannot know where the tile boundaries
are [TRANSPORT 3.1].

The frame header and the tile-row header are replicated into the first datagram
of every band so that a lost datagram never orphans a whole frame, and so that a
client with a gap in its pose ring still decodes [PAPER 6.7].

## 7.2 What a decoder may assume about integrity

* Every payload is AEAD-protected with the 24-byte cleartext header as its
  complete associated data, so a modified header fails the tag check
  [TRANSPORT 2]. A decoder therefore never sees a payload whose header was
  altered in flight.
* A tile is reported received only if its datagram decrypted **and** its
  bitstream decoded without error. A corrupt-but-delivered tile is treated as
  lost [PAPER 4.4].
* A datagram whose `caps` contains a bit the receiver did not negotiate MUST be
  dropped [TRANSPORT 2.2]. This is how a capability is turned off mid-session
  without a version bump.
* Placement is **position-addressed and idempotent**: a datagram is placed by
  `(frame_id mod 4, layer_id, tile_index)` the moment it parses, and arrival
  order is irrelevant. There is no reorder buffer [TRANSPORT 7.1].
* Duplicate suppression exists for statistics and FEC accounting, not for
  correctness — except that a duplicate MUST NOT count twice toward the `k`
  blocks an FEC group needs [TRANSPORT 7.2].

## 7.3 Reference eligibility (normative for the decoder's reference state)

This is the rule that makes the encoder's model of the decoder correct, and the
decoder's real state must match it [TRANSPORT 9].

A tile position `t = (row, col)` of frame `M` is **exact** if the sender knows,
from feedback, that the receiver holds bit-identical samples there:

```
exact(M, t) =
     state(M, t) == RECEIVED
  or (state(M, t) == CONCEALED
      and for all t' in N3x3(t): exact(M-1, t'))
```

`N3x3` is the 3x3 tile neighbourhood clipped to the grid, so a corner has four
neighbours. A position whose band has no feedback yet is UNKNOWN and is never
exact. The recursion terminates at the edge of the eight-frame shadow history,
where `exact` is false.

The recursion is the point. A concealed tile is a legal reference **only if the
source samples its warp read were themselves exact**, and the concealment warp
reads across tile borders exactly as prediction does [TRANSPORT D10]. A
non-recursive reading of [PAPER 4.5] — "concealed tiles are legal references"
— is wrong and would let error propagate silently.

**Reference choice** for tile `t` of frame `N`:

```
for d in 0, 1, 2:
    M = N - 1 - d
    if M >= 0 and for all t' in N3x3(t): exact(M, t'):
          return ref_delta = d
return ref_delta = 3          // intra
```

That is: the newest frame among `N-1`, `N-2`, `N-3` whose 3x3 neighbourhood is
fully acknowledged. With no feedback for four frames every tile goes intra at
the capped size — a QP jump, not a stall. There is no IDR ladder and no DPB
invalidation protocol.

**The decoder's obligation** is narrow but real: it MUST hold a four-slot
reference ring addressed by `frame_id mod 4`, and it MUST be able to predict
from any of the three most recent slots as `ref_sel` / `ref_delta` selects. A
decoder that keeps only the previous frame cannot decode a conforming stream.

The bitstream's `ref_sel` is **authoritative** and the directory's `ref_delta`
is an advisory copy carrying the extra value 3 for "no temporal reference".
They MUST agree; on disagreement the decoding process uses `ref_sel` and the
receiver marks the tile UNDECODABLE, because it can no longer account for the
tile in its reference model. Annex D decision **D-12**, closing Annex C issues
C-16 and C-22. The same authoritative-plus-advisory rule governs `qp_delta`
against `dir_qp` (Annex D **D-13**) and `skip_bitmap` against a `dir_len == 0`
directory entry (Annex D **D-14**).

`frame_id` and `frame_number` are the same counter and MUST be equal; a
datagram whose `frame_id` disagrees with the frame it carries is inconsistent
and MUST be discarded. Annex D **D-11**, closing Annex C issue C-12.

## 7.4 Concealment marking

At the deadline for band `b` of frame `N` [TRANSPORT 7.4]:

1. every tile of the band still in state EMPTY is marked CONCEALED and inherits
   `pose_seq` and `age = age(N-1, t) + 1` from slot `(N-1) mod 4`;
2. the band's feedback packet is generated;
3. tiles arriving afterwards are still placed, marked `late`, and become
   DECODED — they are valid references and better concealment sources. **For
   reference tracking a late tile is a received tile** [PAPER 4.3 item 5].

Per-tile metadata is four bytes per tile per slot: `pose_seq` (16 bits), `age`
(8 bits, saturating at 255), `state` (2 bits: EMPTY, DECODED, CONCEALED,
UNDECODABLE), `late` and `recovered` [TRANSPORT 7.3].

A tile is UNDECODABLE — as distinct from concealed — when the directory was
inconsistent, a fragment is missing, or it references a frame the receiver does
not hold [TRANSPORT 7.5]. Annex D adds four cases: `dir_res_level == 3`
(**D-6**), a `ref_delta` that disagrees with the tile's `ref_sel` (**D-12**), a
directory entry that disagrees with `skip_bitmap` (**D-14**), and a tile whose
header `eye` disagrees with the eye derived from its linear index (**D-3**). The decoding process treats UNDECODABLE exactly as
it treats a missing tile (clause 6.11); the distinction exists for telemetry
and for the feedback the encoder acts on.

A frame containing any concealed tile carries the partial-frame telemetry flag.

## 7.5 What is deliberately out of scope here

Referred to [R-19] in full, with no restatement:

| Topic | Reference |
|---|---|
| AEAD key schedule, per-path subkeys, nonce derivation | [TRANSPORT 4] |
| Payload budgets and the 44 bytes reserved for FEC | [TRANSPORT 5] |
| Fragmentation limits; `dir_len` bounds a fragment, not a tile | [TRANSPORT 3.4], Annex D **D-15** |
| FEC group construction, the protected block, recovery deadline | [TRANSPORT 6] |
| The deadline controller and its hysteresis | [TRANSPORT 7.4] |
| Feedback packet layout, band records, RLE bitmaps | [TRANSPORT 8] |
| Multipath duplication and striping | [TRANSPORT 10] |
| Telemetry | [TRANSPORT 11] |
| Failure modes the library must handle | [TRANSPORT 12] |

Rate control, including the degradation ladder that chooses `qp_delta` and
`res_level` per tile, is an encoder concern and changes nothing in this clause
[I-2].
