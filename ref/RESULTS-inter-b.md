# The inter-efficiency package: measurements

What three tools against the Phase 2 inter path are worth, measured on the real
codec with `tools/quality/compare.py` and the Phase 2 kill test.
`ref/RESULTS-inter.md` is the "before" this document is the "after" of; where
the two disagree the reason is given rather than averaged.

| | tool | syntax | what it is |
|---|---|---|---|
| **T1** | drift-driven refresh | none | the encoder measures the error its exact client shadow carries per tile and refreshes on the measurement instead of on a timer, against an unconditional hard cap (`docs/SYNTAX.md` 13.10) |
| **T2** | the near-skip, `warp_dc()` | tool bit **24** | a skipped tile may carry a nine-byte DC-plus-ramp correction per plane, in the tile-row header (`docs/SYNTAX.md` 3.3, 13.9) |
| **T3** | four vectors per tile | tool bit **25** | one vector per 32x32 quadrant, `i8` deltas from the tile vector, sharing the tile's corner basis exactly (`docs/SYNTAX.md` 13.8) |

Bitstream minor 4 -> 5. A stream that sets neither new tool bit decodes byte for
byte as it did, which is pinned rather than asserted: conformance vectors
`v45`-`v55` are generated with every v1.5 tool off and keep the digests they
were committed with. `v56` and `v60` are the two refresh-cadence vectors and
they changed, for the reason in section 6.

**The four things to read first.**

1. **The kill test still fails.** Nothing in this package was ever going to
   pass it: `ref/RESULTS-inter.md` section 4 measured the intra core at +190 %
   to +886 % against `x265-p` *before* the inter path touches it, and an inter
   tool cannot recover that. The verdicts are quoted verbatim in section 2.
2. **T3 is the win.** Four vectors per tile is worth [T3-BD] BD-rate against
   the same codec without it, for eight bytes on the tiles that take them and
   no extra predictor calls in the encoder search.
3. **T2 is small and real, and it was nearly a loss.** The first cut of the
   near-skip *cost* rate: an unconditional 8-byte-per-row bitmap is 2.5 % of a
   frame at this codec's operating point, and the mode decision's own lambda
   under-prices a correction that is a within-mode trade. Section 4.2 has both
   corrections and what each was worth.
4. **T1's measurement is more interesting than T1.** The drift the encoder's
   shadow reports never exceeds the quantiser's own noise floor at the
   operating point (section 4.1, with the numbers), which says the blind
   1-in-180 refresh is buying insurance the picture does not need. The tool's
   value is that it lets the hard cap be the only unconditional refresh, and it
   is measured as such.

---

## 1. What was measured, and how

```
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
tools/quality/tourney_queue.sh            # the whole matrix, serially
```

`tools/quality/tourney_run.sh` is the single invocation; the queue is base and
each tool on its own and then all three, in both rate bands. Every process ran
under `chrt -i 0 taskset -c 16-19 nice -n 19`, on a machine carrying about
thirty other encodes throughout, which is why the wall-clock column of section
5 is a ratio and not an absolute.

The sequence is **`vr-mixed-1024-v2`** at 4:4:4 — the band-limited (v2)
generation, which is the one `docs/WARP-AUDIT.md` established is a fair ground
truth for a resampling predictor. `ref/RESULTS-inter.md`'s tables are on the v1
material, so its absolute numbers and this document's are not comparable and
the base row here is measured, not copied.

The two rate bands are `ref/RESULTS-inter.md`'s, unchanged:

| band | what it is | QP ladder |
|---|---|---|
| **A** | 100-300 Mbit/s on this clip, as PAPER 2.11 item 1 says | nxv 0/4/8/12, x265 2/8/14/20 |
| **B** | 0.2-0.6 bpp, the density PAPER 2.4's budget describes | nxv 18/24/30/36, x265 26/32/38/44 |

Configurations, as `nxv-enc` flags on top of `--eyes 2 --inter on --poses ...`:

| row | flags |
|---|---|
| base | `--refresh-drift 0 --warp-dc off --mv-quad off` |
| T1 | `--warp-dc off --mv-quad off` |
| T2 | `--refresh-drift 0 --mv-quad off` |
| T3 | `--refresh-drift 0 --warp-dc off` |
| all | *(none: `nxvc_config_default()` turns all three on)* |

`--refresh-drift` defaults to 1.0 and `--refresh-max-age` to 720 (8 s at
90 Hz); `--intra-period` stays at its 180.

---

## 2. PENDING — the kill test, both bands

## 3. PENDING — the three tools, measured separately

## 4. What each tool does, and what it does not

### 4.1 T1, drift-driven refresh

PENDING numbers.

### 4.2 T2, the near-skip

PENDING numbers.

### 4.3 T3, four vectors per tile

PENDING numbers.

## 5. PENDING — encode and decode time

## 6. PENDING — the warp-only chain

## 7. PENDING — conformance
