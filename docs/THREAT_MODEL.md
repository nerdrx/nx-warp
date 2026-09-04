# Threat model

What NX Warp defends, how, and what it explicitly does not defend. The transport design is in
paper 4.1, 4.4 and 4.8, and the normative wire format is [docs/TRANSPORT.md](TRANSPORT.md).

This document describes a design. None of it is implemented and audited yet; see
[ROADMAP.md](../ROADMAP.md) for what exists. Reporting policy for vulnerabilities is in
[SECURITY.md](../SECURITY.md).

## 1. What the system is

A PC on a local network sends a real-time video stream over UDP to a headset on the same network,
usually over WiFi 6 and sometimes over USB tethering, sometimes both at once. Users do run this over
WireGuard or Tailscale style tunnels, which is why the MTU budget leaves room for them (paper 4.1).

Session establishment, authentication and key agreement belong to the host application (WiVRn NX), not
to the codec. The codec's transport starts from a session key that the host handshake has already
agreed, and derives per-path subkeys from it.

## 2. Assets

| Asset | Why it matters |
|---|---|
| Video content | It is the user's screen: desktop panels, private VR spaces, chat windows, whatever is rendered |
| Head pose and gaze | Biometric-adjacent. Gaze in particular is sensitive, and it travels upstream when eye tracking is on |
| Session key material | Compromise means content disclosure and injection |
| Availability of the session | A stream that can be stalled or corrupted by a third party on the LAN is a usable denial of service against a headset the user is wearing |

## 3. Adversaries

| Adversary | Capability | In scope |
|---|---|---|
| Passive network observer on the same LAN or WiFi | reads every datagram, including the cleartext header | yes |
| Active off-path attacker | injects spoofed datagrams, guesses sequence numbers, cannot see traffic | yes |
| Active on-path attacker | reads, drops, delays, reorders, replays and modifies any datagram | yes |
| Malicious or buggy peer | a compromised server sending a hostile bitstream to a client, or a compromised client sending hostile feedback to a server | yes |
| Compromised host OS on either end | full memory access on that end | no |
| Physical attacker with the headset | no |
| Traffic analysis (packet sizes and timing reveal content activity) | partially, see 6 |

## 4. Defences

### 4.1 Confidentiality and integrity of payloads

AES-256-GCM per datagram with a 16-byte tag. ChaCha20-Poly1305 is the negotiated fallback for hosts
without AES instructions (paper 4.1).

- **The 24-byte header is associated data, not ciphertext.** The receiver must read `frame_id`,
  `tile_first`, `path_seq` and the FEC fields before it can do anything with a datagram, and FEC
  repair operates across datagrams. The header is therefore authenticated but not confidential, and
  any modification to it fails tag verification.
- **The nonce is never on the wire.** It is derived from `(stream_id, path_id, path_seq, epoch
  counter)`. Both ends can reconstruct it from the header plus session state.
- **Per-path subkeys** via `HKDF(session_key, path_id)`, so that a key stream is never reused across
  the USB and WiFi paths even though they carry independent sequence spaces.
- GCM is a stream mode, so there is no padding and no alignment constraint on tile bitstreams, and no
  padding oracle.

**Nonce reuse is the critical invariant.** AES-GCM fails catastrophically if a (key, nonce) pair
repeats: it leaks the XOR of two plaintexts and, worse, allows forgery of the authentication key.
The design implications, which the implementation must honour:

- `path_seq` is 14 bits and wraps roughly every 16,384 datagrams, which at 13,000 datagrams per second
  per path is about once a second. The epoch counter is what makes the nonce unique across wraps, so
  it must increment on every wrap, must be tracked per path, and must be part of the derived nonce on
  both ends.
- A session key must never be reused across sessions, reconnects or epoch-counter resets. Rekey on
  reconnect.
- The encoder writes datagrams from a GPU-filled ring; the encryption step must not be able to encrypt
  the same buffer slot twice under the same sequence number, which is a real risk when a send is
  retried at the socket layer.

### 4.2 Replay

Replay protection is per path, using `path_seq` with a sliding receive window:

- A datagram whose `path_seq` falls before the window, or which has already been seen inside it, is
  discarded before decryption is attempted.
