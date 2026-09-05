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

**Entropy: `ENTROPY_LITE`, a table-free parallel entropy tool (OFF by default)**

- Stream tool bit **30** (`docs/SYNTAX.md` 9.10): a table-free coding of the coefficient payload with
  no arithmetic-coder state, so a tile decodes with three prefix sums and then one thread per coding
  unit -- and in the `FIXED` variant one thread per *coefficient*. Two variants, `FIXED` (fixed-width
  magnitude fields, **the one this merge ships**) and `RICE` (Exp-Golomb with an explicit body
  length, defined and reachable but not shipped), selected by the tile header's `table_set` field,
  which a stream with no probability tables has nothing else to mean. Mutually exclusive with
  `SIGN_HIDE` and `CUSTOM_TABLES`. `nxv-enc --entropy rans|lite-fixed|lite-rice`; conformance
  vectors v76-v81.
- Pass A decodes the `FIXED` variant behind specialisation constant 3, `ENTROPY_MODE`, at one
  64-thread workgroup per tile.
- **It ships OFF, and it is a NEGOTIATED tool rather than a default.** Whether the bits are worth
  the decode time is a question only the decoder can answer, from its own measured Pass A: on a
  Pico 4 it cuts Pass A **7.5x, 138.5 ms to 18.4 ms**, and it is the only bitstream-side lever that
  reaches the Adreno frame budget at all; on a desktop RADV, where Pass A already fits, it is 4.1x
  for bits nobody needs to spend. So the decoder asks for it at the handshake and the encoder
  obliges, which is what a tool bit is for. `ref/RESULTS-entropy-lite.md` has the measurement, the
  crossover (~140 Mbit/s on a 12 ms budget) and the caveats.
- The bit cost, measured on the merged encoder over the full clip at QP 24, is **+43.0 % at 4:4:4**
  and **+31.9 % at 4:2:0** -- not one number. The branch's "about +50 %" holds for 4:4:4 only;
  4:2:0 is cheaper than advertised because its two subsampled planes are a smaller and much sparser
  share of the coefficients, which is where `FIXED` is closest to what rANS spends.

**Bitstream syntax v1.6 -- the large transforms**

- Tool bit 27 `XFORM_LARGE` and the tile header's two-bit `xform_size` (word1 bits 29-30): a 16x16 or
  32x32 integer DCT for every plane of a tile instead of the 8x8 one. The transforms are the even/odd
  recursion on the existing Loeffler 8-point core with two constant rotation matrices on the same 512
  scale, 2D gains of exactly `2^21` and `2^22`, and shift chains 7/14 and 8/14; every intermediate
  range is stated in `docs/SYNTAX.md` 6.3 and both passes still clamp to int16.
- The DC plane, the planar interpolation grid, the nine directional predictors, the weighting
  matrices, the scans and the LAST/LEVEL contexts all follow the block size by documented rules
  (`docs/SYNTAX.md` 6.5, 6.7, 7.1, 7.2, 7.4, 9.2, 9.3). No entropy context and no symbol is added at
  any size.
- `nxv-enc --xform 8|16|32|auto` and `nxvc_config::xform_size`; `auto` is a per-tile rate-distortion
  choice. Conformance vectors `v57`-`v62` and rejection vectors `r30`-`r32`.
- Measured in `ref/RESULTS-xform-a.md`, including the two variants that were measured and rejected
  (a per-32x32-quadrant size and a per-size probability-table family).

**Encoder rate-distortion (no syntax change, no tool bit)**

- The rate model now charges **bypass bits**: signs, escape suffixes and `LAST` suffixes were dropped
  from the predicted rate, which under-charged every nonzero level by its sign bit and biased the
  tile mode decision towards coding. `nxvc_encode_stats::bits_predicted_q10` reports the model's own
  prediction so it can be checked against the payload it produced.
- An **exact bound on `last`**: above the highest position whose magnitude reaches half a step,
  level 0 beats level 1 in distortion and in rate, so those positions are provably zero and are
  never searched. The rate half of that argument is a property of the transmitted table, so it is
  **checked** (`RateCost::zero_cheapest`) rather than assumed, and the bound is dropped rather than
  trusted if a table ever violates it.
- The **DC plane goes through the trellis**, at its own coarser quantiser's lambda.
- The **double-charged reference-persistence factor is removed** from the per-tile mode decision:
  it was charged once already on the skip candidate's excess. Worth **-3.4 BD-rate points**, the
  largest single item here. The unified lambda is **0.22**.
- Hierarchical motion search with an extended coarse level and SATD in the fine stage; a cheap
  per-tile QP search (`requant_params()` plus bounded descent, candidates spaced `qp_search_step`
  apart, and the intra mode decision made once and reused rather than re-searched per candidate).
- **Effort presets are a library concept**: `nxvc_config::preset` takes an `nxvc_preset` and the
  library resolves it, so an SDK caller and `nxv-enc --preset` cannot disagree about what `slow`
  means. `slow` does **not** set `--wm auto`.
