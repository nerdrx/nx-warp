# Follow-up: one dispatch over the coded-tile list

Same Pico, fixture and sampling method as the parent experiment. Three rounds
of full refresh, per-tile rectangles, then a single 3D dispatch. All nine
processes exited 0. Each row excludes two startup frames (14 observations).
Binary and fixture hashes are in `provenance.log`; the per-tile binary is the
previous experiment. The single-dispatch binary also supplies the full arm.

| Round | Full GPU p50 ms | Rectangles GPU p50 ms | Single GPU p50 ms | Full call p50 ms | Rectangles call p50 ms | Single call p50 ms |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 7.163 | 4.738 | 6.011 | 9.703 | 7.202 | 8.424 |
| 2 | 6.127 | 6.200 | 5.986 | 9.312 | 8.204 | 8.168 |
| 3 | 4.559 | 3.734 | 5.425 | 7.238 | 6.171 | 8.128 |

The three arms (A full refresh, B per-tile rectangles, C single dispatch) have
mixed timings across rounds; the single dispatch removes the variable per-tile
dispatch count but does **not** show a consistent latency improvement. It loses
to full refresh in round 3. Retain as opt-in only; no default change or live
FPS claim. Fixed order and thermal drift remain confounders.

The current implementation uses the existing coded-tile list as the dispatch
Z dimension, 8x8 workgroups per 64x64 tile, with independent clipped chroma
coordinates. Full and dirty R8/R16 views match after every frame of this stereo
fixture on Pico. The clipped no-direction 194x130 mono fixture also passes; the
default directional mono fixture still fails before the dirty arm, including
with the original decoder binary. The directional cause remains unresolved.

Historical `dispatches` values in these logs omit compose, MATGEN, WRITEBACK,
and ASSEMBLE dispatches. The source counter was corrected after these runs.
Do not interpret the recorded absolute counts as complete pipeline totals.
