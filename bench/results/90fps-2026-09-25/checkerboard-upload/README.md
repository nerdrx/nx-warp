# Checkerboard upload benchmark — 2026-09-25

## Result

In the primary J/K/L comparison, CPU-interleaved selection (K) reduced mean Pico GPU render time from 5.391 ms (J) and 5.400 ms (L) to 3.300 ms. Mean decode time rose from 0.500 ms to 1.073 ms. Mean fresh selected-image rate was 89.691, 89.755, and 89.791 fps for J, K, and L respectively. Each run recorded zero network holes. Payload remained 53.287 Mbps. This is a short-run client result, not evidence of sustained 90 Hz full-pixel refresh.

The verified render-debug run O measured 3.355 ms mean GPU time; phase-moved control N remained at 5.400 ms. Full-rate run W measured 3.218 ms GPU time, 0.600 ms decode time, 89.791 fresh fps, and 74.577 Mbps payload. W uses a different upload rate and is not directly paired with J/K/L.

## Method

Single-photo scene rendered at 2160 × 2160 pixels per eye, padded to 2176 × 2176 for encoding; 500 Mbit/s slider and 433.604 Mbit/s direct-byte budget. The harness flag `CHECKER_STATIC` disables the photo sequence, but the Vulkan graphics plugin still applies its stereo-frame translation (`graphicsplugin_vulkan.cpp:961`). The J/K/L runs used 35 s with 10 s client warmup, adaptation off, compression and predictor cache on, and safety enabled. Clock speeds were unlocked and placement stationary. The plugin alternates a −8 px and +8 px offset every four stereo frames, a 16 px displacement. The two eyes duplicate each position. Across 24 consecutive captured NXDU uploads, the sequence repeated every eight frames: three stable frames at one position, one transition, three stable frames at the other position, then one transition. The checker artifact at each boundary is expected time-mixing from this stepping, not an unresolved compositor or packing defect. This trades a 45 Hz spatial sample cadence for 90 image frames. The benchmark is short and does not establish long-duration thermals, power, or physical motion-to-photon latency.

J and K use the same configured workload; matching frame-byte captures came from later runs M/P. J/K/L geometry matches except for legacy `glow.z` metadata; the old shader also retains a possible vertex-scaling difference. N moved phase while retaining old shader, with phase code in `deband.z` and `glow.z` set to zero; it remained at 5.400 ms GPU time. O independently verified the CPU-merged render at 3.355 ms.

Exact parity covered 9,469,952 pixels on host and Pico against the old shader, including sRGB, using identical paired source bytes. Color precision was preserved. The paired pixel comparison applies to stable intervals; transition frames intentionally mix adjacent translated samples. Standalone Pico CPU timing used 20 warmup operations per phase and 180 measured operations over two actual frames: initial CPU merge was 1.233750 ms p50 / 1.269114 ms p95; optimized merge was 0.413334 ms p50 / 0.480990 ms p95. The measured checksum was `56a9b86f8e2e1417`.

## Interpretation and limits

NXDU CPU-interleaves packed selector data and native grid once, adding two endpoint pairs and four selector words. It does not expand to full RGBA or change the network format. GPU consumes one prepared sample. The decode-time increase in K is visible in the client summary and should be considered with the GPU reduction.

Candidate and old-checker control both showed checker texture at translation boundaries. Precomposite capture looked clean, but changes timing. This is the expected temporal blend between translated positions. The option remains off by default. The final normal APK run Y measured 3.300 ms GPU, 1.091 ms decode, 89.800 selected images/s, 53.287 Mbit/s payload and zero incomplete units. Application timestamps and newly selected image IDs do not prove 90 full-pixel refresh; the image stream can advance at 90 frames/s while spatial samples alternate at 45 Hz. No physical photon measurement was made.

The captured transition upload 903 reproduces the same checker texture when rendered separately. This identifies temporal sample reuse as the cause in this workload. Comparing that moving transition against a stable-frame reference naturally differs; it is not a packing-parity test failure. [Capture sequence hashes](capture-sha256.csv) retain the repeating stable/transition sequence without publishing the images.

Final APK SHA-256: `d82e5852be96d80f781920569a63246a2137ffe9e9ab43dd3509c624466ba1d1`.
Unchanged host server SHA-256: `26da43b3197114f821d67b12f4a2126c01cb9169dde16a796d5acc40a94f556d`.
Integration commit: [`11eff678`](https://github.com/nerdrx/wivrn-nx/commit/11eff6786ca6ac8061e2ca6d09d7c0a0d271cbaf). This report does not include raw logs, photos, or configs.

## Artifacts

- [Plot script](plot_summary.py)
- [Summary graph](checkerboard-upload-summary.png)
- [Sanitized scalar metrics](summary-metrics.csv)
