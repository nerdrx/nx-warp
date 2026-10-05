# ASTC owned-frame metadata / bounded queue audit

Audited source `831aafed88e16569aa09d78f2d34d812ce8a9b6c`; no source, build,
device, or runtime changes.

## What is already safe to own

The ASTC backend returns a packet whose `span` points into a `shared_ptr`
vector retained by the sender job (`server/encoder/video_encoder_astc.cpp:457,
550`; `server/encoder/video_encoder.h:63-70`). `encode()` waits the current
slot fence and finishes reading/compressing its readback before it returns
(`video_encoder_astc.cpp:302-327`). Thus the network sender does not borrow the
compositor image or the ASTC readback. The compositor can keep its current safe
rule: keep `image.busy` until both eye `encode()` calls return, then release it
(`server/compositor/compositor.cpp:1696-1721`). No early image release is
needed for an owned-job design.

The missing ownership boundary is the base encoder's frame context. `data`
currently contains only `encoder*`, a payload span, owning memory, and a control
flag (`video_encoder.h:63-70`). `video_encoder::encode()` writes the connection,
clock offset, per-frame `timing_info`, and shared shard template before calling
the backend, then stamps encode-end and queues the result (`video_encoder.cpp:
760-808`). Later `SendData()` reads/mutates those same members while holding
the encoder mutex: frame/view/shard headers and timestamps, connection routing,
byte/offset counters, FEC group state, history snapshots and end counts,
video dump, parity, and frame-send accounting (`video_encoder.cpp:854-1053`).
It also holds this mutex for the complete paced send. Simply allowing another
`encode()` to write those members while `SendData()` reads them would race and
mislabel shards.

## Smallest plausible seam, and its costs

A real latest-frame experiment needs an ASTC-only sender job that owns an
immutable frame context: session/connection handle, frame and stream IDs,
copied `view_info` (pose/foveation), clock snapshot, encode timestamps, and
packet memory. `SendData`/`send_parity` would consume that context and use
frame-local shard/timing/byte counters, while preserving a serialized
per-encoder send section for stream-owned FEC/history/rate state. The destructor
must still drain jobs because `data.encoder` is currently a raw pointer
(`video_encoder.cpp:95-126, 215-227, 429-433`). Scheduling also reads the
encoder's live connection/rate/pacing state (`video_encoder.cpp:134-188`), so
the refactor must explicitly decide which settings remain live and which are
snapshotted. This crosses the base sender, video encoder, parity path, history,
and async lifetime; it is not a queue-cap tweak.

With motion disabled (the default; `_wivrn_astc_motion_delta=1` is explicit in
`video_encoder_astc.cpp:128-130`), the packet format is independent per frame,
so a dropped queued packet does not become a decoder reference dependency.
Motion mode has ACK-selected reference blocks and a persistent reference ring
(`video_encoder_astc.h:56-66`, `video_encoder_astc.cpp:365-429`); it should be
excluded from an initial latest-frame policy. Even in independent mode, the
existing queue is shared across encoders and its overflow drops the oldest
whole job (`video_encoder.cpp:193-212`). It is not a stereo-pair supersession
policy: jobs from the two eyes can be interleaved and evicting one eye alone
can leave no exact common frame. A safe latest policy therefore needs a paired
frame key or a demonstrated pairing rule, and must preserve normal FEC/history
and loss accounting for frames actually sent.

## Why lowering 8 does not help

`wait_idle(this)` checks both pending and in-flight entries for that encoder
across the sender queues and blocks before the next backend call
(`video_encoder.cpp:215-227, 760-761`). Consequently each encoder can have at
most one queued/in-flight job under the current path. The `max_queued_frames=8`
limit is global per socket queue, not per encoder (`video_encoder.h:94-110`);
lowering it cannot let an encoder produce while its prior send is active. It
can instead evict another encoder's pending frame sooner. The current cap is
not evidence that ASTC frames are being superseded.

## Decision

This is a genuinely different idea from the rejected sender-wait relocation:
that prototype moved the same wait but still depended on mutable base frame
members, and did not establish a stable full-cycle win (`production-overlap-gate/
send-overlap-proposal/AUDIT.md`, `overnight-stereo-gpu-20261004/ASTC_SENDER_OVERLAP_REJECTED.md`). The prior backpressure audit already found that no callback releases
the compositor image before both encode calls and that the owned ASTC payload
does not make base sender state safe (`production-overlap-gate/backpressure/
AUDIT.md`).

**Reject a queue-cap-only change.** The owned-context seam is technically
plausible for independent ASTC, but a safe bounded supersession policy also
needs stereo-aware queue handling and per-frame sender state; the current
global oldest-drop behavior is not sufficient. Do not implement or claim a
latency win from this source audit. A future experiment is justified only with
a narrowly scoped ASTC independent-mode job type, explicit paired-frame
supersession, preserved current GPU fence waits and `image.busy` lifetime, and
checks that exact eye IDs and FEC/history state stay coherent. The old
sender-wait movement is not a substitute for that work.

## Measurement before refactoring

Root verified that the producer's `wait_idle(this)` occurs before the existing
`encode_begin` timestamp. Neither backend fence/compression timings nor
`send_begin - encode_end` includes that prior-send wait. This is an unmeasured
latency component, not evidence that it dominates. A separate default-off
server diagnostic is being built to report that wall wait per stream; do not
change job ownership or drop policy from this source audit alone.

The earlier [freshness audit](FRESHNESS_AUDIT.md) described the global sender
cap; this follow-up clarifies the stricter per-encoder bound. The existing
sender-wait relocation experiment remains rejected. No live behavior changed.
