# Releasing nxvc

How a version number is chosen, what has to be true before a tag exists, and
what ships. `BUILDING.md` covers how to build; this file covers when to stop
building and cut.

---

## 1. Two version numbers

NX Warp carries two independent versions. Conflating them is the mistake this
section exists to prevent.

| | Library version | Bitstream version |
|---|---|---|
| Spelled | `X.Y.Z`, SemVer | a single integer |
| Lives in | git tags `vX.Y.Z`, `NXVC_VERSION_MAJOR/MINOR/PATCH` | `NXWARP_BITSTREAM_VERSION`, the stream header `version` field (paper 1.2) |
| Covers | the C ABI in `include/nxvc/nxvc.h` and the component headers | what goes on the wire |
| Set by | `git describe --tags`, falling back to `project(VERSION)` | `cmake/nxwarp_version.cmake`, matched by `NXVC_VERSION` in `nxvc.h` |
| Moves | every release | almost never |

Both are exposed to code through the generated `<nxvc/version.h>`. That header
contains a static assertion that `NXVC_VERSION` (from `nxvc.h`) and
`NXWARP_BITSTREAM_VERSION` (from the build) agree — if they ever drift, the
build stops.

### What bumps the library version

| Change | Bump |
|---|---|
| Bug fix, no API or bitstream change | **patch** — `1.4.2` → `1.4.3` |
| New API, existing API unchanged | **minor** — `1.4.3` → `1.5.0` |
| A new *optional* tool bit that older decoders may refuse | **minor** |
| Removed or changed function signature, enum value, struct layout | **major** |
| Any change to `NXWARP_BITSTREAM_VERSION` | **major** |
| Faster GPU kernel, identical output | **patch** |
| New component library (`nxvc::stereo`, say) | **minor** |
| Build system, CI, docs, tests only | **patch**, or fold into the next release |

Before `1.0.0` this is advisory: `0.y.z` promises nothing, and the codec is
pre-Phase-1. It becomes binding at `1.0.0`, which is the Phase 1 exit
(CONTRIBUTING.md, paper 3.11).

### What bumps the bitstream version

Almost nothing should. The forward-compatibility scheme in paper 1.2 is a tool
bitmask, not version arithmetic: a new coding tool takes a new bit in `tools`,
old decoders refuse a stream whose mandatory bits they do not know, and the
version field never moves. That is the whole point of the design.

`NXWARP_BITSTREAM_VERSION` moves only when the **structure** changes in a way
the tool mask cannot describe — a different stream header layout, a different
tile record shape, a different entropy coder framing. When it does move:

1. `ref/` changes, because the reference decoder *is* the specification.
2. Every vector under `tests/vectors/` is regenerated **in the same commit**.
3. `docs/SYNTAX.md` and the paper are updated **in the same commit**.
4. `NXVC_VERSION` in `include/nxvc/nxvc.h` and `NXWARP_BITSTREAM_VERSION` in
   `cmake/nxwarp_version.cmake` move together, or the build refuses to compile.
5. The library version takes a **major** bump.

A bitstream change that does not do all five is not a release, it is a bug.

---

## 2. Release checklist

Work down it. `scripts/release.sh X.Y.Z` (dry run) performs the mechanical
items and refuses to continue on the ones it can check; the rest are yours.

### Before

- [ ] `main` is green: `ci.yml` passed on the commit you intend to tag.
- [ ] Working tree is clean, and you are on `main`.
- [ ] `scripts/bootstrap.sh --strict` passes on the release machine.
- [ ] **Bit-exactness.** The GPU/CPU diff harness is green on lavapipe. For a
      Phase 2 or later release, also on RADV and on the Pico 4 nightly runner.
      Cross-vendor determinism is the definition of done, not a nice-to-have.
- [ ] Every conformance vector in `tests/vectors/` decodes to its recorded
      hash. If any vector changed, the bitstream changed — go back to §1.
- [ ] `ctest --preset asan-ubsan` is clean. UBSan does not recover here, so
      this is a real gate, not a log to skim.
- [ ] `ctest --preset tsan` is clean, if `transport/` or the encoder changed.
- [ ] A fuzz run of at least an hour on the reference decoder found nothing
      (`cmake --preset fuzz`; the nightly does 20 minutes, a release does more).
- [ ] `cmake --preset mingw-w64` configures and builds.
- [ ] `cmake --preset android-ndk` configures and builds, if the release
      claims a headset target.
- [ ] The quality harness numbers for this release are recorded somewhere
      quotable — *what you measured*, not what you expect.
