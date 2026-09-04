# Building NX Warp

`CONTRIBUTING.md` is the short version and the rules. This is the reference:
every preset, every option, and what to do when one of them fails.

The codec is C++20 and CMake 3.25+. Every build goes through a preset; there
is no supported "just run cmake with the right flags" path, because the flags
that matter (sanitizers, determinism, the cross toolchains) are exactly the
ones people get wrong from memory.

---

## Quick start

```sh
scripts/bootstrap.sh          # what do I need, and what do I have?
scripts/build-all.sh dev      # configure, build, ctest
```

Or by hand:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Build directories are `build-<preset>/` at the repository root. `.gitignore`
already covers `build*/`. Never build into the source tree.

---

## Presets

`cmake --list-presets` is authoritative; this table says *why* each exists.

| Preset | Build type | For |
|---|---|---|
| `dev` | Debug | The everyday build. Fast to rebuild, pleasant to step through. CPU components only. |
| `dev-vk` | Debug | `dev` plus the Vulkan encoder/decoder. Needs the Vulkan headers, the loader and glslang. |
| `release` | RelWithDebInfo | What ships. `cpack` runs out of this directory. |
| `release-lto` | Release + IPO | The only configuration where inlining across component boundaries is representative. Slow to link. |
| `asan-ubsan` | RelWithDebInfo | AddressSanitizer + UBSan, non-recovering. Run the conformance suite here before touching `ref/`. |
| `tsan` | RelWithDebInfo | ThreadSanitizer, for `transport/` and the encoder pipelining. Cannot be combined with ASan. |
| `gcc` | RelWithDebInfo | Mirrors the `linux-gcc` CI job. Vulkan on. |
| `clang` | RelWithDebInfo | Mirrors the `linux-clang` CI job. Vulkan on. |
| `coverage` | Debug | gcov or llvm-cov instrumentation. |
| `fuzz` | RelWithDebInfo | libFuzzer entry points, `NXVC_FUZZ=ON`, clang only. |
| `mingw-w64` | Release | Cross-compile the CPU components for Windows x86_64. Build only. |
| `android-ndk` | Release | arm64-v8a, API 29, Vulkan on. The headset target. |

Build presets share these names. Test presets exist for everything that can run
tests, all with `--output-on-failure`, plus two filtered ones:

```sh
ctest --preset ref-only    # just the reference codec's tests
ctest --preset vk-only     # just the Vulkan tests (needs an ICD)
```

Workflow presets run configure + build + test in one command:

```sh
cmake --workflow --preset ci          # gcc
cmake --workflow --preset ci-clang
cmake --workflow --preset ci-asan
cmake --workflow --preset ci-mingw    # build only
cmake --workflow --preset release     # + package
```

### Overriding a preset

