# End-assist compatibility gate

This CPU-only mixed-revision replay uses exact archived client/common headers from `c514841f87b9718a50c974e79c3e902ef38a3a82` and current server revision `7b7ae3600951d90b07466053bbcda45f7dcc9b3a`. Current FEC code generates shard recovery blobs and complete ordinary data-shard packets; it verifies each packet equals serialization of the shard rebuilt from its blob. The archived client parser reads those current packets and current history-selector replies. Every current blob is also decoded by archived FEC and compared with its parsed packet for identity, payload and metadata presence. Its NACK is serialized/deserialized with archived headers.

The archived `shard_set` begins with shards 0, 2, and 3. Its NACK asks for known-prefix hole 1. Current history returns that ordinary shard and the real terminal shard 4 with timing metadata. The archived receiver remains incomplete after shard 1 because the tail is unknown, then completes after shard 4. Payloads, stream/shard/frame identity, foveation metadata, and all four timing fields are checked. Normal and strict ASan/UBSan runs pass with 320 payload-byte checks; captured output is in `normal.log` and `san.log`.

Archived and current `common/wivrn_packets.h` are byte-identical (SHA-256 `a12c3ba86742cd8a25d7cd7a0460607d03535f2ad5fbecce894b5e1f53a031bc`). Archived/current FEC implementations differ, and the current-produced blobs are decoded by the archived FEC implementation. The replay calls current `shard_history` collection and terminal selector directly; it does not compile all of `video_encoder::collect_retransmits` into the mixed-header harness. This is protocol-path compatibility evidence, not installed-client/device or whole-session proof.

`run.sh` takes a source checkout at the exact server revision plus an output directory. It reconstructs archived headers from Git into a temporary output subdirectory, builds both harnesses, runs normal and strict sanitizer tests, saves logs and provenance, then removes the temporary headers, binaries, and packet artifact. Build dependencies are the checkout's `build-server/_deps/boost-src/libs/pfr/include`, `build-server/common`, and `external` headers. No downloaded dependencies are needed.

The archived serializer has a zero-length `memcpy(nullptr, 0)` UBSan issue when parsing empty foveation vectors. This gate uses non-empty valid foveation metadata, so it does not exercise that unrelated old-client case. Current serializer guards zero-size copies.

Run:

```sh
./run.sh /path/to/wt-pyrowave-probe /tmp/end-assist-compat-out
```
