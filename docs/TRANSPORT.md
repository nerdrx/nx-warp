# NX Warp transport: normative wire format and state machines

Version 2 (`NXT_VERSION = 2`). Version 1 was never deployed; the v1 to v2 diff is
decisions D19 to D23 in Appendix A, and `transport/RESULTS.md` measures it. This document is normative for the `nxvc_transport`
library (`transport/`). It refines PAPER.md sections 4.1 to 4.11, 6.1, 6.6 and 6.7.
Where the paper leaves something open, or where the paper's arithmetic does not close,
the decision is recorded here and repeated in Appendix A.

The transport is **codec agnostic**. A tile is an opaque byte blob addressed by
`(frame_id, layer_id, row, col)` with a `class` and a small decoder-side descriptor
(`qp`, `mode`, `res_level`). The library contains no sockets and no key exchange:
the integration supplies datagram buffers, a monotonic clock, and AEAD keys.

Conventions: all multi-byte integers are **little endian**. Bit fields inside a byte
are listed **LSB first**. `u16`/`u32` mean unsigned little-endian integers of that
width. Byte offsets are zero-based. "MUST", "SHOULD", "MAY" have their usual force.

---

## 1. Geometry, bands and identifiers

| Name | v1 value | Notes |
|---|---|---|
| `tile_size` | 64 | 32 reserved by the stream header (PAPER 6.2), not used by transport |
| `cols` | 68 | `eyes * cols_per_eye`; see below |
| `rows` | 34 | `ceil(2160 / 64)` |
| `tiles_per_frame` | 2312 | `cols * rows` |
| `band_rows` | 6 | PAPER 4.2 |
| `bands` | 6 | last band is 4 rows |
| `tiles_in_band` | 408 | `cols * band_rows`; last band 272 |
| `ring_slots` | 4 | display-side reference ring (PAPER 4.3, 6.6) |
| `shadow_frames` | 8 | encoder-side feedback history (PAPER 6.6) |

All of these are runtime configuration in `nxt::StreamConfig`; the table gives the
v1 defaults for the first target stream. The library does not assume them.

**Eye-to-picture mapping (spec reconciliation, `spec/annex-d-inter-decisions.md`
D-3).** A *picture* in the bitstream is **one eye**, and the codec's `width` and
`height` are per eye. This grid spans the eye pair:

```
cols_per_eye = ceil(width  / 64)     // 34 at width = 2160
rows         = ceil(height / 64)     // 34
cols         = eyes * cols_per_eye   // 68
```

so `cols = 68` is 34 columns of each eye, not 68 columns of one 4320-wide
picture. A `StreamConfig` whose `cols` is not `eyes * cols_per_eye`, or whose
`rows` is not `ceil(height / 64)`, is invalid. The bitstream's 64-bit
`skip_bitmap` covers one row of one eye and therefore binds `cols_per_eye`, not
`cols`: the configuration above is a legal bitstream, which it appeared not to
be while this sentence was missing.

**Linear tile index**: `tile_index = row * cols + col`. This is the `tile_first`
field and the addressing used everywhere. In terms of the bitstream's per-eye,
in-row index:

```
tile_first = row * cols + eye * cols_per_eye + tile_index_in_row
eye        = (tile_first % cols) / cols_per_eye
```

Tile-row headers appear in the bitstream row-major, eye-minor, so a run is a
contiguous range of linear indices *and* of bitstream bytes, and a whole
left-eye row precedes the right-eye row of the same index — the ordering
`docs/STEREO.md` 6.1 needs. **Band index**: `band = min(row / band_rows,
bands - 1)`. **Tile index within a band**: `tile_in_band = (row - band * band_rows) * cols + col`;
that is the bit position in the feedback `received_bitmap`.

`tile_class` is 0 = A (fovea, quad-layer base), 1 = B (mid eccentricity),
2 = C (periphery, enhancement layers). Value 3 is reserved and MUST NOT be sent.

`path_id` is 0 or 1 in v1 (`NXT_MAX_PATHS = 2`); the field is 2 bits so 4 paths are
forward compatible.

---

## 2. Datagram header (24 bytes, cleartext, AEAD associated data)

Every datagram on the downstream (server to client) begins with this fixed 24-byte
header. It is sent in the clear and is the **complete** associated data for the AEAD
that protects the payload; a modified header therefore fails the tag check.

| off | size | field | bits | description |
|---|---|---|---|---|
| 0 | 1 | `version` | [3:0] | `NXT_VERSION` = 1. A receiver MUST drop other versions. |
| 0 | 1 | `flags` | [7:4] | see 2.1 |
| 1 | 1 | `stream_id` | 8 | WiVRn stream (per quad layer / eye-pair geometry) |
| 2 | 2 | `frame_id` | 16 | wraps every 12.1 min at 90 Hz |
| 4 | 2 | `tile_first` | 16 | linear tile index of the first tile of the run |
| 6 | 1 | `tile_count` | 8 | tiles carried, 1..255; **0 marks a parity datagram** |
| 7 | 1 | `layer_id` | [1:0] | 0 = base, 1..3 = enhancement |
| 7 | 1 | `frag_idx` | [3:2] | fragment index of an oversize tile, else 0 |
| 7 | 1 | `frag_count` | [5:4] | fragments minus one (0 = unfragmented), else 0 |
| 7 | 1 | `fec_class` | [7:6] | protection class of the **datagram**: the strongest class of any tile it carries |
| 8 | 1 | `band` | [2:0] | 0..6; 7 = not band addressed |
| 8 | 1 | `pose_hdr` | [3] | payload begins with the 26-byte frame/pose header (PAPER 6.7) |
| 8 | 1 | `fec_m` | [6:4] | parity datagrams in this FEC group, 0..4; 0 = no FEC |
| 8 | 1 | reserved | [7] | MUST be 0 |
| 9 | 1 | `caps` | 8 | negotiated capability echo, see 2.2 |
| 10 | 2 | `pose_seq` | 16 | index into the client's own 2 s pose ring |
| 12 | 2 | `path_seq` | [13:0] | per-path datagram sequence, wraps at 16384 |
| 12 | 2 | `path_id` | [15:14] | sending path |
| 14 | 1 | `fec_group` | 8 | FEC group number within `(frame_id, band, tile_class)` |
| 15 | 1 | `fec_idx` | [3:0] | index in the group; `fec_idx >= fec_k` marks parity |
| 15 | 1 | `fec_k` | [7:4] | data datagrams in the group, 1..10; 0 = no FEC on this datagram |
| 16 | 4 | `tx_ts` | 32 | server clock, microseconds, wraps 71.6 min |
| 20 | 2 | `payload_len` | 16 | bytes of ciphertext, **excluding** the 16-byte tag |
| 22 | 2 | `enc_us` | 16 | encode-finish minus render-finish for this band, telemetry |

