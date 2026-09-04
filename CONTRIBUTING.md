# Contributing to NX Warp

NX Warp is a VR-only video codec (`nxvc`). This file covers the tooling: how to build each
component, what CI runs, and the one rule that outranks everything else.

## The bit-exactness rule

**The CPU reference decoder in `ref/` is the specification.** Not the paper, not
`docs/01-bitstream.md`, not the GLSL. The syntax document is derived from the reference decoder;
when they disagree, the reference decoder is right and the document is a bug.

Everything that follows from that:

- Every other decoder — the Vulkan compute decoder in `vk/`, anything in `warp/`, any future
  hardware path — must produce **bit-identical** output to `ref/` for every conformance stream, on
  every device. Not "visually identical", not "within 1 LSB". Equal.
- The reference decoder is single-threaded C++20, integer only, no SIMD, no dependencies. Do not
  add floating point to it, do not add a dependency to it, do not make it fast. It is the oracle.
- Conformance vectors live under `tests/vectors/`. A change that alters the bitstream in any way
  must regenerate them in the same commit and say so in the message. A change that does *not*
  intend to alter the bitstream must leave every existing vector decoding to the same hashes.
- The GPU-versus-CPU diff harness hashes RGBA output per tile and reports the first mismatching
  tile and pixel. On CI the GPU is lavapipe (subgroup size 8, which is why the cluster size is 8);
  developer machines add RADV and NVIDIA, and the nightly Pico 4 runner adds Adreno. Cross-vendor
  determinism is the definition of done for Phase 1 and Phase 2 (paper 3.9, 3.11).
- If you find a case where GPU and reference disagree, the GPU is wrong until proven otherwise. If
  the reference turns out to be wrong, fixing it is a bitstream change: vectors, syntax document,
  and a note in the paper, together.

## Layout

| Dir | What | Build |
|---|---|---|
| `ref/` | Bit-exact CPU reference encoder/decoder — the spec | CMake, `NXWARP_BUILD_REF` |
| `warp/` | Pose-warp prediction, CPU + GLSL | CMake, `NXWARP_BUILD_WARP` |
| `transport/` | Segmentation, FEC, loss handling | CMake, `NXWARP_BUILD_TRANSPORT` |
| `rc/` | Rate control | CMake, `NXWARP_BUILD_RC` |
| `vk/` | Vulkan compute encoder/decoder, device probe, GLSL kernels | CMake, `NXWARP_BUILD_VK` |
| `bench/` | Phase 0 Android benchmark app (K1–K6) | Gradle + NDK |
| `tools/quality/` | Quality harness, synthetic capture, BD-rate | Python 3 + numpy |
| `tests/` | Unit and conformance tests, `tests/vectors/` | CTest |

The root `CMakeLists.txt` adds each subdirectory only `if(EXISTS .../CMakeLists.txt)`, so a
partial checkout, or a tree where a component has not landed yet, still configures and builds.
Keep it that way — CI depends on it.

## Prerequisites

| Tool | Minimum | Needed by |
|---|---|---|
| CMake | 3.25 | everything C/C++ |
| Ninja | any | recommended generator |
| GCC or Clang | C++20 | `ref/`, `warp/`, `transport/`, `rc/`, `vk/` |
| glslang (`glslangValidator`) | Vulkan 1.3 / SPIR-V 1.4 | `vk/`, `warp/`, `bench/` |
| SPIRV-Tools (`spirv-val`, `spirv-opt`) | any | shader validation |
| Vulkan headers + loader | 1.3 | `vk/` |
| A Vulkan ICD | lavapipe is enough | `vk/` tests |
| Python 3 + numpy | 3.10 | `tools/quality/` |
| pytest | any | `tools/quality/tests` |
| ffmpeg with libx264, libx265, libvmaf | any | quality anchors (optional) |
| Android SDK + NDK, JDK 17 | NDK r26+ | `bench/` |
| clang-format | 16+ | the format check |

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build ccache clang-format \
    glslang-tools spirv-tools libvulkan-dev vulkan-tools mesa-vulkan-drivers \
    python3 python3-numpy python3-pytest ffmpeg
```

On Arch:

```sh
sudo pacman -S base-devel cmake ninja ccache clang glslang spirv-tools \
    vulkan-headers vulkan-icd-loader vulkan-swrast vulkan-tools \
    python python-numpy python-pytest ffmpeg
```

## Building

### Everything that exists in the tree

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DNXWARP_BUILD_REF=ON \
    -DNXWARP_BUILD_WARP=ON \
    -DNXWARP_BUILD_TRANSPORT=ON \
    -DNXWARP_BUILD_RC=ON \
    -DNXWARP_BUILD_VK=ON \
    -DNXWARP_BUILD_TOOLS=ON \
    -DNXWARP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Use `build*/` for build directories — `.gitignore` already covers them. Never build into the
source tree.

### `ref/` alone (no Vulkan needed)

```sh
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF
cmake --build build-ref
ctest --test-dir build-ref --output-on-failure -R '^ref'
```

`ref/` builds the `nxv-enc`, `nxv-dec` and `nxv-info` CLIs when `NXWARP_BUILD_TOOLS=ON`.

### `vk/` and shaders

`vk/` needs the Vulkan headers, the loader, and glslang at build time. To run its tests without a
GPU, point the loader at lavapipe:

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
export VK_DRIVER_FILES=$VK_ICD_FILENAMES   # loader 1.3.x and newer
vulkaninfo --summary                       # should report "llvmpipe"
ctest --test-dir build --output-on-failure -R '^vk'
```

