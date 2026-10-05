# Sender repair-history cost audit

Read-only source audit at `wt-pyrowave-probe` commit `831aafed88e16569aa09d78f2d34d812ce8a9b6c`; no production files changed. This identifies work, not measured latency.

## Actual path and costs

- `video_encoder::SendData` owns `video_encoder::mutex` from entry through sharding, socket sends, `shard_history::push`, pacing waits/sleeps, and frame-end history bookkeeping (`server/encoder/video_encoder.cpp:854-856, 923-931, 940-959, 995-1013, 1023-1028`). The lock span is dominated structurally by send/pacing lifetime; this audit has no contention timing proving history is the cause.
- Defaults enable both FEC and `shard_retransmit` (`client/configuration.h:156-169`). For ordinary primary-path stream shards, `fec::encode_blob` flattens serialized spans into reusable per-encoder `history_blob`; FEC consumes that same blob, then history copies it into the ring (`video_encoder.cpp:929-931, 983-997`). `encode_blob` reuses thread-local serialization metadata and the persistent vector capacity (`common/fec.h:144-156`; `video_encoder.h:295-301`), so the ordinary path is a payload-sized materialization plus ring copy, not a fresh vector allocation per shard. With FEC off but history on, that flattened temporary exists only to be copied into history.
- Ring copy/bookkeeping is under `shard_history::mutex` (`server/encoder/shard_history.h:123-154`); `end_frame` also takes that mutex even with retransmit disabled (`:157-162`). `enabled()` is atomic (`:109-114`), and disabled shards skip the ring path. Ring allocation occurs on enable (`:93-107`).
- On repair reads, `collect` allocates and copies each hit into owned `hit::blob` while holding the history mutex (`shard_history.h:171-203`). Decode and output append happen after unlock (`video_encoder.cpp:564-585`; `fec.h:195-212`). The owned copy is required by the current ring-overwrite/disable lifetime contract; it lets network send/decode continue after releasing the ring lock. Avoid removing it absent a different ownership scheme.

## Existing evidence

- `tests/fec_history_bench.cpp` is a runnable actual-class microbenchmark (`shard_history`, `fec::group_builder`, `fec::encode_blob`) with five rounds and correctness comparison; its header documents the g++ command. It models 16 × 1200-byte shards and compares reuse of the FEC blob against re-encoding, not a ring-copy removal; explicitly no sockets, encryption, codec, or headset (`tests/fec_history_bench.cpp:1-6, 104-179`). No saved run output was found, so this audit makes no timing claim.
- `overnight-recovery/endpoint-concurrency/README.md` records normal/ASan/TSan producer-reader/enable-disable checks against actual `shard_history` and FEC APIs, and explicitly says it is a concurrency/identity check, not timing. It notes collect copies under lock and decode occurs outside. No encoder/socket latency measurement.
- `overnight-recovery/endpoint-probe/README.md` similarly says its adapter uses actual history/FEC helpers but does not instantiate the private encoder method or socket; its checks are not latency evidence.

## Candidate and smallest gate

One narrowly safe candidate exists for **history enabled, FEC disabled**: serialize directly into the bounded ring under `shard_history`'s lock in a scratch prototype, avoiding the temporary flattened `history_blob` and its subsequent full ring-copy read/write. This keeps the plaintext snapshot before in-place socket encryption and makes bytes owned in the ring before unlock. Keep current code unchanged when FEC is active because the shared blob is also the parity input; do not expose a ring span after unlocking because concurrent disable/resize could invalidate it. This is a candidate only; no cost win is established.

Smallest useful next gate: extend the actual-class `tests/fec_history_bench.cpp` with an FEC-off pair comparing current `encode_blob` + `history.push` to a scratch prototype that serializes the same packet spans directly into ring storage while holding history's mutex. First require byte-identical collected blobs, successful `fec::decode_blob` round trip, wrap/eviction correctness, and ASan/TSan while toggling history. Then alternate warm, matched runs using native ASTC shard sizes/count from production (include one representative 2176² frame split), record process CPU and p50/p95 per-frame CPU time, and report bytes copied/allocated. Include existing FEC-on path as a control and confirm it is unchanged. The current `fec_history_bench.cpp` alone cannot test this candidate because `shard_history` exposes no direct-span insertion API.

If profiling shows CPU time is mostly elsewhere, next measure actual `SendData` mutex wait/hold separately from network send and pacing wait; source inspection alone says those waits are inside the lock, but does not establish their contribution.

## Root review: ordering boundary

`collect_retransmits` (`video_encoder.cpp:537–614`) does **not** acquire the outer `SendData` encoder mutex. It uses the history lock and a separate retransmit quota lock. Do not infer that normal pacing directly blocks NACK collection through the outer mutex.

A direct snapshot must happen before the socket mutates payload spans through encryption. Inserting it then publishes it to concurrent readers earlier than the current post-send `history.push`. The scratch direct-writer benchmark is therefore a lower-bound copy-cost experiment, **not an integration-equivalent sender implementation**. It must exclude control, FEC and TCP-spill routes: spill success is not known before attempting the encrypted secondary send. Retaining the current owned repair-read copy remains mandatory under ring overwrite/disable.

A staged publication protocol would need separate lifetime and ordering evidence and could consume the small saving. No production change is justified by this read-only result alone; the bounded scratch correctness/timing gate is pending. No live server, Pico or route change occurred.
