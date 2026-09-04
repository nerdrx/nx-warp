# ADR-0009: No multicast; multi-user shares the encode, not the air

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 4.8, reconciled in 6.9
- **Affects**: `transport/`

## Context

A shared base layer sent once to N headsets is an obvious-looking win for multi-user sessions. The
obstacle is 802.11 itself: multicast frames go out at a basic rate, typically 6 Mbit/s or lower,
unacknowledged and unretried. A 100 Mbit/s base layer cannot be multicast on any consumer access
point. Access points that convert multicast to unicast simply send N copies, with more loss than the
server would have produced by sending N unicast streams itself, because the AP has no per-client
rate or FEC information from us.

## Decision

No multicast. Multi-user shares the **encode**, not the air: the shared base tiles are encoded once
and sent as N unicast streams, with per-user fovea tiles encoded per user. Wired multi-user through a
switch could use IP multicast later; that is not v1.

## Consequences

- Air bandwidth scales linearly with users, which caps practical multi-user on one access point.
- Encoder work does not scale linearly: the expensive part (E0 warp, E1 analyze, E2 transform, E3
  reconstruct on the shared tiles) is done once. Per-user work is the fovea tiles, packetisation and
  the per-user reference-tracking state.
- Every user keeps independent per-tile reference tracking and independent FEC, which is what makes a
  single lossy client not degrade the others.
- The shared-base optimisation implies the encoder can hold more than one client shadow ring, which
  is a design constraint on `transport/shadow`.

## Alternatives considered

- **802.11 multicast of the base layer.** Rejected on basic-rate transmission, no acknowledgement, no
  retry. This is a property of consumer WiFi, not something the codec can tune around.
- **AP multicast-to-unicast conversion.** Rejected: it is N unicast copies with less information about
  each client than we have.
- **IP multicast over wired Ethernet.** Deferred, not rejected. It is a viable later option for a
  wired multi-user installation.

## References

- Paper 4.8 (multipath and multi-user), 6.9
