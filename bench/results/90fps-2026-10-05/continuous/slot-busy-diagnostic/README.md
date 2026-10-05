# ASTC base slot busy-wait diagnostic

Base revision: `a98d5ac05c8bb4d71c88d8938e9a0e961c975bff`; change commit `dc012b1`. Only production
edits are `server/encoder/video_encoder.cpp` and `server/encoder/video_encoder.h`;
final hashes are in `source-sha256.txt`.

Set `WIVRN_ASTC_SLOT_WAIT_TIMING=1` before encoder construction to enable the
diagnostic for native ASTC only. Exact value `1` is required. One summary per
stream is emitted after 180 calls to the existing base
`state[present_slot].wait(busy)`, with mean and maximum wall milliseconds.
Every call counts, including immediate returns and calls for frames that the
IDR handler subsequently skips. The wait remains before that skip check.

The measurement uses `os_monotonic_get_ns()`. Disabled and non-ASTC paths
retain the original wait and add no clock reads or counter updates. It does not
change the ASTC Vulkan fence wait. This base atomic busy flag covers the prior
encode path and may span waiting for the prior sender job, the current ASTC
fence/readback/compression, and enqueue of the current packet. It is not a
GPU-fence-only measurement and does not include the current packet's later send.
ASTC's subclass fence remains the separate source of GPU completion; an encode
timeout can release the base slot while GPU work is still pending.

Runnable source-extracted check:

```sh
sh run-check.sh /path/to/wt-pyrowave-probe /path/to/slot-busy-diagnostic
```

The check embeds the exact constructor gate and exact wait branch with fake
clock/state/logger. It verifies disabled/non-ASTC clock and counter silence,
exact-value gating, skipped-present ordering, zero-wait inclusion, 180-sample
mean/max, and reset over two windows. It does not exercise Vulkan or a real
atomic wait. Captured pass output is `source-extracted-check.log`.

Build command from the checkout root:

```sh
cmake --build build-server --target wivrn-server --parallel 4
```

The configured server build passed (exit 0); full output is `server-build.log`.
`git diff --check` passed. The option was not set during validation. This
measurement includes lock/notification/scheduling wall time and is not proof of
network, GPU, Pico, or photon latency or of an optimization benefit.

## Root review and scope

Root reviewed both source files and confirmed the logged call remains before
the IDR skip decision. The exact source SHA-256 matches the completed build
evidence. Root independently ran the public source-extracted check (exit0,
`root-check.log`) and reran the sender-wait check against this newer constructor
(exit0, `../sender-wait-diagnostic/root-current-regression.log`). Its mock now
accepts the additional constructor flag; the sender policy remains unchanged.

The mock executes actual extracted source branches with simulated durations.
It does not exercise a real blocked atomic, the full compositor or GPU. The
server build was before commit, so generated version metadata still names the
base a98d5ac; source hashes identify the tested edits. No diagnostic was enabled
in a running server and no live values are claimed. All source whitespace
checks passed. Raw compiler logs remain verbatim.

This aggregate intentionally has no frame-generation pairing. Both waits can
overlap work on other frames and their sample denominators can differ because
slot waits include skipped presents. Do not add slot/sender/backend means to
claim complete frame or photon latency. The
[fence attribution audit and retained graph](../fence-attribution/README.md)
separate the boundaries. An actual authorized compositor measurement remains
necessary before changing ownership, queue policy or fence guards.