- `--chroma-weight` / `nxvc_config::chroma_weight_q8`, explicit and **defaulting to 1.0**. Weighting
  chroma below 1.0 is fitted to the 6:1:1 reporting convention, lowers absolute chroma PSNR, does
  nothing at 4:2:0 and regresses SSIM there; it ships off, documented, and anything quoted with it
  must be quoted on both metrics.
- **Open item:** one branch measured **-25.4 %** SSIM-Y on the Phase 2 kill test that this encoder
  does not reproduce, and it is not a chroma-metric artifact. The two candidate causes are an intra
  early-out gate and a real-RD quarter-pel refinement layered on the SATD stage. Both are cheap to
  try and both have a number to beat.

**Bitstream syntax v1.6 -- Phase 2 inter efficiency**

- `docs/SYNTAX.md` 13.8, **drift-driven intra refresh**. The encoder holds a bit-exact client shadow,
  so the refresh is scheduled from the drift it measures against the source rather than from a fixed
  1-in-`T` permutation. `intra_period` becomes a hard age cap; a tile whose drift exceeds a multiple
  of the quantiser's noise floor `qstep^2/12` loses `WARP_SKIP` from its candidate set but is not
  forced to `INTRA`. Encoder-side only: **no syntax change**, and `docs/RATECONTROL.md` 8.7 asked for
  exactly that. Annex D **D-23**.
- `docs/SYNTAX.md` 3.3 and 13.9, **near-skip**, tool bit 28, in the **tile-row header**. A warped tile
  that is skipped may carry its entire residual as nine signed bytes -- a per-plane DC and one
  horizontal and one vertical ramp -- named by a second per-row bitmap. It reuses the DC plane's
  quantiser step, interpolation and inter combination unchanged, so it adds no arithmetic to the
  decoder, and because it is a *skipped* tile with a bias rather than a very cheap coded one it can
  appear in a warp-only chain, which by definition has no coded tiles. It costs no tile-header bit,
  which is what left word1 with room for both transform tools. Annex D **D-24**.
- `docs/SYNTAX.md` 13.10, **quadrant vectors**, tool bit 29, tile-header word1 bit 31. Four motion
  vectors per tile, one per 32x32 quadrant, as signed nibble deltas from the tile vector in four
  bytes. The warp corner basis stays the tile's, which is what makes the tool free on a GPU: the
  vector is added per sample after the corner interpolation, so a quadrant changes the vector and
  nothing else. There is **one** predictor loop -- `warp_tile()` delegates to `warp_tile_quad()` --
  so "four equal vectors are exactly no quadrant vectors" is a structural fact rather than two code
  paths agreeing, and `warp.quad` pins both halves over 128 cases x 2 modes x 2 filters x 4 splits.
  Annex D **D-25**.
- Conformance vectors `v74`-`v75` and rejection vectors `r40`-`r43`. Every previously committed digest
  is unchanged, which is the compatibility claim stated as a test.
- **Not merged:** sub-tile intra. It was measured at -0.50 and **+0.59** points, ships off, and would
  have spent word1's last reserved bit on a disabled tool. Kept in Annex D as a recorded negative
  result: this corpus cannot produce the disocclusion the tool exists for.
- `ref/RESULTS-inter-a.md`, the before/after measurement on the Phase 2 kill test.
**Perceptual rate control reaches the encoder (`--rc` OFF by default)**

The spatial ladder's measured result is **negative**: the periphery is over-degraded and every
foveated metric loses. This package therefore merges for its *wiring*, its two additive ABI items
and its two `rc/` fixes -- all of which stand on their own -- and **not** for the ladder. `--rc`
ships available to measure, not enabled, on the same discipline as the tool bits: a package whose
measured result is negative does not become the default path. The open issue is at the end of this
entry.

- `nxrc::EncDriver` (`rc/include/nxrc/encdrive.hpp`, library `nxvc_rcenc`): the wire between the
  rate-control library and the reference encoder. Per frame it runs tile statistics, classification,
  the foveation map, the temporal refresh scheduler and the bit allocator, and hands the encoder the
  per-tile `qp_map`, `res_map`, `wm_map` and `force_warp_skip` map. No bitstream syntax changes: all
  four address fields the v1 stream has carried since v1.2.
- `nxv-enc --rc`, with `--rc-bitrate`, `--rc-fov on|off`, `--rc-temporal on|off`, `--gaze x,y`,
  `--rc-fps`, `--rc-panel`, `--rc-fov-deg` and `--rc-map` (a per-tile decision dump as CSV).
- `nxvc_encoder_set_wm_map()`: per-tile weighting-matrix id, the encoder-side way to drive the
  `wm_id` field tool bit 20 already defines (docs/RATECONTROL.md appendix A.5).
- `nxvc_tile_info::warp_mad_q8`: the mean absolute residual of a tile's WARP_SKIP predictor, measured
  by the mode search that builds it. This is the `complexity` input docs/RATECONTROL.md 4.1 asks the
  rate controller for.
- `tools/quality/percept_run.py`, `percept_report.py` and `percept_map_png.py`: the
  equal-perceived-quality harness, its tables and the per-tile decision picture. Results:
  `ref/RESULTS-percept.md` -- the wire works, and at equal foveated quality it currently costs bits
  rather than saving them, for four named and measured reasons.
