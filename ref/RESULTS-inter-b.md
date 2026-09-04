# The inter-efficiency package: measurements

What three tools against the Phase 2 inter path are worth, measured on the real
codec with `tools/quality/compare.py` and the Phase 2 kill test of
`ref/RESULTS-inter.md`. That document is the "before" this one is the "after"
of, and where the two overlap the disagreement is explained rather than
averaged.

The three tools, each measured on its own and then together:

| | tool | syntax | what it is |
|---|---|---|---|
| **T1** | drift-driven refresh | none | the encoder measures the error its exact client shadow carries per tile and refreshes on the measurement instead of on a timer, with an unconditional hard cap (`docs/SYNTAX.md` 13.10) |
| **T2** | the near-skip, `warp_dc()` | tool bit **24** | a skipped tile may carry a nine-byte DC-plus-ramp correction per plane, in the tile-row header (`docs/SYNTAX.md` 3.3, 13.9) |
| **T3** | four vectors per tile | tool bit **25** | one vector per 32x32 quadrant, as `i8` deltas from the tile vector, sharing the tile's corner basis exactly (`docs/SYNTAX.md` 13.8) |

Bitstream minor 4 -> 5. A stream that sets neither new tool bit decodes byte for
byte as it did: conformance vectors `v45`-`v55` are unchanged, and `v56` changes
only because T1 rewrote the refresh clock (section 6).

**PLACEHOLDER — the measurement tables are filled in from the queue in
section 7 as it finishes.**
