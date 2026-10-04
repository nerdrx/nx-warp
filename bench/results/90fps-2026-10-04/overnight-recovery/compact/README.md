# Compact independent ASTC packets — source-only experiment

Default off; no APK installation or live profile change. Native 2176×2176 ASTC 8×8 images keep byte-identical hardware textures. A v4 independent packet removes the fixed 17-bit mode header from each 16-byte block and preserves the 111 variablebits plus one mode selector in 14 bytes, then uses oneZstd 3 pass. The Pico restores blocks backwards in its existing CPU scratch. No reference history, extra GPU pass or downscale is introduced.

![Exact bytes and measured Pico decode cost](compact-results.png)

| Fixture | Ordinary Zstd payload | Compact Zstd payload | Reduction | Pico whole strict decode p50/p95 | Compact strict decode p50/p95 |
|---|---:|---:|---:|---:|---:|
| Dark room |416,379 B|392,362 B|5.77%|1.025 /1.094 ms|1.080 /1.141 ms|
| Forest |258,371 B|245,437 B|5.01%|0.761 /0.825 ms|0.887 /0.927 ms|

The second independently ordered run gives1.019/1.076 versus1.073/1.149 ms (dark), and 0.762/0.805 versus0.888/0.948 ms (forest). Added median CPU cost is about 0.055 and 0.126 ms per eye respectively. Each packet is decoded through the production strict decoder, verified byte-exact after every decode; baseline/candidate order alternates. Device asleep/display OFF, clocks unlocked; cpu0 frequency readings before/after 1,804,800kHz, thermal status 0. These readings do not establish fixed clocks during the samples.

The previous36-frame512² moving camera/object sequence saves4.92% aggregate compressed payload using compactrecords. Native motion/full pipeline benefit remains unverified. Photos remain private; only timings, byte counts and hashes are published.

## PC and wire trade-off

Single-eye host samples include packing plus one selected Zstd 3 pass. Dark p50/p95: ordinary2.036/2.452 ms, compact1.923/2.277 ms. Forest1.309/1.439 versus1.305/1.577 ms. A separate stereo benchmark is slower in compact mode; no guaranteed encoder speedup is claimed.

At an assumed 500Mbit/s, saved payload serialization is 0.384ms dark and 0.207ms forest per eye; at 100Mbit/s it is 1.921ms and1.035ms. These are byte/rate calculations, not measured Wi-Fi, motion-to-photon, or displayed latency savings. Extra decode cost and network queueing must be measured together.

## Integration and reproduction

Source option: `_wivrn_astc_compact=1` per eye encoder. Requires a v4 client; absent keeps the ordinary independent format. Motion packing takes priority. One Zstd attempt, existing LZ4/raw fallback, strict size/content checks. It does not compare both layouts every frame and does not guarantee smallerpackets for all scene/quality combinations.

Production code and validation: [WiVRn NX compact format](https://github.com/nerdrx/wivrn-nx/blob/pyrowave-probe/docs/ASTC_COMPACT_PACKING.md). Host/server and Android builds pass; randomized packing, malformed/trailing Zstd, bounds, legacy v1–v3, ASAN/UBSAN and standalone ARM64 checks pass. Builds prove source compatibility, not usable VR quality or sustained 90 fresh frames/s.

[Production Pico test log and command](production-decode-pico.txt) · [Host encode log](production-encode-pc.txt) · [Test source snapshot](nxastc_compact_test.cpp)

All costs exclude staging copy, Vulkan upload/render, network transport, headset presentation and physical photons. There is no measured HEVC comparison here.
