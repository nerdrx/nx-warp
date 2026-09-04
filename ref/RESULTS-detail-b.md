# Intra detail tools (syntax v1.5): measurements

The "intra detail tools" package, built and measured on branch
`tourney/detail-b`. Four items were asked for; **two are built and shipped,
one was measured and rejected, and one was not reached.**

| # | tool | tool bit | verdict |
|---|---|---|---|
| 1 | chroma from luma | 24 `INTRA_CFL` | **built**, on by default, -1.13 % / -0.28 % BD-rate |
| 2 | 4x4 transform split | 19 `XFORM_4X4_SPLIT` | **built**, on by default, -0.48 % / -0.45 % BD-rate |
| 3 | adaptive dead zone per context class | none | **measured and rejected**; +0.10 % at best, five settings, section 3 |
| 4 | planar / DC-plane refinement | none | not reached; section 6 says what is left and why it is not free |

Everything here was produced by `tools/quality/compare.py` against
`x264 --keyint 1 --tune zerolatency` through ffmpeg n9.0.1, on the **v2
(band-limited) sequence** `vr-mixed-1024-v2` (2048x1024 side by side,
6 frames, 90 fps, `synthetic:mixed:seed1:v2-bandlimited-ss4`), which is the
sequence `$NXQ_SCRATCH/seq` now carries. Every process ran under
`chrt -i 0 taskset -c 24-27 nice -n 19`. Result files are
`$NXQ_SCRATCH/results/tourney-detail-b-*.json`; the driver is
`tools/quality/run-b.sh`.

> **These numbers are not comparable with `RESULTS-intra.md`.** That document
> measured the v1 sequence, whose rates at the same QP are about a quarter of
> the v2 sequence's; the 100-400 Mbit band therefore lands at QP 20-34 here
> instead of QP 0-24, on band-limited material that is harder for us at every
> point. The v1.4 baseline re-measured on this sequence is **+117.67 %** on
> 4:4:4, not the +40.35 % `RESULTS-intra.md` records. The before/after pair
> below is internally consistent and that is what it is for.

---

## 1. The gate, before and after

Each row adds one tool to the v1.4 default (`--intra-dir on --ctx v2
--sign-hide`). `+ INTRA_CFL` is `--split4 off`, `+ XFORM_4X4_SPLIT` is
`--cfl off`, and the last row is the shipped v1.5 default.

PLACEHOLDER_GATE

---

## 2. Per-tool detail

PLACEHOLDER_PERTOOL

---

## 3. The adaptive dead zone: measured and rejected

PLACEHOLDER_DEADZONE

---

## 4. What it costs

PLACEHOLDER_COST

---

## 5. GPU cost accounting for Pass B

PLACEHOLDER_GPU

---

## 6. What is left

PLACEHOLDER_LEFT

---

## 7. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export PATH=$PWD/build-ref/bin:$PATH
cd tools/quality

./run-b.sh base  yuv444p $NXQ_SCRATCH/detail-b-basebin      # the v1.4 baseline
./run-b.sh final yuv444p $PWD/../../build-ref/bin           # v1.5 defaults
./run-b.sh cfl   yuv444p $PWD/../../build-ref/bin --split4 off
./run-b.sh split yuv444p $PWD/../../build-ref/bin --cfl off
```

`run-b.sh` is the whole invocation, including the QP ladder chosen for the v2
sequence and the Phase 1 gate flags; `yuv420p` for the other format.
