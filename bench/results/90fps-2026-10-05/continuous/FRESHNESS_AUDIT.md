# Native ASTC8x8 freshness and queue audit

**Result: no justified cheap source change.** The audited path already bounds queued stale work at the server and decoder, drops older work first, and protects packet/reference ownership. The possible hot waits/copies either belong to the previously rejected sender-wait relocation or are required by current ownership. No source, build, device, or runtime setting was changed.

Audited checkout `wt-pyrowave-probe`, revision `7b7ae3600951d90b07466053bbcda45f7dcc9b3a`.

## Current path and freshness controls

- Server encoder submission has an eight-frame pending cap; `sender::push` discards whole oldest queued frames when full (`server/encoder/video_encoder.h:93`, `video_encoder.cpp:193-212`). The sender removes a frame from that queue and marks it in flight before `SendData`, so it cannot be discarded while its span is being sharded/sent (`video_encoder.cpp:95-126`). This is existing bounded backpressure, not an unbounded stale queue.
- Each encoder waits for its own queued/in-flight send before overwriting shared `cnx`, clock, timing, and shard state (`video_encoder.cpp:732-785`, `:215-227`). Moving this wait after ASTC encode is the same deferred-wait prototype already rejected after mixed sender-overlap results; see `production-overlap-gate/send-overlap-proposal/AUDIT.md:11-15`. I did not repeat or propose it.
- On receive, `shard_accumulator` submits only the oldest frame in order; newer complete frames wait behind it, and stale retirement requires the existing complete-frame skew rule (`client/decoder/shard_accumulator.h:147-154`, `frame_window.h:147-212`; submission at `shard_accumulator.cpp:410-475`). A newer packet alone is not evidence that older content is unusable on the two paths.
- ASTC packet assembly copies shard spans into a reusable owned packet buffer (`client/decoder/astc/decoder.cpp:157-181`). On completion it moves that buffer into a pending deque; when the two-entry queue is full, it recycles the oldest queued packet (`:203-233`; `decoder.h:30-31,67-75`). That already bounds queued delivery and avoids retaining shard storage while decode runs.
- The worker consumes pending packets FIFO, drops a packet if its six-image pool has no free slot, and validates motion-reference age/availability before decoding (`decoder.cpp:287-345`; `decoder.h:30-62`). Dropping an already-dequeued frame opportunistically is not obviously safe: a following motion packet may depend on that frame's decoded reference. The queue's oldest-first drop is safer because the decoder still explicitly rejects unavailable references and waits for an independent frame.
- The initial span copy is the ownership boundary between the accumulator's shard lifetimes and the asynchronous decoder queue. Removing it needs a move/lease protocol spanning partial shard assembly, frame-window retirement, and decoder completion; this is not a local copy-elision tweak. Packet vectors are then moved to pending and recycled after worker use (`decoder.cpp:222-233,514`).

## Gate if freshness becomes a measured problem

The exact path does not currently report pending-queue dwell or count oldest-packet drops, so source inspection cannot show whether either queue actually reaches its limit. If runtime evidence later points here, the smallest useful diagnostic is a fixed-window count of server pending drops and decoder oldest-pending drops plus `frame_completed`-to-worker-dequeue age, tagged by stream and encoding mode. Keep it diagnostic-only; do not cancel in-flight decode or change queue policy until motion-reference behavior is separately proven. Existing ASTC worker timing reports decode/staging copy, prior upload fence, and submit-to-handoff, but not queue dwell (`client/decoder/astc/decoder.cpp:514-524`).

This is a source audit only: no measured frame age, live queue occupancy, latency, Wi-Fi, or display result is claimed.
