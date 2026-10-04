# Continue NXVC work until 06:00 Berlin

Deadline: **5 October 2026, 04:00 UTC / 06:00 Europe/Berlin**. The user explicitly requested continued work until then. Heartbeat `nx-warp-two-hour-optimization` now runs at :00/:30, including the deadline. Check the time and existing root/Luna agents before a new job. Do not duplicate ongoing work. Keep unchanged checks quiet. At deadline stop owned benchmarks, publish actual results, and pause the heartbeat; preserve user applications and VR sessions.

## Checkout and runtime boundaries

- Source: `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, branch `pyrowave-probe`, pushed **4d7a8dd5** with an uncommitted, default-off ASTC sender-overlap prototype. Compact format commit **7e8d74d2**, parallel eyes **4d7a8dd5**.
- Report: `/run/media/nerdrx/Lex/claude/nx-warp`, main **3e4c7ef**, pushed. Unrelated dirty README logo/branding and untracked wordmark/bisect/scratch files remain. Do not broadly stage them. Working README logo is violet64; HEAD is light150. Stage owned prose from HEAD through `hash-object`/`update-index` to preserve branding.
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

## In progress / next bounded queue (17:58 UTC)

1. **Luna coarse_encode_probe:** same-device paced FIFO sender harness, comparing per-eye wait before backend against wait after backend at250/500Mbps/unpaced. Both GPU dispatches must be submitted before either wait, as production does. Previous packet/metadata remain immutable. Source `overnight-stereo-gpu-20261004/src/sender_wait.cpp`; generated CSV is in progress, not final evidence. No production edits.
2. **Luna safety_policy:** finished default-off `_wivrn_astc_overlap_sender=1` prototype. Changed only `video_encoder.cpp`, `video_encoder_astc.{cpp,h}`, root `ASTC_SENDER_OVERLAP.md`. Root reviewed and built server/OpenXR successfully; not committed or enabled. Old sender keeps its connection/clock/shard/timing fields until wait_idle. Error/no-data paths also wait. Legacy non-opt-in timestamp sampling preserved. Commit only if paced harness justifies it.
3. **Luna structural_budget:** investigating the GPU timestamp/fence gap using minimal submit/wait diagnostics and source review. Read-only root17:54 UTC snapshot GPUbusy99%, mem35%, sclk2422MHz: snapshot was taken while probes were active and may include owned harness work. Serialize all owned GPU samples with coarse_encode_probe, record load between jobs, and never stop user apps. No driver/clock modifications or guessed diagnosis.
4. Completed calibrated-timestamp experiment had invalid mapping: mapped GPU end was~1.44ms after host fence-return, beyond measured calibration uncertainty. Do not use its derived queue/return-delay estimates. Original uncalibrated same-device CSV remains authoritative. Scratch `overnight-stereo-gpu-20261004/src/calibrated.cpp`.
5. Completed strict reusable-DCtx probe: native2176² validated/hash-matched inputs, Pico33–36us/eye saving, PC neutral. Strict malformed-then-valid checks passed ASan/UBSan. Do not integrate solely for this marginal gain. Source/log `strict-dctx-probe-20261004/results.txt`; earlier1920×1080 run preserved separately and supports no native claim.
6. Root: publish bounded probe evidence and update this queue. Largest measured offscreen cost remains host fence wait under concurrent load. Do not claim stage timing proves real VR quality or latency.
7. If pursuing partial bands, first solve transport completeness, validated geometry/length/capability, coherent left/right revision barrier and pose-safe presentation. Mixed old/new poses can create seams/jitter. ASTC push_data has no view_info/partial completion event; stream pairing is by frame ID. Upload an assembled CPU image into a free texture-pool item; never patch an image held by the renderer.
8. At deadline clean only owned `/data/local/tmp/nx-astc14-20261004` benchmark files when idle (plus exact other owned paths returned by agents). Preserve app data/certificate and user processes. Pause heartbeat, summarize measured wins and unverified gates.

## Efficiency

Use bounded Luna jobs, RTK and native helpers. Next measurement should change a decision. Keep replies concise/cute. Verify figures and push only owned paths. Headset render loops are not fresh source FPS; offscreen/CPU tests are not sustained motion, HEVC parity or physical photon latency.
