# ADR-0028: The GPU encoder's inter path needs its own integer mode decision, and a reference preset to match it

- **Status**: Proposed
- **Date**: 2026-09-05
- **Source**: paper 2.3, 2.6, 3.6; docs/SYNTAX.md 8 and 13.9-13.11; Annex D
- **Affects**: `vk/encoder/`, `ref/src/codec_impl.inc`, `include/nxvc/nxvc_vk_enc.h`, `tests/vk-encoder/`

## Context

`vk/encoder` is intra-only and byte-identical to `nxv-enc` at a named flag set. The goal is an
inter-capable GPU encoder so a 90 Hz stream fits the Wi-Fi budget, held to the same acceptance test:
byte-identity with the reference at a matching configuration.

Three things were measured or established before any shader was written. All three change the plan
that `vk/encoder/README.md` and `vk/encoder/forward/nxe_enc.h` currently record.

### 1. Inter is worth it, and WARP_SKIP alone is not enough

A 16-frame 1088x1088 4:2:0 synthetic head-turn (`gen_synthetic.py --motion turn`, seed 7, band-limited
v2, peak 123 deg/s, ideal-warp ceiling 24.6 dB full-frame / 32.3 dB centre) at QP 30,
`--no-rdo --intra-dir off --preset fast --me-effort 1 --quad-mv off --near-skip off`, decoded back
through `nxv-dec` and compared against the source. **Measured**:

| configuration | B/frame | Mbit/s per eye at 90 Hz | PSNR-Y | vs intra |
|---|---|---|---|---|
| intra only | 32339 | 23.3 | 38.42 dB | 1.00x |
| WARP_SKIP + INTRA only | 16035 | 11.5 | 36.86 dB | 2.02x |
| full inter (SKIP / STATIC_MV / WARP_MV) | 7926 | 5.7 | 36.62 dB | **4.08x** |

Tile-mode histogram of the full-inter stream over 4624 tiles: WARP_SKIP 81.0 %, INTRA 8.9 %,
STATIC_MV 7.0 %, WARP_MV 3.1 %.

Two readings matter for staging. First, the 3x target is reachable: 4.08x at a cost of 1.8 dB.
Second, and against the obvious plan, **WARP_SKIP alone does not reach it** -- it is 2.02x, and the
last 10.1 % of tiles, the ones that carry a coded vector, are worth as much again as the first
81 %. A first increment of "skip or intra" is a coherent, easily exact subset, but it does not
retire the risk that the budget is met. Note also that **STATIC_MV outweighs WARP_MV more than 2:1**
on this content, so a plan that lands WARP_MV and defers STATIC_MV lands the smaller half.

### 2. The reference's default mode decision cannot be reproduced on a GPU

`decide_tile` chooses between INTRA, the best inter candidate and WARP_SKIP by
`D + lambda*R`, where `R` is a **real** rate: `quantize_tile_ex` quantizes the candidate, drives the
same `LaneMachine` the encoder will drive through `count_units`, and prices the resulting histogram
with `table_set_cost`. That function is

```c
static double row_cost(const u32 *h, const u16 *f) {
    double bits = 0;
    for (int s = 0; s < kNumSym; ++s)
        if (h[s]) bits -= (double)h[s] * std::log2((double)f[s] / kProbTotal);
    return bits;
}
```

`std::log2` is not correctly rounded and is not the same function on the host libm and on a GPU, so
a shader that computes this cost does not get the host's bits. Everything downstream -- `select_set`
over eight table sets, the intra/inter/skip comparison, the `kSkipPersist` excess term -- is a
comparison of doubles derived from it, and a one-ULP difference flips a tile's mode and moves the
whole stream. The distortion side is fine (integer differences summed in `double`, order-fixed) and
`sqrt` is correctly rounded; the entropy term is the blocker.

The transcendental is removable in principle -- `f[s]` is a `u16` out of `kProbTotal`, so
`log2(f[s]/kProbTotal)` is a 4097-entry table the host could compute once and upload, after which
the cost is a fixed-order fp64 dot product and therefore bit-exact. But that only removes the
*arithmetic* obstacle. The decision would still require the GPU to fully quantize, entropy-count and
reconstruct **three candidates per tile** (intra, best inter, skip) plus the near-skip fit, which is
three to four times the E3+E4 work per tile on a pipeline that is already three times over its
budget on a 7900 XTX (`vk/encoder/README.md`, "Measured"). Reproducing the default decision is
expensive rather than impossible, and it is the wrong thing to spend the budget on.

### 3. The inter hook that `nxe_enc.h` documents does not exist

`vk/encoder/forward/nxe_enc.h` says, in the section headed "Room for the merge":

> Inter tools slot in ahead of E3: the per-tile record carries `mode` and the predictor plane
> pointer, and E3 already reads its prediction from a buffer (`pred_src`) rather than deriving it,
> so an inter tile is a different producer for the same buffer.

`pred_src` occurs exactly once in `vk/encoder/`, in that sentence. `forward.comp` has five bindings
-- params, jobs, source, coefficients, modes -- and no predictor buffer; its prediction is
`pred_at()`, recomputed on the fly from the tile's own reconstructed block means, and the file says
so itself: "It is never materialised". `nxe_tile_job::mode` exists but is documented as "INTRA (0)
only in this pipeline".

So the cheap merge the header promises is not available. E3 needs a real predictor binding and a
mode branch. This is the same class of defect as the static-assertion claim `vk/encoder/README.md`
already records against the GLSL mirror: a planning assumption written as an accomplished fact.

## Decision