- The window must be wide enough for genuine reordering across multipath striping and FEC groups, and
  narrow enough to be a meaningful defence. Its width is an implementation parameter of
  `transport/receiver`.
- Position addressing limits the damage a replay could do even if it slipped through: a tile lands in
  the ring by `(frame_id, tile)`, and a `frame_id` outside the four-slot ring is simply discarded
  because the frame buffer has moved on (paper 2.6, 4.8). A stale tile therefore cannot overwrite a
  newer one.
- The epoch counter must advance on `path_seq` wrap so that a replayed datagram from the previous
  epoch fails authentication rather than merely being out of window.

### 4.3 Feedback spoofing

The feedback packet is the most security-sensitive thing on the uplink, because the encoder's entire
reference model is built from it. An attacker who can forge feedback can:

- **Claim tiles were received that were not.** The encoder then predicts from a reference the client
  does not hold, and the artefact persists until the next rolling refresh of that tile (up to 2 s).
- **Claim tiles were lost that were not.** The encoder falls back to older references or to intra,
  which wastes bits: a bounded quality and bitrate attack.
- **Report false `decode_us`, RTT or loss.** The governor sheds work and the bitrate controller backs
  off, degrading the session to the lowest rung of every ladder.

Defences:

- **Feedback is inside the same AEAD envelope as everything else**, with its own path sequence space
  and the same replay window. An attacker who cannot forge a tag cannot forge feedback. This is the
  primary defence and it is sufficient against off-path and passive adversaries.
- Feedback is cumulative over the last three bands, so dropping or delaying a feedback packet costs
  nothing and gains an attacker nothing (paper 4.4).
- Rolling intra refresh bounds the damage of any accepted false-positive acknowledgement to at most
  T = 180 frames (2 s) for a given tile, and `concealed_count >= 3` forces intra independently of what
  feedback claims (paper 2.6). The refresh mechanism exists for shadow-model bugs and bitmap gaps; it
  happens also to bound this attack.
- Implementations should treat wildly implausible feedback (a `decode_us` beyond the frame period by
  orders of magnitude, an RTT below the physical floor, a bitmap for a frame outside the history
  window) as corrupt and discard it rather than acting on it. This is defence in depth against bugs on
  the client as much as against attackers.

### 4.4 Hostile bitstreams

A compromised or buggy server can send a client anything. The decoder is the largest attack surface in
the project, and it parses untrusted input on both the CPU (headers, tile directory) and the GPU (the
entropy payload).

Design defences (paper 3.7, 3.9):

- **Every buffer and image load is bounds-clamped in the shader.** `robustBufferAccess` behaviour
  differs across vendors (zero versus garbage) and the codec explicitly cannot depend on it.
- **Coefficient clamping ranges are normative**, so overflow cannot differ by vendor and cannot be
  used to reach an implementation-specific path.
- **No integer division or modulo in normative shaders**, which removes divide-by-zero traps.
- **A decoder that sees an unknown mandatory tool bit refuses the stream** rather than guessing
  (paper 1.2).
- **Fuzzing is a release gate, not a nicety**: libFuzzer on the reference decoder with a
  structure-aware mutator over tile boundaries and rANS state fields, with the properties "never reads
  out of bounds" and "always emits a frame"; the GPU decoder is fuzzed with the same corpus under the
  Khronos validation layers with GPU-assisted validation on lavapipe, where timeouts count as bugs.
  Phase 1's exit criterion includes a 24 hour clean fuzz run.
- A malformed or truncated tile must fail as a lost tile (concealed, reported as not received), never
  as a crash or a hang. The conformance vector set includes truncated tiles for this reason
  (paper 3.9).

Denial of service by resource exhaustion is bounded by construction rather than by validation: tiles
are capped at `max_tile_bytes` = 1372 bytes, fragmentation is limited to 4 fragments and to lossless
tiles in the Pro profile, and the dispatch is one workgroup per tile over a fixed tile grid. There is
no unbounded loop driven by stream content in the decoder.

### 4.5 Availability

- Loss and reordering are expected states, not error states: FEC repairs random single losses at fixed
  latency and re-prediction from acknowledged references repairs bursts (paper 4.4).