Total 192 bits = 24 bytes.

> **Decision D1.** The paper's field list sums to 178 bits, not the stated 192. The
> 14 free bits are spent on `frag_count` completion, a class field (2), `band` (3),
> `pose_hdr` (1) and `caps` (8) — 192 exactly.
>
> **Decision D19 (v2).** The per-tile `ref_delta` and `tile_class` move out of the
> header into the tile directory, and `layer_id` shrinks from 4 bits to 2 (the paper
> defines exactly four layers). That frees 6 bits, of which 2 become `fec_class` —
> the *datagram's* protection class, which the receiver needs before it can decrypt
> — and 3 become `fec_m`. One bit is reserved. The header is still exactly 192 bits.

### 2.1 Flags nibble (byte 0, bits 4..7)

| bit | name | meaning |
|---|---|---|
| 4 | `KEYFRAME_RUN` | every tile in the run is intra coded |
| 5 | `PARTIAL_FRAME` | the sender knows this frame is already incomplete (rate overrun / governor drop) |
| 6 | `LOSSLESS` | the run carries lossless tiles; fragmentation is legal only with this bit |
| 7 | `LAST_RUN_OF_FRAME` | last data datagram of `frame_id` on **any** path |

`LAST_RUN_OF_FRAME` is set on exactly one data datagram per frame per stream. It is
advisory: the receiver's frame completion is driven by the deadline (section 7), not
by this bit.

### 2.2 Capability byte (`caps`)

Negotiated out of band at connect time and echoed in every datagram. A receiver MUST
drop a datagram whose `caps` contains a bit it did not negotiate. This lets a
capability be turned off mid-session (one band of skew) without a version bump.

| bit | name | meaning |
|---|---|---|
| 0 | `CAP_FEC` | RS FEC groups are in use; the payload budget is reduced (section 5) |
| 1 | `CAP_MULTIPATH` | more than one path is active; `path_id` may be non-zero |
| 2 | `CAP_JUMBO` | the path MTU probe found > 1500; the run budget is the probed value |
| 3 | `CAP_FRAGMENT` | oversize lossless tiles may be fragmented |
| 4 | `CAP_POSE_HDR` | `pose_hdr` may be set (PAPER 6.7 pose replication) |
| 5 | `CAP_RLE_FEEDBACK` | the feedback bitmap may use RLE mode (section 6.2) |
| 6-7 | reserved | MUST be 0 |

---

## 3. Payload layout

The **plaintext** payload of a data datagram is:

```
  [26 bytes]  frame/pose header       -- present iff pose_hdr == 1
  [4*N bytes] tile directory          -- N = tile_count entries, in tile order
  [ ... ]     tile bitstreams         -- concatenated, in tile order, no padding
```

The ciphertext on the wire is the AEAD encryption of exactly this, followed by the
16-byte tag. `payload_len` counts the ciphertext only. For GCM/ChaCha20-Poly1305
the ciphertext length equals the plaintext length, so
`plaintext_len == payload_len`.

### 3.1 Tile directory entry (4 bytes, u32 little endian)

| bits | field | description |
|---|---|---|
| [11:0] | `len` | tile bitstream length in bytes, 0..4095. 0 means an empty (skip) tile |
| [17:12] | `qp` | 0..63 |
| [20:18] | `mode` | 0 WARP_SKIP, 1 STATIC_MV, 2 WARP_MV, 3 INTRA, 4 STEREO, 5..7 reserved |
| [22:21] | `res_level` | 0 full, 1 half, 2 quarter; 3 **reserved, MUST NOT be sent** |
| [23] | `lossless` | this tile is lossless coded |
| [24] | `chroma444` | this tile is 4:4:4 |
| [25] | `alpha` | this tile carries an alpha plane |
| [27:26] | `tile_class` | 0=A, 1=B, 2=C; 3 reserved (v2) |
| [29:28] | `ref_delta` | reference frame = `frame_id - 1 - ref_delta`; 3 = intra (v2) |
| [31:30] | reserved | MUST be 0 (v3: edge mask / contour mode) |

The sum of all `len` in the directory plus `4*N` plus 26 if `pose_hdr` MUST equal
the plaintext length. A receiver that finds otherwise MUST discard the whole
datagram and count it as lost (it cannot know where the tile boundaries are).

