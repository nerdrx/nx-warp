# Continue NXVC work until 06:00 Berlin

Deadline: **5 October 2026, 04:00 UTC / 06:00 Europe/Berlin**. The user explicitly requested continued work until then. Heartbeat `nx-warp-two-hour-optimization` now runs at :00/:30, including the deadline. Check the time and existing root/Luna agents before a new job. Do not duplicate ongoing work. Keep unchanged checks quiet. At deadline stop owned benchmarks, publish actual results, and pause the heartbeat; preserve user applications and VR sessions.

## Checkout and runtime boundaries

- Source: `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, branch `pyrowave-probe`, pushed **27026bbd**; sender-overlap prototype was rejected/removed. UDP pool and one-shard stack handoff are committed and built. Compact format commit **7e8d74d2**, parallel eyes **4d7a8dd5**.
- Report: `/run/media/nerdrx/Lex/claude/nx-warp`, main **23e5053**, pushed. Unrelated dirty README logo/branding and untracked wordmark/bisect/scratch files remain. Do not broadly stage them. Working README logo is violet64; HEAD is light150. Stage owned prose from HEAD through `hash-object`/`update-index` to preserve branding.
- Actual profile: native **2176×2176 per eye**, ASTC8×8, stream_scale1, no foveation/JPEG/blur/object-motion warp. Installed client remains **c514841f**, independent v1/v2 packets. New experiments have not been installed or enabled live.
- Existing server parent **1721736**, runtime child **1757965**, unchanged through 19:25 Berlin. Child is not a duplicate server. Disk binary was rebuilt, running processes were not restarted. Pico **PA8150MGGB110166G**, A8110, app `org.meumeu.wivrn.nx` PID9926 was asleep/display OFF before and after isolated CPU tests. Recheck state before device work. No UI, property, clock, route or driver changes.
- Keep private photo inputs, RGBA/ASTC/packet payloads and decoded photos out of GitHub. Publish aggregate data, source, and procedural animations only.

## Completed evidence

### Compact independent format — default off

Private option `_wivrn_astc_compact=1` requires a v4 client. Fixed mode bits are removed into14-byte records; the client restores exact16-byte ASTC blocks backwards in existing CPU scratch. One Zstd attempt, LZ4/raw fallback, strict bounds/content checks; motion packing takes priority. Native photos save **5.01–5.77% payload**, adding about **0.055–0.126ms CPU per eye** in the production strict decoder. PC single-eye samples are near parity, but compact stereo packing was slower; no universal encoder speedup. Host/server, Android, compatibility v1–v3, malformed data, randomized roundtrip and sanitizers pass. Source only, no live enablement.

Authoritative Pico log: `nx-scratch/overnight-structural-20261004/production-decode-pico.txt`. Earlier separate inverse timings are stale. Public report: `overnight-recovery/compact`.

### PC parallel eyes — default off

Environment `WIVRN_ASTC_PARALLEL_EYES=1`. Applies only to two independent ASTC encoders with no auxiliary encoder. Shared-pointer snapshot retains both; right `std::async` plus caller left, then future.get before image release/next frame. Creation failure falls back serial; existing per-eye watchdog handling remains. Ownership/sender/VMA/queue review and server build pass.

CPU two-eye batch: **3.516/4.096 →2.130/2.474ms p50/p95**, identical packets. Same-device offscreen Vulkan test: **13.865/15.432 →11.364/13.007ms**, 2.50ms median improvement. GPU shader time unchanged. Both dispatches submit before CPU waits. Upload excluded; no live compositor, Wi-Fi, Pico viewer, fresh-FPS, photon or HEVC claim. Scratch `overnight-stereo-encode-20261004` and `overnight-stereo-gpu-20261004`; public `overnight-recovery/stereo-packing` and `stereo-gpu`.

### Independent regions/bands — not integrated

Native static ASTC snapshots repeated36 sends over300 trials stress packet count, not motion/freshness. Idealized1400-byte records, XOR8+1 and four-way interleave are model assumptions, not exact WiVRn framing. At IID2% loss, 256px squares recover **98.7–99.1% area** versus **61.5–73.9% complete whole-frame sends**, costing **1.6–5.7% modeled wire**.

Original square CPU probe allocated a Zstd context per record and was expensive. Reusing contexts reduces256px squares to about30% median Pico overhead. Full-width256/512/1024px bands need9/5/3 records and decompress directly into contiguous ASTC rows:256px costs about4.6–4.7% extra,512/1024 near whole-frame parity. The band loss model retains **93.1–95.0% area at IID2%** for256px, with about0.1–1.0% modeled wire overhead across band sizes. Partial bottom-strip retention is a CPU cost demonstration, not a proposed XR refresh pattern.

Scratch `overnight-region-cpu-20261004/results-dctx.txt`; current square source hash starts92fb1582. Original one-shot log/source hash retained separately; original source was overwritten by the context revision. Public `overnight-recovery/band-cpu` and `regions` include logs, hashes and figures.

Actual source is WiVRn ASTC, not the original NX Warp NXT transport. A mistaken NXT audit was removed and corrected before publication. WiVRn uses optional view/timing metadata, adaptive XOR group sizes16/8/4, interleave and FEC bitrate share. Its accumulator forwards a contiguous prefix and calls frame_completed only after all shards and final timing arrive. Merely putting bands inside a whole packet does NOT permit partial recovery.

### Safety backup feasibility — not integrated

192²/eye raw ASTC8 bound is **13.27Mbps at90Hz**. Sampled-image GPU thumbnail probe is0.26–0.29ms, but synchronous host fence waits dominate around9ms. No intermediate resize image/pass. Coherent two-refresh-period selection policy tested standalone. `view_info.display_time` is a predicted display target, not source capture time.

Current four slots are eyes0/1, alpha2, quad3; native profile disables alpha/quad. A negotiated safety role/geometry, independent readiness, coherent stereo selection and reserved scheduler opportunity are necessary. Current shared FIFO can leave backup behind the primary. No live20Mbps takeover or photon proof.

### Rejected work

sendmmsg loopback ~2.3% excludes real sender cost; byte residuals lose10–19%; separated ASTC fields lose16%; no integration. Older v3 motion packing saves23.49% on one native photo-derived fixture but only1.55% on36-frame512² camera/object motion at gap1, and none with older references. Keep it off. Do not repeat weak bitstream microprobes.

## In progress / next bounded queue (18:42 UTC)

1. **UDP reuse retained:** source27026bbd pushed.32-slot pool preserves externally held bytes;1.25MiB active/cache batch bound. One-shard stack span consumes synchronously. Actual UDP ordering/move/lifetime/oversize/tiny-encrypted tests pass normally and ASan/UBSan. Android native-client and server/OpenXR builds pass. No APK install/restart/profile change. Matched -O2 ABBA40runs: allocations376→16 (95.7%less); median batchp50 1.965→1.940µs, p95both2.560µs. Initial Debug/O2 mismatch discarded. Root reviewed CSV, hashes and graph.
2. **Luna structural_budget:** implementing one bounded spare ASTC packet vector to remove repeated assembler growth allocations. Existing mutex ownership; recycle at worker exits and pending eviction, preserve parse/decode/staging/upload policy. Must account actual vector capacity, not just size. No production lifecycle test exists yet; no unmeasured FPS claim. Do not duplicate this work.
3. **Luna safety_policy:** read-only audit of other receive/shard copy, serialization and late-discard costs. No GPU/Pico/UI action. Need meaningful next decision, avoid tiny repeated entropy probes.
4. **Luna coarse_encode_probe:** finished/released. Faithful sender model submits both eyes before backend waits.250Mbps median unchanged;500Mbps median-0.653ms butp95+0.203ms. Production prototype removed; invalid pre-submit and later load-changing runs excluded. Publicsender-overlap source/CSV/graph ready; no further reruns.
5. Tiny empty/compute timestamp diagnostic:20warmups/condition,50samples/condition100total. Calibrated GPU completion violates host fence ordering by~1.45ms; derived queue margins discarded, no driver cause. PublicCMakebuild passes. Unknown highGPUload; ownedGPU jobs serialized.
6. Strict reusedDCtx33–36µs/eye rejected as too small; report23e5053, no production change. Root is publishing UDP/overlap/diagnostic reports while preserving unrelated branding and private photos.
7. Partial bands remain unintegrated: solve transport completeness, geometry/length/capability, coherent stereo revision barrier and pose-safe presentation first. Never patch a renderer-held image.
8. At deadline clean only exact owned idle benchmark paths, preserve appdata/certificate/user processes; pause heartbeat and summarize actual wins/limits. Live server stays unchanged unless user requests otherwise.

## Efficiency

Use bounded Luna jobs, RTK and native helpers. Next measurement should change a decision. Keep replies concise/cute. Verify figures and push only owned paths. Headset render loops are not fresh source FPS; offscreen/CPU tests are not sustained motion, HEVC parity or physical photon latency.
