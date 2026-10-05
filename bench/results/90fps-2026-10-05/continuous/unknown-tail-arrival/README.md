# Unknown-tail arrival replay

CPU-only deterministic replay compares the current newest-frame behavior with a candidate: after two 90 Hz display periods and the production 2.5 ms quiet gate, send one ordinary NACK bit for `data.size()` only when the newest frame is incomplete, has no confirmed hole, and has a round left. The candidate reserves no response capacity: receiver sends known holes first; server response limit stays 64.

The fixture reuses `tests/nack_test.cpp` frame/FEC setup, actual `shard_set` completeness/hole/reconstruction, actual `shard_history` collection and terminal-candidate helper, and actual blob encoding/decoding. It exercises sender-complete/no-new-frame, uncached request then later retry, cached data before `end_frame`, parity before the probe and parity between the NACK and its reply, confirmed interior-hole priority, frame advance using existing `frame_over` logic, stream index plus first-shard stereo view metadata, and the two-round ceiling. The compatibility behavior for the terminal assist is modeled by the small `answer()` adapter using the production history methods; it returns only decoded shards and only appends a real `timing_info` terminal candidate after an ordinary hit.

**Limit:** this does not execute `shard_accumulator::try_nack()` or the network poll. Those require live `application` config, XR timestamps, weak scene ownership, and send callback. The candidate due predicate is replay code, not a production helper. Arrival ordering, poll wake, and the 22.22 ms threshold are deterministic scenarios, not measured traffic. Stereo coverage checks owning stream and first-shard metadata, not dual-accumulator scheduling. No Wi-Fi, latency, or photon claim.

The replay shows the opportunity and the cost: a completed sender can answer the missing terminal with one request without a newer frame; if the next shard is not cached, the first request misses and consumes one of two rounds; a later retry can succeed. Parity before the probe prevents it. In the NACK/reply race, parity can reconstruct first and the later reply is rejected by actual `shard_set::insert()` as a duplicate; payload, timing marker, populated-shard-slot count, and last-arrival time stay unchanged. The internal received counter is private and is not read directly. The request/reply packet can still be redundant. This is enough to confirm wire/helper feasibility, but not enough evidence to justify production scheduling changes or the extra request/round in a live arrival distribution. Recommendation: keep current behavior until an actual-path bounded test can quantify that race and preserve the ordinary repair ordering.

Run against the exact source checkout:

```sh
bash run.sh /path/to/wivrn-nx /new/output/directory
```

Normal and halt-on-error ASan/UBSan passed: 30 checks, 0 failures. Raw rows and captured check/build logs are under the output directory. Source revision is `7b7ae3600951d90b07466053bbcda45f7dcc9b3a`; no production files were changed.
