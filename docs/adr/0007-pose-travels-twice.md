# ADR-0007: The pose travels twice, cheaply

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 1.2, 4.1, reconciled in 6.7
- **Affects**: `transport/`, `ref/`, `android/`

## Context

Sections 1 and 4 disagreed about where the render pose lives. Section 1 put a 26-byte pose (7 x f16
quaternion plus 3 x f32 position) in the frame header, because the pose-warp predictor needs it and a
decoder should be able to decode a frame from the frame's own bytes. Section 4 sends only a 16-bit
`pose_seq` per datagram, an index into the client's own 2 second pose ring, because the headset
generated that pose in the first place and re-sending it downstream is redundant.

Both are right about different failure modes. The ring can have a gap (client restart, a stall long
enough to wrap, a late joiner); and a per-datagram pose would cost 26 bytes on every one of about
13,000 datagrams per second.

## Decision

Both, at the cost of 26 bytes per band.

- The **frame header carries the pose**, and the frame header is replicated in the first datagram of
  every band, so a client with a gap in its pose ring still decodes.
- **Every datagram header carries the 16-bit `pose_seq`**, so the warp delta is normally computed from
  shared history that both ends already hold.
- The pose is never otherwise sent downstream: the client keeps a 2 second ring (about 1000 entries at
  the tracking rate) and the server echoes the sequence number.

## Consequences

- Cost is 26 bytes per band, six bands per frame, about 0.14 Mbit/s at 90 Hz. Negligible.
- Frameless presentation becomes free: every tile knows the pose it was rendered for, so the client's
  reprojection can warp each output tile from its own `pose_seq` to the display pose (paper 4.3).
- The client pose ring must outlive the longest `pose_seq` in flight, which is a hard 2 second
  requirement on the client (paper 4.9).
- The homography itself is not re-derived from the pose on the wire: the server quantises H to nine
  int32 in Q8.24 and sends those, so both ends use identical integers (ADR-0010). The pose in the
  frame header is what makes the client's own reprojection and any recovery path possible.

## Alternatives considered

- **Pose only in the frame header.** Costs nothing extra but loses the shared-history property that
  makes per-tile pose lookup at presentation time possible.
- **`pose_seq` only.** Rejected: a ring gap becomes an undecodable frame for no saving worth having.
- **Pose in every datagram.** Rejected on 26 bytes times about 13,000 datagrams per second.

## References

- Paper 1.2 (frame header), 4.1 (datagram header), 4.3 (frameless presentation), 6.7