> **Decision D2.** The directory is 4 bytes as the paper mandates. In v2 thirty of
> its thirty-two bits are defined; the class and reference fields moved here from the
> header precisely so a run no longer has to be homogeneous in them.
>
> **Decision D24 (spec reconciliation, `spec/annex-d-inter-decisions.md` D-6,
> D-12, D-13).** `res_level == 3` does **not** mean "DC-plane only". The
> bitstream reserves the value and so does this directory; a receiver that finds
> it marks the tile UNDECODABLE. The degradation ladder's step 4 is
> `res_level == 2` with only the DC unit coded, which needs no fourth value and
> no decoder branch.
>
> `ref_delta` and `dir_qp` are **advisory copies** of the tile header's
> `ref_sel` and `clamp(base_qp + qp_delta, 0, 63)`. The bitstream is
> authoritative in both cases — the transport is codec agnostic and must never
> be able to change a decoded sample. `ref_delta` MUST equal `ref_sel`, except
> that an INTRA or STEREO tile carries `ref_delta == 3` ("no temporal
> reference") while its `ref_sel` bits are zero and ignored. On a `ref_delta`
> disagreement the receiver marks the tile UNDECODABLE, because it can no longer
> account for the tile in its reference model; on a `dir_qp` disagreement it
> counts an integrity fault and decodes normally. The same rule governs a
> `dir_len == 0` / `dir_mode == WARP_SKIP` entry against the bitstream's
> `skip_bitmap`, which is authoritative.

### 3.2 Run homogeneity

A run MUST be homogeneous in `stream_id`, `frame_id`, `layer_id`, the `LOSSLESS`
flag and **tile row**, and its tiles MUST be contiguous and ascending in `col`.
`ref_delta` and `tile_class` may vary freely inside a run.

The datagram's `fec_class` is the **strongest** (numerically smallest) `tile_class`
of any tile it carries: a run containing one fovea tile is protected as fovea.

> **Decision D3 (v1).** With `ref_delta` and `tile_class` in the header a run also
> had to be homogeneous in both. A 68-tile row crosses two or three foveation
> classes, so runs ended at class boundaries at about 9 tiles instead of filling the
> payload budget, doubling the datagram rate and the per-tile header cost.
>
> **Decision D20 (v2).** Runs pack to the payload budget. To stop a single fovea tile
> promoting a long periphery run to class A protection, the packetizer breaks a run
> on a class change once the run already holds `kClassBreakMin` (24) tiles; below
> that it absorbs the change and takes the stronger class. Measured effect at the
> paper's tile-size distribution: 11.3 tiles per run against 9.0, and the header and
> directory overhead falls from 8.7 % to 7.8 %.
>
> The ceiling is not 20 tiles. PAPER 4.1 derives 5.5 % from "a run of 20 average
> tiles ... 1800 payload bytes", but 20 tiles of 90 bytes plus their 4-byte directory
> entries is 1880 bytes, which does not fit a 1400-byte MTU under any header scheme.
> At a 1400-byte MTU with FEC the arithmetic ceiling is 14 average tiles, and the
> heavy-tailed real distribution (fovea tiles run 3x the mean) brings the measured
> mean to 11.3.

### 3.3 Frame/pose header (`pose_header`, 26 bytes, PAPER 6.7)

Replicated in the first datagram of every band so a client with a gap in its pose
ring still decodes.

**This document owns the layout, and it is the only 26-byte pose layout in the
format** (spec reconciliation, `spec/annex-d-inter-decisions.md` D-2). The
bitstream frame header's 26 opaque `pose` bytes are byte-identical to it;
`docs/SYNTAX.md` 3.2's alternative layout of 7 x `binary16` + 3 x `binary32` is
superseded, which is what makes PAPER 6.7's replication argument true rather
than merely plausible. Angular velocity is not carried: it is a client-side
quantity recovered from the client's own pose ring, which `pose_seq` indexes.

| off | size | field |
|---|---|---|
| 0 | 2 | `pose_seq` (same value as the datagram header) |
| 2 | 8 | orientation, 4 x s16 quaternion, Q15, `(x, y, z, w)` |
| 10 | 12 | position, 3 x s32 millimetres x 256 (Q8) |
| 22 | 4 | `render_finish_ts` u32, server clock, microseconds |

The transport treats these 26 bytes as opaque: it copies them and reports their
presence. Only `render_finish_ts` is read, for telemetry. The codec treats them
as opaque too — the decoding process never interprets a pose, because it
performs no floating-point arithmetic.

`frame_id` and the bitstream's `frame_number` are the same 16-bit counter and
MUST be equal; a datagram whose `frame_id` disagrees with the frame it carries
is inconsistent and MUST be discarded (spec reconciliation,
`spec/annex-d-inter-decisions.md` D-11).

### 3.4 Oversize tiles and fragmentation

`max_tile_bytes` is `run_payload_budget - 4` (section 5). A tile larger than that is
an **oversize tile**.

* In normal (lossy) modes the encoder MUST NOT produce one. The rate controller caps
  the tile and re-encodes at `QP + 6`, at most twice (PAPER 4.1). The transport
  exposes this as a hook: `Packetizer::OversizePolicy` with
  `kReject` (return an error to the encoder, the default),
  `kDropTile` (emit the tile as `len = 0` with `mode = WARP_SKIP` so the client
  conceals it), and `kFragment`.
* `kFragment` is legal only when `LOSSLESS` and `CAP_FRAGMENT` are both set. The tile
  is split into `frag_count + 1` (2..4) fragments, each in its **own** datagram with
  `tile_count = 1`, the same `tile_first`, ascending `frag_idx`, and a directory entry
  whose `len` is that fragment's length. The tile is delivered only if all fragments
  arrive; a tile with any fragment missing is a lost tile.

**`dir_len` bounds a fragment, not a tile** (spec reconciliation,
`spec/annex-d-inter-decisions.md` D-15). The three published limits bound three
different quantities: the bitstream's `payload_len` (65535) bounds the tile,
`dir_len` (4095) bounds one fragment in one datagram, and `max_tile_bytes`
bounds an *unfragmented* tile. Each fragment MUST satisfy
`dir_len <= min(4095, max_tile_bytes)`, so the reachable maximum at a 1400-byte
MTU is `4 * 1312 = 5248` bytes. **The ~12 kB worst-case lossless 64x64 4:4:4
tile of PAPER 1.8 is therefore not transportable at a 1400-byte MTU under any
fragmentation scheme** and requires `CAP_JUMBO`, where `max_tile_bytes` is 8844
and one fragment carries it. That is a constraint on the Pro profile and belongs
in a level.

---

## 4. AEAD, key schedule and nonce derivation

The library defines an abstract `Aead` with `seal(key, nonce, aad, plaintext) ->
ciphertext||tag` and `open(...)`, tag length 16, nonce length 12. Three
implementations ship: `NullAead` (a keyed byte-rotation plus a 16-byte
non-cryptographic tag, for tests and the simulator only, never for a real session),
`Aes256Gcm` and `ChaCha20Poly1305`, the latter two compiled only when OpenSSL or
libsodium headers are found.

### 4.1 Key schedule

Per path and per direction:

```
  subkey(path, dir) = HKDF-SHA256(
        ikm  = session_key (32 bytes, from the WiVRn NX handshake),
        salt = session_salt (32 bytes, from the handshake),
        info = "nxvc-transport/v1" || u8(path_id) || u8(dir),
        len  = 32)
```

`dir` is 0 for server to client (datagrams) and 1 for client to server (feedback).
Per-path subkeys are what PAPER 4.1 calls "per-path subkeys via HKDF".

### 4.2 Nonce (12 bytes, no nonce material on the wire)

```
  nonce[0]     = stream_id
  nonce[1]     = path_id            (0..3)
  nonce[2..3]  = u16 epoch          (rekey counter, both ends)
  nonce[4..11] = u64 path_seq_ext   (little endian)
```

`path_seq_ext` is the full 64-bit per-path, per-direction datagram counter. Only its
low 14 bits travel, in `path_seq`. Both ends extend it:

```
  extend(expected, seq14):
      base = expected & ~0x3FFFull
      for cand in {base + seq14, base + seq14 - 0x4000, base + seq14 + 0x4000}:
          keep the candidate with the smallest |cand - expected|, cand >= 0
```
after which the receiver sets `expected = cand + 1`. A datagram whose extended
counter is more than `2^13` behind the highest seen is **replay** and MUST be
dropped. The counter resets to zero only when `epoch` increments; `epoch` increments
only on rekey. Uniqueness therefore holds within a subkey.

**Feedback nonce.** The upstream direction has no `path_seq`, so its counter is
derived from the packet's own header: `counter = frame_ext * bands + band`, where
`frame_ext` is the 16-bit `frame_id` extended by the same rule against each end's
own frame counter. Both ends track frames monotonically, so both compute the same
counter without a wire field.

> **Decision D4.** The paper says the nonce is derived from
> `(stream_id, path_id, path_seq, epoch counter)` but `path_seq` is only 14 bits,
> which repeats after 16384 datagrams — about 1.2 seconds at 13.5 kpps. Deriving from
> the *extended* 64-bit counter is the only safe reading and is what is specified here.
> The extension window (±8192) is far larger than any real reorder on a single path.

---

## 5. Budgets

Let `mtu` be the probed path MTU payload (1400 by default; 4000 or 8900 on a jumbo
USB path, PAPER 4.1).

```
  header_bytes      = 24
  tag_bytes         = 16
  fec_reserve       = 44   if CAP_FEC else 0        (see section 5.1)
  run_payload_budget = mtu - header_bytes - tag_bytes - fec_reserve
  max_tile_bytes     = run_payload_budget - 4
```

For `mtu = 1400`: `run_payload_budget` is **1360** without FEC and **1316** with FEC;
`max_tile_bytes` is 1356 / 1312.

> **Decision D5.** PAPER 4.1 states `max_tile_bytes = 1400 - 24 - 4 = 1372`, which
> omits the 16-byte GCM tag it mandates in the same section, and omits the space the
> parity datagram needs to carry a padded copy of the largest protected datagram.
> The correct cap with FEC on at a 1400-byte MTU is **1312 bytes**. The library
> rounds `run_payload_budget` down to a multiple of 4 so the directory stays aligned.

### 5.1 Why 44 bytes are reserved for FEC

A parity datagram carries `u16 L` plus one parity block of `L + 2` bytes, in a
24-byte header with a 16-byte tag: `24 + 2 + (L + 2) + 16 = L + 44`. Keeping the
parity datagram inside the MTU requires `L <= mtu - 44`, and `L` is the largest
protected data datagram, `24 + payload_len + 16`. Hence the reserve.

---

## 6. FEC (PAPER 4.4)

Reed-Solomon over GF(256), polynomial `0x11D`, systematic, **Cauchy** generator so
every k-subset of rows is invertible.

```
  GF(256): x^8 + x^4 + x^3 + x^2 + 1  (0x11D), generator 0x02
  parity row j, data column i:  G[j][i] = 1 / ((128 + j) XOR i)     j < m, i < k
```

`k = 10` data datagrams per group; a group with fewer than 10 available datagrams
uses `k =` that count (PAPER 4.4). Groups **never cross a band boundary**, and
(Decision D6) never cross an `fec_class`, a `layer_id` or a `frame_id` either.

**Parity count (v2).** The paper's 3 / 1 / 0 per ten becomes a *ratio* of the realised
group size, so a short group at the end of a band does not pay three parity blocks
for three data blocks:

```
  m(class, k) = clamp(max(min_parity[class], round(ratio[class] * k / 100)), 0, 4)
```

| measured loss | ratio A/B/C | floor A/B/C |
|---|---|---|
| < 0.1 % | 20 / 0 / 0 | 1 / 0 / 0 |
| nominal | 30 / 10 / 0 | 1 / 0 / 0 |
| > 2 % | 40 / 20 / 10 | 1 / 1 / 0 |

**What the sender actually spends (v2, decision D25): class A parity only.** The
table above is the syntax's range, not the policy. Measured over eight scenarios
spanning 0 to 65 % headroom and 0 to 10 % link loss (`transport/RESULTS.md`), the
class B row cost concealed tiles in **every one** — up to 45 per frame — and the
loss escalation cost tiles wherever it fired. The default policy is therefore

```
  class A: 20 % of k below 0.1 % loss, else 30 %, floor 1 block
  class B: none.   class C: none.   no escalation on loss.
```

The headroom machinery below is implemented and tested but is **not** the default,
because no headroom tested made class B pay. It is kept for the re-run against a
quality metric that should happen before class B is removed from the syntax rather
than merely from the policy:

```
  headroom = 1 - wire_rate / sum(delivery_rate of up paths)      clamped to [0, 1]
  headroom = 0                       if any up path's RTT > 1.5 x its baseline
  room     = headroom >= 0.50        (0.42 to stay enabled: hysteresis)

  class A parity   always, floor 1 block
  if room:   A = 20/30/40 % by loss,  B = 10/20 %,  C = 0/10 %   (the full ladder)
  else:      A = 20/30 % by loss,     B = 0,        C = 0        (no escalation)
```

The `else` branch is the part that matters most. On a link with no headroom the
measured loss is largely **congestion** loss caused by our own bytes, so answering it
with more parity is a positive feedback loop: more parity, more queue drops, more
measured loss, more parity. The simulator caught exactly that — the first
implementation, which kept the paper's loss escalation ungated, spent 23.6 % of the
wire on parity where the fixed policy spent 17.2 % and concealed *more* tiles for it.
The ladder may only climb when there is room to absorb the climb.

`wire_rate` is the sender's own EWMA of bytes put on the wire per frame, which it
knows exactly. `delivery_rate` per path is the integration's BBR estimate (PAPER
4.6), the same one the striper weights with. The feedback packet contributes the
two guards: `path_loss` is the ladder's secondary input, and `path_rtt_ms` drives
the RTT check, because a queue that is filling means the headroom is already spent
whatever the rate estimate says.

