# nxvc-d3dinterop: the NX Warp Windows interop probe

`nxvc-d3dinterop.exe` answers, on real hardware, the two questions that stand
between the NX Warp encoder and the WiVRn NX Windows helper:

1. **Does the D3D11 to Vulkan zero-copy path work here, and what does the
   per-frame handoff cost?** The helper receives SteamVR frames as
   `ID3D11Texture2D` and stages them through a shared, keyed-mutex ring. The
   encoder wants those pixels as a `VkImage` on a shared timeline, with no
   copy through system memory. Paper 3.8 specifies the path; this measures it.
2. **Which encoder profile does this adapter get?** Paper 3.7 has a vendor
   table (subgroup width, ballot, int16 storage, what to force and what to
   avoid). The probe reads the actual capabilities and prints the decision the
   encoder would make, including any blockers.

It is a single self-contained console exe. No installer, no DLLs to ship, no
window, no GUI: it is meant to be scp'd to a box, run over SSH, and read back
as one JSON object on stdout.

## What it actually does

```
D3D11.4 device (matched to a DXGI hardware adapter)
  -> shared NT-handle RGBA8 texture, 2048x2048 and then 2160x2160
  -> shared D3D11 fence (ID3D11Device5::CreateFence, D3D11_FENCE_FLAG_SHARED)
     imported into Vulkan as a timeline semaphore via
     VK_KHR_external_semaphore_win32 (D3D12_FENCE handle type)
  -> texture imported as a VkImage via VK_KHR_external_memory_win32
     (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, dedicated allocation)
  -> trivial compute dispatch writes a 64x64 checkerboard into the image
  -> D3D11 waits on the fence, copies to a staging texture, maps it, and
     verifies every byte
  -> 600 timed iterations of the signal/wait handoff, p50 and p99
```

The checkerboard uses only byte values that survive the UNORM round trip
exactly, so the verification is byte-for-byte rather than approximate. Its cell
size is 64x64, the codec tile size of paper 6.2, which makes a partially
corrupted readback readable at a glance.

The two texture sizes are deliberate: 2048x2048 is the per-eye working size of
paper 3.1, and 2160x2160 is the Pico 4 native eye buffer and *not* a power of
two, which is where tiling and pitch assumptions in a driver's shared-resource
path tend to break.

### Handoff latency: what the number means

Each timed iteration is a full round trip:

```
D3D11 signals fence value N  ->  Vulkan waits N, dispatches, signals N+1
                             ->  CPU wakes on the fence reaching N+1
```

so the sample includes the D3D11 signal propagation, the Vulkan queue wake, the
(trivial) dispatch, the signal, and the CPU wake. It is not a pure semaphore
latency, and it is not meant to be: it is the cost the encoder pays per frame
per eye to take ownership of a SteamVR texture, which is the number the frame
budget in paper 3.1 needs.

### Keyed mutex fallback

Paper 3.8 keeps `VK_KHR_win32_keyed_mutex` as the fallback where a driver
cannot import a `D3D12_FENCE` handle. That path is a **build-time**
alternative, because the two need different `D3D11_RESOURCE_MISC_*` flags on
the texture and a different submit structure:

```
platform/win/build.sh           # shared fence (default)
platform/win/build.sh --keyed   # VK_KHR_win32_keyed_mutex
```

In the keyed-mutex build the timed round trip is
`ReleaseSync(1) -> vkQueueSubmit with VkWin32KeyedMutexAcquireReleaseInfoKHR ->
AcquireSync(0)`, using the same key convention as the helper's staging ring
(key 0 = D3D11 owns it, key 1 = Vulkan owns it).

## Building

Cross-compiled on Linux with llvm-mingw, the same toolchain and layout the
WiVRn NX Windows port uses:

```sh
platform/win/build.sh              # -> build-win/win/nxvc-d3dinterop.exe
platform/win/build.sh --keyed      # -> build-win-keyed/win/nxvc-d3dinterop.exe
platform/win/build.sh --clean
```

`build.sh` pins compiles to the idle scheduling class on four cores. The build
dirs live under the repo and are gitignored.

Manual configure, if you want different options:

```sh
cmake -S platform -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=platform/cmake/llvm-mingw-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-win -j4
```

Useful cache variables:

| Variable | Meaning |
|---|---|
| `LLVM_MINGW_ROOT` | llvm-mingw install (default: `tools/llvm-mingw` on the dev box) |
| `NXWARP_VULKAN_INCLUDE_DIR` | directory containing `vulkan/vulkan.h`; auto-detected from `$VULKAN_SDK` and the dev-box checkouts |
| `NXWARP_WIN_KEYED_MUTEX` | `ON` for the keyed-mutex path |
| `NXWARP_GLSLC` / `NXWARP_GLSLANG` | shader compiler used to build `checker.comp` |

