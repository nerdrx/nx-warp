# ADR 0023: Bit-exactness stays; transmitted entropy parameters are a watch item

Status: Accepted, 2026-09-04

## Context

docs/RESEARCH-ACADEMIC.md entry 1 (MLVC, Pärnamaa et al., ECCV 2026) reports that a
learned codec can transmit its entropy model's scale parameters and tolerate small
arithmetic differences between encoder and decoder, removing the need for bit-exact
integer arithmetic across GPU vendors. The paper (3.9, 5.4) treats cross-vendor
determinism as the highest-risk item for any learned tool, and the scout recommends
relaxing the principle now.

## Decision

Bit-exactness remains a normative requirement of the NX Warp bitstream (ADR 0010).
Reasons:

1. Every component built tonight is verified by it: the GPU decoder, the warp, the
   passes, the fuzzers and the conformance vectors all rest on a zero-tolerance
   diff against the CPU reference. That is the project's strongest quality
   property and it has already caught real bugs (IDCT overflow, DC-plane QP,
   corner saturation) that a tolerance would have hidden.
2. Our entropy coder is rANS with static tables, where drift is catastrophic, not
   graceful. The MLVC result concerns a learned probability model; it does not
   transfer to the coding tools we use.
3. A tolerance-based normative path needs a conformance methodology we do not
   have (error bounds per stage, drift accumulation across the reference ring).

Transmitting entropy scale parameters is recorded as a watch item for the learned
tools of paper 5.4 only, to be revisited when such a tool is actually proposed.

## Consequences

No change to the spec. spec/09-conformance.md keeps tolerance zero. The learned
upsampler and in-loop filter of 5.4 must be bit-exact int8 on both sides (as the
paper already states) or be out-of-loop.