> **Two false leads, recorded because both looked like results.** The first version
> of this sweep ran against the v1 deadline controller and its climb dead zone
> (decision D24), which inflated the parity-off column on fast links to 154 concealed
> tiles per frame and made FEC look like a large win there; with D24 fixed the same
> row is 38 and the win disappears. The second was a headroom-gated ladder fitted to
> that inflated control: it enabled class B above 50 % headroom and escalated parity
> on measured loss, which on a link whose loss is mostly congestion loss caused by its
> own parity is a positive feedback loop — it spent 25.9 % of the wire and concealed
> more than either fixed setting. Neither survived measurement.

> **Decision D25.** The paper's ladder keys off measured loss alone, and spends
> class B and C parity unconditionally. Both are wrong here. Parity is extra bytes in
> the same band window as the data it protects, so it delays the next band past its
> deadline; class A parity is cheap enough that the datagrams it recovers outnumber
> the ones it delays, and the class B row is not, at any headroom measured. Loss is
> also the wrong variable to escalate on, because on a loaded link most measured loss
> is congestion loss caused by the parity itself. The policy is therefore class A
> only, at the nominal ratio, with no escalation.
>
> The transport cannot compute `delivery_rate` on its own: the feedback packet
> (section 8) carries loss and RTT but no received-byte count. A v3 feedback with a
> 2-byte per-path received-kilobyte field would close that, and is the cheapest way
> to make the gate self-contained.

