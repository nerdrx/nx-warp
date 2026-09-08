# Direct PLANAR integration

This experimental path connects the GPU flat-region encoder to a graphics renderer in custom WiVRn NX. The earlier 90 Hz results measured an offscreen Pico executable; they did not exercise this integration or establish live streaming performance.

## Contract

- Encoder: `NXVC_VKE_PLANAR_GPU_FLAT` in `nxvc_vke_create_info.planar`; WiVRn option `planar-gpu-flat=true`, Vulkan backend, INTER enabled, atlas disabled.
- Stream: 8-bit 4:2:0, no color transform or alpha, one or two eyes, each eye's width and height divisible by 64.
- Every tile carries a complete R2/coarse PLANAR body with zero slopes. Region masks preserve edges; each region has a constant color. This deliberately sacrifices texture detail.
- Client opt-in: `adb shell setprop debug.wivrn.nx.planar_direct 1`. Reconnect using a matching custom client and server build.
- Renderer: `nxvc::PlanarDirect` borrows the application's graphics queue and writes a free RGBA8 pool image. It validates the complete frame, renders all tiles, waits for completion, then publishes the image for compositor sampling.
- Mixed, malformed or unsupported frames are rejected. The renderer does not update a generic decoder reference ring. Completed independent frames can be acknowledged without those references.

The initial integration refreshes the entire image. The offscreen centre-first scheduler is not enabled here: retaining peripheral pixels across rotating pool images needs an explicit freshness policy. The pressure experiments already show that centre-only admission can starve the periphery.

## Validation boundary

Passing a codec build, rendering a fixture on the Pico, connecting the headset, and presenting fresh application frames are separate checks. A live result requires the installed client to log `direct PLANAR RGBA8 graphics output enabled`, publish decoded frames, and report positive stream-layer submissions and new-source updates. Panel refresh or repeated presentation alone is not source FPS.

Report encoder time, receiver decode time, fresh-source cadence, compositor cost and dropped updates together. These measurements still do not establish motion-to-photon latency without a physical timing method.
