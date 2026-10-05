# Direct history serialization scratch gate

CPU-only scratch prototype; production checkout remains clean at `831aafed88e16569aa09d78f2d34d812ce8a9b6c`. Scratch copies of `fec.h` and `shard_history.h` add one metadata-span helper and `push_spans`, which copies serialized spans directly into ring storage while holding its mutex, preserving ring wrap, eviction, and owned-hit behavior. No source or runtime changes.

## Correctness

`direct_gate.cpp` normal build and run passed (exit 0); ASan/UBSan run passed (exit 0); TSan run passed (exit 0). The check compares exact collected blobs against `fec::encode_blob` + original `shard_history::push`, decodes valid first-shard view info, last-shard timing and every deterministic payload byte, tests ring wrap/eviction and latest retention, verifies disabled/secondary writes are ignored, and runs a concurrent producer, reader/decode loop, and 100 enable/disable cycles. These test the scratch helper's memory/lifetime behavior; they do not test sender ordering.

## Bounded CPU trial

Warm, alternating matched 30 frames per treatment; one persistent history ring and output buffer per treatment. Each frame models 500 Mbit/s aggregate stereo at 90 Hz: 347,222 payload bytes per eye, 1400-byte non-FEC shard budget, metadata on first/last shards. Captured `direct_gate time` output:

```text
legacy_encode_blob_then_ring_push,n=30,wall_p50_ms=0.0162,wall_p95_ms=0.0168,cpu_p50_ms=0.0161,cpu_p95_ms=0.0166,blob_bytes=10450680
direct_spans_into_ring,n=30,wall_p50_ms=0.0130,wall_p95_ms=0.0134,cpu_p50_ms=0.0129,cpu_p95_ms=0.0132,blob_bytes=10450680
```

The scratch lower bound saves about 3.2 microseconds/frame, or about 0.29 ms of one-core CPU per second per eye at 90 Hz. It removes the intermediate output-vector flatten write and subsequent read before the ring write; both modes still copy serialized bytes into owned ring storage. This is a tiny absolute saving and desktop load was uncontrolled. No latency or headset claim.

FEC-on reference control: ran the existing `tests/fec_history_bench.cpp` unchanged against the production headers; its correctness check passed and raw five-round output is `fec_history_bench.log`. It exercises FEC blob reuse + history push, not the direct helper. This prototype is restricted to the FEC-off case; no FEC-on behavior was changed.

## Integration blocker and decision

The direct helper publishes the ring entry before the socket send. Actual `SendData` must snapshot plaintext before sending because socket serialization encrypts payload spans in place; however `collect_retransmits` can concurrently read history without taking `SendData`'s outer encoder mutex. A NACK can therefore observe a shard before its original send completes. Holding the history mutex over network send/pacing is unacceptable, and writing into the ring before publishing can overwrite still-live entries before the send succeeds. A safe staged commit needs additional ring generation/reservation semantics and changes observable ordering. Given the measured saving is only ~3.2 us/frame (under 0.03% of one core at 90 Hz per eye), do not pursue production integration from this probe.

## Reproduction

From repository root, include paths and commands used:

```sh
g++ -std=c++23 -O2 -pthread /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate.cpp common/smp.cpp -I/run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate -Icommon -Iserver/encoder -Ibuild-server/common -Iexternal -Ibuild-server/_deps/boost-src/libs/pfr/include -lcrypto -o /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate
/run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate
/run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate time

g++ -std=c++23 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate.cpp common/smp.cpp -I/run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate -Icommon -Iserver/encoder -Ibuild-server/common -Iexternal -Ibuild-server/_deps/boost-src/libs/pfr/include -lcrypto -o /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate_asan
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate_asan

g++ -std=c++23 -O1 -g -pthread -fsanitize=thread /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate.cpp common/smp.cpp -I/run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate -Icommon -Iserver/encoder -Ibuild-server/common -Iexternal -Ibuild-server/_deps/boost-src/libs/pfr/include -lcrypto -o /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate_tsan
TSAN_OPTIONS=halt_on_error=1 /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/direct_gate_tsan

g++ -std=c++23 -O2 tests/fec_history_bench.cpp common/smp.cpp -Icommon -Iserver/encoder -Iexternal -Ibuild-server/_deps/boost-src/libs/pfr/include -Ibuild-server/common -lcrypto -o /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/fec_history_bench
/run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/fec_history_bench > /run/media/nerdrx/Lex/claude/nx-scratch/overnight-recovery/history-direct-gate/fec_history_bench.log
```

Source SHA-256: production `shard_history.h` `4ab418b9b23e3f5f92e59f481273e7ba17c12c8df27ec4c323a39dd9f8f7eb80`; production `fec.h` `7b15a9c41ba83cbc0a7841776fa882a9d7944eba21a3de9aba716de6486712be`; scratch `shard_history.h` `29fd52bb366d33ee8e31af1522838229a5b99f231aa440f21db736001eab95b7`; scratch `fec.h` `4172c61a4192eade71d3898e31a15b1d60a8bc6c151cdcc710b278f1ff93aea9`; harness `direct_gate.cpp` `7f8b82b32d3864048094fbcd5f1dce7c2071744d66fe8ba39d16509f6bb4de55`.
