# ADR-0019: C++20 with a small C ABI, not Rust

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 3.10
- **Affects**: `include/nxvc/nxvc.h`, `ref/`, `vk/`, `android/`, build system

## Context

The codec has to link into the WiVRn NX server (C++/CMake on Linux and Windows) and into an NDK C++
Android client, and its substance is in shaders and integer tables rather than in host code. The host
code is buffer management, descriptor setup, packet assembly and a reference decoder.

## Decision

C++20, CMake 3.25 or newer, with presets for Linux, Windows and Android. The codec ships as a static
library with a small C ABI (`include/nxvc/nxvc.h`) plus C++ convenience headers. Library and codec
identifier: `nxvc`.

The reference decoder is single-threaded, integer-only, SIMD-free C++20 with no dependencies, about
3000 lines, so that it is readable as a specification (ADR-0010).

WiVRn NX gets a `video_encoder` implementation that owns the encoder passes and exposes segments to
the existing pacer and FEC; the client gets a `decoder` implementation next to the MediaCodec one,
selected by a new value in the protocol's codec enum.

## Consequences

- No FFI seam and no cargo-ndk in the build; the Android build is the NDK toolchain it already was.
- The C ABI keeps the integration surface small and stable, and makes a future binding from another
  language possible without changing the core.
- Rust's memory-safety benefit is given up on a codebase that parses untrusted network input. That is
  a real cost and it is paid with tooling instead: libFuzzer with a structure-aware mutator on the
  reference decoder, GPU-assisted validation layers on the shader path, and the rule that every buffer
  and image load is bounds-clamped in the shader (paper 3.7, 3.9).
- Sanitiser and fuzzing coverage of `ref/` therefore becomes a release gate rather than a nicety.

## Alternatives considered

- **Rust.** Rejected: it adds cargo-ndk and an FFI seam for no gain in the hot path, which is shaders.
  The safety argument is real and is answered by fuzzing plus bounds-clamping rather than dismissed.
- **C99.** Rejected: the host-side buffer and pipeline management is where C++20's type safety and
  RAII actually pay.
- **C++ ABI as the public interface.** Rejected: a C ABI is what survives compiler and standard
  library differences across three platforms.

## References

- Paper 3.9 (testing and fuzzing), 3.10 (project structure and language)
- ADR-0010 (reference decoder as specification), ADR-0018 (GLSL)
