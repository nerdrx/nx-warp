# ADR-0017: The freedom-to-operate review is scoped to four items, before Phase 3

- **Status**: Accepted. The review itself has not been performed.
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 1.9, 5.7, reconciled in 6.12
- **Affects**: the whole project, and the release process

## Context

The codec is built from public-domain and expired tools by policy (design principle 7, ADR-0020), but
"assembled from safe parts" is not the same as "the assembly is safe". Four constructions in this
design have live commercial neighbours.

This is an engineering map of where the mines are, not legal advice.

## Decision

No freedom-to-operate cost is incurred before Phase 2, because Phase 0 to Phase 2 are research code
with no distribution. **Before Phase 3 ships in a WiVRn NX release, a formal review of exactly four
items:**

1. **Pose delta as global motion parameters for a streamed VR frame.** The nearest relatives are
   expired MPEG-4 Part 2 global motion compensation (1999) and royalty-free AV1 global motion, both of
   which use the same corner-then-interpolate structure. Neither ties the parameters to a tracked
   pose. Meta, Qualcomm, NVIDIA and Microsoft filings on pose-based prediction for split rendering
   (roughly 2016 to 2020, plus Microsoft's Holographic Remoting work) must be searched.
2. **View synthesis prediction in 3D-HEVC against the `STEREO` mode.** It is the closest patented
   relative of inter-view prediction driven by a known camera geometry.
3. **The LCEVC family (MPEG-5 Part 2, V-Nova) against the enhancement layer.** Specifically the
   enhancement-over-hardware-base structure, which is the strongest overlap in the design.
4. **The 2022 Microsoft rANS patents against the fixed-precision construction.** Those claims concern
   selective switching of state precision and related encoder features; v1 uses a single fixed state
   width and a single fixed probability precision precisely to stay outside them.

Alongside the review, one continuing obligation from day one: **keep a written record of the
public-domain or expired source for every tool.** Paper 1.9 and 5.7 are that record's starting point
and the ADRs extend it.

H.264 and HEVC are only ever touched through the device's own licensed hardware decoder.

## Consequences

- Phase 3 has a non-engineering exit criterion, and it is listed as such in the roadmap.
- Each of the four items has a defined fallback, so a negative finding is a downgrade rather than a
  dead end:
  - pose-warp: the warp mechanism itself has expired and royalty-free prior art; the exposure is the
    pose linkage.
  - `STEREO`: droppable. It is Phase 4, off in Lite, and worth an estimated 5 to 10 percent on average.
  - enhancement layer: fall back to spatial-only scalability, as in H.263 Annex O (1998, expired),
    disabling the temporal hypothesis (ADR-0014).
  - rANS: the offset-table and bit-plane fallbacks exist for other reasons and would serve here too.
- The review is scoped, so it is affordable. An unbounded FTO review of a video codec is not.

## Alternatives considered

- **Full FTO review before Phase 0.** Rejected: expensive, and premature for research code that is
  not distributed.
- **No review at all.** Rejected: the project intends to ship inside a WiVRn NX release.
- **Design around every possible claim in advance.** Rejected as unachievable; the design instead
  keeps a documented fallback for each of the four exposed constructions.

## References

- Paper 1.9 (patent hygiene), 5.7 (patent and royalty summary), 6.12
- ADR-0003 (rANS construction), ADR-0014 (hybrid layer), ADR-0020 (licence and tool policy)
