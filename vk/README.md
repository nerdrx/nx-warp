# `vk/` — the Vulkan side of NX Warp

```
vk/
  common/    nxvc_vk_common  — the shared runtime.  Everything else links it.
  tools/     nxvc-vkprobe, nxvc-vksubgroup — headless CLIs
  encoder/   E0..E5            (docs/PAPER.md 3.6)
  decoder/   Pass A / B / C    (3.2, 3.5)
```

`vk/common` is a C++20 static library with a C ABI. It contains no codec logic
at all: it is the device, memory, synchronisation and pipeline layer that the
encoder, the decoder and the WiVRn NX client and server share, plus the
capability probe that decides which of them may run.

---

## The capability probe (PAPER.md 3.7)

The probe reads a `VkPhysicalDevice` and returns one of four verdicts.

| Profile | Meaning |
|---|---|
| `full` | Pure-compute decoder and encoder at the full 64×64 tile geometry. |
| `lite` | Pure compute, but the device's LDS or workgroup limits force reduced geometry. |
| `hybrid-only` | No pure-compute path; hardware base layer plus Pass C (3.5) only. |
| `unsupported` | Not even the hybrid path. |

The pure-compute requirements, as a bitmask (`NXVC_VK_CAPS_REQUIRED_PURE`):

- Vulkan 1.1, a compute queue family
- subgroup **basic**, **vote**, **ballot**, **shuffle** and **arithmetic**, all
  available in the `COMPUTE` stage
- subgroups of **at least 8 lanes**, either natively or guaranteed by pinning
  with `VK_EXT_subgroup_size_control`
- `storageBuffer16BitAccess`
- `timelineSemaphore`
- timestamp queries on the chosen compute family

`full` additionally wants 32 KB of shared memory and 256-invocation workgroups.
Below the Lite floor (16 KB, 128 invocations) a device drops to `hybrid-only`.

Two rules come straight from the 3.7 vendor table:

- **Subgroup width < 8 is refused.** Clusters are 8 lanes wide, so a device
  that cannot promise 8 cannot run the normative shaders. Mali Bifrost (4–8
  lanes, partial ballot) and Midgard are additionally blocklisted by name.
- **`subgroupClustered*` is reported but not required.** 3.2.6 forbids it in
  normative shaders because Adreno's proprietary compiler handles it badly;
  clusters are emulated with `subgroupBallot` plus masks from
  `gl_SubgroupInvocationID & ~7`. The probe still reports the bit, because the
  conformance kernel uses the real clustered ops as an oracle.

### Subgroup-size pinning

