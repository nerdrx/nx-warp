# Continue NXVC work until 06:00 Berlin

Deadline: **5 October 2026, 04:00 UTC / 06:00 Europe/Berlin**. The user explicitly requested continued work until then. Heartbeat `nx-warp-two-hour-optimization` now runs at :00/:30, including the deadline. Check the time and existing root/Luna agents before a new job. Do not duplicate ongoing work. Keep unchanged checks quiet. At deadline stop owned benchmarks, publish actual results, and pause the heartbeat; preserve user applications and VR sessions.

## Checkout and runtime boundaries

- Source: `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, branch `pyrowave-probe`, pushed **076d55da**; sender-overlap prototype was rejected/removed. UDP pool and one-shard stack handoff are committed and built. Compact format commit **7e8d74d2**, parallel eyes **4d7a8dd5**.
- Report: `/run/media/nerdrx/Lex/claude/nx-warp`, main **dd1ca50**, pushed. Unrelated dirty README logo/branding and untracked wordmark/bisect/scratch files remain. Do not broadly stage them. Working README logo is violet64; HEAD is light150. Stage owned prose from HEAD through `hash-object`/`update-index` to preserve branding.
- Actual profile: native **2176×2176 per eye**, ASTC8×8, stream_scale1, no foveation/JPEG/blur/object-motion warp. Installed client remains **c514841f**, independent v1/v2 packets. New experiments have not been installed or enabled live.
- Earlier server parent **1721736**, runtime child **1757965** are stale after a host reboot observed at19:29UTC (uptime~247s). No wivrn process found in /proc; root did not restart it. Child is not a duplicate server. Disk binary was rebuilt, running processes were not restarted. Pico **PA8150MGGB110166G**, A8110, app `org.meumeu.wivrn.nx` PID9926 was asleep/display OFF before and after isolated CPU tests. Recheck state before device work. No UI, property, clock, route or driver changes.
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

## In progress / next bounded queue (20:17 UTC)

1. Source HEAD **076d55da** pushed, including default-off age trial. Final server/OpenXR and Android native-library builds pass. Profile and installed APK unchanged; no feature enabled or installed. Host reboot observed at19:29UTC (uptime~247s); no wivrn process exists now. Earlier parent/child PIDs are stale. Report HEAD **dd1ca50** pushed with combined/Pico/component reports and graphs. Root reviewing current bounded work; do not duplicate agents/GPU jobs.
2. Default-on bounded packet buffer recycling **b712179**: one idle spare, actual capacity <= raw+32. Component normal/ASan/UBSan/TSan and final Android build pass. Native 1000-packet fragmented component test reduced vector growth allocations10000→20. Timing is not a complete decoder/Pico improvement claim.
3. Default-on direct-span FEC XOR and checked bool bytes **78b93a0**: final matched O2 CPU3 ABBA condition/block mean recovery1.844→1.665us (-9.7%), allocated bytes6135→4727 (-23%). Wire blobs/type hashes unchanged. Actual FEC normal+halt-on-error ASan/UBSan4292checks pass. Removing an existing malformed-bool UB is included in final comparison.
4. Opt-in fast independent Zstd **221f6834**, default3 unchanged, level1 ignored for motion. Pinned 800-row production packet probe: compact1 vs ordinary3 serial packing3.812→2.505ms p50 and2.95% fewer bytes on two native fixtures. Identical decoded ASTC; actual network/Pico timing excluded. Compact requires new v4 client; installed client lacks it.
5. Opt-in source-index ASTC skew **5d8f3f7**, default3 unchanged. debug.wivrn.nx.astc_skew=0/1 (Android NXASTC only), read at construction. Actual window/shard-set tests188checks normal+ASan/UBSan pass. Smaller skew can forfeit NACK/FEC repair and coherent stereo updates; no physical-time deadline or live gate passed.
6. Same-device three-condition offscreen stereo test finished and root reviewed/packaged `overnight-recovery/combined-stereo`:50 measured per mode after20 warmups; interleaved serial3 / parallel1 / parallelcompact1. Wall p50/p95:4.956/5.657 →2.595/3.121 →2.634/3.137ms. Paired bytes672903 /693119 /652905; exact decoded data within this run. GPU dispatch ~.85-.88ms; upload/network/Pico excluded. Fresh raw/output vectors and query handling inside timed harness, not production end-to-end. Public CMake build passed after reboot. No GPU job running.
7. Per-image upload-fence audit rejected complexity: prior-upload-fence180-frame window means async n392 median.8us/p952.4us/max28.3us, syncn78 median1.0us/p951.5us/max7.4us. These are window mean percentiles, not single-fence tails. `upload-fence-audit` report/CSV/parser/figure packaged. Earlier restored coarse/structural agents stuckpending_init after reboot, interrupted; do not duplicate/rely on them.
8. Root isolated PicoCPU check19:40UTC: actual strictdecode ordinary3/ordinary1/compact1,20warm100callspermode ABCCBA,300CSVrows, identical ASTCaftereverycall. Sequential paired p50/p95:1.865/1.936,1.822/1.920,2.041/2.150ms; compact1+.176ms versusordinary3. Bytecountsmatch pinnedCPUfixtures (notGPUfixturegeneration):674798/694843/654922. ADBasleepOFFbeforeafter, thermal0, cpu0freq1075200→1612800unlocked. NoUI/APK/property/app/sessionchanges; exactownednewdevice/tmpdir deleted. Public `fast-zstd-pico` artifacts prepared, no private sources/payloads/binaries. Hostnormal+halt-on-errorASan/UBSan probe passed first.
9. Default-OFF actual-period reassembly trial committed **cbe970d4**. ExactAndroidNXASTCproperty debug.wivrn.nx.astc_deadline=1 atconstruction. Incompletefront firstreceive age>=2 actualdisplayperiods ONLYwithnewerCOMPLETEframe. Unknown/rollback/overflowguards, FEC/NACKbeforepump, exactstereomatchingunchanged. Arrival-driven NOtimer/harddeadline. Rootreran normal+halt-on-errorASan/UBSan200checks and finalAndroidnativebuildPASS; no install/propertyactivation. Actual window replay completed; single-eye age falls but stereo repair race loses coherent frame1, adding one held refresh. Rule remains default OFF. Fresh Luna deadline_patch_finish is auditing whether existing safe complete-ID publication can guard stereo coherence. No production edits, device or GPU jobs by agent; do not duplicate.
10. Retained UDPpool27026bbd and one-shard stack path, alloc95.7%cut/timingneutral, report pushed. Sender overlap and ~33-36us DCtx reuse rejected. Invalid calibrated GPU queue margins discarded. Do not repeat weak entropy probes or assert driver cause.
11. Partial bands/coarse safety remain unintegrated. Exact WiVRn accumulator requires complete frame and timing; inner bands alone do not recover partial frames. Any new delivery work must change an actual measured decision.
12. Default-on source **076d55da** reuses bounded FEC metadata per function/thread; no payload-sized cache. Matched O2 CPU3 ABBA recovery mean1.635→1.325us, allocation calls65→2; warm encode85→40.5ns, allocations12→0. 4296checks normal/ASan/UBSan/TSan and final server/OpenXR/Androidnative builds pass, unchanged wire hashes. Source pushed; no installed client change. Public fec-metadata-reuse report/graph ready.
13. Root next bounded direction: offscreen direct host-cached shader output versus current device-local output plus transfer, same native inputs/shader and packet work. Must check exact ASTC output, GPU durations, memory flags, matched order/load and Vulkan validation before considering production. No experiment running yet; earlier calibrated-clock probe remains invalid.
14. Deadline06:00Berlin: stop only owned benchmarks, preserve appdata/certificate/user processes and unchanged live server, pause heartbeat, publish actual wins and limits. No90 fresh motion/HEVC parity/photon claim.

## Efficiency

Use bounded Luna jobs, RTK and native helpers. Next measurement should change a decision. Keep replies concise/cute. Verify figures and push only owned paths. Headset render loops are not fresh source FPS; offscreen/CPU tests are not sustained motion, HEVC parity or physical photon latency.
