# Stereo coherence guard audit

## Finding

**Reject the decoded-common-ID guard and keep the per-eye age trial off for stereo.** Exact decoded peer IDs exist in `latest_frames`, but requiring them is circular: the incomplete front blocks its own newer complete frame from reaching the decoder. The guard would defer to existing skew/window retirement and defeat the intended deadline.

The ownership audit below records the initially considered query and its costs; the follow-up establishes why it is not useful. No such guard was implemented. A common complete-ID check across reassembly windows might avoid that cycle, but adds peer coupling and does not prove decoder acceptance. It is not recommended as a production change without a separately justified bounded experiment.

## Ownership and publication

`stream::create()` starts one `network_thread`, which polls and dispatches
packets in `stream::process_packets()` ([client/scenes/stream.cpp:318-328],
[client/scenes/stream_network.cpp:34-47]). Data and parity callbacks take `decoder_mutex`
shared and call the selected accumulator synchronously
([client/scenes/stream_network.cpp:237-258]). `push_shard()` performs insertion, FEC repair,
NACK processing, then `pump()` on that network callback ([client/decoder/shard_accumulator.cpp:101-139]).
Since `try_nack(now)` runs just before `pump(now)`, a threshold-crossing arrival
can send a retransmission request and retire its frame in the same callback; a
later repair is refused as too old.
The pump can submit a complete frame to its decoder, but the independent ASTC
decoder queues it for its worker; the queue is capped at two and can drop its
oldest queued packet ([client/decoder/astc/decoder.h:28-31], [client/decoder/astc/decoder.cpp:203-234]).
The worker decodes/uploads, then calls `push_blit_handle()`
([client/decoder/astc/decoder.cpp:244-270], [client/decoder/astc/decoder.cpp:467-479]).

`push_blit_handle()` first takes `decoder_mutex` shared, then `frames_mutex`
exclusive. It inserts successfully decoded visible handles into the per-stream
rolling ring and stores motion metadata separately ([client/scenes/stream.cpp:970-979],
[client/scenes/stream.cpp:1051-1065]). Handles remain owned by `shared_ptr`s in those rings
and in `current_blit_handles`; a guard needs only compare IDs while holding the
mutex and must not return a handle after unlocking.

The render thread calls `common_frame()` while holding `decoder_mutex` shared;
`common_frame()` takes `frames_mutex` ([client/scenes/stream.cpp:1860-1864],
[client/scenes/stream.cpp:1104-1109]). It intersects exact frame IDs across all active view
streams ([client/scenes/stream.cpp:1156-1198]). Independent ASTC with zero de-jitter chooses
the newest common ID ([client/scenes/stream.cpp:1201-1241]). When the current independent
ASTC pair has matching IDs and no newer common ID remains, it returns that
current pair ([client/scenes/stream.cpp:1204-1212]). Thus the display keeps a coherent image
during a mismatch; it does not combine different eye IDs.

The visible image history has four array slots, with three retained by default
and a fourth only when `debug.wivrn.nx.motion_retain4=1`
([client/scenes/stream.h:67], [client/scenes/stream.cpp:138-147], [client/scenes/stream.cpp:1058-1064]). Other bounded
histories are 32 motion-pose entries ([client/scenes/stream.cpp:1051-1056]), 16 ASTC motion
references with max age eight ([client/decoder/astc/decoder.h:57-69],
`nxastc_packet::motion_reference_capacity`), and two pending ASTC packets.

## What can be reused

`complete_ring` is a four-entry lock-free scalar ring, but it publishes only
stream 0's decoded frame ID/display time/completion time
([client/scenes/stream.h:215-239], [client/scenes/stream.cpp:1006-1021]). Its type deliberately excludes
image pointers, and its producer is serialized by `frames_mutex`
([client/utils/frame_ring.h:83-108], [client/utils/frame_ring.h:126-163]). It cannot establish that the
other eye has decoded the same ID.

`latest_frames` is the right evidence: its entries are published only after a
decoder produces a visible handle, and the renderer uses the same rings for
exact intersection. A minimal guard for independent two-eye ASTC would, only
after the local two-period age predicate passes, lock `frames_mutex` and ask
whether any ID newer than the front exists in every active view ring. The query
should inspect IDs only and return a boolean. The packet callback already holds `decoder_mutex` shared, so
the new query must not reacquire that non-recursive shared mutex. `stream_roles`
and the decoder array are stable under the caller's existing lock. Retiring on
this evidence would preserve the current default-skew retirement rule; it would
gate only the new age path. It cannot guarantee repair retention past the
existing skew-three rule, which remains unchanged.

