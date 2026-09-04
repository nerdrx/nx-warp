# ADR-0006: References are the newest acknowledged 3x3 neighbourhood; there is no IDR

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 2.6, 4.5, reconciled in 6.6
- **Affects**: `transport/shadow`, `rc/`, `vk/encoder`, `android/`

## Context

Two sections modelled references differently. Section 2 modelled a single previous-frame reference
with an encoder-side shadow of the client: the encoder keeps the last K = 8 frames plus a client
shadow buffer, and when feedback reports a lost tile it replays the client's deterministic
concealment on its mirror. Section 4 pointed out a hole in that: the warp reads reference pixels from
a window that crosses into neighbouring tiles, so a tile's reference is only safe if its whole 3x3
neighbourhood in the reference frame was received. If the encoder assumes otherwise, prediction
diverges silently and the error spreads.

## Decision

Combining both:

- The client holds a **four-slot reference ring** in display format, matching NVENC's DPB depth of 4.
- For tile `t` of frame N, the reference is the newest frame M in {N-1, N-2, N-3} whose 3x3 tile
  neighbourhood around `t` is fully acknowledged. `ref_delta = N - 1 - M`, a 2-bit datagram header
  field, where 3 means intra.
- "Acknowledged" means received, or lost and therefore filled by the deterministic concealment warp,
  which is identical to `WARP_SKIP` with the last vector. The encoder replays the same fill on its
  mirror ring, so the shadow stays exact.
- Per-band feedback carries a received-tile bitmap. A tile counts as received only if its datagram
  decrypted and its bitstream decoded without error, so a corrupt-but-delivered tile is a lost tile.
- The encoder keeps eight frames (about 90 ms) of history as bitstream plus decoded pictures.
- Rolling intra refresh of 1/T of the tiles per frame, T = 180 (2 s), selected by a fixed
  pseudo-random permutation so there is no visible refresh wave, and forced when
  `age_since_intra > T` or `concealed_count >= 3`.
- **There is no IDR.** Full intra happens only on stream start, profile change, or a bitmap history
  gap. The invalidate to refresh to IDR ladder and the NVENC DPB=4 workaround in WiVRn NX are retired
  when the codec is active.

## Consequences

- After a loss, visible drift lasts one round trip plus jitter, estimated at 10 to 30 ms on WiFi 6,
  and then heals completely without an intra refresh and without a frame-level IDR (paper 2.6).
- Because concealment is the same kernel as a legitimate skip, there is no separate concealment code
  path to test, and concealed pixels are legal references.
- Cost: on WiFi, with feedback RTT of 5 to 8 ms, the bottom bands often reference N-2. Estimated at
  5 to 10 percent more bits at equal PSNR (paper 4.5, 6.6). It is unmeasured and is a Phase 3 number.
  If it exceeds 15 percent the fix is feedback per half band at about 0.8 Mbit/s uplink.
- With no feedback for 4 frames every tile goes intra at the capped size: a QP jump, not a stall.
- The encoder must run a full-frame replay decode whenever feedback reports loss (estimated 0.2 ms on
  a 7900 XTX, 1 ms on an RX 580), and that replay must be bit-exact or the artefact is permanent until
  the next refresh. Loss-injection fuzzing with a bit-exact shadow assertion every frame is a Phase 2
  and Phase 3 gate (paper 2.11 risk 4).
- Rolling refresh costs an estimated 0.014 bpp, under 5 percent of the budget (paper 2.6).

## Alternatives considered

- **Single N-1 reference with a shadow, no neighbourhood check.** Rejected: silent divergence when a
  neighbour was lost, which is the worst failure mode available.
- **A full DPB with long-term references and B-frames.** Rejected by the whole architecture: B-frames
  add latency by construction and a DPB adds state the loss model would have to track per tile.
- **Client-side inpainting of lost tiles.** Rejected: non-deterministic relative to the encoder, and
  worse than the warp on VR content (paper 2.7).
- **IDR on loss (what ALVR and WiVRn do today).** Rejected: the bitrate spike is 4 to 8x the budget
  and stalls the pipeline; the whole point of per-tile references is to remove it.

## References

- Paper 2.6 (reference model), 2.7 (concealment), 4.5 (reference epoch), 6.6, 4.11
- ADR-0001 (loss unit), ADR-0005 (modes), ADR-0010 (determinism)
