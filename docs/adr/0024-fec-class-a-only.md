# ADR 0024: FEC parity is class A only, with no loss escalation

Status: Accepted, 2026-09-04 (amended the same day after the deadline controller fix, TRANSPORT.md D24)
Amends: paper 4.4, TRANSPORT.md D23

## Context

The transport simulator (transport/RESULTS.md, 21 rows, 900 frames each) compared
no parity, class A parity only, and the paper's 30/10/0 policy at 300 Mbit on WiFi
across six loss scenarios, with a 600 Mbit control.

| Setting at 300 Mbit | Concealed tiles per frame, mean over scenarios | Tiles on N-1 |
|---|---|---|
| No parity | 151 | 98 percent |
| Class A only | 135 | 2 percent |
| 30/10/0 | 162 | 1 percent |

Class A only beat no parity in six of six scenarios. The full policy beat no parity
in one. At 600 Mbit both parity settings win clearly (41 and 45 concealed against
154). The mechanism is the band deadline: parity is extra bytes in the same band
window, so parity for band b delays band b+1 into its own deadline when the link has
no headroom. A second-order cost is that parity pushes nearly every tile onto an
N-2 reference, which the paper prices at a further 5 to 10 percent of bits.

Structural resilience holds regardless: the intra fraction never exceeds 1.6 percent
in any row because a concealed tile remains an exact reference.

## Decision

1. Class A (fovea) parity is always on at the nominal ratio, floor one parity block.
2. Class B and C parity are off. With the deadline controller fixed (D24), the
   class B row was negative in all eight scenarios, including at 60 and 70 percent
   headroom. The earlier evidence for a headroom-gated ladder was an artefact of a
   dead zone in the controller's climb rule.
3. No loss escalation of parity. On a loaded link most measured loss is congestion
   loss caused by the parity bytes themselves, so escalating on loss is a positive
   feedback loop.
4. The headroom estimator stays implemented and tested but is not the default; it is
   kept for a re-run against a perceptual metric before class B is removed from the
   syntax rather than just the policy.
5. Retransmission and per-tile re-prediction remain the primary loss tools.

Measured at 300 Mbit on WiFi with no link loss, against the previous default: wire
overhead 21.4 to 17.2 percent, blended parity 17.2 to 11.3 percent, concealed tiles
92.7 to 47.1 per frame. Class A alone beat no parity in 7 of 8 scenarios.

## Consequences

Paper 4.4's 14.5 percent blended FEC target and its loss-keyed ladder are retired.
Expected wire overhead at 300 Mbit on WiFi is about 17 percent. The sweep counts
concealed tiles, not quality; it must be re-run against a perceptual metric since a
concealed fovea tile and a concealed periphery tile are not the same event.
