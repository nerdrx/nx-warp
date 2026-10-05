# Production ASTC overlap audit

## Finding

No hard production barrier was found that inherently prevents the two eyes' current-frame ASTC submits and readbacks from overlapping. The existing `WIVRN_ASTC_PARALLEL_EYES=1` path already launches the right eye with `std::async` while the compositor encoder worker processes the left eye, then joins before releasing the compositor image. This ordering is materially consistent with the scratch harness, which submits both command buffers before either encode/readback call.

The remaining integration risk is timing-dependent prior-frame backpressure: `compositor::layer_commit` calls each encoder's `present_image` serially, and ASTC `present_image` waits that eye's reusable slot fence before recording/submitting the current command buffer. If the first eye's prior slot is still in flight, this can delay submission of the second eye. It is a per-eye slot wait, not an unconditional wait for the other eye's current submission.

## Evidence

- `server/compositor/compositor.cpp:1181-1204`: after submitting the compositor work and signaling its timeline semaphore, `layer_commit` walks encoders serially and calls `present_image` for each eye; only after both calls does it publish `encode_request`. Thus the ASTC encoder worker cannot start its current-frame readback before both eye submits have been attempted.
- `server/encoder/video_encoder.cpp:697-707`: every encoder protects its own rotating slot with `state[present_slot].wait(busy)` before recording a present. The two eye instances have separate slot state.
- `server/encoder/video_encoder_astc.cpp:211-221`: ASTC waits its own slot fence, then invalidates that slot generation before recording. This is the possible prior-frame serialization point noted above.
- `server/encoder/video_encoder_astc.cpp:269-274`: each ASTC submit briefly takes the shared Vulkan queue mutex, sets the timeline wait stage mask to compute plus transfer, and submits. The queue mutex is shared with Monado's main queue (`server/compositor/compositor.cpp:1904-1908`); this serializes submit calls, but does not wait for the first eye's GPU work to finish before the second eye is submitted.
- `server/compositor/compositor.cpp:1642-1643,1696-1721`: exact startup env opt-in, limited to two ASTC encoders with no auxiliary encoder; right eye runs asynchronously, caller handles left, and the worker joins before `image.busy=false`.
- `server/encoder/video_encoder_astc.cpp:277-302`: each eye waits its own fence, reads that slot's timing/readback, invalidates memory, then independently compresses. Encoder instances own their buffers and compressor contexts (`server/encoder/video_encoder_astc.h:29-74`); no shared ASTC mutex couples them in independent mode. Motion mode locks are per-instance.
- `server/encoder/video_encoder.cpp:760-809`: both threads may wait for their own prior sends; `sender::wait_idle` checks only the given encoder pointer (`server/encoder/video_encoder.cpp:215-227`). After encoding, the outputs enter the shared sender (`:804-809`), whose UDP queue sends serially (`:95-125`). That can delay send completion and later same-eye encodes, but does not create a current-frame GPU-fence dependency between eyes.
- `server/compositor/compositor.cpp:1717-1721`: source compositor image reuse is held until both encoder calls finish. Because each ASTC `encode()` waits its fence and reads back before returning, the internal compositor image remains protected while both eyes consume it. This audit found no image-lifetime blocker for the current input path.

The prior scratch report [`fence-overlap-recheck/README.md`](../fence-overlap-recheck/README.md) measured lower host complete-call wall in its native harness while the summed GPU query intervals stayed effectively unchanged. It supports that harness's host-overlap result only; it does not validate compositor slot contention, main-queue interference, or live frame timing.

## Smallest next gate

A future gate can instrument only the existing compositor path around each eye's slot-fence wait, command submit, ASTC fence wait/readback, and packet-ready return, then replay a short paired `WIVRN_ASTC_PARALLEL_EYES=0/1` offscreen compositor sequence with deterministic timeline input. Verify both submits precede either current-frame readback, image reuse follows both completions, and compare complete-call wall plus per-eye wait spans. Keep query data as attribution only; do not subtract it into a queue-delay or photon claim. The current source and scratch harness are unchanged by this audit.

## Verification scope

Read against source7b7ae360. Root independently checked the serial present loop, delayed encode-request publication, per-eye slot fences, parallel worker join and existing trace calls. This is a source audit, not a production compositor benchmark. The current local build has `WIVRN_USE_PERFETTO=OFF`; no trace or runtime option was enabled. The next bounded task checks whether the existing shared Perfetto CPU tracks can reliably represent overlapping threads before adding trace spans.