This is lock-order compatible with current code: decoder publication and
render selection both acquire `decoder_mutex` before `frames_mutex`; setup
does the same ([client/scenes/stream.cpp:977-979], [client/scenes/stream.cpp:1108],
[client/scenes/stream.cpp:3394-3409]). Several render helpers take `frames_mutex` while the
render path already holds `decoder_mutex` shared ([client/scenes/stream.cpp:1860-1864],
[client/scenes/stream.cpp:2549-2551], [client/scenes/stream.cpp:2731-2733]). No inspected path takes
`frames_mutex` and then asks for `decoder_mutex`.

## Costs and limits

The check would block the network packet callback behind render's short
`frames_mutex` critical section. `common_frame()` also has a bounded optional
wait, but releases `frames_mutex` during that wait ([client/scenes/stream.cpp:1110-1141]).
The guard should be called only after the age threshold, not for every ordinary
arrival. If no common decoded successor exists, each later shard may retry the
lock until a pair appears or the normal skew/window rule advances the front.
That adds work exactly on loss paths, where packet processing and NACK timing
already matter.

The check is a point-in-time snapshot. The other eye can evict the matching
ring entry immediately after unlock and before the next render selection.
That cannot create a mismatched pair: selection still intersects IDs and keeps
the pinned current pair. It does mean the guard proves a common decoded ID
existed, not that the renderer presented it. An invisible scene does not
publish into `latest_frames` (`push_blit_handle()` returns before taking the
locks at [client/scenes/stream.cpp:970-978]), so the guard would conservatively defer to the
existing skew/window limits while invisible.

Adding a second lock-free coherent-frame ring could avoid the packet-path
mutex, but that is new publication machinery and is not justified for this
default-off experiment. Reusing `complete_ring` by changing its meaning would
also alter the render/JIT readiness signal, so it is not a safe shortcut.

## Rejected proposed experiment

The initially proposed `frames_mutex` query is superseded by the causal check below. Do not implement it as an age-retirement gate.

## Follow-up: decoded-common guard is circular

The proposed `latest_frames` guard cannot release the condition it is meant to
check. `frame_window::drain()` visits only `front()` and returns on `step::wait`
unless a stale predicate advances it ([client/decoder/frame_window.h:188-212]).
`try_submit_front()` may append contiguous partial shards to the decoder, but
returns `wait` until the front is complete; only then does it call
`frame_completed()` ([client/decoder/shard_accumulator.cpp:361-420]). A newer
complete set can therefore be recorded in `window.note_complete()`, but remains
behind the incomplete front and is never submitted to that eye's decoder. ASTC
handles enter `latest_frames` only after a completed decode/upload reaches
`push_blit_handle()` ([client/decoder/astc/decoder.cpp:203-234, 467-479];
[client/scenes/stream.cpp:1051-1065]). Thus, while eye A's front is blocked,
A cannot publish a newer decoded ID; there cannot be a newer decoded common ID
in both eyes. Waiting for that evidence prevents age retirement indefinitely
(or until existing skew/window retirement fires), so the previous proposed
`frames_mutex` query does not provide a useful age guard.

A same-ID complete reassembly in both eye windows could avoid that cycle: all
video datagrams are dispatched serially by the one `process_packets()` polling
thread ([client/scenes/stream.cpp:318-328]; [client/scenes/stream_network.cpp:34-47]),
and each receive callback synchronously inserts, FEC-repairs, notes completion,
tries NACK, then pumps ([client/decoder/shard_accumulator.cpp:101-139,
155-170]). A bounded query over the other accumulator's six-slot window could
in principle compare complete IDs without a new lock. But the current API only
exposes a local `has_newer_complete_than_front()` boolean, not exact complete
IDs; exposing/consulting a peer window would add cross-accumulator coupling and
would need to prove that the active composited streams are exactly the two
independent ASTC eyes. A partial/shard-complete pair is also weaker evidence
than the decoded common pair: it says both eyes have all datagrams now, not
that both decoders will accept and publish those frames. No such guard is
implemented or validated here.

**Decision:** reject the decoded-common-ID guard and do not extend the current
age trial for stereo. Keep default behavior (including NACK-before-pump and
skew-three retirement); do not claim the age rule has a safe stereo-coherent
benefit. The only bounded next experiment, if specifically justified later, is
a scratch-only test of exact common complete IDs across the two reassembly
windows and their relationship to decoder acceptance. That would first require
an explicit, bounded read-only query design; it is not a production change or
recommendation to add cross-window access now. Prefer improving the actual
independent transport/retransmission path over widening this frame-retirement
policy.
