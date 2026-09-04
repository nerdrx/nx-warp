# Entropy and context modelling: measurements

What the syntax v1.5 entropy package is worth, tool by tool, measured with
`tools/quality/compare.py` on the **v2 (band-limited)** sequences in
`$NXQ_SCRATCH/seq` -- `vr-mixed-1024-v2`, 2048x1024 side-by-side, 90 fps,
`synthetic:mixed:seed1:v2-bandlimited-ss4`. Every process ran under
`chrt -i 0 taskset -c 28-31 nice -n 19`; result files are under
`$NXQ_SCRATCH/results/tourney-ctx-a/`. The first 12 frames of the 36-frame
sequence are used, which for intra is 12 independent measurements of the same
content class and for inter is a complete refresh cycle plus its P frames.

The package is four items, and they are reported in the order they were
measured:

| # | tool | bit | what it is |
|---|---|---|---|
| 1 | `CTX_V3` | 24 | 27 coefficient contexts: neighbour-conditioned `CBF`/`LAST`, a `LEVEL` context for scan position `LAST`, a `LEVEL` context for the DC term of a DC plane (SYNTAX.md 9.8) |
| 2 | `VEC_ENT` | 25 | the tile vector is the payload's first coding unit instead of two raw header bytes (SYNTAX.md 9.9) |
| 3 | — | none | encoder only: a table set is transmitted only when it pays for itself, and each tile's set is chosen against the tables in force (SYNTAX.md decision 57) |
| 4 | — | none | two experiments: 12-bit probabilities and 16 lanes. **Both rejected**, with the numbers in section 5 |

Item 3 needs no tool bit and no decoder change, which is the whole point of it:
`tables_present` already says which sets a frame transmits, and a decoder that
never saw the old behaviour cannot tell the difference. It is therefore
measured as the first cumulative row, so that items 1 and 2 are measured on top
of an encoder that is not wasting header bytes.

---

## 0. Placeholder

(Filled in by the measurement runs; see section 1.)

---

## 5. The two experiments

### 5.1 12-bit probabilities: measured, rejected, and what to do with it

The probability precision is 10 bits (`M = 2^10`, SYNTAX.md 9.5), so every
context row is a 16-way partition of 1024 slots. The question is what that
quantisation costs against exact probabilities, and how much of it 12 bits
would recover.

Measured directly on the reference encoder's own per-tile symbol histograms
(`nxvc_debug_tile_histograms`, the same hook `nxv-gentables` uses), on the left
eye of frame 0 of `vr-mixed-1024-v2` 4:4:4 at the shipped default settings with
`CTX_V3`, aggregated over all 256 tiles and all 29 contexts. "ideal" is the
empirical entropy of the symbol stream; each column is the cost of coding the
same symbols with a row normalised to that precision by the procedure in 9.4.

| QP | ideal | 10-bit | 12-bit | 16-bit | 10 -> 12 |
|---|---|---|---|---|---|
| 8 | 68 188 B | +0.304 % | +0.071 % | +0.004 % | **-0.233 %** |
| 16 | 41 379 B | +0.486 % | +0.116 % | +0.007 % | **-0.369 %** |
| 24 | 22 353 B | +0.888 % | +0.213 % | +0.013 % | **-0.670 %** |
| 32 | 12 005 B | +1.664 % | +0.410 % | +0.025 % | **-1.234 %** |

The shape is the expected one: quantisation error is a fixed number of bits per
context row, so it matters more the fewer symbols there are to amortise it
over, and the payload shrinks by 6x across this ladder.

**What 12 bits would cost a decoder: nothing.** The decode step is
`slot = x & (M-1)`, a 4-step branchless binary search over the same 16
cumulative frequencies, one multiply and two adds. `cum` values reach 4096
instead of 1024, which still fits the `uint` the Pass A table is already made
of, so `s_cum` does not grow by a byte and the search does not gain a step.
On the encoder the renormalisation bound `x >= (f << 22)` becomes `f << 20`,
which still holds for every legal `f <= 4095`.

**What it would cost as a tool bit: a lot.** A per-stream probability precision
makes the mask and the shift in the innermost loop of Pass A runtime values
rather than constants, and it invalidates all three built-in table families and
every conformance vector at once.

**Verdict: not adopted, and recorded as the thing to take at the next
incompatible revision** (SYNTAX.md Appendix A decision 59). At the Phase 1
operating band, QP 0 to 24, it is 0.23 % to 0.67 % -- smaller than either of
the tools in this package -- and it is free exactly when the format is already
breaking.

### 5.2 16 lanes per tile: measured, rejected

`nsub_log2` already permits 16 and 32 lanes; no syntax change was needed to
measure it. One 2048x1024 4:4:4 frame, `--ctx v3`, everything else default:

| QP | 8 lanes (`--nsub 3`) | 16 (`--nsub 4`) | 32 (`--nsub 5`) | the encoder's own choice (`auto`) |
|---|---|---|---|---|
| 8 | 196 300 B | 208 742 B (+6.3 %) | 236 244 B (+20.4 %) | **189 466 B** |
| 16 | 117 298 B | 131 576 B (+12.2 %) | 161 560 B (+37.7 %) | **108 822 B** |
| 24 | 70 560 B | 85 786 B (+21.6 %) | 116 368 B (+64.9 %) | **60 242 B** |
| 32 | 45 736 B | 61 002 B (+33.4 %) | 92 376 B (+102.0 %) | **34 152 B** |

Against the encoder's own per-tile choice, 16 lanes is +10.2 % at QP 8 and
+78.6 % at QP 32.

The reason is not subtle: a lane costs 4 bytes of initial rANS state in every
tile that uses it, and the encoder already spends lanes only where the payload
can carry them -- at this band it averages 1 to 3 lanes per tile, not 8. More
lanes is more flush on a payload that is not getting any shorter.

The GPU side offers nothing to trade against that. The shared table is indexed
by **table set**, not by tile, so halving the tiles per workgroup does not
shrink it; a 16-lane cluster needs `subgroupSize >= 16`, which forces the LDS
fallback read-pointer path on every ICD with a subgroup of 8 (lavapipe today);
and the reference already caps its automatic choice at 8 for the same reason
the table above shows.

**Verdict: not adopted** (SYNTAX.md Appendix A decision 60). 8 lanes stays the
v1 configuration and the number a GPU decoder assumes as the maximum.
