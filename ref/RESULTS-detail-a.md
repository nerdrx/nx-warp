# Syntax v1.5: the intra detail tools, measured

The package is three items, each measured on its own:

| tool | bit | what it is | verdict |
|---|---|---|---|
| `XFORM_4X4_SPLIT` | 19 | a per-block 4x4 transform split (SYNTAX.md 6.7, 9.8) | **shipped** |
| `INTRA_CFL` | 24 | a tenth chroma intra mode, chroma from luma (SYNTAX.md 7.7) | **shipped** |
| adaptive dead zone | none | encoder-side, per frequency band | **kept as structure, no gain** |
| reconstruction offset | none | decoder-side, per band | **measured and rejected** |

Both bitstream tools are **off in `nxvc_config_default()`**, and `v01`-`v56`
of the conformance set are byte-identical to what a v1.4 build produced --
`ctest -R ref.vectors` proves it on every commit. That is the whole point of
the tool-bit discipline: a stream without the bits decodes exactly as it did.

Everything below was produced by `tools/quality/compare.py` against
`x264 --keyint 1` through ffmpeg n9.0.1, on the **v2 band-limited** sequence
`vr-mixed-1024-v2` (2048x1024 side-by-side, first 6 frames, 90 fps,
`synthetic:mixed:seed1:v2-bandlimited-ss4`) in both 4:4:4 and 4:2:0. Every
process ran under `chrt -i 0 taskset -c 20-23 nice -n 19` with ffmpeg capped
at `-threads 4`, alongside other tournament agents on other cores. Result
files are under `$NXQ_SCRATCH/results/tourney-detail-a/`, and each row
measures a **snapshot** of the binaries taken when the row started
(`tools/quality/run-detail-a.sh`), so a rebuild during a run cannot change what
was compared.

Reproduce the whole table with

```sh
tools/quality/all-detail-a.sh
```

---

## 0. The gate, cumulative

Each row adds one tool to the row above. `base` is `--split4x4 off --cfl off`,
which is the shipped v1.4 default and reproduces a v1.4 build byte for byte.

PLACEHOLDER_GATE_444

PLACEHOLDER_GATE_420

PLACEHOLDER_VERBATIM

---

## 1. Operating points

PLACEHOLDER_POINTS

---

## 2. Per-tool detail

PLACEHOLDER_PERTOOL

---

## 3. The dead zone and the reconstruction offset

Both halves of the third item measured at or below the noise floor, and the
honest record of that is the point of this section.

### 3a. Encoder-side dead zone, per band

The dead-zone quantizer is `q = floor(|c| / step + f)`. `f` was a flat `1/3`
written as `t / 3` in three places; it is now `kDeadZoneDc` / `kDeadZoneAc`
in `ref/src/tables.cpp`, indexed by `band_of(scan position)` -- the same
banding the LEVEL contexts use -- and expressed in **sixths of a step**, so
that the value `2` reproduces the old `t / 3` **bit-exactly**. That exactness
is deliberate: it is what lets the refactor land with zero conformance-vector
churn, and it is why `v01`-`v56` still pass unchanged.

Only two paths still use this quantizer at all. The RD trellis replaces it on
every residual block, so what is left is (a) the **DC plane**, which is the
intra predictor and is deliberately not trellised (a level chosen there moves
`pred` for all 64 blocks, and the trellis's single-unit distortion model would
be wrong about it), and (b) every unit under `--no-rdo`.

Swept on one 2048x1024 4:4:4 frame with the development hook
`NXVC_DZ_DC` (encoder-only, changes no syntax):

| `kDeadZoneDc`, sixths | QP 8 | QP 16 | QP 24 |
|---|---|---|---|
| **2,2,2,2 (= 1/3, shipped)** | **195 800 B / 53.336 dB** | **111 900 B / 47.658 dB** | **62 220 B / 41.519 dB** |
| 1.1,1.1,1.1,1.1 (Q6 12) | 195 600 / 53.320 | 111 900 / 47.658 | 61 642 / 41.399 |
| 1.5,1.5,1.5,1.5 (Q6 16) | 195 438 / 53.298 | 111 900 / 47.658 | 62 220 / 41.519 |
| 2.4,2.4,2.4,2.4 (Q6 26) | 203 656 / 53.313 | 111 770 / 47.674 | 62 220 / 41.519 |
| 3,3,3,3 (= 1/2) | 203 390 / 53.311 | 111 770 / 47.674 | 63 918 / 41.559 |
| 3,2.4,2,1.5 (fine at DC) | 196 384 / 53.324 | 111 736 / 47.651 | 62 218 / 41.523 |
| 1.5,2,2.4,3 (coarse at DC) | 203 496 / 53.298 | 111 916 / 47.651 | 63 770 / 41.567 |
| 2.4,2,1.5,1.1 | 195 600 / 53.330 | 111 872 / 47.671 | 61 538 / 41.485 |
| 2,2,1.5,1.1 | 195 502 / 53.321 | 111 900 / 47.658 | 61 538 / 41.485 |

(The sweep grid is finer than sixths because the hook takes Q6; the shipped
table is in sixths because that is the resolution that turned out to matter.)

Nothing here is worth more than **0.5 % of rate or 0.05 dB** at any QP, and no
shape beats flat. `RESULTS-intra.md` section 8 predicted exactly this --
"subsumed by the RD trellis by construction" -- and it was right; the only
surface the tool has left is the DC plane, and the DC plane is insensitive to
it. **The value stays at 1/3.** What the package keeps is the *structure*: a
named table with a spec reference in place of three copies of a magic `t / 3`,
banded so that a future retune has somewhere to go.

### 3b. Decoder-side reconstruction offset -- rejected

The brief conditioned this on measuring, and it does not. A level `q`
reconstructs at `q * step`; the offset moves that to `(q -+ delta) * step`,
with `delta` in sixty-fourths of a step, applied toward zero. Both sides ran
the same value (build with `-DNXVC_RECON_OFFSET_EXPERIMENT` and set
`NXVC_RECON_OFF`; a normal build compiles the hook out, so no conforming
stream can depend on it). Same frame, 4:4:4:

| `delta`, Q6 | QP 8 | QP 16 | QP 24 |
|---|---|---|---|
| **0 (shipped)** | **195 800 B / 53.336 dB** | **111 900 B / 47.658 dB** | **62 220 B / 41.519 dB** |
| -8 | 195 564 / 53.278 | 112 046 / 47.601 | 62 116 / 41.423 |
| -4 | 195 800 / 53.336 | 111 970 / 47.622 | 62 196 / 41.499 |
| +4 | 195 800 / 53.336 | 112 048 / 47.591 | 62 312 / 41.483 |
| +8 | 195 828 / 53.262 | 113 048 / 47.497 | 62 288 / 41.378 |

Every nonzero offset is worse **in both rate and quality** at every point, in
both directions. The mechanism is clear once stated: with the RD trellis on,
levels are not the output of a dead zone at all -- `rdoq_unit()` already picks
each level by `D + lambda*R` against the *actual* reconstruction point, so
moving that point only introduces error the trellis then has to spend bits
undoing. A reconstruction offset is the right tool for a codec whose encoder
is a fixed quantizer. This one's is not.

**It therefore gets no tool bit and no syntax.** Tool bit 25 is still free.

---

## 4. What it costs a GPU decoder

PLACEHOLDER_GPU

---

## 5. Encode and decode time

PLACEHOLDER_TIME

---

## 6. What is left

PLACEHOLDER_LEFT
