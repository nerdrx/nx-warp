# ADR-0010: Integer-only normative path; the CPU reference decoder is the specification

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 2.2, 3.7, 3.9
- **Affects**: `ref/`, `warp/`, `vk/`, `tests/`

## Context

The encoder runs the decoder to build its references (E3 is the decoder's Pass B), so both sides must
compute identical predictions, and they run on different vendors: an AMD or NVIDIA PC encodes, a
Qualcomm or ARM headset decodes.

Floating point in shaders is not portable. Vulkan permits 2.5 ULP for fp32 division, FMA contraction
differs between compilers, Adreno honours `RelaxedPrecision` aggressively, and AMD, NVIDIA and
Qualcomm round differently. A single ULP difference in a sampling coordinate flips a rounding decision
and the mismatch then propagates through every subsequent frame, because the reference is the previous
output. There is no IDR to wash it out (ADR-0006).

## Decision

The normative path is integer only, and the CPU reference decoder in `ref/` is the specification.
SPIR-V is validated against the reference, never the other way round.

Rules for every normative shader (paper 3.7):

- int32 arithmetic only, with int16 storage. No float, no fp16, no int64, no integer division or
  modulo.
- Rounding shifts are written as `(x + (1 << (s - 1))) >> s` with an arithmetic shift, which SPIR-V
  defines exactly.
- Shift amounts are compile-time constants or masked to the operand width, because SPIR-V leaves
  out-of-range shifts undefined.
- No `OpSDiv`, `OpSRem` or `OpSMod` anywhere in normative shaders.
- Every buffer and image load is bounds-clamped in the shader. `robustBufferAccess` behaviour differs
  across vendors (zero versus garbage) and the codec cannot depend on it.
- Coefficient clamping ranges are normative, so overflow cannot differ by vendor.
- **The hardware sampler is not used for the normative predictor.** Sampler weight precision is
  vendor-specific (8-bit fractions on AMD and NVIDIA, undocumented on Adreno), so a sampler-based
  predictor would drift by plus or minus 1 LSB per frame. Four explicit loads with integer weights
  are used instead.

The warp specifically (paper 2.2): the server computes H per eye in double precision, quantises it to
nine int32 in Q8.24 with `h22 = 2^24`, and sends those 36 bytes per eye. The decoder computes source
coordinates for the four tile corners only, with 64-bit products through `OpUMulExtended` /
`OpSMulExtended` (core SPIR-V, no `shaderInt64` required) and a fixed 32-iteration restoring division,
giving Q.6 (1/64 pel). Inside the tile the coordinate is bilinearly interpolated from the corners with
integer adds; the quarter-pel MV is added, rounded to 1/16 pel, and sampled with an integer filter,
bilinear (Lite) or 4-tap Catmull-Rom over 64 from a 16-entry table (Full).

## Consequences

- Bit-exactness across vendors is true by construction, not by testing. Testing then proves it:
  cross-vendor determinism (encode on AMD, decode on NVIDIA, lavapipe and Adreno, all hashes equal to
  the reference) is the definition of done for Phase 1 and Phase 2.
- The quantisation error in H lands in the residual, which is the correct place for it.
- Four divisions per tile, not per pixel. The 32-iteration restoring division is affordable at that
  rate.
- Interior interpolation error is bounded: under 1/32 pel at 32 px tiles, under 1/16 pel at 64 px
  below 250 deg/s head rotation (paper 2.2, estimate). That is inside the sampling grid.
- The CPU reference is about 3000 lines of single-threaded, dependency-free, SIMD-free C++20, and it
  is what the fuzzer runs against (paper 3.9).
- Cost: the bit-exact gather-4 predictor is more expensive than one sampler tap. Phase 0 kernel K2b
  exists specifically to quantify that cost.
- Every future tool inherits this rule. Learned tools are out of loop (a post-filter the reference
  never sees) unless both sides can run integer-exact inference and exchange a weights hash at connect
  (paper 5.4).

## Alternatives considered

- **Float shaders with a tolerance.** Rejected: there is no IDR, so any tolerance accumulates without
  bound.
- **Float with a periodic forced refresh to bound drift.** Rejected: it reintroduces exactly the
  refresh spikes the architecture removes, and it makes the encoder's shadow model approximate.
- **Hardware sampler for prediction.** Rejected on undocumented weight precision. Kept as legal for
  the hybrid base layer only, where the base is not in the normative bit-exact path (paper 3.5).
- **SPIR-V as the specification.** Rejected: a specification must be readable and independently
  implementable, and a shader binary is neither.

## References

- Paper 2.2 (determinism, integer warp), 3.7 (vendor differences and bit-exactness rules),
  3.9 (testing), 3.10 (project structure), 5.4 (future tools)
- Prior art for corner-then-interpolate warping: MPEG-4 Part 2 global motion compensation (1999,
  expired) and AV1 global motion (royalty free)
- ADR-0006 (no IDR), ADR-0018 (GLSL), ADR-0011 (compute)
