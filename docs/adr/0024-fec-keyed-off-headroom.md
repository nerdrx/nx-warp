# ADR 0024: FEC parity is keyed off link headroom, class A always, B and C conditional

Status: Accepted, 2026-09-04
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

1. Class A parity is always on with a floor of one parity block per group.
2. Class B and C parity are enabled only when measured link headroom (link rate
   minus wire rate, from the per-path feedback statistics) exceeds a threshold set
   from the sweep and recorded in TRANSPORT.md D25.
3. The parity ladder keys off headroom first and measured loss second.
4. Retransmission and per-tile re-prediction remain the primary loss tools.

## Consequences

Paper 4.4's 14.5 percent blended FEC target is retired. Expected wire overhead at
300 Mbit on WiFi is about 18 percent with class A only, and about 21 percent when
headroom permits the full policy. The sweep must be re-run after the deadline
controller fix (D24) and again against a perceptual metric, since a concealed
fovea tile and a concealed periphery tile are not the same event.
