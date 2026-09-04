# ADR-0003: Interleaved rANS, eight lanes per tile, static per-frame tables

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 1.6, 3.2.2, reconciled in 6.3
- **Affects**: `ref/entropy`, `vk/decoder/passA`, `vk/encoder` (E4)

## Context

The entropy coder must decode with 64 lanes in lockstep, with no divergence in the hot loop and no
serial dependency longer than symbol count divided by lane count. Three families were evaluated
(paper 1.6, all figures design estimates):

| Scheme | Decode cost per symbol | Parallelism | Bits vs CABAC class | Patent status |
|---|---|---|---|---|
| Interleaved rANS, static tables | about 10 ops + 1 LUT | N independent substreams in lockstep | +3 to 6 percent | public domain by the author, with a Microsoft caveat |
| Adaptive binary (CABAC / M-coder class) | about 15 ops, serial per bin | one bin at a time per stream | reference | H.264 CABAC largely expired, HEVC/VVC context designs live |
| Bit-plane significance plus Golomb (EZW/SPIHT, VC-2 class) | about 4 ops per bit-plane pass | per block, all lanes | +10 to 15 percent | expired, VC-2 royalty free |

Sections 1 and 3 then disagreed on substream count. Section 1 proposed 1 to 32 substreams chosen per
tile so each lane decodes about 128 symbols. Section 3 designed the decoder around a fixed eight
lanes, so eight tiles fill one 64-wide Adreno wave and the same code runs on wave32, wave64, wave128
and lavapipe's width of eight.

## Decision

Interleaved rANS with static per-frame tables. **v1 fixes eight lanes per tile.** The `nsub_log2`
header field stays in the syntax so v2 can vary it under a tool bit.

- 32-bit state, `L = 2^16`, 16-bit renormalisation, 10-bit probability precision `M = 2^10`.
- Decode step: `slot = x & 1023; s = lut[ctx][slot]; x = freq[ctx][s] * (x >> 10) + slot - cum[ctx][s];`
  renormalise with `x = (x << 16) | read_u16()` when `x < L`. All of it fits in uint32: no int64, no
  division in the decoder.
- One interleaved byte stream, no offset table. Each lane decides whether it renormalises this step; a
  `subgroupBallot` masked to the 8-lane cluster plus a bit count gives each lane its offset from the
  shared read pointer, and the cluster advances the pointer by the popcount. Clusters are derived from
  `gl_SubgroupInvocationID & ~7` and never straddle a subgroup, since 8 divides 8, 32, 64 and 128.
- Alphabet: `CBF` (2 symbols), `LAST` (16 symbols), `LEVEL` (16 symbols, 0..14 plus ESC with
  Exp-Golomb order-3 raw bits). Signs and other raw bits are bypass symbols on the same state.
- Tables: a table set is 12 contexts x 16 symbols of 10-bit frequencies, transmitted as 5-bit
  log-domain deltas from the built-in default, about 120 bytes per set, up to 8 sets per frame. Every
  set index has a built-in default so `tables_present = 0` is a valid, loss-tolerant frame.
- Flush cost is about 16 bytes per tile (8 states x 4 bytes, of which roughly 2 bytes per state are
  real overhead because the final state carries useful bits).
- Fallbacks: `ENT_OFFSET_TABLE` (per-substream byte ranges, about 8 extra bytes per tile) where
  ballot is unavailable or slow, and `ENT_BITPLANE` as the Lite-profile coder that needs no tables and
  no LDS LUT.

Fixed state width and fixed probability precision are deliberate: they keep the construction inside
Duda's published rANS and outside the 2022 Microsoft claims on selective state-precision switching,
pending the review in ADR-0017.

## Consequences

- Estimated 3 to 6 percent more bits than a CABAC-class coder on the same coefficient statistics
  (paper 1.6 says within 5 percent of a CAVLC-class coder and roughly 8 percent behind CABAC). The
  pose-warp predictor is expected to more than pay for it.
- Adreno subgroup ballot availability and cost becomes the load-bearing assumption of the whole
  layout. It is a Phase 0 gate check (K4), and the offset-table fallback exists but costs bytes
  (paper 1.12 risk 1, 3.12).
- Adaptivity is given up. rANS encodes in reverse, so an adaptive model would force the encoder to
  record the model trajectory forward and replay it backward, a full second pass with per-symbol
  state, and per-lane adaptation on the decoder loses most of the benefit anyway (paper 1.6).
- The encoder gains a histogram dispatch per frame (one atomic-add pass after quantisation) and a
  backwards encode with the same ballot trick writing bytes from the end of the tile's slot.
- LDS budget: 8 KB of cumulative-to-symbol tables per Pass A workgroup, which is what limits Pass A
  occupancy rather than registers (paper 3.2.2).

## Alternatives considered

- **CABAC or any adaptive binary coder.** Serial per bin; would need 64 independent streams to fill a
  wave, and HEVC/VVC context designs are patent-live. Rejected.
- **Bit-plane significance coding.** Kept as `ENT_BITPLANE` for Lite, because it needs no tables and
  no LUT, but its estimated 10 to 15 percent cost on fovea tiles is real.
- **Per-tile adaptive substream count (1 to 32).** Rejected for v1 in favour of a fixed 8 so that one
  binary runs on every subgroup width. The syntax field survives for v2.
- **Adaptive probability models per frame.** Rejected, see above. Static per-frame tables built from
  the frame's own histogram recover most of the gain.

## References

- Duda, asymmetric numeral systems (published without patent); Giesen, interleaved rANS
- Paper 1.6 (entropy coding), 3.2.2 (Pass A), 6.3 (reconciliation), 1.9 and 5.7 (patent notes)
- ADR-0010 (integer-only path), ADR-0017 (FTO scope)
