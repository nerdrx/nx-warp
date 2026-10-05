# Quiet frame retirement boundary

This is a virtual-time check of four exact production method bodies, not a headset or network-loop run. It exposes a conditional gap in the opt-in reassembly deadline: quiet repair polling alone does not service frame retirement.

## Result

Normal and halt-on-error ASan/UBSan runs each pass **42 assertions**. An independent invocation of the published wrapper reproduces the generated source and decision rows.

The fixture gives frame 0 its first and final shard, withholding interior shard 1. Frame 1 is complete. The default three-frame skew tolerance keeps frame 0 at the front. The scene stub supplies a 90 Hz display period of 11,111,111 ns.

The exact `poll_nacks()` / `try_nack()` methods spend their two allowed repair rounds. At 100 ms beyond the two-period deadline, another explicit `poll_nacks()` call still leaves frame 0 at the front and delivers no frame to the decoder stub. An explicit call to the existing `pump()` then retires frame 0 and invokes the decoder stub's completion callback for frame 1. Deadline-disabled and no-newer-complete controls keep waiting.

| Deadline enabled | Newer complete frame | Front after explicit pump | Decoder stub completions |
|---|---|---:|---:|
| Yes | Yes | 2 | 1 |
| No | Yes | 0 | 0 |
| Yes | No | 0 | 0 |

The 100 ms interval is deliberately advanced **virtual time**, not a measured stall. The check confirms behavior at the polling entry and pump boundary; it does not establish how often this state occurs in VR.

## Methods and scope

`extract.py` preserves the exact `pump`, `try_submit_front`, `poll_nacks` and `try_nack` bodies from source `85cbc286`, plus its actual window alias. Real `frame_window`, `shard_set`, packet and deadline helpers run. Source spans and SHA256 hashes are retained in `raw/provenance.json`.

The class constructor, scene, clock, configuration and decoder are narrow stand-ins. The decoder counts bytes and completed frame IDs; it performs no GPU work. Feedback and NACK callbacks record requests. Inputs enter through real `shard_set::insert`, with window completion notification; production `push_shard`, `push_parity`, network scheduling, XR, rendering and feedback transport are excluded. No live FPS, recovery duration or photon-latency claim follows.

The actual deadline is Android-only and default-off (`debug.wivrn.nx.astc_deadline=1`). Recovery polling is separately default-off. No source option, headset property, installation or running session changed for this check.

## Follow-up held

Adding a pump call alone would still leave the next wake-up dependent on NACK deadlines or the 100 ms network fallback. A complete fix needs a retirement wake-up as well as retirement service, with tests for expired fronts, unavailable scenes, period changes and disabled options. Keep both opt-in modes off until that gate passes; do not advertise a two-frame wall-clock bound from this check.

## Reproduce

From this directory, with the existing configured source checkout:

```sh
bash run-check.sh /run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe /tmp/nx-retirement-check
```

Extracted code retains the source's GPL notice. Compiler warnings from existing dependencies are preserved in the raw logs.