- A stalled path is detected within one band (RTT above 3x its baseline, or 20 ms with no datagram
  while the other path flows) and its weight goes to zero; probing resumes with class C runs only
  (paper 4.8).
- A third party who can inject enough traffic to congest the air can degrade the session, and no
  codec-level defence changes that. It appears in the telemetry as RTT growth and bottom-band holes,
  and is answered with bitrate rather than buffering (paper 4.11).

## 5. What is deliberately out of scope

- **Session establishment, authentication and key agreement.** Owned by the host application. The
  codec's transport starts from an already-agreed session key. If that handshake is weak, nothing here
  helps.
- **Pairing and trust-on-first-use decisions.** Same reason.
- **Endpoint compromise.** A compromised host OS on either end reads the framebuffer directly; the
  codec is not a defence against it.
- **Physical access to the headset.**
- **DRM and content protection.** This codec does not attempt to protect content from the user of the
  headset, and it has no path to a protected output. Do not use it where that is a requirement.
- **Side channels in the decoder** (cache and timing). Content-dependent decode timing exists and is
  even reported in telemetry as `decode_us`. It is not treated as an attack surface.
- **Metadata minimisation at the IP layer.** Source and destination addresses, and the existence of a
  session, are visible to anyone on the network.

## 6. Known residual exposures

These are accepted, and stated so that nobody discovers them as a surprise.

- **The datagram header is cleartext.** An observer learns `frame_id`, `tile_first`, `tile_count`,
  `layer_id`, `ref_delta`, `pose_seq`, path and FEC structure, `tx_ts` and `payload_len` for every
  datagram. It is authenticated, so it cannot be modified, but it is readable.
- **Traffic analysis is effective against this design.** Payload sizes are content-dependent by
  construction, and the rate controller makes that dependence sharper: a scene cut, a head turn, a
  bright flash and an idle menu all have distinctive bitrate signatures. `ref_delta` and the
  intra-tile pattern additionally leak where loss is happening. Padding to hide this would cost
  exactly the bits the codec exists to save, and it is not done.
- **`pose_seq` is on the wire in cleartext.** It is an index, not a pose, so an observer learns the
  rate and continuity of head tracking but not the pose itself. The pose in the frame header is inside
  the AEAD envelope.
- **Gaze data travels upstream when eye tracking is on**, piggybacked on the pose packet
  (paper 5.1.4). It is inside the AEAD envelope on the wire. Whether it is stored, and where, is a
  host application question and should be treated as sensitive.
- **The hybrid path's base layer goes through MediaCodec**, a large vendor-supplied surface outside
  this project's control. Its hardening is the platform's.
- **`enc_us` and `tx_ts` are telemetry in the clear**, which leaks server-side timing behaviour to an
  observer. They are in the header because the receiver needs them before decryption is complete.

## 7. Implementation obligations

A checklist for anyone implementing or reviewing `transport/`:

1. Nonce uniqueness across `path_seq` wrap, per path, per session. Rekey on reconnect; never reuse a
   session key across an epoch reset.
2. Replay window per path, checked before decryption is attempted.
3. Constant-time tag comparison, provided by the AEAD implementation, and no partial-plaintext release
   before tag verification.
4. Header fields validated before use: `tile_first + tile_count` within the tile grid, `payload_len`
   against the received datagram length, `frame_id` within the ring history, `layer_id` within
   `num_layers`.
5. Tile directory entries validated: byte lengths must sum to no more than the payload, and no entry
   may point outside it.
6. Feedback plausibility checks, as above.
7. Key material zeroised on session teardown, and never written to logs or telemetry.
8. Fuzzing of `ref/` and of the receive path is part of the release gate, not a follow-up task.

## References

- Paper 4.1 (encryption and the datagram header), 4.4 (FEC and feedback), 4.8 (multipath),
  4.11 (failure modes), 3.7 and 3.9 (bit-exactness rules, testing and fuzzing), 1.2 (capability
  negotiation and unknown tool bits)
- [docs/TRANSPORT.md](TRANSPORT.md) (normative wire format), [SECURITY.md](../SECURITY.md) (reporting)
