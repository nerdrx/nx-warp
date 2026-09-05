# Architecture Decision Records

Every decision here was made in [docs/PAPER.md](../PAPER.md), the design source of truth. These
records exist so that a reader can find one decision, its cost, and the options it beat, without
reading 25,000 words. Where the paper's sections disagreed with each other, section 6 of the paper
reconciled them; those reconciliations are ADR-0001 through ADR-0009 and ADR-0015 through ADR-0017.

Nothing in these records has been measured. Where a number appears it is a design estimate from the
paper and is labelled as one. The Phase 0 gate (paper 3.4, `bench/`) is what replaces estimates with
measurements, and an ADR whose estimate turns out wrong gets superseded, not edited.

## Process

- One ADR per decision, numbered in the order it was written, never renumbered.
- Format: Context, Decision, Consequences, Alternatives considered, Status. Use
  [template.md](template.md).
- An ADR is immutable once Accepted. A changed decision gets a new ADR that supersedes the old one,
  and the old one's status line is updated to point at it. This is the only edit permitted to an
  accepted ADR.
- The paper stays the design narrative; the ADRs are the decision log; `ref/` and
  [SYNTAX.md](../SYNTAX.md) are normative for the bitstream. Where an ADR and the reference codec
  disagree, the reference codec is right and the ADR is stale. See [GOVERNANCE.md](../../GOVERNANCE.md).

## Index

| ADR | Decision | Status | Paper |
|---|---|---|---|
| [0001](0001-datagram-is-a-tile-run.md) | The datagram is a tile run, not a tile | Accepted | 4.1, 6.1 |
| [0002](0002-64x64-tiles.md) | Tile size is 64x64 luma | Accepted | 1.1, 6.2 |
| [0003](0003-rans-eight-lanes.md) | Interleaved rANS, eight lanes per tile, static tables | Accepted | 1.6, 3.2.2, 6.3 |
| [0004](0004-dc-plane-intra-no-directional-modes.md) | Intra is a DC plane, not directional modes | Accepted | 1.4, 3.2.4, 6.4 |
| [0005](0005-one-mv-per-tile-five-modes.md) | One quarter-pel vector per tile, five modes | Accepted | 2.1, 2.3, 6.5 |
| [0006](0006-acknowledged-neighbourhood-references-no-idr.md) | References are the newest acknowledged 3x3 neighbourhood; no IDR | Accepted | 2.6, 4.5, 6.6 |
| [0007](0007-pose-travels-twice.md) | The pose travels twice: in the frame header and as `pose_seq` | Accepted | 1.2, 4.1, 6.7 |
| [0008](0008-foveation-per-tile-not-in-the-warp.md) | Foveation is per-tile resolution and QP; the warp never sees a non-uniform grid | Accepted | 5.1, 6.8 |
| [0009](0009-no-multicast.md) | No multicast; multi-user shares the encode, not the air | Accepted | 4.8, 6.9 |
| [0010](0010-integer-only-normative-path-cpu-reference-is-the-spec.md) | Integer-only normative path; the CPU reference decoder is the specification | Accepted | 2.2, 3.7, 3.9 |
| [0011](0011-vulkan-compute-over-fixed-function.md) | Vulkan compute on both ends, not fixed-function video hardware | Accepted | abstract, 5.6, 5.8 |
| [0012](0012-ycocg-r-display-format-references.md) | YCoCg-R, with references stored in display format | Accepted | 1.3 |
| [0013](0013-degradation-ladder-blur-never-block.md) | Under budget pressure, blur, never block | Accepted | 4.6.1 |
| [0014](0014-layered-bitstream-hybrid-mode.md) | One layered bitstream serves pure compute and hybrid decode | Accepted | 1.7, 2.9, 3.5 |
| [0015](0015-compute-budget-verdict-pending-phase-0.md) | The compute budget verdict is pending Phase 0 | Proposed | 3.1, 3.4, 6.10 |
| [0016](0016-motion-smoothing-handover.md) | Motion smoothing consumes the codec's MV field | Accepted | 2.8, 6.11 |
| [0017](0017-fto-review-scope.md) | The freedom-to-operate review is scoped to four items, before Phase 3 | Accepted | 5.7, 6.12 |
| [0018](0018-glsl-via-glslang.md) | GLSL through glslang, not Slang or HLSL | Accepted | 3.10 |
| [0019](0019-cpp20-with-a-c-abi.md) | C++20 with a small C ABI, not Rust | Accepted | 3.10 |
| [0020](0020-apache-2-0-and-patent-hygiene.md) | Apache-2.0, and only public-domain or expired coding tools | Accepted | 1.9, 5.7 |
| [0021](0021-stream-level-color-space-ycbcr-passthrough.md) | Stream-level colour space, with a YCbCr 4:2:0 passthrough path | Accepted | 1.3, amends |
| [0022](0022-hybrid-mode-is-not-a-quality-tool.md) | Hybrid mode is not a quality tool, and does not gate Phase 3 | Accepted | 1.7, 3.5, 6.10 |
| [0023](0023-bit-exactness-stays.md) | Bit-exactness stays; transmitted entropy parameters are a watch item | Accepted | 3.9, 5.4 |
| [0024](0024-fec-class-a-only.md) | FEC parity is class A only, with no loss escalation | Accepted | 4.4, 6.6 |
| [0025](0025-directional-intra-is-negotiated.md) | Directional intra is a negotiated tool; restriction A is its default schedule | Accepted | 3.2.4, 6.4 |
| [0026](0026-sparse-coefficient-transfer.md) | The Pass A to Pass B coefficient buffer is sparse | Accepted | 3.2.1, 3.2.5 |
| [0027](0027-no-spatial-hybrid.md) | No spatial hybrid; foveation inside the codec is the lever | Accepted | 3.5, 5.1 |
