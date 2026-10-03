# Integrated branch gate

[WiVRn implementation `f45767d3`](https://github.com/nerdrx/wivrn-nx/commit/f45767d3) uses `PYROWAVE_HAAR_FORMAT=ON` on both ends. It is an incompatible optional format, default off. The matching inverse is selected even if a caller requests the CDF fragment path. Public class layout is identical in both builds. The client now uses the actual decoder output stage for image barriers and brackets the complete decode with timestamps.

Root-built native Android `wivrn` shared modules linked with the option both on and off. No APK was installed, and the streamer was not started.

The root-built Haar archive was linked into a standalone Pico helper using the production header. At 4352×2176 4:2:0, 12 warmups +30 measured decodes gave **12.4662/12.951 ms GPU p50/p95**, and **15.3647/17.5196 ms synchronous call**. Actual Y/Cb/Cr readback was byte-identical to the prior fused desktop planes. These figures corroborate the separate matched comparison; neither run proves live VR throughput.

The inverse crops stores to the real image size for dimensions not divisible by 32. A constant selected when creating the pipeline removes these checks from aligned frames. Unconditional image bounds checks first increased GPU p50 to 14.0655 ms, versus an adjacent 12.4802 ms control; that version was replaced.

Passing a CDF packet with the standalone container magic changed to Haar still failed inside the codec with `Unrecognized sequence header mode 0.` Thus the code gate is checked independently of the container magic.

Device serial PA8150MGGB110166G, Adreno 650, Vulkan 1.1. Static isolated decode; no Wi-Fi, encode, compositor, or photon measurement. Build script and raw samples are retained here. The binaries, original photo, packet, and full raw planes remain private.

Additional shape gate: 1920×1080 4:2:0 passed host Vulkan validation and actual Pico Y/Cb/Cr output matched host byte-for-byte. A fresh manually linked sequential host comparator disagreed, so it is not used as conformance evidence. The old near-matching comparator has validation warnings and is not promoted.
