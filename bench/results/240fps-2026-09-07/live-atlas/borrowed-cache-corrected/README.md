# Corrected borrowed-cache handoff

The current v2 capture package is [v2-live](v2-live/README.md); earlier exploratory timing is retained below for provenance.

This directory records the corrected R8 borrowed-target cache path. The focused
validation is eight changing frames against a scalar full-reference decoder
while cycling three target generations. Each target is checked in both
`GENERAL` and `SHADER_READ_ONLY_OPTIMAL` layouts, with clean GPU synchronization
validation before timing is interpreted.

The correction preserves ordinary atlas uploads and legacy image transitions,
tracks changes across target reuse, binds a separate dirty-list buffer, and
explicitly preserves each reused target’s image layout. These are correctness
requirements; they do not imply a speed gain. The prior CPU staging `memcpy`
crash remains archived separately in
[borrowed-cache-rejected](../borrowed-cache-rejected/README.md).

The reusable regression target and deterministic fixture instructions are in
[`test_borrowed_r8_catchup.cpp`](../../../../../tests/vk-decoder/atlas/test_borrowed_r8_catchup.cpp),
[`README-catchup.md`](../../../../../tests/vk-decoder/atlas/README-catchup.md), and
[`generate_atlas8_fixture.py`](../../../../../tests/vk-decoder/atlas/generate_atlas8_fixture.py).
Run the explicit test with `nxvc-atlas-borrowed-r8-catchup-test /tmp/atlas8.nxv`.
The target is intentionally not default CTest coverage because the fixture is
caller-supplied.

## Initial live timing pair

A later exploratory pair is included as sanitized relative timing data:
`hevc-fast` versus `cache-on2`. For stream 0 after a 10 s warmup, arrival to
render selection was 19.272/34.646/37.361 ms versus 8.831/12.554/13.308 ms
(p50/p95/p99; 12,963 versus 5,476 paired frames). This stage result does not
establish whole-pipeline performance or hardware-codec superiority. The
`cache-on2` screenshot shows stronger cube-edge/block trails than `hevc-fast`,
so the visual tradeoff remains unresolved. Reproduce with
`python3 reproduce-latency.py`; the accompanying screenshots are
[hevc-fast](hevc-fast-screen-34.png) and [cache-on2](cache-on2-screen-34.png).
