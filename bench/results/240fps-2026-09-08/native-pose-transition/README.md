# Native-resolution pose transition failure

A user-run Pico session exposed a coverage gap on 2026-09-08. Stream 0 used
4352×2176 stereo (2312 tiles). The first image encoded; subsequent pose-changing
frames repeatedly failed with `assembly warp params too large`. Connection and
session-focus checks had therefore not established usable streaming.

## Cause and fix

Atlas-to-picture assembly rejected parameter payloads larger than a fixed
128 KiB staging slice. Native stereo requires a larger per-tile matrix payload.
The encoder now reserves a staging tail sized from the allocated warp buffer
and tile-record count, with separate non-overlapping upload ranges. This removes
the arbitrary warp and tile-record caps without changing bitstream syntax.

## Reproduction

The registered `vk.encoder.acid.api.atlas.native.transition` test uses a
4352×2176, two-eye, three-frame fixture with a pose change followed by the same
pose. It exercises ATLAS → PICTURE → ATLAS through the public encoder API.

| Binary | Result |
|---|---|
| Before fix | Exit 1: `assembly warp params too large` on frame 1 |
| Fixed | Exit 0; frame flags `0x01`, `0x28`, `0x08` |
| Fixed, original small fixture | Exit 0; same expected flag sequence |

The native fixed run passed host Vulkan synchronization validation. This is a
regression result, not a throughput result or a demonstrated moving-head Pico
session. The previous sparse/static-pose performance measurements did not cover
this transition at native resolution.

The same live session also entered a reconnect rejection loop and the client
terminated after a broken-pipe exception. Its final crash stack was in Android
global finalization (`exit` → `__cxa_finalize` → `RefBase::decStrong`). The logs
do not establish that the encoder failure caused the socket shutdown. Reconnect
compatibility diagnosis and shutdown handling are separate from this fix.
