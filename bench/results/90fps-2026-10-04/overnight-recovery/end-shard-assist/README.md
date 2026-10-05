# Cached final-shard assist: recovery without changing the wire

## Finding

A lost frame-ending shard can hide how many data shards remain missing. The current receiver can request known interior holes before the end arrives, but for an older frame with an unknown tail it requests only the next unseen index. With a two-round recovery limit, a contiguous lost tail may therefore remain incomplete even when the sender still has all its bytes.

The new server experiment appends the **actual cached final data shard** after a successful ordinary NACK. Its index comes from the sender's recorded `end_frame` count; decoding must confirm real `timing_info`. Once that shard arrives, the unchanged receiver knows the extent and can request the remaining holes in its next round. The branch is **default off**, NXASTC-only, enabled at server startup by exact `WIVRN_NX_REPAIR_END=1`. No client installation or new protocol message is needed. It has not been enabled in a live session.

## Deterministic result

The fixture contains 256 data shards. Each run withholds a consecutive tail and all parity, then exercises the existing quiet gate and at most two NACK rounds. `frame_over=true` models an older frame after a newer frame arrives; it does not invent a final index at the client.

| Lost tail shards | Baseline recovered / ready | Assist recovered / ready | Assist rounds | Assist replies / duplicates |
|---:|---:|---:|---:|---:|
| 1 | 1 / yes | 1 / yes | 1 | 1 / 0 |
| 2 | 2 / yes | 2 / yes | 1 | 2 / 0 |
| 16 | 2 / no | 16 / yes | 2 | 17 / 1 |
| 64 | 2 / no | 64 / yes | 2 | 65 / 1 |
| 128 | 2 / no | 66 / no | 2 | 66 / 0 |

![Recovery within two rounds](recovery.png)

This is an improvement in the tested recovery state machine, **not measured Wi-Fi or display latency**. The 64-reply cap applies per request, so 65 total replies across two rounds do not exceed it. In the 16- and 64-tail cases the second round repeats the final marker because the request carries no acknowledgement of its receipt. The receiver ignores that duplicate; the experiment adds no persistent peer state. At 128 losses the second round spends all 64 slots on requested data and remains incomplete.

An additional case loses interior shard 7 and final shard 255, without a newer-frame assumption. The ordinary request names only 7. Baseline repairs that interior hole and remains incomplete; assist returns 7 followed by the real 255 and completes the frame. A separate actual FEC case reconstructs an isolated final loss and emits no NACK.

## Implementation and limits

- The shared `shard_history::collect_frame_end_candidate` validates the matching frame count before narrowing, rejects zero/out-of-range counts, checks that the exact final entry is still cached, and skips already-collected indices.
- Requested data keeps priority. The assist uses only a remaining slot inside the existing 64/request and 2,000/second per-encoder budgets. The normal send path still handles replies; the current unpaced repair burst behavior is unchanged.
- TCP-only finals, evicted entries, absent counts, empty/no-hit requests, full budgets and blobs without real timing metadata produce no extra reply.
- The NACK bitmap-popcount metric retains its meaning. The unrequested marker is not evidence of a lost shard: it may already be present. `tail_loss` records injected fixture loss separately from `nacked_metric`.
- A **newest contiguous prefix with no ordinary NACK** remains unresolved by this server-only branch. Its poll helper still returns the existing 100 ms fallback when no actionable holes are known.
- No deadline is extended and no repair cap is raised. Actual link congestion, duplicate traffic, adaptive parity behavior and headset presentation remain live validation gates.

## Validation and reproduction

The production history selector is tested directly in `tests/nack_test.cpp`: **832 assertions**, normal and halt-on-error ASan/UBSan. The retained endpoint replay calls that same selector plus actual FEC encode/decode, shard storage, missing-shard selection and NACK serialization: **468 assertions**, normal and strict sanitizers, with matching ten-row CSVs. The private encoder method itself is reviewed source glue, not instantiated by this CPU replay. A full `wivrn-server` build also passes.

The tail sweep deliberately withholds parity; it is not a complete K16/depth4 loss simulation. Its separate parity reconstruction test does not turn the sweep into a network benchmark. No fresh FPS, HEVC parity, motion quality or photon latency is established here.

`run.sh` takes the WiVRn NX source checkout and an optional output directory. It needs g++, OpenSSL and the generated headers/Boost include path from a configured `build-server` tree. It compiles and runs only CPU tests; it does not start the server or touch the headset:

```sh
bash run.sh /path/to/wivrn-nx/source /tmp/nx-end-assist-check
```

`results.csv` and `results-sanitizer.csv` contain the measured fixture counts. Raw endpoint stdout is retained in `normal-run.log` and `sanitizer-run.log`; check summaries are separate. `plot.py` regenerates the figures from CSV. Source revision and artifact hashes are recorded in `VALIDATION.md`.
