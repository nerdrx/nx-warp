# Native PLANAR direct-render probe

This is a standalone measurement and validation probe for the PLANAR-coded
YUV path. It parses frames through the decoder's private parser linkage,
uploads the validated tile records and PLANAR bodies, and renders a fresh
RGBA image with Vulkan. It deliberately accepts only complete all-PLANAR
frames: 8-bit 4:2:0, CT_NONE, no alpha, and 64-pixel-aligned dimensions.
Mixed PLANAR/non-PLANAR frames are rejected, so this is not a drop-in WiVRn
decoder or transport integration.

`planar.frag` evaluates the coded Y/Cb/Cr PLANAR body for CT_NONE and is the
exact coded-YUV experiment; `validate.py` compares its output against the reference
decoder. Fixture manifests from `make-fixture.py` carry a SHA-256 for every
source frame and the encoded stream. `flat.frag`, `compact.frag`
and the vertex-fed shaders are approximations for renderer experiments:
the flat arm ignores slopes, while compact is limited to its supported coarse
two-region form. They must not be described as exact PLANAR reconstruction.
`fit.comp` is a cheap GPU-fit proof of concept and is likewise approximate.

The probe bypasses the normal decoder's rANS, inverse transforms, reference
ring, and image-store path, but still performs full-resolution RGBA rendering.
Its timing CSV `interval_ms` includes inter-frame CPU cleanup and has buffered
stdout after timing; it excludes cold startup and device/pipeline creation.
Run an explicit warmup before collecting measurements. Existing captures show
the GPU renderer interval and CPU tail are separate quantities; no new timing
result is published here.

Build host artifacts from the workspace with:

```sh
cd /run/media/nerdrx/Lex/claude/nx-warp
HOST_VULKAN_INCLUDE=$PWD/../tools/Vulkan-Headers-1.4.309/include \
PLANAR_HOST_LIB=$PWD/build-vkdec/vk/decoder/libnxvc_vk_decoder.a \
PLANAR_BUILD=$PWD/probe/planar-direct/build \
  ./probe/planar-direct/build.sh --host-only
```

For Android, set `ANDROID_NDK` and `PLANAR_ANDROID_LIB` to the NDK and
ARM64 decoder-library paths in the same command, then omit `--host-only`.
The Android binary
is intended for the controlled fresh-frame changing-pixel pan fixture
(approximately 157 Mb/s); its manifest records a SHA-256 per source frame. No
PC encoder,
network, XR compositor, head-motion pose warp, or 240-FPS live session is
included.

The experimental `NX_PLANAR_RGB565=1` output arm was rejected after paired
device runs showed no consistent GPU or latency benefit; the probe remains
RGBA8-only.

Usage is `nx-planar-direct STREAM SHADER_DIR [MAX_FRAMES] [READBACK_PREFIX]`.
The supported environment switches are `NX_PLANAR_FLAT`,
`NX_PLANAR_COMPACT`, `NX_PLANAR_TILE`, and `NX_PLANAR_SPIN` (approximation and
renderer arms), `NX_PLANAR_REUSE_COMMANDS` (command reuse),
`NX_PLANAR_PACE_FPS` (scheduled admission), `NX_PLANAR_PACE_SPIN` (busy-wait admission; increases CPU use), `NX_PLANAR_QUEUE_PRIORITY=high|realtime` (explicit Vulkan queue priority; fails if denied), and `NX_PLANAR_ASYNC` (a wired
two-slot asynchronous submission scope). These switches do not change the
all-PLANAR input restriction; `validate.py` clears them for exact validation.

Run validation with:

```sh
python3 probe/planar-direct/validate.py --probe <build>/host/nx-planar-direct \
  --shaders <build>/host --ref-decoder <path>/nxv-dec \
  --fixtures-dir <fixtures> --out <validation-output>
```

This exercises exact, flat, compact, and rejection cases. Future integration must
define mixed-frame handling and preserve the decoder's existing parser and
reference-state semantics before this probe can become a decoder path.