**The GPU encoder's inter path does not reproduce the reference's default mode decision. It
implements a separate, fully integer decision, and that decision is added to the reference as a
first-class preset so both sides remain byte-comparable.**

The reference gains a configuration -- a `nxvc_config` field, surfaced by `nxv-enc`, resolved in
`resolve_effort` next to the existing presets -- under which `decide_tile`:

1. computes the WARP_SKIP predictor at the stored vector and its SSE, as today;
2. takes the skip when `skip_sse <= skip_gate * npix`, which is already an integer comparison
   against a per-frame constant and is already the reference's own early-out;
3. otherwise searches STATIC_MV and WARP_MV at **integer pel only**, SAD only, over the existing
   seed set and one `+-mv_range` sweep, with no SATD stage, no quarter-pel stage and no `me_effort`
   2/3 hierarchy;
4. chooses between INTRA, the best coded-vector candidate and WARP_SKIP by an integer cost in the
   SAD domain with a fixed integer lambda, never by `D + lambda*R` over a quantized candidate.

Every quantity in that path is an `i64` formed from sample differences and shifts. No `log2`, no
`double`, no table-set feedback, and no trial encode. The GPU computes the identical expression in
the identical order, which is what makes byte-identity a property of the arithmetic rather than of
two floating-point libraries agreeing.

Rolling intra refresh stays exactly as the reference has it (`refresh_stagger` / `refresh_due`,
fixed scheme, `drift_refresh` off), because it is already integer and already shared.

The acceptance test is the existing acid test's shape: one description of one synthetic sequence with
one pose track drives `nxv-enc` at this preset and the GPU encoder, the two streams must be
byte-identical, and `nxv-dec` and the Vulkan decoder must then decode both to identical pixels.

## Consequences

- The GPU encoder's streams are **not** the reference's best streams. The preset is a worse encoder
  than the default on rate-distortion, by an amount that must be measured on the same clip before
  the preset is called done; the 4.08x above is the *default* decision's figure and is an upper
  bound on what the preset achieves, not a prediction of it.
- The reference grows a second decision path in `decide_tile`, which is a maintenance cost and a
  place for the two paths to drift. It is contained by the preset being resolved in one place
  (`resolve_effort`) and by the byte-identity test covering exactly it.
- `include/nxvc/nxvc_vk_enc.h` must stop refusing inter at `create()` and grow: inter on/off, the
  intra period, the pose/view input (`nxvc_vke_view` exists and is currently accepted-and-ignored),
  and the received-tiles feedback (`nxvc_vk_encoder_set_received_tiles`, likewise). The header's own
  rule -- every tool this pipeline does not implement is *named and refused*, not silently coded as
  something else -- applies to STEREO, NEAR_SKIP, QUAD_MV and `drift_refresh`, which stay refused.
- E3 gains a predictor binding and a mode branch; this is new work the current header text says is
  already done, and `nxe_enc.h` must be corrected so the next reader is not planning against it.
- The reference ring and the reconstruction that fills it are **not** re-derived: they are the
  decoder's own Pass W (`vk/decoder/inter/warp_pred.comp`) and Pass B
  (`vk/decoder/passB/reconstruct.comp`), byte-identical SPIR-V, per the E3b rule in
  `vk/encoder/README.md`. That rule is unaffected by this ADR and is the reason the ADR is only
  about the *decision*.
- Staging follows the measurement rather than the intuition: **WARP_SKIP and the coded-vector modes
  (STATIC_MV first, then WARP_MV) belong in the same increment**, because skip alone is 2.02x and
  the budget needs 3x. NEAR_SKIP and QUAD_MV are separate and later; they were measured at 0.0 % of
  tiles on this clip with the tools off and are not on the critical path.

## Alternatives considered

- **Reproduce the default decision exactly on the GPU.** Rejected on cost, not on possibility: the
  `log2` can be tabulated to 4097 fp64 entries and the sums order-fixed, but the decision would then
  need three to four full quantize-and-entropy-count passes per tile per frame, on a pipeline
  already three times over its 7900 XTX budget. It also pins the GPU encoder to every future change
  in the reference's RD tuning, which is the opposite of what a real-time encoder wants.
- **Export the reference's decisions to the GPU as an input**, the way directional intra modes are
  handled today. Rejected because it needs the CPU reference in the loop at frame rate, which is the
  thing `vk/encoder` exists to remove; it is a diagnostic mode, not a product.
- **Drop byte-identity for the inter path and test on PSNR and rate instead.** Rejected: it is the
  project's one acceptance test and the only reason the intra path can be trusted. A tolerance test
  cannot distinguish a shader bug from a tuning difference, which is exactly the confusion the
  `fast`-rdoq bug fixed alongside this ADR lived inside for as long as it did.
- **Land WARP_SKIP only as the first exact increment.** Not rejected as wrong, but demoted: it is
  exact and cheap, and at 2.02x it does not answer the question the project is asking. It is a
  useful first *commit*, not a useful first *milestone*.

## References

- paper 2.3 (motion search), 2.6 (rolling refresh), 3.6 (encoder budget)
- `docs/SYNTAX.md` 8 (inter prediction), 13.9-13.11 (NEAR_SKIP, QUAD_MV, STEREO)
- `spec/annex-d-inter-decisions.md`; `docs/INTEGRATION-DECISIONS.md` (received-tiles contract)
- `vk/encoder/README.md` (E3b, the minor-6 tool table, the measured timings)
- `vk/decoder/README.md` ("Pass W, and why it is a third dispatch", "The reference ring")
- ADR-0023 (bit-exactness stays), ADR-0025 (directional intra is negotiated)
