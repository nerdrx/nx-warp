# ADR-0001: The datagram is a tile run, not a tile

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 4.1, reconciled in 6.1
- **Affects**: `transport/`, `vk/encoder` (E5), `android/`

## Context

The original brief said "tile = packet": one 64x64 tile, one datagram, so a lost datagram is exactly
one lost tile and the concealment unit and the loss unit are identical. Sections 3 and 4 of the paper
independently rejected it by arithmetic.

At 150 Mbit/s and 90 Hz a stereo frame is 208 KB, so an average 64x64 tile carries about 90 bytes
(about 22 bytes at 32x32). UDP/IP plus codec header is about 52 bytes per datagram, which would very
nearly double the bitrate in headers alone. It would also mean 208,000 datagrams per second, against
an XR2 Gen 1 practical UDP receive ceiling of 50,000 to 100,000 packets per second even with
`recvmmsg` (paper 4.1, 6.1; both figures are design estimates).

The opposite end of the range fails too: at 1 Gbit/s a fovea tile is 4 to 8 KB, above any MTU, so
"tile = packet" is not even expressible there.

| Quantity | Value | Kind |
|---|---|---|
| Average tile payload at 150 Mbit, 90 Hz, 64x64 | about 90 bytes | estimate |
| Datagrams per second if tile = packet | 208 k | estimate |
| XR2 Gen 1 practical UDP receive ceiling | 50 to 100 k pps | estimate |
| Datagrams per second with 1400-byte tile runs | about 13 k | estimate |

## Decision

A datagram carries a **tile run**: a contiguous sequence of tiles from one tile row, packed until a
payload budget of about 1400 bytes is reached. Tiles remain independently decodable bitstreams; the
datagram is only the loss unit and the tile stays the concealment unit.

- Fixed 24-byte header, little endian, sent in the clear as AEAD associated data. It enumerates
  `tile_first` and `tile_count`, so a lost datagram maps to a known set of lost tiles and per-tile
  reference tracking still works.
- A 4-byte per-tile directory entry (QP, mode, byte length) at the head of the payload.
- MTU budget 1400 bytes to leave room for WireGuard or Tailscale style tunnels and 802.11
  encapsulation. The connect handshake probes DF datagrams of 1400, 4000 and 8900 bytes per path and
  uses the largest that echoes; USB NCM/RNDIS often accepts 9000.
- Oversize tiles are not fragmented in normal modes. The rate controller caps a tile at
  `max_tile_bytes` = 1400 - 24 - 4 = 1372 bytes, and the encoding workgroup re-encodes at QP + 6 if
  the entropy coder overruns, at most twice, in the same dispatch.
- The single exception is lossless UI text tiles, whose worst case is 12 KB for 64x64 4:4:4 8-bit.
  They may fragment into at most 4 datagrams, in the Pro profile only, and a tile with any fragment
  missing is a lost tile. The rate controller keeps fragmented tiles under 1 percent of the stream.

Every mention of "tile as packet" elsewhere in the project reads as "tile run".

## Consequences

- Header overhead falls to about 5.5 percent (104 header bytes against 1800 payload bytes for a run
  of 20 average tiles) from an estimated 30 to 50 percent (paper 4.1).
- Packet rate lands at about 13 k pps at 150 Mbit and about 90 k pps at 1 Gbit. The latter is the
  honest ceiling of the client receive path, which is why 1 Gbit over WiFi is not a first-release
  target and jumbo MTU on USB is the mitigation (paper 4.11).
- Loss granularity is coarser than a tile: one lost datagram loses up to about 12 tiles of one row.
  This is survivable only because concealment is per tile and deterministic (ADR-0006), and because
  FEC is prioritised by tile class (paper 4.4).
- E5 must do real work: a prefix sum over tile lengths, greedy packing under the payload budget, and
  header writing, all on the GPU (paper 4.6).
- The rate controller acquires a hard per-tile byte cap, which constrains quality at the fovea at low
  bitrates and forces the re-encode path in E2/E4.

## Alternatives considered

- **Tile as packet.** Rejected on packet rate and header overhead, above. It is the cleanest model and
  the arithmetic simply does not allow it.
- **RTP.** Rejected: 12 bytes of header plus a timestamp model built around whole-frame media time,
  which fights per-tile poses (paper 4.1).
- **QUIC datagrams.** Rejected: userspace stack cost on the XR2 with no gain over raw UDP, given that
  the project brings its own FEC and its own retransmission-free loss model.
- **IP fragmentation.** Rejected: one lost fragment loses the whole datagram, which is strictly worse
  than choosing the run size ourselves.
- **Whole-row datagrams.** Not viable at 1 Gbit (a row exceeds the MTU) and wasteful at 20 Mbit.

## References

- Paper 4.1 (datagram layout), 4.4 (FEC classes), 6.1 (reconciliation), 4.11 (Android receive path)
- [docs/TRANSPORT.md](../TRANSPORT.md), normative
- ADR-0002 (tile size), ADR-0006 (per-tile reference tracking)