`m` travels in the header (`fec_m`), so the receiver knows a group's full membership.

**Group membership (v2, decision D22).** Within one class and band, datagrams are
grouped by **descending payload length** rather than by transmission order. Parity
blocks are padded to the longest member (6.1), so grouping datagrams of similar
length removes most of the padding; it also scatters a group's members across the
band in time, which makes an A-MPDU burst less likely to take more than `m` members
of any one group. Membership is carried by `fec_group` and `fec_idx`, so the receiver
is indifferent to order. Measured effect: blended parity falls from 19.2 % to 17.0 %.

### 6.1 The protected block

The FEC protects **whole datagrams**, headers and tags included, so a recovered
datagram is indistinguishable from a received one and is authenticated normally.

For data slot `i` of a group, with `D_i` the complete on-wire datagram of length
`n_i`, and `L = max_i n_i`:

```
  B_i = u16(n_i) || D_i || 0^(L - n_i)          |B_i| = L + 2
```

Parity block `P_j` for `j` in `0..m-1` is the GF(256) linear combination
`P_j = sum_i G[j][i] * B_i` byte-wise. The parity datagram is:

```
  header (24 bytes, tile_count = 0, fec_idx = k + j, fec_k = k,
          tile_first = tile_first of slot 0, band/class/layer/frame of the group)
  AEAD( u16(L) || P_j )      (L + 2 + 2 bytes of ciphertext)
  tag (16 bytes)
```

A parity datagram's `payload_len` is `L + 4`.

Recovery: with `r >= k` blocks among the `k + m` present (data slots padded back to
`B_i` form, parity slots taken from their payloads), take any `k` of them, build the
`k x k` submatrix of the systematic generator, invert it in GF(256) and multiply.
Each recovered `B_i` yields `n_i` and then `D_i`, which is then decrypted with the
header's own `path_id` / `path_seq`. If fewer than `k` blocks are present the group
is unrecoverable and every missing datagram stays lost.

> **Decision D7.** The paper does not say what the parity covers. Covering the whole
> datagram is the only choice that survives the loss of a header (which carries
> `payload_len`, `tile_first` and `tile_count` — without them a recovered payload is
> unparseable), and it keeps end-to-end authentication: a datagram recovered from
> parity still has to pass its own tag check.

### 6.2 Recovery deadline

A group is attempted once `k` blocks are present **and at least one parity datagram
has arrived**, and abandoned when the band deadline (section 7) passes. There is no
reorder buffer and no other wait.

> **Decision D21 (v2).** Attempting as soon as `k` blocks are present repairs groups
> whose members were merely reordered: at 0 % loss the v1 simulator "recovered"
> 19.9 MB per ten seconds, none of which was needed. Parity is transmitted after its
> group's data on the same path, so requiring a parity block is real evidence that
> something is missing. `fec_m` on the wire additionally lets the decoder size itself
> exactly instead of assuming the maximum.

---

## 7. Receiver: frame ring, placement and the deadline (PAPER 4.3)

### 7.1 Position-addressed placement

