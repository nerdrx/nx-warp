# ADR-0018: GLSL through glslang, not Slang or HLSL

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 3.10
- **Affects**: `vk/`, `warp/glsl`, `bench/shaders`, build system

## Context

The codec's substance is in about fifteen shaders and a set of integer tables. The shader language
choice therefore buys very little expressiveness and costs a toolchain that every developer, every CI
runner and the Android build must carry.

Constraints: the shaders must compile to SPIR-V 1.4 with Vulkan semantics; they must be one binary for
every vendor with specialisation constants rather than vendor ifdefs (ADR-0010); and they need exactly
two extensions, `GL_EXT_shader_explicit_arithmetic_types_int16` and `GL_KHR_shader_subgroup_ballot`.

## Decision

GLSL 4.60 with Vulkan semantics, compiled to SPIR-V 1.4 by glslang at build time and embedded as byte
arrays. `spirv-val` and `spirv-opt` run in CI. `recon.comp` is included by both the decoder and the
encoder, which is what makes E3 byte-identical to Pass B.

Syntax constants and tables live in a single header shared by C++ and GLSL through a common-subset
macro layer, so the reference codec and the shaders cannot drift apart on a constant.

## Consequences

- The toolchain matches what WiVRn (`reprojection.glsl`), Monado and every driver in the target table
  already use, so there is nothing new to install on any target.
- Contributors need no new language.
- GLSL's weaknesses are accepted: no generics, no module system, and preprocessor-based code sharing.
  With fifteen shaders that is tolerable.
- The shared-header trick between C++ and GLSL is fragile by nature and needs a test that the two
  agree; that belongs to the conformance suite.

## Alternatives considered

- **Slang.** Considered seriously for its generics and module system. Rejected for now because it adds
  a toolchain that neither WiVRn nor the Android build has, for fifteen shaders.
- **HLSL through DXC.** Rejected on integer-semantics history and because there is no D3D target; the
  Windows port imports D3D11 textures into Vulkan rather than running D3D shaders (paper 3.8).
- **Hand-written SPIR-V.** Rejected: unreadable, and the specification is the CPU reference codec
  anyway (ADR-0010).

## References

- Paper 3.10 (project structure and language), 3.2.6 (subgroup portability rules), 3.8 (Windows)
- ADR-0010, ADR-0019
