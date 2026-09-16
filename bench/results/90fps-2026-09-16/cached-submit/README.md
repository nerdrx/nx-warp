# Cached presentation without another GPU submission

Status: Android candidate built offline; **not installed or measured on the Pico**. This follows the [headroom profile](../../../../docs/PICO_HEADROOM.md).

## Removed work

The existing unchanged-image cache already skipped the full defoveation draw. It still submitted a Vulkan command buffer with GPU timestamps and decoder semaphore waits on every cached refresh. The new path skips that submission when the cached projection image is valid and no decoder image transition was recorded. It keeps submitting the OpenXR projection layer so the runtime can perform its normal head-pose reprojection.

This does not lower image resolution, shrink the sharp centre, change the source frame rate or synthesize object motion. It removes queue work only on eligible repeated images. A stream with a new image on every refresh gains little or nothing from this change.

## Synchronization conditions

- End recorded command buffers even when they will not be submitted. They can be reset at the next refresh.
- Leave the already-signaled render fence alone when skipping; reset it only immediately before a real submission.
- Still submit if decoder image transitions were recorded, because software image-layout state has already advanced.
- Read timestamps once per actual submission. Do not reread old results on skipped refreshes or wait for unsubmitted queries.
- Promoted quad layers continue through the existing render path; they are not eligible for this cache shortcut.

The render log now distinguishes `GPU submissions` from `cached refreshes without GPU submission`. These counts must be measured on a live stream; they are not a replacement for fresh-frame rate, panel rate, or latency.

## Verification limits

The Android release build passes. A separate headless Vulkan synchronization smoke exercises the fence/query/command-buffer lifecycle on the host. That exercise is not the production OpenXR render loop and does not measure Pico GPU cost, Android decoder interaction, power or visual stability. Live verification must include fresh frames, repeated frames, overlays, alpha streams, reconnect and head movement.

There is no FPS or latency improvement claim until the device run. No device was contacted or installed during this work.

## Reproduction and observed host result

Source: [WiVRn 4edb00e2a161dc8a3f0f15abf4707cbf761a15db](https://github.com/nerdrx/wivrn-nx/commit/4edb00e2a161dc8a3f0f15abf4707cbf761a15db). [Runnable smoke](https://github.com/nerdrx/wivrn-nx/blob/4edb00e2a161dc8a3f0f15abf4707cbf761a15db/tests/cached_submission_smoke.cpp) uses Vulkan headers and `-lvulkan`; compile without `NDEBUG`. The source file includes the build command. This machine used the Vulkan-Headers 1.3.268.0 include directory already fetched by the Android build.

On AMD Radeon RX 7900 XTX / RADV NAVI31, with the validation layer enabled: **48 iterations, 24 submissions, 24 skips, 24 query reads, zero validation errors, sentinel intact**. The constructed schedule includes 12 fresh writes, 12 cached iterations with a required image transition, and 24 cached iterations without one. These counts follow the test schedule; they do not represent a measured live cache-hit ratio or a 50% performance improvement. [Raw output](host-smoke.log).

[Android build log](android-build.log) · [APK identity and uninstalled status](status.json). The prepared local APK is `nx-scratch/motion-regions/headroom-submit/client.apk`; the motion-off property helper from the headroom profile remains a separate step.
