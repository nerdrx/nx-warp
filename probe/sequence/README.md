# Sequential decoder and native-render probe

`sequence.cpp` decodes a real `.nxv` stream and submits each returned atlas
frame to the native atlas renderer, waiting for the GPU completion fence before
advancing. The reported `completed_pairs` and wall timings therefore describe
completed decoder-plus-renderer pairs in this offscreen probe. They exclude
encoding, transport, XR scheduling and headset presentation; they do not prove
240 Hz delivery or display FPS.

Build both host and Android ARM64 artifacts with:

```sh
./build.sh
```

Use `./build.sh --host-only` when the Android NDK or decoder library is not
available.

The script compiles the copied native vertex and fragment shader source with
`glslc -fshader-stage=vert -DVERT_SHADER=1` and
`glslc -fshader-stage=frag -DFRAG_SHADER=1`, then links each executable against
the corresponding prebuilt `nxvc_vk_decoder` library and Vulkan loader. Override cached locations with
`NX_SEQUENCE_HOST_DECODER_BUILD`; Android builds require
`NX_SEQUENCE_ANDROID_DECODER_BUILD` and `ANDROID_NDK` (or `ANDROID_NDK_HOME`).
Override source names with `NX_SEQUENCE_RENDERER`,
`NX_SEQUENCE_VERT`, and `NX_SEQUENCE_FRAG`.

Run the host probe as `build/host/nx-sequence-bench stream.nxv 1200 120 shader-dir` (stream, frame count, warmup count, shader directory; the fourth argument is optional).
The Android binary is an ARM64 executable and is intended for a controlled
offscreen/device run; the build script does not install or launch it.

See the retained [Pico sequence-throughput evidence](../../bench/results/240fps-2026-09-08/sequence-throughput/README.md).

`make-fixture.py` reproduces the deterministic 4352x2176 sparse-motion input;
it requires `--encoder` and cleans up its FIFO and child process on failure.
The [Khronos fragment density map sample](https://docs.vulkan.org/samples/latest/samples/extensions/fragment_density_map/README.html) describes the optional hardware path. Our short FDM runs were inconclusive; it remains disabled by default.

Optional environment switches (all disabled by default):

| Switch | Effect |
|---|---|
| `NX_SEQUENCE_ASYNC=1` | Submit decode without an intermediate CPU wait; same-queue barriers and the final render fence still enforce completion. |
| `NX_SEQUENCE_UNORM=1` | Use UNORM output and omit the shader's inverse gamma conversion together. This requires separate compositor validation before live use. |
| `NX_SEQUENCE_FAST_SRGB=1` | Approximate inverse gamma with a cubic; up to about 5/255 encoded grayscale error. Ignored when UNORM output omits inverse gamma entirely. |
| `NX_SEQUENCE_FDM=1` | Request full center density and peripheral density 127/255. Fail closed if required device features or allocation are unavailable. |
| `NX_SEQUENCE_RENDER_REPEATS=4` | Render each decoded source four times. This tests static-pose capacity, not fresh-frame throughput or pose-aware presentation. Values 1–8 accepted. |
| `NX_SEQUENCE_SPIN_US=4000` | Poll completion for at most this many microseconds, then block normally. Accepts 0–10000; zero retains ordinary blocking. CPU-intensive diagnostic, not a recommended Pico setting. |
| `NX_SEQUENCE_GPU_TIMESTAMPS=1` | Collect optional renderer GPU timestamp intervals in CSV `gpu_ms`; blank means disabled or unavailable. Instrumented runs must be compared with a query-free control. |

The decoder's `NXVC_VKD_ATLAS_VIEW_DIRTY=1` separately enables dirty atlas-view
updates; it was enabled in every combined Pico run. GPU timestamp suppression
is a separate diagnostic, `NXVC_VKD_NO_TIMESTAMPS=1`; wall-clock completion
measurements remain active.

In repeat mode, `completed_pairs` and `steady_fps` count decoded source cycles,
each including all its renders. `completed_renders` and `render_steady_fps`
count actual fence-completed renders. Render latency includes decode cost on
the first render of each cycle. Warmup counts source cycles, not individual
renders. CSV `frame` identifies the source frame, `render_index` its repeated
render, and `total_ms` that render's wall time; asynchronous `decode_ms` is CPU
submission time, not isolated decoder GPU time. No output readback, file write,
or cleanup is included in capture timing.

`process_cpu_s` measures process CPU time across the capture, including warmup;
it excludes final image readback and cleanup. It is not GPU time or a power
measurement. GPU timestamp intervals cover the renderer command interval and
may include dependency stalls; they do not measure photon latency.

When GPU queries are enabled, query retrieval occurs after the per-render wall
timer; its CPU overhead remains included in source-cycle time and aggregate
throughput. Compare instrumented runs with timestamp-disabled controls.
