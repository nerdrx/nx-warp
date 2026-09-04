# hybrid/ -- the hybrid-mode experiment

PAPER.md 6.10 expects the Pico 4 to land in **hybrid mode**: a hardware HEVC
base layer through MediaCodec, with our pose-warped enhancement layer on top
in compute. That expectation costs 8 to 12 ms of base-decoder latency (3.5)
and gives up base-layer tile pipelining. This directory answers, with numbers,
whether it is worth it, and how the bits should split.

The question, precisely:

> For a given total bitrate, how should bits split between an HEVC base layer
> (at full, 3/4 or 1/2 resolution) and our enhancement layer, and what quality
> results versus (a) HEVC alone at the full bitrate and (b) our pure codec?

* `sim/` -- `nxvc-hybridsim`, the simulator
* `RESULTS.md` -- the sweep tables and the recommendation
* `../docs/HYBRID.md` -- the design consequences and the LCEVC differentiation
  notes for the FTO review

## Why Python

The simulator is Python + numpy. It is not the codec and never will be: it is
a rate-distortion model whose job is to be *read and argued with*. At the
1024^2 x 90-frame test size a whole sweep point runs in about 100 s, which is
fast enough, and every stage (warp, blend, transform, quantiser, bit estimate)
is a dozen lines that a reviewer can check against PAPER.md. `ref/` is where
the real thing lives.

## Running it

```sh
cd hybrid/sim

# render the synthetic sequence (once; lands in the scratch dir)
./nxvc-hybridsim material --size 1024 --frames 90

# one configuration
./nxvc-hybridsim one --kind hybrid --base-scale 0.5 --base-frac 0.55 \
                     --total-mbit 150

# the whole grid: 3 base resolutions x 5 splits x 4 bitrates, plus anchors
./nxvc-hybridsim sweep --workers 4 --out $NXVCH_SCRATCH/results/sweep-main.json

# tables
./nxvc-hybridsim report $NXVCH_SCRATCH/results/sweep-main.json --out ../RESULTS.md
```

Every ffmpeg/x265 child runs under `chrt -i 0 taskset -c 12-15 nice -n 19`
with `-threads 4` / `pools=4` (`nxvchybrid/cpu.py`; override with `NXVCH_CPUS`,
`NXVCH_THREADS`, `NXVCH_NO_CPU_LIMIT`). All material and intermediates go to
`$NXVCH_SCRATCH` (default
`/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/hybrid`), never the repo and
never `/tmp`.

## What the simulator models

| Stage | Model | Paper |
|---|---|---|
| Material | procedural equirectangular panorama, pinhole camera, pose log to 300 deg/s, 2x supersampled, plus screen-space movers | 2.2 |
| Base layer | ffmpeg/libx265, zerolatency, P-only, `ref=1`, one IDR, tight VBV, at 1x / 0.75x / 0.5x | 1.7, 3.5 |
| Spatial hypothesis | decoded base, separable Catmull-Rom upsample | 1.7 |
| Temporal hypothesis | `warp(Out(N-1))` through `H = K R_{N-1}^T R_N K^-1` (float), plus a per-tile integer MV, +-6 px | 2.2, 2.3, 2.9 |
| Blend | per 64x64 tile, weight from a 2-bit alphabet {0, 1/4, 1/2, 1}, chosen by least residual energy | 1.7 |
| Residual | 8x8 orthonormal DCT, dead-zone quantiser on the HEVC QP ladder | 1.4, 1.5 |
| Rate | per-frame QP bisection to a leaky-bucket budget, then per-tile skip at the floor | 4.6, 4.6.1 |
| Bits | coded-block flag + per-frequency significance entropy + sign + `log2(1+|q|)` | 1.6 |

## What it does *not* model

Stated up front, because they bound how far the numbers travel:

* **The integer warp.** PAPER.md 2.2 defines the warp in Q8.24 fixed point so
  encoder and decoder cannot drift. The simulator warps in float and runs the
  same code on both sides, so it does not charge the residual for the
  homography quantisation error. That error is small (the paper argues under
  1/32 pel at 32 px tiles) but not zero.
* **Foveation.** Flat quantisation everywhere. Foveated bit allocation (5.1)
  is a separate axis that would move every row in the same direction.
* **Entropy-coder engineering.** No RDO, no trellis quantisation, no adaptive
  contexts, no deblocking, no directional intra, one MV per 64x64 tile. x265
  has all of these. The pure-codec anchor is therefore a *floor* on what our
  codec can do, not an estimate of it.
* **YCbCr limited range and the YCoCg-R conversion** of the imported
  AHardwareBuffer (3.5). The simulator keeps one full-range 8-bit domain
  throughout; the real base round trip loses a little more.
* **Loss.** No packet loss, no concealment. 2.7 and 4.4 are their own study.

## Reading the numbers

Quality figures are at the simulator's own 1024^2 resolution; bitrates are
always quoted as their **2 x 2048^2 x 90 Hz equivalent** (`sweep.py` scales by
the pixel ratio). Absolute PSNR is not a prediction of headset PSNR. What
travels is the *comparison between configurations at a matched bit budget*,
which is what the experiment was built to answer.