- [ ] Version decided per §1, and it is not a number already tagged.

### Cutting

- [ ] `scripts/release.sh X.Y.Z` — dry run. Read every line of its output and
      read the generated release notes.
- [ ] Edit the notes if the generated ones bury the lede. A release whose
      headline is "bump CI ubuntu version" when it also changed the rate
      control is a badly written release.
- [ ] `scripts/release.sh X.Y.Z --publish` — tags, pushes the tag, and creates
      a **draft** GitHub release with the artifacts attached.
- [ ] Open the draft on GitHub, read it once more, publish it there.

### After

- [ ] The tag is reachable from `main`; nothing was force-pushed over it.
      Releases are immutable. A mistake becomes `X.Y.Z+1`, never a moved tag.
- [ ] WiVRn NX's pin is updated if this release is meant for it.
- [ ] If the bitstream version moved, say so at the top of the release notes,
      in those words, before anything else.

---

## 3. Artifacts

`scripts/release.sh` builds the `release` preset and runs CPack. What comes out:

| Artifact | Generator | Contents |
|---|---|---|
| `nxvc-<version>-Linux-x86_64.tar.gz` | TGZ | libraries, headers, CLIs, CMake package, `nxvc.pc` |
| `nxvc-<version>-Linux-x86_64.zip` | ZIP | the same, for people who prefer zip |
| `nxvc_<version>_amd64.deb` | DEB | the same, where the build machine has `dpkg` |
| `nxvc-<version>-src.tar.gz` | source TGZ | the tree without build output, `.git` or captured media |
| `SHA256SUMS` | — | checksums for all of the above |

The version in a filename is the **full** `git describe` output, not just
`X.Y.Z`, so an artifact built off a branch can never be mistaken for the tagged
release.

Inside a binary package:

```
bin/            nxv-enc, nxv-dec, nxv-info, and the component simulators
include/nxvc/   the C ABI, the generated version.h, component headers
include/nxrc/   rate control
include/nxfov/  foveation
lib/            libnxvc_ref.a and the other component archives
lib/cmake/nxvc/ nxvcConfig.cmake, nxvcConfigVersion.cmake, nxvcTargets.cmake
lib/pkgconfig/  nxvc.pc
share/doc/nxvc/ LICENSE, README.md
```

Not shipped: the Android APK (built by CI from `bench/`, uploaded as a workflow
artifact, not a release asset), captured VR sequences, and quality harness
output. Those live in `$NXQ_SCRATCH` and stay there.

### Consuming a release

```cmake
find_package(nxvc CONFIG REQUIRED COMPONENTS ref transport)
target_link_libraries(app PRIVATE nxvc::ref nxvc::transport)
```

`find_package` succeeds with whatever components that build produced, so ask
for the ones you need by name and get a clear error instead of a link failure.

---

## 4. What a component `CMakeLists.txt` should provide

Install and packaging are wired at the root, and `cmake/nxwarp_install.cmake`
discovers component targets rather than being told about them — so a component
that does nothing still ends up in the package. These four lines make that
discovery unnecessary and the result correct, and every component should
adopt them:

```cmake
# 1. An alias, so a typo is a configure error rather than a link error, and
#    so the target means the same thing in-tree as it does after install.
add_library(nxvc::mycomp ALIAS nxvc_mycomp)

# 2. Include directories that survive installation.
target_include_directories(nxvc_mycomp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# 3. The shared warning set, instead of a local -Wall -Wextra.
target_link_libraries(nxvc_mycomp PRIVATE nxwarp::warnings)

# 4. Join the export set.
list(APPEND NXWARP_EXPORT_TARGETS nxvc_mycomp)
set(NXWARP_EXPORT_TARGETS "${NXWARP_EXPORT_TARGETS}" PARENT_SCOPE)
```

Until a component does (2), the root rewrites its
`INTERFACE_INCLUDE_DIRECTORIES` into the `BUILD_INTERFACE`/`INSTALL_INTERFACE`
form so `install(EXPORT)` accepts it. That shim
(`nxwarp_make_exportable` in `cmake/nxwarp_install.cmake`) exists to keep the
tree releasable while components land in parallel, and should be deleted once
they have all adopted the convention.

Public headers go under `<component>/include/<prefix>/`, where the prefix names
the library (`nxvc/`, `nxrc/`, `nxfov/`). The install rules copy those trees
verbatim, so `#include <nxvc/transport/wire.h>` means the same thing against an
installed package as against the source tree.
