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

## 6. The warp-only chain (PAPER.md 2.11 item 2)

`ref/warp_chain.py` raises the `WARP_SKIP` gate above anything real content
produces and pushes the refresh period past the clip, so frame 0 is an
ordinary intra frame and every frame after it is nothing but the pose warp of
its predecessor. **A near-skip is still a skipped tile**, so a `warp_dc()`
correction is still a warp-only chain -- the tool is measured through this test
rather than excluded from it, and `--enc-arg=--warp-dc --enc-arg=off` turns it
off.

`vr-mixed-1024-v2` 4:4:4, 36 frames, QP 8, and `vr-turn-256-v2` 4:4:4, 12
frames:

| sequence | config | frame 1 | last frame | decay | frames above 35 dB | verdict |
|---|---|---|---|---|---|---|
| `vr-mixed-1024-v2` | base | 28.71 dB | 19.12 dB | -9.59 dB | **0** | **FAIL** |
| `vr-mixed-1024-v2` | all three | 28.79 dB | **21.22 dB** | **-7.57 dB** | **0** | **FAIL** |
| `vr-turn-256-v2` | base | 30.31 dB | 22.94 dB | -7.37 dB | **0** | **FAIL** |
| `vr-turn-256-v2` | all three | 30.32 dB | **23.30 dB** | **-7.02 dB** | **0** | **FAIL** |

The verdict is unchanged and was never in reach: the chain starts below the bar
on this material, which `docs/WARP-AUDIT.md` established is a property of the
generator's ground truth and of the content it deliberately contains, not of
the predictor.

**What did change is the slope, which is the half of item 2 the audit left
standing.** Over 35 warped frames on `vr-mixed-1024-v2` the chain loses 7.57 dB
instead of 9.59, and the last frame is **2.10 dB** better:

| warped frame | 1 | 5 | 10 | 20 | 35 |
|---|---|---|---|---|---|
| base | 28.71 | 25.08 | 24.73 | 19.68 | 19.12 |
| all three | 28.79 | 25.42 | 25.39 | 21.48 | 21.22 |

**All of it is T2.** Run again with only `--warp-dc off` and the chain is the
base row to the hundredth of a dB -- 28.71 / 19.12, decay 9.59 -- because in a
warp-only chain no tile is coded, so `mv_quad` never appears, and the refresh
period is past the clip, so the drift rule never fires. Nine bytes on a
skipped tile buy 2 dB of chain, which is what item 2 says the refresh rate
would otherwise have to buy.

That is the strongest single result in this package and it is not the one the
rate tables show, because a chain is the regime where the codec leans hardest
on the warp and the rate tables average over frames where it does not. It is
also the regime `ref/RESULTS-inter.md` section 4 says the codec spends two
thirds of its tiles in at the paper's own operating density.

## 7. PENDING — conformance