Do not edit `CMakePresets.json` for local preferences. Write
`CMakeUserPresets.json` next to it — git ignores it — and inherit:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "my-dev",
      "inherits": "dev",
      "cacheVariables": {
        "NXWARP_QUALITY_PYTHON": "/home/me/venvs/nxq/bin/python",
        "NXWARP_WERROR": "ON"
      }
    }
  ]
}
```

---

## Options

| Option | Default | Effect |
|---|---|---|
| `NXWARP_BUILD_<COMPONENT>` | `ON` (`VK`: `OFF`) | Build `ref`, `warp`, `transport`, `rc`, `fov`, `stereo`, `hybrid`, `vk`, `tools`, `platform` |
| `NXWARP_BUILD_TESTS` | `ON` | Register the CTest suites |
| `NXWARP_BUILD_EXAMPLES` | `ON` | Build `examples/` |
| `NXWARP_WERROR` | `OFF` | Warnings become errors |
| `NXWARP_WARN_CONVERSION` | `ON` | `-Wconversion -Wsign-conversion` in the shared warning set |
| `NXWARP_IPO` | `OFF` | Link-time optimisation, Release configurations only |
| `NXWARP_CCACHE` | `ON` | Use `ccache`/`sccache` as the compiler launcher when found |
| `NXWARP_SANITIZER` | `off` | `address`, `undefined`, `address+undefined`, `thread`, `memory` |
| `NXWARP_COVERAGE` | `OFF` | gcov / llvm-cov instrumentation |
| `NXWARP_FUZZ` | `OFF` | `-fsanitize=fuzzer-no-link`, defines `NXVC_FUZZ=1` |
| `NXWARP_INSTALL` | `ON` | Generate install rules, the CMake package and CPack config |
| `NXWARP_BITSTREAM_VERSION` | `1` | Paper 1.2 stream header `version`. Do not change casually — see `RELEASE.md` §1 |

Component options are only consulted when the directory exists. The root
`CMakeLists.txt` adds each component `if(EXISTS .../CMakeLists.txt)` and globs
`tests/*` the same way, so a partial checkout, or a tree where a component has
not landed, still configures. CI depends on that; keep it.

---

## What the build gives your code

Three INTERFACE targets, all opt-in:

| Target | Does |
|---|---|
| `nxwarp::warnings` | The shared `-Wall -Wextra -Wpedantic -Wconversion ...` set, plus `-Werror` when `NXWARP_WERROR=ON`. Link it `PRIVATE` instead of writing a local `target_compile_options`. |
| `nxwarp::deterministic` | `-ffp-contract=off` (`/fp:strict` on MSVC). For any component that touches floats and has to produce the same answer twice. |
| `nxwarp::version` | The generated `<nxvc/version.h>` include path. Also on the include path for every subdirectory automatically, so linking it is a convenience. |

`<nxvc/version.h>` is generated into `build-<preset>/generated/include/nxvc/`
and carries `NXVC_VERSION_MAJOR/MINOR/PATCH`, `NXVC_VERSION_STRING`,
`NXVC_VERSION_FULL_STRING` (the raw `git describe`), `NXVC_VERSION_DIRTY` and
`NXVC_BITSTREAM_VERSION`. It static-asserts that the bitstream version and
`NXVC_VERSION` in `include/nxvc/nxvc.h` agree.

The version comes from `git describe --tags --dirty`. With no tags — which is
the state of the repository right now — it falls back to the `project(VERSION)`
number, and says so at configure time. That fallback is also what a source
tarball and a shallow CI clone get.

---

## Vulkan

`vk/` needs the Vulkan headers, the loader and glslang at build time, so it is
**off by default**: a preset that fails to configure on a machine without them
is a preset nobody uses. Turn it on with `dev-vk`, `gcc`, `clang`, or
`-DNXWARP_BUILD_VK=ON`.

To run the Vulkan tests without a GPU, point the loader at lavapipe. The path
differs by distribution, so find it rather than hard-coding it — which is what
`scripts/build-all.sh` does for you:

```sh
icd=$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' | head -n1)
export VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd"
vulkaninfo --summary        # should say llvmpipe
ctest --preset vk-only
```

lavapipe is not a fallback here. Its subgroup size of 8 is why the cluster size
is 8, and it is the device CI proves determinism against.

---

## Cross-compiling for Windows

```sh
cmake --preset mingw-w64
cmake --build --preset mingw-w64
```

The toolchain file, `cmake/toolchains/mingw-w64.cmake`, looks for llvm-mingw in
this order: `-DLLVM_MINGW_ROOT=`, `$LLVM_MINGW_ROOT`, an
`x86_64-w64-mingw32-clang` already on `PATH`, and finally
`/run/media/nerdrx/Lex/claude/tools/llvm-mingw` — the dev box default, the same
tree the WiVRn NX Windows port uses. It prepends the toolchain's `bin/` to
`PATH` itself, so no shell setup is needed first.

GCC-flavour mingw works too — it is what CI installs:

```sh
cmake --preset mingw-w64 \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix
```

Use the **posix** threading flavour. The win32 flavour has no `std::thread`,
and the failure it produces names a missing header rather than the real cause.

Vulkan and tests are off for this preset: it is a portability check, and there
is no Windows machine to run ctest on from here.

---

## Cross-compiling for Android

```sh
cmake --preset android-ndk
cmake --build --preset android-ndk
```

`cmake/toolchains/android.cmake` wraps the NDK's own toolchain file and finds
the NDK from `-DANDROID_NDK=`, `$ANDROID_NDK_ROOT`/`$ANDROID_NDK_HOME`, the
newest `ndk/<version>` under `$ANDROID_SDK_ROOT`, and finally the dev box SDK
at `/run/media/nerdrx/Lex/claude/tools/android-sdk`.

Target: arm64-v8a, `android-29`, `c++_static`. API 29 is the Pico 4 / Quest
floor and the level at which AHardwareBuffer's Vulkan external-memory import is
reliable. `c++_static` means there is no `libc++_shared.so` to ship next to the
`.so`.

Vulkan is on: the compute decoder is the reason an Android build exists. Tests
are off, because ctest cannot run an arm64 binary on the host — for on-device
numbers, use `bench/` (see `CONTRIBUTING.md`).

---

## Sanitizers

```sh
scripts/build-all.sh asan-ubsan
scripts/build-all.sh tsan
```

Sanitizer flags are applied globally rather than through an INTERFACE target,
on purpose: an ASan build in which only some objects are instrumented reports
false positives, and a false positive in a conformance run costs more than the
build directory it would have saved. That is why each sanitizer gets its own
build directory.

UBSan is configured **not to recover** (`-fno-sanitize-recover=all`). A report
that does not abort is a report nobody reads, and the reference decoder's
integer overflow checks are precisely the ones worth stopping for.

ThreadSanitizer cannot be combined with AddressSanitizer; asking for both is a
configure error rather than a link failure.

The test presets set the right `ASAN_OPTIONS` / `UBSAN_OPTIONS` /
`TSAN_OPTIONS` for you.

---

## Coverage

```sh
scripts/build-all.sh coverage
```

GCC produces gcov data and the script renders it with `gcovr` to
`build-coverage/coverage.html`. Clang produces `.profraw`, which the script
merges with `llvm-profdata` and summarises with `llvm-cov report`. Debug, not
Release: an optimised coverage map lies about which branch ran.

---

## Fuzzing

```sh
cmake --preset fuzz
cmake --build --preset fuzz
build-fuzz/bin/<target> tests/vectors -max_total_time=3600
```

Clang only. `NXWARP_FUZZ=ON` adds `-fsanitize=fuzzer-no-link` everywhere and
defines `NXVC_FUZZ=1`, which is the spelling `nightly.yml` greps for. Seed the
corpus from `tests/vectors/` — those are real streams, and a fuzzer that starts
from `/dev/urandom` spends its first hour rediscovering the magic number.

---

## Formatting and lint

```sh
scripts/format.sh            # format in place
scripts/format.sh --check    # what CI does
scripts/format.sh --tidy     # + clang-tidy over compile_commands.json
```

`.clang-format` is the only style authority. `.clang-tidy` is advisory and will
stay advisory: its naming rules were read off the existing `ref/` and
`transport/` sources rather than imposed, and the checks that would push the
reference decoder toward spans and algorithms are off, because a diff to `ref/`
is a diff to the bitstream specification.

Optional hooks:

```sh
pip install --user pre-commit
pre-commit install
pre-commit run --all-files
```

---

## Container

The CI image is Ubuntu 24.04 with the full toolchain and lavapipe:

```sh
docker build -f docker/Dockerfile.ci -t nxwarp-ci .
docker run --rm -it -v "$PWD:/src" -w /src nxwarp-ci scripts/build-all.sh dev
docker run --rm nxwarp-ci nxwarp-selftest      # report the toolchain
```

`.devcontainer/devcontainer.json` uses the same image, with clangd pointed at
`build-dev/compile_commands.json` so the editor and the terminal read the same
compile database and the same `.clang-tidy`.

---

## Installing, and consuming from another project

```sh
cmake --preset release
cmake --build --preset release
cmake --install build-release --prefix /usr/local
```

Then, from a consumer:

```cmake
find_package(nxvc CONFIG REQUIRED COMPONENTS ref transport)
target_link_libraries(app PRIVATE nxvc::ref nxvc::transport)
```

or without CMake:

```sh
pkg-config --cflags --libs nxvc
```

No install step is needed to consume the build tree — useful for a sibling
WiVRn NX checkout:

```sh
cmake -S wivrn -B build -Dnxvc_DIR=/path/to/nx-warp/build-release
```

Packaging is `cmake --build build-release --target package`, or
`scripts/release.sh`. See `RELEASE.md`.

---

## When it fails

| Symptom | Cause |
|---|---|
| `Vulkan headers not found` | `vk/` is on without the headers. Install them, or drop to `dev`. |
| `no such configure preset` | Typo, or you are not at the repository root. |
| mingw build fails on `<thread>` | win32 threading flavour. Use the posix one. |
| `llvm-mingw not found under ...` | Pass `-DLLVM_MINGW_ROOT=/path`. |
| `Android NDK not found` | Pass `-DANDROID_NDK=/path/to/ndk/<version>` or export `ANDROID_SDK_ROOT`. |
| ASan reports something absurd in libc | Something is compiled without instrumentation. Do not mix build directories; reconfigure clean. |
| `ThreadSanitizer and AddressSanitizer cannot be combined` | Working as intended. Pick one. |
| `tools/quality: ... lacks pytest or numpy` | Advisory. Point `-DNXWARP_QUALITY_PYTHON=` at a venv that has them. |
| Version reads `0.0.1` and mentions a fallback | No git tags exist yet. Expected until the first release. |
| Configure says a manually-specified variable was unused | A preset set an option a component has not defined yet. Harmless while components land. |

If a component's own `CMakeLists.txt` is the thing that broke, that component's
author owns it — the root only adds subdirectories that exist and never reaches
inside one.