- `tools/quality/nxq/fvvdp.py` and `nxq.yuv.yuv_to_rgb`, borrowed verbatim from branch
  `tourney/metric` (commit 25bc7a4) and marked as borrowed, so FovVideoVDP scores come from the same
  code that package will land.

### Changed

- `nxrc::RefreshScheduler`: the foveal floor is now tested before the static-tile shortcut, so a tile
  that was static last frame and starts moving this one cannot have a residual withheld in the fovea
  (docs/RATECONTROL.md 8, `admissible_divisor`).
- `nxrc::RateController::update_model()`: a tile that came back with no bits is now evidence rather
  than a missing measurement. It used to be discarded, which is exactly the set of tiles the
  encoder's mode search codes as `WARP_SKIP`, so their bit model never fell and the controller
  undershot its budget permanently (measured: `actual/predicted` between 0.33 and 0.67 for eleven
  consecutive frames, 4.3 dB below flat QP at equal rate; 1.6 dB after). See
  `ref/RESULTS-percept.md` 7.1.

**OPEN ISSUE: rc spatial ladder calibration -- periphery over-degradation.**
The spatial ladder degrades the periphery further than the acuity model justifies, and every
foveated metric loses as a result. Calibrating it needs two things that do not exist yet:
**2160-px material**, because the current corpus tops out too low for the periphery of the ladder to
be exercised at a realistic eccentricity -- so the measurement is not testing what the ladder is for
-- and **the encoder's skip decision fed to the allocator before allocation**, so the allocator
stops spending bits against tiles the encoder is about to skip. Until both are in place the ladder
cannot be calibrated and `--rc` stays off by default.


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
- Syntax v1.6, the intra detail tools, each behind its own tool bit and measured separately
  (`ref/RESULTS-detail-a.md`):
  - **`XFORM_4X4_SPLIT` (tool bit 19)**: a per-block 4x4 transform split, signalled by tile-header
    bit 28 plus one bypass bit after a nonzero `CBF`. A 4x4 integer DCT scaled to the 8x8's `2^10`
    gain, so it reuses the same dequantiser, shift chain and weighting matrices; the four sub-blocks
    stay in one 64-value coding unit, so `CBF`, `LAST`, the level chain and the lane schedule are
    unchanged. Costs the GPU decoder no extra dependent step and no LDS.
  - **`INTRA_CFL` (tool bit 24)**: a tenth chroma intra mode predicting chroma from the co-located
    reconstructed luma by a per-block linear model fitted to the block's own reconstructed
    neighbours. Integer throughout, with one 256-entry reciprocal table as its only division.
    Tile-independent; costs the GPU decoder one barrier per tile.
  - Both are **on by default**: together -18.8 BD-rate points on 4:4:4 and -16.0 on 4:2:0 at the
    Phase 1 operating point, for 1.7x encode time and no measurable decode cost.
    `nxv-enc --split4x4 off --cfl off` writes a v1.4 stream byte for byte.
  - A named per-band encoder dead zone (`kDeadZoneDc` / `kDeadZoneAc`) replacing the magic `f = 1/3`.
    Encoder-only, no syntax. The decoder-side reconstruction offset was built, measured and
    **rejected**: it is worse in both rate and quality at every operating point.
- 29 further conformance vectors and 15 further rejection vectors; `v01`-`v56` and `r01`-`r29` are
  byte-identical across the v1.6 change, which is what proves both tools additive.
- Syntax v1.6, the entropy and context package (`ref/RESULTS-ctx-a.md`, `ref/RESULTS-ctx-b.md`):
  tool bit 25 `CTX_V3`, a **27-context** model that keeps v2's sixteen rows and adds eleven —
  `CBF` and `LAST` conditioned on the class of the previous coding unit *the same rANS lane*
  finished in the same plane (none / uncoded / coded-sparse / coded-dense), `LEVEL` split at scan
  position `LAST` at two bands, and a row for the DC term of a DC plane. The conditioning is per
  **coding unit**, the 8x8 coefficient group, never per transform block, so the model never reads
  a transform size; it is causal inside the lane, so it costs the GPU decoder no barrier and no
  cross-lane read, only 5.6 KiB more of shared cumulative-frequency table. And tool bit 26
  `TAB_V2`, a per-row "use the built-in default" flag that takes a transmitted table set from the
  largest single overhead in a low-rate frame (14.45 %) to 3.41 % — which is what makes a model
  this wide affordable, and the reason the two had to be measured together. Both ship **off by default**: `vk/decoder/passA` does not
  implement them, and an encoder default the project's own GPU decoder refuses would make "a
  default stream" mean two different things.
- Encoder-side with them, no tool bit: `nxvc_config::table_iters`, which refines the eight
  per-frame probability table sets by Lloyd iteration over the per-tile histograms and scores each
  tile against the tables the stream will carry rather than against the built-in ones. `0` means
  off; leave it unset for the default of 3. Reassignment without retraining was measured at
  **-1.8 %** — worse than doing nothing — so the two always move together.

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
- `hybrid/`: a Python simulator for the base-plus-enhancement layering.
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
