# NX Warp documentation

A video codec built only for VR streaming: Vulkan compute on both ends, independent 64x64 tiles,
pose-warped prediction, no IDR, no CPU on the hot path. Library and codec identifier `nxvc`, designed
for [WiVRn NX](https://github.com/nerdrx/wivrn-nx).

> **Nothing has been measured on a headset.** Every performance figure in this documentation is a
> design estimate from the paper, labelled as one. The Phase 0 gate has not been run on a device; a
> headless host run exists in `bench/results/` and is a regression signal, not the verdict. See
> [PERFORMANCE.md](PERFORMANCE.md) and [ROADMAP.md](../ROADMAP.md).

## Start here

| If you want to | Read |
|---|---|
| Understand the system in one document | [ARCHITECTURE.md](ARCHITECTURE.md) |
| Know why it exists and what it is not | [FAQ.md](FAQ.md) |
| Look up a term | [GLOSSARY.md](GLOSSARY.md) |
| Know why a decision was made | [adr/](adr/) |
| Read the full design | [PAPER.md](PAPER.md) |
| Implement a decoder | [spec/](../spec/) and `ref/` |
| Know what actually exists today | [ROADMAP.md](../ROADMAP.md) |

## The documents, and what each is for

### Design

- **[PAPER.md](PAPER.md)** - the design paper, and the source of truth for the design. Section 6
  reconciles the places where its sections disagreed; section 4.6.1 is the degradation ladder;
  section 3.4 is the Phase 0 gate. It is a dated design document, not a living specification.
  Split by section: [front matter](00-front-matter.md), [1 bitstream](01-bitstream.md),
  [2 prediction](02-prediction.md), [3 Vulkan](03-vulkan.md), [4 transport](04-transport.md),
  [5 perception and landscape](05-perception-landscape.md),
  [6 reconciliation](06-reconciliation.md), [7 conclusion](07-conclusion.md).
- **[ERRATA.md](ERRATA.md)** - corrections to the paper found during implementation. The paper text is
  left as written; the normative documents carry the corrected values.

### Architecture and orientation

- **[ARCHITECTURE.md](ARCHITECTURE.md)** - the whole system: the PC encoder E0 to E5, the network, the
  headset decoder Pass A/B/C, the presentation ring, the data model, threading and synchronisation,
  profiles, colour space, and the control loops. With diagrams of the end-to-end pipeline, the
  row-band timeline, the tile-run datagram, the decoder dataflow, the presentation state machine and
  the 3x3 reference eligibility rule.
- **[GLOSSARY.md](GLOSSARY.md)** - every term of art, with the paper section that defines it.
- **[FAQ.md](FAQ.md)** - why not AV1, HEVC or JPEG XS; does it beat H.264; will it run on a Pico 4, a
  Quest, a Mali device; patents; licensing.

### Normative specification

The reference codec in `ref/` is normative for decoded output. These documents are normative for the
wire format and for behaviour `ref/` does not exercise.

- **[../spec/](../spec/)** - the specification proper: [scope](../spec/00-scope.md),
  [references](../spec/01-references.md), [definitions](../spec/02-definitions.md),
  [conventions](../spec/03-conventions.md), [bitstream syntax](../spec/04-bitstream-syntax.md),
  [semantics](../spec/05-semantics.md), [decoding process](../spec/06-decoding-process.md),
  [transport](../spec/07-transport.md), [profiles and levels](../spec/08-profiles-levels.md),
  [conformance](../spec/09-conformance.md), and annexes for
  [tables](../spec/annex-a-tables.md), [patents](../spec/annex-b-patents.md) and
  [open issues](../spec/annex-c-open-issues.md).
- **[SYNTAX.md](SYNTAX.md)** - the normative bitstream syntax.
- **[TRANSPORT.md](TRANSPORT.md)** - the normative transport: tile runs, AEAD, FEC, feedback,
  multipath.
- **[STEREO.md](STEREO.md)** - stereo inter-view prediction.
- **[XR_EXT_NXWARP.md](XR_EXT_NXWARP.md)** - the OpenXR extension carrying velocity, depth and stencil
  from the engine. It changes no bitstream syntax and the codec works without it.
- **WARP.md** - pose warp (in progress).
- **[RATECONTROL.md](RATECONTROL.md)** - rate control, allocation, the degradation ladder and the
  decode-time governor.
- **HYBRID.md** - the hybrid hardware-base path (in progress).

### Integration

- **[INTEGRATION.md](INTEGRATION.md)** - the WiVRn NX integration plan.
- **[INTEGRATION-DECISIONS.md](INTEGRATION-DECISIONS.md)** - the decisions the integration spike
  forced, including the 4:2:0 colour path.

### Engineering targets

- **[PERFORMANCE.md](PERFORMANCE.md)** - the budget tables from paper 3.1, 3.2.5 and 4.2 as targets,
  with a measured column that is empty on purpose, and instructions for filling it in.
- **[COMPATIBILITY.md](COMPATIBILITY.md)** - the GPU, OS and headset matrix with support levels
  (Full, Lite, hybrid only, unsupported), everything marked expected until measured.
- **[THREAT_MODEL.md](THREAT_MODEL.md)** - assets, adversaries, AEAD with the header as associated
  data, replay, feedback spoofing, hostile bitstreams, and what is deliberately out of scope.

### Decisions

- **[adr/](adr/)** - 21 architecture decision records with an [index](adr/README.md) and a
  [template](adr/template.md). Each one records the context, the decision, its consequences and the
  alternatives that lost. Accepted ADRs are immutable; a changed decision gets a new record.

## Project documents (repository root)

| Document | What it covers |
|---|---|
| [../README.md](../README.md) | The project at a glance |
| [../ROADMAP.md](../ROADMAP.md) | Phases, exit criteria, and what exists in the tree today |
| [../CHANGELOG.md](../CHANGELOG.md) | Keep a Changelog format; everything is currently unreleased |
| [../GOVERNANCE.md](../GOVERNANCE.md) | Who decides, and what has authority over what |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Build, style, tests, commit conventions |
| [../SECURITY.md](../SECURITY.md) | How to report a vulnerability, and the scope |
| [../CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md) | Contributor Covenant 2.1 |
| [../BUILDING.md](../BUILDING.md) | How to build |
| [../TESTING.md](../TESTING.md) | The test pyramid, how to run it and how to read it |
| [../RELEASE.md](../RELEASE.md) | Release process |
| [../LICENSE](../LICENSE) | Apache-2.0 |

## Component documentation

| Component | Directory | Its own notes |
|---|---|---|
| Reference codec | `ref/` | [ref/README.md](../ref/README.md) |
| Vulkan | `vk/` | [vk/README.md](../vk/README.md), [passA](../vk/decoder/passA/README.md), [passB](../vk/decoder/passB/README.md), [encoder](../vk/encoder/README.md) |
| Phase 0 benchmark | `bench/` | [bench/README.md](../bench/README.md) |
| Android client | `android/` | [android/README.md](../android/README.md) |
| Quality harness | `tools/quality/` | [tools/quality/README.md](../tools/quality/README.md) |
| Test corpus | `corpus/` | [corpus/README.md](../corpus/README.md) |
| Examples | `examples/` | [examples/README.md](../examples/README.md) |
| Windows platform | `platform/win/` | [platform/win/README.md](../platform/win/README.md) |
| Brand assets | `brand/` | [brand/README.md](../brand/README.md) |

## Reading order for a new contributor

1. [../README.md](../README.md), then [FAQ.md](FAQ.md), for what this is and what it is not.
2. [ARCHITECTURE.md](ARCHITECTURE.md), for the shape of the system, with
   [GLOSSARY.md](GLOSSARY.md) open alongside it.
3. [adr/README.md](adr/README.md), skimming the index and reading the two or three records that touch
   the part you care about.
4. [PAPER.md](PAPER.md) sections 1 and 2 if you are working on the codec, section 3 if you are working
   on the Vulkan implementation, section 4 if you are working on transport or rate control, section 5
   for perception and foveation.
5. [../ROADMAP.md](../ROADMAP.md) and [../CONTRIBUTING.md](../CONTRIBUTING.md), for where the work is
   and how to submit it.

## A note on numbers

The paper's estimates are back-of-envelope figures from vendor peak numbers and field experience with
other workloads, and the paper says they could be wrong by 2x in either direction. This documentation
labels them as estimates every time, cites the paper section they come from, and keeps a measured
column next to them that stays empty until somebody runs the benchmark. When a measurement arrives it
sits beside the estimate rather than replacing it, because the size of the error is the most useful
thing the table can record.
