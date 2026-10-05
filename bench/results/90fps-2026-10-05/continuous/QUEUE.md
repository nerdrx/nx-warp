# Continuous NXVC work queue

User resumed work on 2026-10-05, until explicitly told to stop. Automation `nxvc-continuous-improvements`; no fixed deadline. Keep tasks bounded and token use low. Quiet unchanged state; notify meaningful outcomes only. [Completed overnight report](../../90fps-2026-10-04/motion-packing/OVERNIGHT_STATUS.md).

## Current verified state — 09:05 UTC

- Source `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, branch `pyrowave-probe`, clean/pushed831aafed. Reports `/run/media/nerdrx/Lex/claude/nx-warp`, main; preserve unrelated README logo/branding, bisect/scratch and supplied private photos. Commit/push owned files only. Check actual dirty files before resuming.
- No WiVRn server/dashboard process observed at resume. No installation, restart or option activation. Last recorded native baseline2176²/eye ASTC8×8, no foveation/JPEG/blur/object-motion warp; last recorded installed clientc514841f. Verify current device/runtime before any live work. Experimental v4, parallel-eye, quiet poll and terminal assist remain off.
- Use cheap Luna, Ponytail, Caveman, RTK. Offscreen tests only; no mouse/focus/global runtime/routes/drivers/clocks/user app manipulation. Pico testing only when verified unused. Serialise owned timed workloads; CPU/GPU component or viewer-loop timings never prove freshFPS, HEVCparity, livequality or photon latency.

## Completed bounded tasks

1. **Fixed Zstd jobs, default off:** root production-like system-library replay preserved contexts/buffers and q6 selection; 30 pairs improve, mean −1.058ms. Pico CPU ABCCBA replay: 40 calls/mode, exact blocks, essentially unchanged cost. Root caught bundled server MT support OFF; source now enables support server-only, client/no-server OFF. Actual static-lib tests pass normal/SAN and no-MT fallback. Actual bundled library: two separate 30-pair runs save paired mean 0.864/1.025ms, but run1 p95 regresses3.111→3.300ms. **Retain both; no stable-tail/live claim.** Source ca777722 built/pushed, option `_wivrn_astc_zstd_jobs="1"` remains off. Same six packets match byte-for-byte across libraries and Pico. [Graphs, raw rows, methods, gates](zstd-jobs/README.md). Scratch `overnight-recovery/zstd-workers`, `zstd-workers-pico`, `zstd-workers-bundled`.
2. **Unknown-tail arrival replay:** 30 checks normal/strictSAN, including parity after NACK before reply. Actual history/shard/FEC helpers; private receiver polling remains modeled. A miss spends one of two rounds; arrival distribution unknown. [Retained replay](unknown-tail-arrival/README.md). No production policy change justified.
3. **Freshness audit:** sender cap8/decoder cap2 already bound stale queues; skipping in-flight decode can break reference/ownership. No cheap policy/copy removal justified. Queue dwell and drop counts are missing evidence. [Actual source audit](FRESHNESS_AUDIT.md).
4. **Queue diagnostic:** source1a1901bf/831aafed adds exact-1, default-off Android queue timing property. New mean/max dwell, all-dequeue denominator, oldest-pending drops and stream tag; no new clocks/counters when disabled, no policy change. Complete Android RelWithDebInfo native target passes after final source/header changes. Debug target's stale NXWarp cache failed; retained alongside successful actual target evidence. No APK/install/property activation/runtime-value verification. [Configuration and limits](QUEUE_DIAGNOSTIC.md).

5. **ASTC finer weights:** source audit followed by CPU reference pack/parse/decode of15 legal16B one-partition CEM8 blocks. Binary8x8 exactly fits three aligned two-colour edges, but ramp MSE rises59.93→4644.64. 6x6 improves some edges/colour patches while worsening others. Root independently reproduces exact CSV/pixels. Toy farthest-pair fitter differs from production PCA/q6-refit/dual-plane; raw phase deltas include intended motion. Follow-up translated16x16 selector preserves the toy gradient, exactly fits toy binary edges, and diagonal compressed bytes grow281→300. Motion-aligned source residual is0; decoded artifact residual is computed with borders excluded. Root independently reproduces source CSV/pixels. No shader/live/hardware/bandwidth win. [Moving-pattern gate](astc-fine-weights-gate/followup/README.md), [Mode audit](ASTC_FINE_WEIGHTS_AUDIT.md), [runnable reference gate and labeled synthetic figure](astc-fine-weights-gate/README.md).

6. **Send batching audit:** ordinary typed UDP video shards remain individually paced/retried; batching would change pacing/FEC/striping. Existing batch helper deliberately permits a dropped tail on positive partial send and returns intended payload bytes. Prior production-typed IPv6 loopback probe already showed only~0.07–0.10us/datagram apparent savings; at2k repair shards/s ceiling this is~0.2ms CPU/s, not a measured bottleneck. No new test or source change justified. [Actual path and prior gate](SEND_BATCH_AUDIT.md).

7. **Native-paired binary weights rejected:** exact retained2176²RGBA8 and actual q6 production-shader blocks match hashes/conversion/archivedpacketbytes. All73,984 Q2 candidates/eye legal. All-Q2 saves26.9% packetbytes but RGBMSE grows4.30×/8.27×. Strict20% error-improvement selector chooses158/1blocks, combinedSSE only−0.163%, packets+29B. Root independently rebuilt generic-path publicharness and reproducedbothfull-eye rows. No productionshaderchange justified for this candidate; no GPU/Pico/timing claim. [Raw rows, provenance, runnable code and figure](astc-production-selector-gate/README.md).

## Active bounded tasks — inspect agents before starting anything

- Reused cheap Luna `zstd_worker_gate`, next25min CPU-only balanced6x6Q4 gate on same verified2176²source/baseline pairs. Precompute actual ASTC decimation pseudoinverse36x64 once, fit source projections against baseline endpoints, pack Q80 endpoints/Q4 weights. Reference-decode and strict20% SSE selector; recordMSE/selectedshare/full-eyeZstd3 bytes. Scratch `overnight-recovery/astc-balanced-selector-gate`. No GPU/Pico/source/runtime edit or privatephoto/payload output; no timing/liveclaim. Record blocker if exact grid fit exceedsbound. Inspect running agent before dispatching anything.
- Root: native-paired Q2 candidate independently replayed/rejected; report/figure published. Both ASTC synthetic gates independently replayed; labeled figures inspected and published. Public oracle main now explicitly returns0 for warning-free helper inclusion; results unchanged. Queue diagnostic/source/docs evidence published; send batching audit reviewed and rejected as a worthwhile current optimization. Preserve unrelated README branding. All owned timed jobs and builds finished; `deadline_patch_finish` completed. Pico CPU files cleaned up; display remained OFF, VR mode false, thermal0.

## Next bounded gates

- Diagnose queue dwell before changing drop policy. Do not bypass repair/history or motion-reference ownership based on source speculation.
- Fixed-job timing has variable tails under normal desktop load. Before enabling, measure actual live fresh stereo delivery and game-load contention with explicit opt-in, preserving active user sessions.

## Do not repeat

- Rejected entropy layouts, weak motion savings, naive repeated conversion, packed shared-cache GPU probe, sender-wait relocation with missing old fixtures, unsafe early image release or idealised band transport.
- Do not call an option live/proven merely because source built. Existing ordinary-L3 stereo overlap saves2.620ms paired mean offscreen; real compositor/Pico motion gate remains.
- Native90/240freshFPS, fullscenequality/HEVCparity and physical photon latency remain unproven.

## Stop policy

When user asks stop/pause, stop owned jobs safely, preserve user sessions, publish actual progress/limits and disable automation through tool. Do not edit automation TOML by hand.
