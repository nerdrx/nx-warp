# Continuous NXVC work queue

User resumed work on 2026-10-05, until explicitly told to stop. Automation `nxvc-continuous-improvements`; no fixed deadline. Keep tasks bounded and token use low. Quiet unchanged state; notify meaningful outcomes only. [Completed overnight report](../../90fps-2026-10-04/motion-packing/OVERNIGHT_STATUS.md).

## Current verified state — 08:18 UTC /10:18 Berlin

- Source `/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe`, branch `pyrowave-probe`, clean/pushed831aafed. Reports `/run/media/nerdrx/Lex/claude/nx-warp`, main; preserve unrelated README logo/branding, bisect/scratch and supplied private photos. Commit/push owned files only. Check actual dirty files before resuming.
- No WiVRn server/dashboard process observed at resume. No installation, restart or option activation. Last recorded native baseline2176²/eye ASTC8×8, no foveation/JPEG/blur/object-motion warp; last recorded installed clientc514841f. Verify current device/runtime before any live work. Experimental v4, parallel-eye, quiet poll and terminal assist remain off.
- Use cheap Luna, Ponytail, Caveman, RTK. Offscreen tests only; no mouse/focus/global runtime/routes/drivers/clocks/user app manipulation. Pico testing only when verified unused. Serialise owned timed workloads; CPU/GPU component or viewer-loop timings never prove freshFPS, HEVCparity, livequality or photon latency.

## Completed bounded tasks

1. **Fixed Zstd jobs, default off:** root production-like system-library replay preserved contexts/buffers and q6 selection; 30 pairs improve, mean −1.058ms. Pico CPU ABCCBA replay: 40 calls/mode, exact blocks, essentially unchanged cost. Root caught bundled server MT support OFF; source now enables support server-only, client/no-server OFF. Actual static-lib tests pass normal/SAN and no-MT fallback. Actual bundled library: two separate 30-pair runs save paired mean 0.864/1.025ms, but run1 p95 regresses3.111→3.300ms. **Retain both; no stable-tail/live claim.** Source ca777722 built/pushed, option `_wivrn_astc_zstd_jobs="1"` remains off. Same six packets match byte-for-byte across libraries and Pico. [Graphs, raw rows, methods, gates](zstd-jobs/README.md). Scratch `overnight-recovery/zstd-workers`, `zstd-workers-pico`, `zstd-workers-bundled`.
2. **Unknown-tail arrival replay:** 30 checks normal/strictSAN, including parity after NACK before reply. Actual history/shard/FEC helpers; private receiver polling remains modeled. A miss spends one of two rounds; arrival distribution unknown. [Retained replay](unknown-tail-arrival/README.md). No production policy change justified.
3. **Freshness audit:** sender cap8/decoder cap2 already bound stale queues; skipping in-flight decode can break reference/ownership. No cheap policy/copy removal justified. Queue dwell and drop counts are missing evidence. [Actual source audit](FRESHNESS_AUDIT.md).
4. **Queue diagnostic:** source1a1901bf/831aafed adds exact-1, default-off Android queue timing property. New mean/max dwell, all-dequeue denominator, oldest-pending drops and stream tag; no new clocks/counters when disabled, no policy change. Complete Android RelWithDebInfo native target passes after final source/header changes. Debug target's stale NXWarp cache failed; retained alongside successful actual target evidence. No APK/install/property activation/runtime-value verification. [Configuration and limits](QUEUE_DIAGNOSTIC.md).

## Active bounded tasks — inspect agents before starting anything

- Reused cheap Luna `zstd_worker_gate`, 15min read-only starting08:08UTC: legal standard ASTC8x8 finer weight-grid / lower-weight-precision feasibility at same16B footprint. Also compare dual-plane/two-partition usefulness for colour blobs. Inspect actual mode fields and local astcenc reference tables. No source edits, compiler/GPU/device/app tests yet. Avoid rejected footprint switching and entropy/motion microprobes. Scratch `overnight-recovery/astc-fine-weights-audit`. Need smallest byte-valid synthetic reference/visual gate, with palette limits and possible shimmer explicit. Do not claim same hardware cost without Pico measurement.
- Root: finish queue diagnostic evidence and preserve unrelated README branding. All owned timed jobs and builds finished; `deadline_patch_finish` completed. Pico CPU files cleaned up; display remained OFF, VR mode false, thermal0.

## Next bounded gates

- Diagnose queue dwell before changing drop policy. Do not bypass repair/history or motion-reference ownership based on source speculation.
- Fixed-job timing has variable tails under normal desktop load. Before enabling, measure actual live fresh stereo delivery and game-load contention with explicit opt-in, preserving active user sessions.

## Do not repeat

- Rejected entropy layouts, weak motion savings, naive repeated conversion, packed shared-cache GPU probe, sender-wait relocation with missing old fixtures, unsafe early image release or idealised band transport.
- Do not call an option live/proven merely because source built. Existing ordinary-L3 stereo overlap saves2.620ms paired mean offscreen; real compositor/Pico motion gate remains.
- Native90/240freshFPS, fullscenequality/HEVCparity and physical photon latency remain unproven.

## Stop policy

When user asks stop/pause, stop owned jobs safely, preserve user sessions, publish actual progress/limits and disable automation through tool. Do not edit automation TOML by hand.
