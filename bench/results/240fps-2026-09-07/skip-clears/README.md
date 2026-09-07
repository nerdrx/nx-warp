# ATLAS skipped tile clear comparison

Each JSON was produced from its paired raw log with `scripts/summarize-vkdec.py --warmup 2`. Runs use the same binary, fixture, device, and dirty ATLAS view path. `clear` is the forced-clear control (`NXVC_VKD_ATLAS_FORCE_CLEAR=1`); `skip` leaves the candidate clear elision enabled.

The retained optimization omits coefficient/unit-length fills only for absent
ATLAS tiles (presence bit 0). Coded PLANAR tiles and every PICTURE/non-ATLAS
clear remain intact. The Pico and host logs show full plus forced-clear and
dirty plus elided-clear R8/R16 comparisons for static mono, changing mono, and
stereo fixtures; the view is recreated at frame 2 and every frame is checked.
`static-dispatches.log` records the all-skip, compose-only frames.

| Pair | Control GPU p50/p95 ms | Candidate GPU p50/p95 ms | Control wall p50/p95 ms | Candidate wall p50/p95 ms |
|---|---:|---:|---:|---:|
| 1 | [5.626 / 8.060](242-clear1.json) | [5.129 / 6.864](242-skip1.json) | [7.774 / 11.646](242-clear1.json) | [5.860 / 10.060](242-skip1.json) |
| 2 | [3.759 / 4.701](242-clear2.json) | [5.036 / 7.125](242-skip2.json) | [6.601 / 8.765](242-clear2.json) | [6.117 / 9.718](242-skip2.json) |
| 3 | [4.969 / 7.310](242-clear3.json) | [3.961 / 6.536](242-skip3.json) | [7.985 / 9.955](242-clear3.json) | [5.001 / 9.776](242-skip3.json) |

The device call/wall-time p50 is lower for the candidate in all three pairs, but
the GPU p50 moves in both directions (lower in pairs 1 and 3, higher in pair 2).
The result does not establish a consistent GPU timing effect. The paired clock
captures vary substantially, including 441.6, 490, 525, and 587 MHz, so this
short three-run sample is consistent with clock and run noise masking the
small clear overhead. Treat the wall trend as suggestive only.

**Decision: retain clear omission as the default.** Dirty atlas-view updates
remain opt-in and are a separate experimental switch.

Raw provenance is recorded in [skip-clear-provenance.log](skip-clear-provenance.log).
