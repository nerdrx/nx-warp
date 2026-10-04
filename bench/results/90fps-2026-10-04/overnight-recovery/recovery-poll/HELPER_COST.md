# Recovery-poll helper cost

This is a local CPU microbenchmark of the production `nack_poll_deadline`,
`shard_set::complete`, and `shard_set::missing_shards` operations in the same
six-set traversal order as `shard_accumulator::next_nack_deadline`. It excludes
XR-clock calls, config and weak-pointer checks, mutex acquisition, socket waits,
packet parsing, FEC decode, and NACK sends. It is not a network, Pico, or headset
measurement.

Each fixture contains six 521-slot `shard_set`s. The cases are deliberately
synthetic stress occupancies, not claims about observed live window state:

| Case | Before quiet deadline | At deadline |
|---|---|---|
| `late_interior_hole` | Five older incomplete frames have a late hole at shard 519; newest frame is complete. | Runs missing-shard scans and finds actionable holes. |
| `unknown_tail` | Five older frames are complete; newest has a full contiguous prefix without its end marker. | Scans newest holes, finds no actionable tail, returns no deadline. |
| `parity_suppressed` | Five older incomplete frames have one parity-covered hole at shard 519; newest is complete. | Parity removes each hole from NACK eligibility; returns no deadline. |

Completed older sets are retained deliberately to stress the complete checks;
the fixture does not call the production drain, so this occupancy is not a claim
about normal runtime. Exact helper results are asserted in the harness.

`build-run.sh` also rebuilds this fixture and produces `helper-cost.csv`.

The `-O2` run warmed each fixture with 1,000 calls per phase, then used five matched blocks with alternating before/due order per case, with 10,000 helper invocations per phase and a
compiler barrier per iteration. Median nanoseconds
per invocation from `results.csv`:

| Case | Before due | At due |
|---|---:|---:|
| Late interior hole | 1,153 ns | 2,108 ns |
| Unknown tail | 920 ns | 1,125 ns |
| Parity suppressed | 1,022 ns | 2,114 ns |

These are one host/compiler's per-call timings, not tail statistics. The due
phase is a single quiet-gate evaluation; actual call frequency depends on socket
traffic. The production setting remains default-off, and these results say
nothing about idle power or live-device behavior.
