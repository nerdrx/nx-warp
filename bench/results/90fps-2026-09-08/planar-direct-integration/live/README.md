# Live integration smoke test

The installed custom WiVRn NX client enabled the direct PLANAR RGBA renderer
and published frames from a PC OpenXR `hello_xr` application running inside
headless gamescope. Source resolution: 4352 × 2176 stereo; compositor output:
2160 × 2160 per eye; panel target: 90 Hz.

Across the 11 complete reporting windows archived in `live-current.log`
(22 seconds), the client reported **89.73 layer submissions/s** and
**87.0 fresh-source updates/s**. Individual fresh-source windows ranged
from **80.5 to 90.0/s**. The server reported approximately 2–3 ms encoding;
the client compositor's own GPU pass was approximately 6–7 ms. Packet holes
and missed updates remain visible in the logs. This is not a sustained 90 FPS pass.

**Visual and moving-head validation remain outstanding.** The headset was
unworn. ADB screenshots were entirely black, and `sys.pxr.screenstatus` was 0
despite Android reporting Awake/display ON. Layer submission counters alone
cannot prove useful visible pixels; the blank capture is retained as
`screen-blank.png`. There was no physical head movement during this smoke test.

The earlier offscreen moving-camera test checks the reusable renderer's output
and timing independently. It must not be substituted for the missing live
visual/motion check.

Installed APK SHA-256:
`089ad798bae69ff0dceb64af8996af608ccf60b87623c4c9616ce0d49d8a7889`,
package `org.meumeu.wivrn.nx.warp`, version `1.0-nx-planar`.
Client property `debug.wivrn.nx.planar_direct=1`; server options are Vulkan,
INTER, GPU-flat PLANAR, atlas off, automatic rate control, QP 22–40.

Bring-up uncovered a stale installed NX header and a stale WiVRn object file.
Refreshing the codec installation and rebuilding that object was necessary;
successful compilation alone had not enabled the new API mode. The server log
also retains the initial missing-runtime-manifest failure before the OpenXR
runtime target was rebuilt.

Archived logs retain WiVRn messages only and trim trailing whitespace; unrelated Android service messages are excluded.