The path differs by distribution (`lvp_icd.x86_64.json`, `lvp_icd.i686.json`); CI finds it with a
`find` over the ICD directories rather than hard-coding it, and so should you.

GLSL is compiled with glslang to SPIR-V 1.4 and embedded as arrays. Only
`GL_EXT_shader_explicit_arithmetic_types_int16` and `GL_KHR_shader_subgroup_ballot` may be used.
Run `spirv-val` on anything new.

### Cross-compiling for Windows

The CPU components cross-compile with mingw-w64; CI does this on every push as a portability
check, with `NXWARP_BUILD_VK=OFF` and no tests.

```sh
cmake -S . -B build-mingw -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/mingw-w64-x86_64.cmake \
    -DNXWARP_BUILD_VK=OFF -DNXWARP_BUILD_TESTS=OFF
cmake --build build-mingw
```

Use the **posix** threading flavour of the mingw toolchain; the win32 flavour has no
`std::thread`.

### `bench/` — the Phase 0 Android app

Needs JDK 17, the Android SDK, and the NDK.

```sh
cd bench
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/*.apk
```

CI builds the debug APK and uploads it as an artifact, but only once `bench/` contains a Gradle
project (`build.gradle[.kts]` or `settings.gradle[.kts]`). Until then the job is skipped.

### `tools/quality/` — the quality harness

Pure Python 3 + numpy. ffmpeg with libx264/libx265/libvmaf enables the anchors and VMAF; without
it those metrics are skipped rather than fatal.

```sh
cd tools/quality
python3 -m pytest -q

# generate a small synthetic VR sequence and run the comparison
export NXQ_SCRATCH=/path/to/scratch          # NOT the repo
python3 capture/gen_synthetic.py --out "$NXQ_SCRATCH/seq" --name vr-mixed-512 \
    --frames 10 --eye-width 512 --eye-height 512 --motion mixed
python3 compare.py --seq "$NXQ_SCRATCH/seq/vr-mixed-512.yuv444p.json" \
    --codec-enc 'python3 dummy_codec.py enc' --codec-dec 'python3 dummy_codec.py dec' \
    --qp 16,22,28,34 --anchor-qp 16,22,28,34 \
    --out "$NXQ_SCRATCH/results/small.json"
python3 report.py --results "$NXQ_SCRATCH/results/small.json"
```

Never write sequences, YUV or reports into the repository; `$NXQ_SCRATCH` exists for that.

## Tests

Tests are CTest-registered from `tests/CMakeLists.txt`, which the root adds when it exists. For CI
to pick a component's tests up, the component must:

1. Register its tests through `add_test()` reachable from `tests/CMakeLists.txt` (either directly
   or via `add_subdirectory(<component>)` from there), so a plain
   `ctest --test-dir build` finds them.
2. Guard its own `add_subdirectory()` on `EXISTS`, the way the root does, so a missing sibling
   component does not break configure.
3. Prefix test names with the component (`ref.`, `warp.`, `transport.`, `rc.`, `vk.`) so
   `ctest -R` can select them and a failure names its owner.
4. Skip, not fail, when an optional dependency is missing — a `vk` test with no ICD should return
   the CTest skip code (`set_tests_properties(... PROPERTIES SKIP_RETURN_CODE 77)`), because CI
   runs on lavapipe and developer machines vary.

Conformance vectors go in `tests/vectors/`. Keep them small and keep them checked in; they are the
contract between `ref/` and every other decoder.

## Formatting

`.clang-format` (LLVM base, 4-space indent, 100 columns) and `.editorconfig` are at the root.

```sh
clang-format -i --style=file $(git ls-files '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.inc')
```

The `Format` workflow runs `clang-format --dry-run --Werror` over `ref/ warp/ transport/ rc/ vk/
bench/` and **reports without failing** for now, so parallel work is not blocked by whitespace. It
becomes a hard gate once every component has had one format pass — do not add drift on the
assumption that it stays advisory.

## CI

| Workflow | Trigger | What |
|---|---|---|
| `ci.yml` | push/PR to `main` | `linux-gcc`, `linux-clang` (full build + ctest on lavapipe), `windows-mingw` (cross-compile check), `android-bench` (APK artifact), `python` (pytest) |
| `format.yml` | push/PR to `main` | advisory clang-format |
| `nightly.yml` | 03:17 UTC daily | 20 minutes of libFuzzer on `ref/`, quality harness in small mode |

Every job is gated on the presence of the directory it needs, so CI stays green while components
land one at a time. `ctest` is skipped (with a notice, not a failure) when no tests are registered
yet.

The fuzz job is picked up automatically once `ref/` contains a source matching `*fuzz*` or a
`NXVC_FUZZ` / `NXWARP_FUZZ` option; it builds with
`-fsanitize=fuzzer-no-link,address,undefined` and seeds the corpus from `tests/vectors/`.

## Commits

- One component per commit where possible; the tree is built by several people at once.
- A bitstream change touches `ref/`, `tests/vectors/`, the syntax document and the paper together.
- Say what you measured, not what you expect.