There is no reorder buffer (PAPER 4.8). A datagram is placed by
`(frame_id mod 4, layer_id, tile_index)` the moment it decrypts and parses. Arrival
order is irrelevant. A datagram for a `frame_id` older than the ring's oldest live
slot is dropped; a datagram for a newer `frame_id` **advances** the ring, evicting
slot `frame_id mod 4` and resetting its per-tile metadata (this is what "tile (N, t)
lands in slot N mod 4" means once N runs past the ring).

### 7.2 Duplicate suppression

Class A runs are duplicated on both paths (PAPER 4.8). The two copies are
byte-identical except for `path_id`, `path_seq` and `tx_ts` (and therefore the nonce
and the tag). Duplicates are suppressed by the key

```
  data:   (stream_id, frame_id, layer_id, tile_first, frag_idx)
  parity: (stream_id, frame_id, band, tile_class, layer_id, fec_group, fec_idx)
```

A second copy is counted in per-path statistics and then discarded. Placement is
idempotent, so suppression is an optimisation for statistics and FEC accounting, not
a correctness requirement — except for FEC, where a duplicate MUST NOT be counted
twice towards the `k` blocks needed.

### 7.3 Per-tile metadata (4 bytes per tile per slot, PAPER 4.3 item 2)

| bits | field |
|---|---|
| [15:0] | `pose_seq` |
| [23:16] | `age` — frames since this position was last decoded, 0 = fresh, 255 saturates |
| [25:24] | `state` — 0 EMPTY, 1 DECODED, 2 CONCEALED, 3 UNDECODABLE |
| [26] | `late` — arrived after the band deadline |
| [27] | `recovered` — arrived via FEC |
| [31:28] | reserved 0 |

### 7.4 Deadline state machine

Per band, the deadline instant is

```
  band_deadline(N, b) = predicted_display_time(N)
                        - reproject_budget - runtime_margin + deadline_offset
```

`reproject_budget` and `runtime_margin` come from the client (about 3 ms on Pico).
`deadline_offset` is the controller's output, `0 .. 4 ms`, initially 0, and it moves
the deadline **later** (decision D16).

At the deadline for band `b` of frame `N`:

1. every tile of the band with `state == EMPTY` is marked `CONCEALED` and given
   `pose_seq` and `age = age(N-1, t) + 1` from slot `(N-1) mod 4`;
2. the band's feedback packet is generated (section 8);
3. tiles arriving afterwards are still placed, with `late = 1`, and their `state`
   becomes `DECODED`, so they are displayed and are better concealment sources.
   They are counted in `late_tiles` but their bit in the `received_bitmap` stays
   **clear**: a late tile is decoded but never acknowledged (decision D17), so it
   is not used as a prediction reference by either end.

> **Decision D17.** PAPER 4.3 item 5 says "for reference tracking a late tile is
> the same as a received tile". It cannot be, because the sender learns about it
> only if a later cumulative feedback happens to arrive, and any lost feedback
> then leaves the client holding pixels the sender's shadow believes are concealed
> — the silent divergence section 4.5 exists to prevent. Marking late tiles
> decoded-but-unacknowledged keeps the shadow bit-exact under every loss pattern,
> which `tests/transport/test_shadow.cpp` verifies by fuzzing; the cost is that a
> late tile only improves this frame's picture, not the next frame's prediction.

Controller (PAPER 4.3): let `miss(N)` be the fraction of the frame's tiles whose
state at their band deadline was not `DECODED`.

```
  if miss(N) > 0.10:  consecutive_miss += 1  else  consecutive_miss = 0
  if consecutive_miss >= 5 and deadline_offset < 4000 us:
        deadline_offset += 1000 us ; consecutive_miss = 0
  clean_frames += 1 while miss(N) == 0, reset to 0 otherwise
  every 90 clean frames (1 s): deadline_offset = max(0, deadline_offset - 200 us)
```

> **Decision D16.** PAPER 4.3 says the deadline "moves 1 ms earlier ... trading
> latency for fewer holes". Moving it earlier leaves *less* time for datagrams and
> therefore produces *more* holes; the stated trade (accept latency, gain
> completeness) requires moving it **later**, which is what `deadline_offset` does
> here. With the paper's sign the simulator conceals every tile of every frame on a
> 300 Mbit/s link.

> **Decision D8.** "Relaxes 0.2 ms per clean second" is implemented as a counter of
> consecutive frames with **zero** missing tiles, because a frame with one hole is not
> a clean second in any useful sense. The counter is reset, not decremented, on a miss.

### 7.5 Presentation classification

At an arbitrary present time the receiver classifies each tile of the current slot as

* **fresh** — `state == DECODED` and `age == 0`;
* **stale** — `state == DECODED` and `age > 0` (a skip tile, or a late arrival for an
  older frame that was never re-sent);
* **concealed** — `state == CONCEALED`;
* **undecodable** — `state == UNDECODABLE` (directory inconsistent, missing fragment,
  or a reference the client does not hold).

A frame with any concealed tile carries the partial-frame telemetry flag.

---

## 8. Feedback packet (client to server, PAPER 4.4)

One per band, cumulative over the last 3 bands, AEAD protected with the direction-1
subkey of the path it is sent on, with its own 8-byte header as associated data. It
is *not* preceded by a datagram header: the uplink has its own small header.

### 8.1 Feedback header (8 bytes)

| off | size | field | description |
|---|---|---|---|
| 0 | 1 | `version` [3:0] / `flags` [7:4] | flags: bit4 `DEADLINE_MOVED`, bit5 `PATH0_STALLED`, bit6 `PATH1_STALLED`, bit7 `REKEY_REQ` |
| 1 | 1 | `stream_id` | |
| 2 | 2 | `frame_id` | frame of the newest band record |
| 4 | 1 | `band` | band index of the newest band record |
| 5 | 1 | `band_count` | number of band records that follow, 1..3 |
| 6 | 2 | `tiles_in_band` | bit count of a raw bitmap; also validates the geometry |

### 8.2 Band record (20 bytes + bitmap), newest first

| off | size | field |
|---|---|---|
| 0 | 2 | `frame_id` |
| 2 | 1 | `band` |
| 3 | 1 | `flags`: [1:0] `bitmap_mode`, [2] `complete`, [3] `deadline_missed`, [7:4] reserved 0 |
| 4 | 4 | `rx_ts_first` (client clock, us) |
| 8 | 4 | `rx_ts_last` |
| 12 | 2 | `decode_us` |
| 14 | 2 | `conceal_tiles` |
| 16 | 2 | `late_tiles` |
| 18 | 1 | `fec_recovered` |
| 19 | 1 | `fec_failed` |
| 20 | var | bitmap, per `bitmap_mode` |

`bitmap_mode`:

* **0 `RAW`** — `ceil(tiles_in_band / 8)` bytes, bit `i` (LSB first within a byte)
  set iff tile `i` of the band was received before its band deadline *and* decoded.
  51 bytes for a 408-tile band.
* **1 `ALL`** — zero bytes; every tile of the band was received.
* **2 `RLE`** — `u8 nruns`, then `nruns` records of `u16 start, u8 len`; each record
  is a run of consecutive **missing** tiles. Legal only with `CAP_RLE_FEEDBACK`.
* **3** — reserved, MUST NOT be sent.

The generator picks the smallest legal encoding: `ALL` if nothing is missing, else
`RLE` if it is shorter than `RAW`, else `RAW`.

> **Decision D9.** The paper's feedback is "about 100 bytes ... cumulative over the
> last 3 bands", but three raw 51-byte bitmaps alone are 153 bytes, and the full
> packet with the paper's fields is 225 bytes, giving 0.97 Mbit/s of uplink, not the
> stated 0.4 Mbit/s. `bitmap_mode` restores the paper's number on average: a clean
> band costs 20 bytes, a band with two loss bursts costs 26. The simulator measures
> and reports the real uplink rate. The 225-byte worst case is still under any MTU.

A tile bit is set only if the datagram decrypted **and** the tile bitstream decoded
without error **and** it arrived before the band deadline, so a corrupt-but-delivered
tile and a late tile both count as lost (PAPER 4.4, decision D17). Because the bitmap
is re-derived from the live ring each time a band is repeated in a cumulative packet,
every copy of a band's report carries the same bits: losing one feedback packet can
never change what the sender concludes. The
transport library sets the bit on successful placement and offers
`Receiver::mark_tile_undecodable()` for the decoder to clear it before the deadline.

### 8.3 Trailer (4 bytes)

| off | size | field |
|---|---|---|
| 0 | 1 | `path_loss[0]` — loss fraction, Q8 (`round(loss * 255)`) |
| 1 | 1 | `path_loss[1]` |
| 2 | 1 | `path_rtt_ms[0]` — 255 saturates |
| 3 | 1 | `path_rtt_ms[1]` |

Loss is measured from `path_seq` gaps over a 1-second window, per path, **before**
FEC recovery. RTT is the sender's echo estimate; the client fills what it knows from
its own ping/pong and the server overrides with its own measurement when it disagrees.

---

## 9. Reference eligibility (PAPER 4.5, 6.6)

This is the rule the encoder's client shadow implements and the one the receiver's
real state must match.

**Exactness.** A tile position `t = (row, col)` of frame `M` is *exact* if the sender
knows, from feedback, that the client holds bit-exact pixels there:

```
  exact(M, t) =
      state(M, t) == RECEIVED                            -- feedback bit set
   or (state(M, t) == CONCEALED                          -- feedback bit clear,
       and for all t' in N3x3(t): exact(M-1, t'))        -- band feedback did arrive
```

A position whose band has no feedback yet is `UNKNOWN` and is never exact. The
recursion terminates at the edge of the 8-frame shadow history, where `exact` is
false. Grid edges: `N3x3` is clipped to the grid, so a corner has 4 neighbours.

> **Decision D10.** The paper says a concealed tile "is a legal reference" because
> the concealment warp is deterministic and the encoder replays it. That is only true
> if the *source* pixels of the warp are themselves exact, and the warp reads across
> tile borders exactly as prediction does. The recursive definition above is the
> sharpened rule; it is what makes the shadow provably equal to the receiver's real
> state, which the fuzz test in `tests/transport/test_shadow_equivalence.cpp` checks
> under arbitrary loss patterns.

**Reference choice.** The chosen `ref_delta` is written into the tile's directory
entry (v2), so tiles with different references share a datagram. For tile `t` of
frame `N`:

```
  for d in 0, 1, 2:
      M = N - 1 - d
      if M >= 0 and for all t' in N3x3(t): exact(M, t'):
            return ref_delta = d
  return ref_delta = 3          -- intra
```

That is "the newest frame in {N-1, N-2, N-3} whose 3x3 neighbourhood is fully
acknowledged" verbatim. With no feedback for four frames every tile goes intra, at
the capped size — a QP jump, not a stall.

**Concealment marking.** When the sender applies a band's feedback
(`ClientShadow::apply_feedback`), every tile of that band whose bit is clear is
marked `CONCEALED` and the sender records the concealment source
`(M-1, t)` so the encoder's mirror ring can replay the identical warp. Tiles reported
`late` in a later feedback are upgraded from `CONCEALED` to `RECEIVED` — a late tile
is a received tile for reference purposes (PAPER 4.3 item 5) — but the upgrade only
affects frames still inside the 8-frame history.

The rolling intra refresh of 1/180 of tiles per frame (PAPER 6.6) is a safety net on
top and is the encoder's business, not the transport's; the transport reports which
positions have been non-exact longest via `ClientShadow::staleness()`.

**The receiver's half of this is one decoder call.** The rule above is only true if
the client's concealment is the same deterministic `WARP_SKIP` replay the sender's
shadow assumes, so a receiver has to be able to tell its decoder which tiles it did
not get before it hands over the frame. Both decoders offer that:
`nxvc_decoder_set_lost_tiles()` on the CPU reference and
`nxvc_vk_decoder_mark_missing()` on the Vulkan one, in either case for the next frame
only. `tests/vk-decoder/conformance`'s loss arm drives the two side by side over 100
frames of random loss, mono and stereo, and requires them byte-identical; that is
what makes "the encoder replays the identical warp" a checked property of this
codebase rather than a claim about it.

---

## 10. Multipath (PAPER 4.8)

* **Class A** runs are duplicated on every up path when the combined delivery rate
  allows it. The library's rule: duplicate iff `n_up >= 2` and
  `bytes_this_band * (1 + dup_share) <= sum_of_path_capacities * band_period`, where
  `dup_share` is the class A share of the band's bytes. Duplication is decided once
  per band, not per datagram, so a band never mixes duplicated and non-duplicated
  class A runs.
* **Class B and C** runs are split by weighted round robin (a deficit round robin over
  bytes) with weights equal to the measured per-path delivery rate.
* **Stall.** A path stalls when its RTT sample exceeds 3x its minimum-filtered
  baseline, or when 20 ms pass with no datagram received on it while the other path is
  flowing. Its weight goes to zero at the next band boundary. Probing resumes with
  class C runs only, at 1/8 weight, until two consecutive bands arrive with RTT under
  1.5x baseline.
* **No reorder buffer.** Tiles are position addressed; the only wait is the FEC
  group's, bounded by the band deadline.

---

## 11. Telemetry (PAPER 4.9)

Carried on the wire: `tx_ts` and `enc_us` per datagram, `render_finish_ts` in the
pose header, `rx_ts_first` / `rx_ts_last` / `decode_us` per band record in the
feedback. Everything else is local.

The library keeps, per band and per frame, a `nxt::BandStamps`:

```
  render_finish_ts, encode_finish_ts, first_tx_ts, last_tx_ts      (server clock)
  first_rx_ts, last_rx_ts, decode_finish_ts, deadline_ts           (client clock)
  clock_offset_us  (per path, supplied by the integration)
```

and derives `queue_us = first_tx - encode_finish`, `air_us = first_rx - first_tx -
offset(path)`, `spread_us = last_rx - first_rx`, `decode_us`, `margin_us = deadline -
decode_finish`. `nxt::Telemetry` aggregates p50/p99 over a 1-second window.

Because the clock offset has 0.3 to 1 ms of error (PAPER 4.11), the deadline
controller of section 7.4 uses only client-clock quantities. Cross-clock values are
telemetry, never control input.

---

## 12. Failure modes the library must handle (PAPER 4.11)

| Condition | Library behaviour |
|---|---|
| Header version mismatch | drop, count `bad_version` |
| `caps` bit not negotiated | drop, count `bad_caps` |
| Tag check failure | drop, count `auth_fail`; never partially place |
| Replay (counter behind window) | drop, count `replay` |
| Directory sum != plaintext length | drop whole datagram, count `bad_directory` |
| `tile_first + tile_count > tiles_per_frame` | drop, count `bad_range` |
| `mark_tile_undecodable()` called with `row >= rows`, `col >= cols` or `layer >= layers` | reject, change nothing, return false, count `bad_range` |
| Run crosses a tile row | drop, count `bad_range` |
| `frame_id` older than the ring | drop, count `stale_frame` |
| FEC group with < k blocks at the deadline | abandon, count `fec_failed` |
| No feedback for 4 frames | shadow reports every tile non-exact, encoder goes intra |
| Both paths stalled | sender keeps packetizing into the send ring; the integration's socket layer backpressures. The library never blocks. |

---

## Appendix A. Decisions this document makes that the paper leaves open

| # | Decision |
|---|---|
| D1 | The header's 14 unspent bits become `frag_count` completion, `tile_class` (2), `band` (3), `pose_hdr` (1) and `caps` (8), reaching the stated 192 bits. |
| D2 | The tile directory keeps 4 bytes with 8 reserved bits, matching the paper's overhead arithmetic. |
| D3 | A run is homogeneous in stream, frame, layer, `ref_delta`, class, band and tile row. |
| D4 | The AEAD nonce uses the **extended 64-bit** per-path counter, not the 14-bit wire field; the extension rule is specified in 4.2. |
| D5 | `max_tile_bytes` is 1312 at a 1400-byte MTU with FEC on, not the paper's 1372: the paper omits the AEAD tag and the parity datagram's own overhead. |
| D6 | FEC groups never cross a band, class, layer or frame. |
| D7 | FEC parity covers the **whole datagram** (header, ciphertext, tag) prefixed by its length and zero padded to the group maximum. Recovered datagrams are authenticated normally. |
| D8 | "0.2 ms per clean second" is counted in consecutive zero-miss frames, reset (not decremented) by any miss. |
| D9 | The feedback bitmap gets three encodings (`RAW`, `ALL`, `RLE`) so the average packet meets the paper's 100-byte / 0.4 Mbit/s figure; the raw worst case is 225 bytes / 0.97 Mbit/s. |
| D10 | Reference exactness is **recursive**: a concealed tile is exact only if the 3x3 neighbourhood it was warped from is itself exact. Without this the shadow is not equal to the receiver's state. |
| D11 | Parity datagrams are marked by `tile_count == 0` as well as `fec_idx >= fec_k`, so a receiver can classify a datagram without reading the FEC fields. |
| D12 | `payload_len` excludes the 16-byte tag; the on-wire datagram is `24 + payload_len + 16`. |
| D13 | The oversize policy is a three-way hook (`kReject`, `kDropTile`, `kFragment`) rather than the paper's implicit "the encoder never does this". |
| D14 | Feedback packets are AEAD protected with a direction-separated subkey (`dir = 1`), which the paper does not state; without it the uplink is a trivial forgery channel into the reference model. |
| D15 | Superseded by D17: no late-tile upgrade happens at all. |
| D16 | The deadline controller moves the deadline **later**, not earlier as PAPER 4.3 words it, because that is what "trading latency for fewer holes" means. |
| D17 | A tile arriving after its band deadline is decoded and displayed but never acknowledged, so the sender's shadow is provably equal to the client's reference state under any loss pattern. |
| D18 | The upstream (feedback) AEAD nonce counter is `frame_ext * bands + band`, derived from the feedback header rather than carried on the wire. |
| D19 | **v2**: `ref_delta` and `tile_class` move from the header into the tile directory and `layer_id` shrinks to 2 bits; the freed bits become `fec_class` (2), `fec_m` (3) and one reserved bit. The header stays 192 bits. |
| D20 | **v2**: runs pack to the payload budget, breaking on a tile-class change only once the run holds 24 tiles. The datagram's `fec_class` is the strongest class it carries. The paper's 20-tiles-per-run figure is arithmetically impossible at a 1400-byte MTU; the ceiling is 14 and the measured mean is 11.3. |
| D21 | **v2**: FEC recovery waits for a parity datagram, not merely for `k` blocks, so reordered groups are never repaired needlessly. `fec_m` is on the wire to make the group's membership known. |
| D22 | **v2**: FEC group membership within a class and band is by descending payload length, which removes parity padding waste and scatters a group across the band in time. |
| D23 | **v2**: parity per group is a ratio of the realised `k` (30 / 10 / 0 percent nominal) with a floor of one parity block for class A, instead of a fixed 3 / 1 / 0 per group of any size. |
| D24 | **v2**: the deadline relaxes 250 us only when the worst arrival margin over a full 180-frame window keeps 1 ms of slack, with a one-window hold after any change. The v1 rule (90 consecutive zero-miss frames) oscillates on a link with headroom. |
| D25 | **v2**: the shipped parity policy is class A only at the nominal ratio, with no class B or C and no loss escalation. Measured over eight scenarios from 0 to 65 % headroom, the class B row cost concealed tiles in every one and the escalation cost tiles wherever it fired. `FecPolicy::set_from_headroom` and `Sender::measured_headroom` remain implemented and tested for the quality-metric re-run, but are not the default. |
| D24 | **Spec reconciliation** (`spec/annex-d-inter-decisions.md`): `res_level == 3` is reserved, not "DC-plane only"; `ref_delta` and `dir_qp` are advisory copies of authoritative bitstream fields with defined disagreement rules; `cols == eyes * cols_per_eye` and a picture is one eye; the 26-byte `pose_header` is the format's only pose layout and this document owns it; `frame_id == frame_number`; `dir_len` bounds a fragment. |