Where `VK_EXT_subgroup_size_control` exists and the range is not a single
value, pipelines pin **32** and set
`VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT`. 32 is the common
denominator across NVIDIA (fixed 32), Intel ANV ("force 32") and AMD RDNA ("32
or 64, never assume which"). Devices with no control keep what they report —
Adreno 6xx at 64, lavapipe at 8. The size the pipeline will actually run at is
handed to the shader as specialization constant 0, so the SPIR-V never guesses.

### Measured results

| Device | Profile | Subgroup | Notes |
|---|---|---|---|
| llvmpipe (Mesa 25.2.4, LLVM 21) | **full** | 8, fixed | 32 KB LDS, 1024 invocations |
| AMD RX 7900 XTX (RADV, Mesa 26.2.1) | **full** | 32–64, pinned to 32 | dedicated compute queue, host-cached heap, 64 KB LDS |
| SwiftShader (Subzero, Vulkan 1.3.0) | **hybrid-only** | 4, fixed | no 16-bit storage, no clustered ops |

The SwiftShader result is worth flagging against 3.9, which names it alongside
lavapipe as a GPU-less CI target: **the SwiftShader build available here cannot
run the pure-compute path**, because its subgroup size is 4 and it lacks
`storageBuffer16BitAccess`. lavapipe is the only software ICD that can. That is
also a piece of luck: lavapipe's 8-lane subgroups are the narrowest width the
codec claims to support, so CI exercises exactly the boundary case.

---

## Owning a device, or borrowing one

```cpp
// We create everything.
auto ctx = nxvc::vk::Context::create({.prefer_software = true});

// The host already has a device -- WiVRn's server runs on Monado's VkDevice
// (3.6), so there is no external memory anywhere, only barriers.
auto ctx = nxvc::vk::Context::adopt({
    .instance = inst, .physical_device = pd, .device = dev,
    .queue = q, .queue_family = family, .api_version = VK_API_VERSION_1_2,
    .enabled_device_extensions = host_extension_list,
});
```

An adopted `Context` destroys nothing on the way out, and masks its probe down
to the extensions the host actually enabled — so the encoder never calls into
an extension that was merely *available*.

A host that creates its own device asks the library what to enable first:

```c
const char* ext[16];
uint32_t n = nxvc_vk_required_device_extensions(&probe, ext, 16);

nxvc_vk_feature_chain chain;   /* caller-owned, must outlive vkCreateDevice */
void* pnext = NULL;
nxvc_vk_fill_feature_chain(&probe, &chain, &pnext);
device_create_info.pNext = pnext;
```

---

## What else is in here

| Header | Contents |
|---|---|
| `nxvc/vk/nxvc_vk.h` | The C ABI: probe, JSON, context create/adopt, accessors. C99, handle-based, versioned by `NXVC_VK_ABI_VERSION`. |
| `nxvc/vk/context.hpp` | `Context`, `Probe`, `Error`, `NXVC_VK_CHECK`, memory-type selection, resolved device function pointers. |
| `nxvc/vk/resources.hpp` | `Buffer` (device-local, host-cached, upload, readback, BAR), `Image` (storage + sampled, layout tracking), `Sampler`. |
| `nxvc/vk/commands.hpp` | `CommandPool`, `OneShot`, `PersistentCommands`, `TimelineSemaphore`, `BinarySemaphore`, `TimestampPool` + `TimingReport`. |
| `nxvc/vk/pipeline.hpp` | `SpecConstants`, `ShaderModule`, `DescriptorSetLayout`, `DescriptorSet`, `ComputePipeline`, file-backed `PipelineCache`. |
| `nxvc/vk/external.hpp` | dma-buf / opaque-fd / sync-fd, `AHardwareBuffer`, Win32 shared handles. |
| `nxvc/vk/vk_common.hpp` | Umbrella + `buildInfo()`, `divRoundUp`, `computeBarrier`. |

There is no VMA. The codec allocates a handful of large, long-lived resources
per stream and nothing per frame, so a suballocator would be weight in a
library that also has to build with nothing but the NDK.

### The send ring (3.6)

`BufferKind::HostCached` asks for `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED`
so `sendmmsg` reads straight out of the mapping through the data cache. Check
`buffer.cached()` afterwards, and the probe's `host_cached_is_device_local`:
RADV, NVIDIA, ANV and the Windows AMD/NVIDIA drivers have a system-memory heap
with these flags; lavapipe and SwiftShader satisfy them from a type that is
also `DEVICE_LOCAL` (on a software or UMA device everything is). Where no such
type exists at all the caller must stage through a device-local buffer. Writing into the device-local
host-visible BAR heap was rejected in 3.6 — host *reads* of write-combined
memory are slow — which is why `DeviceLocalHostVisible` is a separate kind
meant for small GPU-read-only uniform rings.

### Per-pass timing (3.4)

```cpp
TimestampPool timing(ctx, {"passA", "passB"}, /*slots=*/3);
// in the command buffer:
timing.reset(cmd, slot);
{ TimestampPool::Scope s(timing, cmd, slot, 0); vkCmdDispatch(...); }
// some frames later:
if (auto r = timing.read(slot)) std::puts(r->toString().c_str());
```

`timestampPeriod` is applied and the delta is masked to `timestampValidBits`,
so a wrapping counter still yields the right interval. A device without
timestamp support gives `valid() == false` and every call becomes a no-op
rather than an error — a decoder that cannot measure itself still decodes.

---

## Shaders

`cmake/NxvcEmbedShaders.cmake` compiles `*.comp` to SPIR-V with `glslc` (or
`glslangValidator`) and embeds each blob as an `inline constexpr uint32_t[]`
header. Nothing is loaded at runtime; the Android client ships one `.so`.

```cmake
nxvc_add_shaders(my_target SHADERS shaders/passB.comp)
# -> #include <nxvc/vk/shaders/passB.h>  ->  nxvc::vk::shaders::passB_spv
```

The compiler is found on `PATH`, in `$VULKAN_SDK/bin`, or under the NDK's
`shader-tools/<host>/glslc`. Override with `-DNXVC_GLSLC=`.

The default target environment is **`vulkan1.1` (SPIR-V 1.3)**, not the SPIR-V
1.4 that 3.10 asks for: SPIR-V 1.4 needs Vulkan 1.2 or `VK_KHR_spirv_1_4`, and
the Pico 4's Adreno driver is a plain 1.1 implementation. Raise it with
`-DNXVC_VK_SPIRV_TARGET_ENV=vulkan1.2` where the device set allows.

Reserved specialization constant ids (see `pipeline.hpp`); ids ≥ 16 are free:

| id | meaning |
|---|---|
| 0 | subgroup size the pipeline runs at |
| 1 | tile size (64) |
| 2 | cluster width (8) |
| 3 | 10-bit mode |
| 4 | workgroup size |

---

## Tools

```
nxvc-vkprobe  [--json|--text] [--device N] [--require full|lite|hybrid]
              [--quiet] [--selftest] [--software]
nxvc-vksubgroup [--software] [--device N] [--verbose]
```

`nxvc-vkprobe --selftest` creates a real device and exercises every helper the
encoder and decoder lean on -- buffers of each kind, an image and its layout
transition, one-shot and pre-recorded command buffers, a timeline semaphore
signalled from the host *and* from the queue, and a timestamp pool around a
real dispatch -- reporting one JSON line per step. It is the smoke test that
tells a downstream agent whether a failure is theirs or the runtime's, and it
runs on hybrid-only devices too. It found two bugs in this library on its first
run (an unconditional `storageBuffer16BitAccess` request that broke device
creation on SwiftShader, and a host-cached-heap search that wrongly excluded
device-local types and so missed lavapipe's).

Both are headless — no surface, no window — and both exit **77** when there is
no ICD, which `ctest` reads as *skipped*.

`nxvc-vksubgroup` is the conformance test behind rule 3.2.6. It runs the same
SPIR-V at every pinnable subgroup size and checks that the cluster-of-8
emulation agrees (a) with an independently written CPU reference, (b) with
itself across sizes, and (c) with the real `subgroupClustered*` ops where the
device has them. Current results:

```
lavapipe : size 8            pass
RADV     : size 32, size 64  pass, clustered oracle agrees
```

---

## Building

```sh
cmake -S . -B build-vk -DNXWARP_BUILD_VK=ON
cmake --build build-vk -j4
ctest --test-dir build-vk -R '^vk\.'
```

`NXWARP_VK_SUBDIRS` narrows the set of subdirectories, e.g.
`-DNXWARP_VK_SUBDIRS="common;tools"`.

Vulkan headers come from `find_package(Vulkan)`, or from
`-DNXVC_VK_HEADERS_DIR=/path/to/Vulkan-Headers/include`. With headers but no
loader the library still *compiles*, which is what the cross-compile checks
need; tools and tests are then skipped.

### Cross-compile checks

Android (arm64, NDK 29) — this is a real build of the AHardwareBuffer path,
not a stub:

```sh
cmake -S . -B build-vk-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DNXWARP_BUILD_VK=ON -DNXWARP_VK_SUBDIRS=common
```

Windows (llvm-mingw, compile-only — there is no import library to link
against, and a static library needs none):

```sh
cmake -S . -B build-vk-win -DCMAKE_TOOLCHAIN_FILE=<mingw toolchain>.cmake \
  -DNXWARP_BUILD_VK=ON -DNXWARP_VK_SUBDIRS=common \
  -DNXVC_VK_HEADERS_DIR=... -DNXVC_VK_HEADERS_ONLY=ON
```

Both produce a `libnxvc_vk_common.a` containing the platform interop symbols.

---

## CI notes

The tests find a lavapipe manifest at configure time under
`/usr/share/vulkan/icd.d/lvp_icd.*.json`, the user's `.local` prefix, or the
Android SDK's `emulator/lib64/vulkan/lvp_icd.json`; override with
`-DNXVC_LAVAPIPE_ICD=`. When one is found the suite gains lavapipe-pinned
copies of the probe and semantics tests, plus `vk.probe_lavapipe_is_pure`,
which fails if lavapipe ever stops reaching the Lite profile — because 3.9's
whole CI story depends on it.
