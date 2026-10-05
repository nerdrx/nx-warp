# Feedback timestamp provenance

Audited source `412a2bfe9ef54333fdc35cf9919c6443fa409909` (read-only).

The stream packet callback passes each parsed shard to `shard_accumulator::push_shard` (`client/scenes/stream_network.cpp:279–289`). That handler samples `instance.now()` and passes it to `shard_set::insert`; the first inserted shard stamps `received_first_packet` (`client/decoder/shard_accumulator.cpp:171–172`; `client/decoder/shard_set.h:167–181`). The wire shard carries stream/frame/shard indices, but no receive timestamp (`common/wivrn_packets.h:1512–1525`).

For a complete frame, reassembly calls `decoder_->push_data` before sampling `received_last_packet` (`client/decoder/shard_accumulator.cpp:427–458`). Thus these are client software-processing timestamps, not NIC or hardware arrival stamps. `instance.now()` calls `clock_gettime(CLOCK_MONOTONIC)` and converts the result to `XrTime` (`client/xr/instance.cpp:370–378`); source does not establish effective resolution.

Inference, not a live-runtime finding: if dispatch is delayed while packets queue, then queued shards are handled close together; first/last processing times can compress the measured span and inflate the server’s bytes-per-span sample (`server/driver/bitrate_controller.cpp:472–484`). The final timestamp is also taken after `push_data`, so it includes that call’s processing time.
