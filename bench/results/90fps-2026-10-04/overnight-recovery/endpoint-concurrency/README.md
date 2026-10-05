# Concurrent terminal-shard history check

This retained CPU check drives production `shard_history`, `collect_frame_end_candidate`, and `fec::{encode_blob,decode_blob}` directly. One producer emits 20,000 monotonically indexed 4-shard frames; two readers select and decode candidates while a third thread performs 500 disable/enable cycles. Only the actual terminal shard carries `timing_info`, and `end_frame` publishes its true shard count.

The payload independently identifies its source: the first 8 bytes encode the complete frame ID, the next 2 encode the shard index, and the remaining bytes use a deterministic per-frame/index pattern. Terminal timing fields are also derived from that full frame ID. Readers verify all these values after decoding the returned copied blob. The helper's `frame_idx`/`shard_idx` decode arguments are therefore not used as the only identity evidence. Cache misses are expected during disable and after ring/entry eviction; every returned candidate must have the exact terminal payload and real timing marker. Program success additionally requires at least one hit, zero identity failures, `published == 20,000`, `held <= max_entries`, and ring bytes equal to `capacity`.

Captured runs all exited 0:

- Normal: `normal-run.log` — 1,793,192 hits, 4,994,514 misses, 0 identity failures.
- ASan/UBSan with halt-on-error: `san-run.log` — 53,836 hits, 1,893,461 misses, 0 identity failures.
- TSan: `tsan-run.log` — 21,116 hits, 557,302 misses, 0 identity failures or race reports.

Final held entries happened to be 4,096 in these captures; the check only requires the documented upper bound. Hit and miss totals vary with instrumentation and thread scheduling. This is a finite concurrency/identity check, not proof for all interleavings or a live network claim. The production helper intentionally separates count lookup and blob collection locks; eviction between them may yield a miss. Collection copies the blob while holding the history lock, before FEC decode occurs outside it.

## Reproduce

From this report directory, supply the source checkout at7b7ae360 or a compatible later revision with a configured `build-server` tree. The script builds/runs normal, strict ASan/UBSan and TSan versions, each with a45-second execution limit. Runtime sanitizer support is required; a platform failure is not a pass.

```sh
bash run.sh /path/to/wivrn-nx/source /tmp/nx-history-concurrency
```

The script prints each captured result and preserves build/run logs. Source and artifact hashes are recorded in `VALIDATION.md`. No production source, device or live runtime setting was changed. These checks do not measure latency or FPS.