Headers only, no import libraries: `vulkan-1.dll` is resolved at runtime with
`LoadLibrary` and every entry point comes from `vkGetInstanceProcAddr` /
`vkGetDeviceProcAddr`. That is what lets the probe start, and print a proper
verdict, on a box with no Vulkan ICD installed at all. `d3d11`, `dxgi`,
`dxguid` and `ole32` are linked normally, and the C++ runtime is static, so the
exe is the only file that has to be copied.

The compute shader is compiled to SPIR-V at build time and embedded as a
`uint32_t` array (`platform/cmake/embed_spv.cmake`), for the same reason.

## Running it on NX-WIN

```sh
platform/win/run-on-nxwin.sh --dry-run     # print every remote command, send nothing
platform/win/run-on-nxwin.sh               # build if needed, deploy, run, fetch the JSON
platform/win/run-on-nxwin.sh --keyed --iterations 2000
```

Defaults: host `xlerm@192.168.1.215`, key `~/.ssh/wivrnnx_windows`, deploy dir
`C:\wivrnnx\nxwarp-probe\`. Override with `--host` / `--key` / `--dir`, or the
`NXWIN_HOST` and `NXWIN_KEY` environment variables. The fetched JSON lands in
`platform/win/results/` (gitignored) with `latest.json` pointing at the newest
run, and a short human summary is printed.

`run-on-nxwin.sh` is the only thing in this directory that opens a network
connection.

Running the exe by hand on the Windows box:

```
C:\wivrnnx\nxwarp-probe\nxvc-d3dinterop.exe --iterations 600 --out C:\wivrnnx\nxwarp-probe\probe.json
```

Options: `--iterations N`, `--adapter N` (hardware DXGI adapter index),
`--out FILE`, `--quiet` (suppress the stderr progress log), `--help`.
JSON goes to stdout, progress to stderr, so `... > probe.json` is always clean.
Exit code is 0 on pass, 1 on fail, 2 on a bad command line.

## Output

One JSON object:

| Key | Meaning |
|---|---|
| `interop_mode` | `shared-fence` or `keyed-mutex` |
| `adapter` | DXGI name, vendor/device id, VRAM, D3D feature level, UMD driver version |
| `vulkan` | instance/device API version, driver name and version, whether the device LUID matched the DXGI adapter, extensions present and missing, `subgroup`, `features` |
| `profile` | the paper 3.7 decision: `id`, `verdict` (`supported` / `hybrid-only` / `unsupported`), `required_subgroup_size`, `cluster_size`, `notes`, `blockers` |
| `sizes[]` | per texture size: byte-exact `verify` result and `handoff_ms` with `p50`, `p99`, `min`, `max`, `mean` |
| `amf` | whether `amfrt64.dll` loads and its version. **Informational only** - NX Warp does not use AMF; it tells us whether the hybrid HEVC base of paper 2.9 / 3.5 is available on this box |
| `pass` | true only if every stage succeeded and every readback verified |
| `error` | `{stage, message}` when it did not, so a failure says *where* it stopped |

The adapter block is emitted even when the probe fails later, so a failed run
still identifies the machine.

## Tests

There is no unit test suite here; the thing under test is a driver, and it is
not present on the build machine. What exists:

- **Wine smoke test** (`platform/win/wine-smoke.sh`). Runs the exe under Wine
  in a fresh prefix under `nx-scratch`, with the crash dialog disabled before
  the first run and `DISPLAY`/`WAYLAND_DISPLAY` cleared so nothing can map a
  window. It asserts only that the binary loads, resolves its imports, and
  emits a well-formed JSON verdict with a named failure stage rather than
  crashing. Wine has no D3D11 to Vulkan external-memory path, so a failure
  there is the expected result; the test is about failing *gracefully*, which
  is the property that matters when the exe lands on an unknown box.

  Exit codes: 0 smoke pass, 1 smoke fail, 2 no binary, 77 Wine not installed.

- **`run-on-nxwin.sh --dry-run`** exercises everything local in the deploy
  script (argument parsing, path translation, quoting for `cmd /c`) and prints
  the exact remote commands without contacting the host.

Real validation only happens on NX-WIN, and its output is the JSON above.

## Layout

```
platform/
  CMakeLists.txt              standalone-buildable; dispatches on target system
  cmake/
    llvm-mingw-toolchain.cmake
    embed_spv.cmake           SPIR-V -> uint32_t array header
  win/
    CMakeLists.txt
    build.sh                  cross-build (both interop modes)
    run-on-nxwin.sh           deploy + run + fetch JSON  [the only networked script]
    wine-smoke.sh             does-it-load-and-fail-gracefully check
    shaders/checker.comp      trivial compute kernel
    src/
      main.cpp                D3D11 + Vulkan interop, timing, JSON
      vk_loader.h             runtime vulkan-1.dll loading
      profile.h/.cpp          the paper 3.7 decision, kept pure
      json.h                  dependency-free JSON writer
```

`platform/` is not built by the repo root: it cross-compiles for a foreign
target and would otherwise drag the host build of the reference codec along.
