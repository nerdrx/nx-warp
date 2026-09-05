# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project intends
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html) on the `nxvc` library once it
releases, with the bitstream version tracked separately in the stream header
(see [GOVERNANCE.md](GOVERNANCE.md)).

**There has been no release.** Everything below is unreleased pre-release work, and nothing in it has
been measured on target hardware. See [ROADMAP.md](ROADMAP.md) for what any of it is meant to become.

## [Unreleased]

### Added

**Design**

- The design paper, `docs/PAPER.md`: bitstream and coding tools, prediction and loss concealment, the
  Vulkan implementation and compute budget, transport and rate control, perception and the competitive
  landscape, cross-section reconciliation, and the roadmap.
- The degradation ladder (paper 4.6.1): blur, never block.
- `docs/ERRATA.md`, recording corrections to the paper found during implementation, with the paper text
  left as written.

**Specification**

- `spec/`, the normative specification: scope, references, definitions, conventions, bitstream syntax,
  semantics, the decoding process, transport, profiles and levels, conformance, plus annexes for
  tables, patents and open issues.
- `docs/SYNTAX.md`, the normative bitstream syntax document.
- `docs/TRANSPORT.md`, the normative transport document.
- `docs/STEREO.md`, `docs/RATECONTROL.md` and `docs/XR_EXT_NXWARP.md`.
- `docs/INTEGRATION.md` and `docs/INTEGRATION-DECISIONS.md`, the WiVRn NX integration plan.

**Reference codec**

- `ref/`, the bit-exact CPU reference encoder and decoder that is the normative specification of
  decoded output: headers, integer DCT, rANS entropy coding, DC-plane intra, quantisation tables,
  tool-bit gating, and the `nxv-enc`, `nxv-dec` and `nxv-info` tools.
- 32 conformance vectors in `tests/vectors/` with an md5 manifest, covering intra, lossless, alpha,
  resolution levels, custom tables, odd frame sizes and multi-frame streams.

**Vulkan**

- `vk/common/`: device context, resource helpers, capability probe and JSON probe output.
- `vk/decoder/passA/`: rANS entropy decode, model, layout and GLSL.
- `vk/decoder/passB/`: reconstruct pass, model, layout, `reconstruct.comp`, runtime self-test.
- `vk/encoder/`: E0 convert, E1 stats, E2 prefix, a CPU stats reference, and the YCbCr two-plane
  passthrough path.

**Prediction and transport**

- `warp/`: integer homography quantisation, the CPU reference warp, a warp oracle, and
  `warp_tile.comp`.
- `transport/`: wire format, packetizer, sender, receiver, AEAD, FEC, multipath scheduling and the
  encoder-side shadow model.
- `rc/` and `fov/`: tile classification, bit allocation, the degradation ladder, the decode-time
  governor, a synthesis harness, and foveation map generation.

**Phase 0 and platform**

- `bench/`: the Phase 0 gate application from paper 3.4, kernels K1 to K6, with an Android
  NativeActivity front end and a headless host front end, a thermal mode, a self-test against the CPU
  reference, and report tooling. Host results are recorded in `bench/results/`; **the gate has not been
  run on a headset**, and host numbers are explicitly not the verdict.
- `android/`: the client shell, with the UDP receive path, frame ring, HUD and feedback.
- `platform/`: Windows D3D11 interop probe, an llvm-mingw toolchain file, SPIR-V embedding, and a Wine
  smoke test.
- `hybrid/`: a Python simulator for the base-plus-enhancement layering, and -- since ADR 0022 closed
  that arrangement -- `nxvc-hybridsim spatial`, which measures the *spatial* one instead: a
  full-resolution HEVC periphery plus a fovea inset coded by the **real** codec from `build-ref`,
  composited with a feathered boundary. `docs/SPATIAL-HYBRID.md` and `hybrid/RESULTS-SPATIAL.md`.
- `stereo/`: a CPU experiment for inter-view prediction, with recorded simulation results and FTO
  scoping notes.

**Tooling and tests**

- `tools/quality/`: the quality harness, a synthetic VR test-material generator, comparison and report
  tooling, foveated metrics, BD-rate, and example reports.
- `corpus/`: manifest schema, generator-driven fetch and hash verification for test material.
- `examples/`: decode, encode, round-trip PSNR, tile walk and transport loopback examples.
- `python/`: package skeleton and tests.
- `fuzz/`: fuzz drivers.
- `tests/`: unit and conformance tests for `ref`, `warp`, `transport`, `rc`, `stereo`, `hybrid`, `vk`,
  the decoder and the encoder.

**Build and CI**

- Root CMake with per-component options, CMake presets, and the cross-compilation toolchain files.
- GitHub Actions workflows for CI, formatting and nightly runs; Dependabot; issue forms, a pull
  request template and a label set.
- `BUILDING.md`, `TESTING.md` and `RELEASE.md`.
- A Docker environment under `docker/` and helper scripts under `scripts/`.

**Project documentation**

- `docs/ARCHITECTURE.md`: the two-sided pipeline, the data model, threading and synchronisation,
  profiles, and diagrams of the end-to-end pipeline, the row-band timeline, the tile-run datagram, the
  decoder passes, the presentation state machine and reference eligibility.
- `docs/adr/`: 21 architecture decision records with an index and a template, covering the
  reconciliations of paper section 6 plus the compute, language, licensing, colour and degradation
  decisions.
- `docs/GLOSSARY.md`, `docs/FAQ.md`, `docs/THREAT_MODEL.md`, `docs/PERFORMANCE.md` (targets with an
  explicitly empty measured column), `docs/COMPATIBILITY.md` (support levels, all marked expected).
- `ROADMAP.md`, `SECURITY.md`, `GOVERNANCE.md`, `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1),
  `CONTRIBUTING.md`.
- `mkdocs.yml` and `docs/stylesheets/nx.css` for a Material for MkDocs site, with
  `.github/workflows/docs.yml` deploying it to GitHub Pages.
- `brand/`: logo, mark and social assets.

### Changed

- The datagram is a tile run rather than one tile per packet, reconciling the original brief against
  the packet-rate and header-overhead arithmetic
  ([ADR-0001](docs/adr/0001-datagram-is-a-tile-run.md)).
- v1 fixes eight rANS lanes per tile rather than choosing 1 to 32 per tile, so one binary runs on every
  subgroup width ([ADR-0003](docs/adr/0003-rans-eight-lanes.md)).
- The stream header carries a `color_space` field, with a YCbCr 4:2:0 passthrough path alongside
  YCoCg-R, because the Linux compositor delivers 4:2:0 and the client consumes 4:2:0
  ([ADR-0021](docs/adr/0021-stream-level-color-space-ycbcr-passthrough.md)).
- The normative inverse DCT shift chain is 7 then 13, not the paper's 7 then 12
  (`docs/ERRATA.md`).

### Known gaps

- **The Phase 0 gate has not been run on a headset.** Only a headless host run exists, which
  `bench/README.md` states is a regression signal rather than the verdict. Every performance figure in
  this project remains a design estimate ([docs/PERFORMANCE.md](docs/PERFORMANCE.md)).
- No exit criterion of any phase has been met, because no measurement has been taken
  ([ROADMAP.md](ROADMAP.md)).
- The freedom-to-operate review is scoped but not performed
  ([ADR-0017](docs/adr/0017-fto-review-scope.md)).
- `docs/WARP.md` and `docs/HYBRID.md` are in progress.
- There is no end-to-end streaming pipeline: components exist, the system does not run.

[Unreleased]: https://github.com/nerdrx/nx-warp/commits/main
