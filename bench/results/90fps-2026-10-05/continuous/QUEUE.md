# Continuous NXVC work queue

User resumed work on 2026-10-05, until explicitly told to stop. Automation `nxvc-continuous-improvements`; no fixed deadline. Keep tasks bounded and token use low. Quiet unchanged state; notify meaningful outcomes only. [Completed overnight report](../../90fps-2026-10-04/motion-packing/OVERNIGHT_STATUS.md).

## Current verified state — 12:40 UTC

- Source `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, branch `pyrowave-probe`, clean/pushed6902940f; source hashes match tested code. Verify actual dirty state before resuming. Reports `/run/media/nerdrx/Lex/claude/nx-warp`, main; preserve unrelated README logo/branding, bisect/scratch and supplied private photos. Commit/push owned files only. Check actual dirty files before resuming.
- No WiVRn server/dashboard process observed at resume. No installation, restart or option activation. Last recorded native baseline2176²/eye ASTC8×8, no foveation/JPEG/blur/object-motion warp; last recorded installed clientc514841f. Verify current device/runtime before any live work. Experimental v4, parallel-eye, quiet poll and terminal assist remain off.
- Use cheap Luna, Ponytail, Caveman, RTK. Offscreen tests only; no mouse/focus/global runtime/routes/drivers/clocks/user app manipulation. Pico testing only when verified unused. Serialise owned timed workloads; CPU/GPU component or viewer-loop timings never prove freshFPS, HEVCparity, livequality or photon latency.

## Completed bounded tasks

1. **Fixed Zstd jobs, default off:** root production-like system-library replay preserved contexts/buffers and q6 selection; 30 pairs improve, mean −1.058ms. Pico CPU ABCCBA replay: 40 calls/mode, exact blocks, essentially unchanged cost. Root caught bundled server MT support OFF; source now enables support server-only, client/no-server OFF. Actual static-lib tests pass normal/SAN and no-MT fallback. Actual bundled library: two separate 30-pair runs save paired mean 0.864/1.025ms, but run1 p95 regresses3.111→3.300ms. **Retain both; no stable-tail/live claim.** Source ca777722 built/pushed, option `_wivrn_astc_zstd_jobs="1"` remains off. Same six packets match byte-for-byte across libraries and Pico. [Graphs, raw rows, methods, gates](zstd-jobs/README.md). Scratch `overnight-recovery/zstd-workers`, `zstd-workers-pico`, `zstd-workers-bundled`.
2. **Unknown-tail arrival replay:** 30 checks normal/strictSAN, including parity after NACK before reply. Actual history/shard/FEC helpers; private receiver polling remains modeled. A miss spends one of two rounds; arrival distribution unknown. [Retained replay](unknown-tail-arrival/README.md). No production policy change justified.
3. **Freshness audit:** sender cap8/decoder cap2 already bound stale queues; skipping in-flight decode can break reference/ownership. No cheap policy/copy removal justified. Queue dwell and drop counts are missing evidence. [Actual source audit](FRESHNESS_AUDIT.md).
4. **Queue diagnostic:** source1a1901bf/831aafed adds exact-1, default-off Android queue timing property. New mean/max dwell, all-dequeue denominator, oldest-pending drops and stream tag; no new clocks/counters when disabled, no policy change. Complete Android RelWithDebInfo native target passes after final source/header changes. Debug target's stale NXWarp cache failed; retained alongside successful actual target evidence. Unsigned APK subsequently packaged in item12; no install/property activation/runtime-value verification. [Configuration and limits](QUEUE_DIAGNOSTIC.md).

5. **ASTC finer weights:** source audit followed by CPU reference pack/parse/decode of15 legal16B one-partition CEM8 blocks. Binary8x8 exactly fits three aligned two-colour edges, but ramp MSE rises59.93→4644.64. 6x6 improves some edges/colour patches while worsening others. Root independently reproduces exact CSV/pixels. Toy farthest-pair fitter differs from production PCA/q6-refit/dual-plane; raw phase deltas include intended motion. Follow-up translated16x16 selector preserves the toy gradient, exactly fits toy binary edges, and diagonal compressed bytes grow281→300. Motion-aligned source residual is0; decoded artifact residual is computed with borders excluded. Root independently reproduces source CSV/pixels. No shader/live/hardware/bandwidth win. [Moving-pattern gate](astc-fine-weights-gate/followup/README.md), [Mode audit](ASTC_FINE_WEIGHTS_AUDIT.md), [runnable reference gate and labeled synthetic figure](astc-fine-weights-gate/README.md).

6. **Send batching audit:** ordinary typed UDP video shards remain individually paced/retried; batching would change pacing/FEC/striping. Existing batch helper deliberately permits a dropped tail on positive partial send and returns intended payload bytes. Prior production-typed IPv6 loopback probe already showed only~0.07–0.10us/datagram apparent savings; at2k repair shards/s ceiling this is~0.2ms CPU/s, not a measured bottleneck. No new test or source change justified. [Actual path and prior gate](SEND_BATCH_AUDIT.md).

7. **Native-paired binary weights rejected:** exact retained2176²RGBA8 and actual q6 production-shader blocks match hashes/conversion/archivedpacketbytes. All73,984 Q2 candidates/eye legal. All-Q2 saves26.9% packetbytes but RGBMSE grows4.30×/8.27×. Strict20% error-improvement selector chooses158/1blocks, combinedSSE only−0.163%, packets+29B. Root independently rebuilt generic-path publicharness and reproducedbothfull-eye rows. No productionshaderchange justified for this candidate; no GPU/Pico/timing claim. [Raw rows, provenance, runnable code and figure](astc-production-selector-gate/README.md).

8. **Balanced6x6Q4 rejected:** same verified native inputs/baselines; exact36x64 pseudoinverse of ASTC decimation computedonce. All73,984 blocks/eye legal Q4/Q80, CEM8/9. All-Q4 grows error/bytes. Strictselector picks10.57%/15.84% blocks; combinedRGBSSE−2.24% but packets+8.40% (+56,531B). Root independently rebuilds/reproduces bothfull-eye rows. No useful candidate tradeoff; further finer-weight probes held. [Methods, raw rows and figure](astc-balanced-selector-gate/README.md).

9. **Repair-history cost audit:** ordinary FEC+history reuses one flattened blob, no warmed per-shard vector allocation. History-off/FEC-off snapshot removal is inapplicable; narrow history-on/FEC-off/direct-primary candidate could remove temporary flatten+ringcopy. Root source review: NACK collection doesnot take SendData outermutex; early direct-ring snapshot changespublicationorder. No source/timingwin inferred. [Actual paths and integration hazard](HISTORY_COST_AUDIT.md).

10. **Direct history-copy gate rejected:** normal/ASan/UBSan/TSan correctness passes; 30 retained matched pairs give mean processCPU −3.1803µs/eye-frame (30/30), wall −5.5388µs (27/30, one large legacy outlier). Exact blobs; historyon/FECoff/directprimary payloadmodel only. Early publication differs from actual sender, so extra reservation/commit complexity is unjustified. No source change. [Runnable code, raw rows, logs and paired figure](history-direct-gate/README.md).

11. **Post-refit endpoint coarsening rejected by quality gate:** ordinary CEM8/Q64 endpoint fields only; exact modes/weights and other blocks preserved, 17/4 orientation flips guarded. Native packets save7.71/8.99%, RGBPSNR falls0.995/3.981dB. Meets5%byte target, fails≤0.1dB quality limit. Normal/SAN rows match; root independently rebuilds and replays public wrapper, exactCSV. No distraction/motion/speed claim, no production change. [Raw metrics, runnable checks, failure boundary and figure](q6-coarse-endpoints-gate/README.md).

12. **Queue-diagnostic unsigned APK packaged:** supported Gradle `-Pnxwarp_dir` fixes stale sibling dependency/cache selection without source/cache-file edits. Correct existing33s4w1c5 native cache, full offline release assembly55s/exit0. Root independently verifies exact packaged/stripped ELF bytes, diagnostic property, parsed identityorg.meumeu.wivrn.nx.local v1.0/code1, alignment and expected unsigned signature-verification failure. No install-ready/live correctness claim; no APK/ELF publication/install/property/restart. [Logs, hashes and limits](queue-diagnostic-apk/STATUS.md).

13. **Owned-frame queue audit:** ASTC owns output bytes already, but shared sender frame metadata blocks safe overlap; per-encoder prior-send wait enforces one pending/in-flight job. Globalcap8 is not eight queued ASTC frames per eye. Cap-only tweak rejected; metadata/stereo-aware supersession needs its own justified gate. [Source audit and measurement gap](SENDER_OWNED_FRAME_AUDIT.md).

14. **Producer wait diagnostic built, default off:** source a98d5ac exposes wall wait before existing encode timestamps; per-stream180-call mean/max, idle calls included, zero new clocks/counters when off/non-ASTC. Root executes actual extracted branches and verifies gating/denominator/reset; complete configured server builds and source diff checks pass. No activation/live values or latency win. [Runnable check, logs and scope](sender-wait-diagnostic/README.md).

15. **Partition architecture audit corrected:** root executes actual bundled weight-mode parser;0x0E1/0x1BF/0x141 legal,0x1AE invalid. Corrected bit capacity/padding/Q2 interpolation and1920x1080 prior fitter scope. Existing evidence rejects that fitter, not every partition idea. No new photo/GPU/Pico probe/source feature justified. [Actual-parser check and scoped decision](two-partition-audit/README.md).

16. **Fence attribution boundaries corrected:** existing offscreen GPU/host rows cannot isolate compositor slot contention or semaphore/queue delay. Base CPU busy wait precedes subclass GPU fence; encode timeout doesnot clear slot validity. Root keeps guards and plots retained20 serial samples with distinct boundaries; no new run/current-source/live claim. [Actual source audit and scientific figure](fence-attribution/README.md).

17. **Base slot busy-wait diagnostic built, default off:** source dc012b1 reports per-stream 180-call mean/max around the existing atomic busy gate before IDR skip. Disabled/non-ASTC adds no clocks/counters; skipped/idle calls count. Root executes actual-source mock checks and existing sender regression; complete server build and source diff checks pass. ASTC GPU guard unchanged; no live wait/latency value or optimization claim. [Checks, build evidence and scope](slot-busy-diagnostic/README.md).

18. **Wire-budget contract audited:** FEC data-share reserve already accounts for parity payload; fixed framing and requested repairs lie outside ordinary frame-byte numerator. Arbitrary extra FEC multiplier rejected. Historical aggregate/max-span concurrency assumption scoped separately in item19. [Actual-source audit](WIRE_BUDGET_AUDIT.md).

19. **Stereo delivery-rate scope corrected:** production BBR used aggregate eye bytes over one eye's widest span. Actual-controller serial fixture overestimated533.333 vs266.667Mbps. Source6902940f now samples matching per-stream bytes over interval union, excluding untimed/invalid bytes and idle gaps; app-limited threshold unchanged, requires matched-byte timing. Congestion utilisation and aggregate quality scaling unchanged. Normal/SAN BBR86checks; root five suites and3additional ordering/boundary cases pass; complete server builds. Single/overlap cases unchanged. Source pushed; no install/restart/live-change or live smoothness claim. [Raw rows, runnable checks, scientific figure and limits](stereo-rate-scope/README.md).

20. **Coupled serial-eye budget response reproduced:** actual controller/pacing-slot helpers, paired-ready ideal service, fully loaded media target,500Mbps/2Gbps assumed capacities. After1200virtual frames, narrow baseline estimates1Gbps/targets850Mbps; corrected estimates500/targets425Mbps. Wide control retains1Gbps ceiling. Root independently reproduces all4800rows byte-for-byte, including output path spaces and basic schedule checks. Unbounded ideal-service overrun is NOT actual sender backlog; queues/drops/FEC/encoder load/feedback delay omitted. Periodic probes still temporarily exceed the narrow capacity/deadline. No source/live change from this follow-up. [Raw traces, model limits, runnable gate and figure](stereo-rate-coupled/README.md).

## Active bounded tasks — inspect agents before starting anything

- No running child or owned benchmark jobs after the completed gates. Source6902940f is clean/pushed; item19 report and item20 follow-up are owned publication work. Verify report remote after commit. No install, server restart, experimental flag activation or Pico work.

## Next bounded gates

- New coupled trace exposes short periodic probes whose modeled total serial interval exceeds the desired period even when max per-eye utilisation is below1. Review existing probe/ceiling controls and prior tests before considering a bounded native deadline-aware probe gate. Do not restore a merged utilisation envelope for unrelated NX-direct wire IDs, disable capacity rediscovery blindly or infer real missed refreshes from this model.
- Matched accounting still needs actual per-stream arrival/byte correlation. Pair those with producer/slot waits and fresh stereo delivery before any live benefit claim; default-off diagnostic source exists. No silent install/activation of an active session.

- Diagnose queue dwell before changing drop policy. Do not bypass repair/history or motion-reference ownership based on source speculation.
- Fixed-job timing has variable tails under normal desktop load. Before enabling, measure actual live fresh stereo delivery and game-load contention with explicit opt-in, preserving active user sessions.

## Do not repeat

- Uniform step-two rounding of retained q6 CEM8 endpoints: failed quality gate on both native fixtures; no further tie-rounding/threshold microprobes without stronger evidence.

- Direct history-copy microbenchmark: tiny CPU saving and unsafe sender publication shortcut. Do not spend another gate on this without evidence of material history contention.

- Rejected reused-endpoint8x8Q2 and6x6Q4 finer grids on native pairs. Hold further weight-mode microprobes without a new justified fitter/representation.
- Rejected entropy layouts, weak motion savings, naive repeated conversion, packed shared-cache GPU probe, sender-wait relocation with missing old fixtures, unsafe early image release or idealised band transport.
- Do not call an option live/proven merely because source built. Existing ordinary-L3 stereo overlap saves2.620ms paired mean offscreen; real compositor/Pico motion gate remains.
- Native90/240freshFPS, fullscenequality/HEVCparity and physical photon latency remain unproven.

## Stop policy

When user asks stop/pause, stop owned jobs safely, preserve user sessions, publish actual progress/limits and disable automation through tool. Do not edit automation TOML by hand.
