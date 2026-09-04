# GPU decoder cost: sparse coefficients, wavefront occupancy, fused stores

This is a **decoder-cost** package, not a compression package. It changes no
bitstream syntax, adds no tool bit, and produces byte-identical output: every
number below is a time or a byte count, and the rate columns a compression
package would carry do not exist because nothing about the rate moved.
`vk.decoder.conformance` is the check that says so — 152 streams, 0
mismatching samples, on RADV and on lavapipe, after every step.

Branch `tourney/sparse`. Method, hardware and the raw harness output are at
the end.

---

## 0. What changed, in one table

| | before | after |
|---|---|---|
| coefficient traffic, 2048 tiles 4:2:0, QP 36 | 25.6 MB | **0.93 MB** |
| coefficient traffic, QP 24 | 25.6 MB | **11.6 MB** |
| Pass B fixed cost per tile, RADV | 890 ns | **248 ns** |
| Pass B fixed cost per tile, lavapipe | 148 µs | **49 µs** |
| Pass B slope against payload, RADV | ~0 | **0.33 ms/MB** |
| wavefront occupancy, `kDirSched` 0 | 4.5 % | **18.2 %** |
| Pass B at `kDirSched` 0, QP 24, RADV | 1.780 ms | **1.183 ms** |
| Pass B with two display stores, QP 24, RADV | 2.271 ms | **1.685 ms** |
| Pass A fixed cost per tile, RADV | 124 ns | 170 ns |

Four changes, in the order they landed:

1. **Sparse coefficient transfer** (ADR 0026). Inside a coding unit,
   coefficient `k` is stored at slot `k` in *scan* order rather than at its
   raster index, and Pass A publishes `LAST + 1` per unit and writes slots
   `[0, LAST]` only. `LAST` is already in the syntax (SYNTAX.md 9.2) and the
   scan is already normative (5), so the layout is a re-indexing of numbers
   the stream already carries.
2. **Two fast paths the lengths make possible.** A unit of length 0 coded
   nothing, so its dequantize and both passes of its IDCT are skipped; a
   plane whose intra modes are all mode 0 skips the `INTRA_DIR` wavefront and
   takes the parallel path, bit-identically, because mode 0 reads no
   neighbour.
3. **16 threads per block in the wavefront** instead of 4.
4. **Both display stores from one Pass B dispatch** instead of two.

---

## 1. Sparse coefficient transfer

2048 tiles, 4:2:0, `res_level` 0, the shape the headset streams. Traffic is
what Pass A writes and Pass B reads; device traffic is about twice it.

| QP | payload | dense | sparse | ratio |
|---|---|---|---|---|
| 63 | 0.102 MB | 25.6 MB | **0.87 MB** | 29x |
| 51 | 0.115 MB | 25.6 MB | **0.88 MB** | 29x |
| 36 | 0.155 MB | 25.6 MB | **0.93 MB** | 27x |
| 24 | 1.424 MB | 25.6 MB | **11.6 MB** | 2.2x |
| 12 | 2.869 MB | 25.6 MB | **13.6 MB** | 1.9x |

The floor is the length words themselves: 66 uints per tile, 0.54 MB per
frame. Read the exact figure back with `NXVC_VKD_FLAG_COEF_STATS`; turn the
whole thing off with `NXVC_VKD_FLAG_DENSE_COEF`.

### Time, RADV, sparse against dense with everything else equal

Best of 30, both layouts in the same process, three runs:

| QP | Pass A dense → sparse | Pass B dense → sparse |
|---|---|---|
| 12 | 1.57 → 1.44 ms | 1.33 → 1.28 ms |
| 24 | 1.28 → 1.26 ms | 1.13 → 1.19 ms |
| 36 | 0.56 → 0.54 ms | 1.14 → 1.20 ms |

**The bytes bought no time on a discrete GPU**, and cost about 5 % of Pass B
where most units are coded. That is the honest result and it is not a
surprise once stated: 960 GB/s was never the constraint on a 7900 XTX, and
checking every coefficient against its unit's length is real work. lavapipe
agrees more strongly — at QP 12, where almost every unit is coded and the
coefficient fetch is an L2 hit whichever layout it is, sparse costs 15 % or
more.

What the layout *did* buy is section 2.

---

## 2. The fast paths, and Pass B stops being fixed-cost

Full decode, RADV, best of 30, `nxvc_encoder` defaults (`INTRA_DIR` +
`CTX_V2` + `SIGN_HIDE` all on), before against after:

| QP | frame | coef SSBO | Pass A | Pass B | GPU total |
|---|---|---|---|---|---|
| 12 | 2.64 MB | 25.6 → 13.6 MB | 1.63 → 1.44 ms | 1.90 → **1.28 ms** | 3.53 → **2.72 ms** |
| 24 | 1.32 MB | 25.6 → 11.6 MB | 1.12 → 1.26 ms | 1.77 → **1.19 ms** | 2.89 → **2.46 ms** |
| 36 | 0.17 MB | 25.6 → 0.93 MB | 0.34 → 0.54 ms | 1.63 → **1.20 ms** | 1.99 → **1.75 ms** |

The QP ladder is where the shape of the change shows. RADV, best of 30,
after:

| QP | payload | coef SSBO | Pass A | Pass B |
|---|---|---|---|---|
| 63 | 0.102 MB | 0.87 MB | 0.29 ms | **0.20 ms** |
| 51 | 0.115 MB | 0.88 MB | 0.31 ms | **0.20 ms** |
| 36 | 0.155 MB | 0.93 MB | 0.50 ms | 1.09 ms |
| 24 | 1.424 MB | 11.6 MB | 1.31 ms | 1.18 ms |
| 12 | 2.869 MB | 13.6 MB | 1.94 ms | 1.62 ms |

Before, that column read 1.76 / 1.81 / 1.64 / 1.77 / 1.72 ms — flat to within
3 % over a 28x range of payload.

| | fixed, per tile | slope, per MB |
|---|---|---|
| Pass A, RADV, dense | 121 ns | 0.54 ms |
| Pass A, RADV, sparse | **170 ns** | 0.48 ms |
| Pass B, RADV, dense | 890 ns | ~0 |
| Pass B, RADV, sparse | **248 ns** | 0.33 ms |
| Pass A, lavapipe, dense | 19.4 µs | 84.6 ms |
| Pass A, lavapipe, sparse | **12.6 µs** | 56.6 ms |
| Pass B, lavapipe, dense | 148 µs | ~0 |
| Pass B, lavapipe, sparse | **49 µs** | 71 ms |

Pass B no longer fits a line, so read those as a summary rather than a model.
The step between QP 51 and QP 36 is not bandwidth: it is the mode-0 fast
path. At QP 51 and above the encoder's directional modes are almost all mode
0, so the plane takes the parallel path and the wavefront does not run at
all; below that it does.

**Pass A got more expensive** at low rates, 124 → 170 ns per tile of fixed
cost. Part is the length words and their atomicOr; most is the loss of the
zeroing loop, which used to write the coefficient region in whole cache lines
and so warmed it for the scattered stores that followed. Pass B gained 642 ns
per tile against it, and Pass A is now the larger of the two passes at every
QP above 24 — which is the most useful thing this package found.

---

## 3. Wavefront occupancy: 16 threads per 8x8 block

`vk/decoder/README.md` had this in "Open issues" and called it free. What kept
it at 4 was that the residual lived in the four threads' registers. It does
not have to: a block's own 8x8 region of the shared sample store holds its
residual until its step and its reconstruction afterwards, and `dirAt()` never
reads the region of a block that is not done — it reads `base`. So the column
pass stages the residual in the sample store (one extra barrier per plane, no
extra memory) and `dirBlockOfStep()`, the arithmetic inverse of
`dirStepOf()`, enumerates a step's blocks so each gets 16 threads.

2048 tiles, 4:2:0, QP 24, Pass B only. RADV is the median of three runs at
best-of-30; lavapipe is a range across two runs at best-of-3 and best-of-6.

| schedule | steps | barriers/tile | occupancy | rate | Pass B, RADV | Pass B, lavapipe |
|---|---|---|---|---|---|---|
| `INTRA_DIR` off | — | 23 | 100 % | — | 0.255 → **0.241 ms** | 90 → 112 ms |
| 0 — as written | 22 | 62 → 65 | 4.5 → **18.2 %** | — | 1.780 → **1.183 ms** | 252 → 261–291 ms |
| 1 — no above-right | 15 | 49 → 52 | 6.7 → **26.7 %** | +0.24 % | 1.310 → **0.885 ms** | 229 → 244–266 ms |
| 3 — + 32x32 sub-tiles | 7 | 41 → 44 | 14.3 → **57.1 %** | +1.8 % | 1.041 → **0.732 ms** | 246 → 200–289 ms |

−34 %, −32 %, −30 %. Directional intra costs 4.9x on Pass B rather than 7.3x.

lavapipe says nothing here, and that is the expected answer: llvmpipe's
workgroups are CPU tasks with no occupancy to gain and no barrier cost to
lose, so a change that is purely about occupancy should move RADV and not
lavapipe. It is the reason to believe the RADV column.

RADV shader statistics (`RADV_DEBUG=shaderstats`, RDNA3, wave64):

| | Pass A | Pass B, 4:4:4 store | Pass B, two-plane 4:2:0 |
|---|---|---|---|
| SGPRs / VGPRs | 108 / 60 | 108 / 136 (was 144) | 108 / 84 |
| spilled | 0 / 0 | **0 / 0** | 0 / 0 |
| LDS | 12.0 KB (was 10.2) | 25.8 KB | 13.3 KB |

The VGPR fall is `predictCols()` (two columns, `P0[8]` and `P1[8]`) becoming
`predictOne()` (one sample). Pass A's LDS growth is the per-tile length words
it accumulates before flushing them once.

---

## 4. Skip tiles

A unit whose published length is 0 coded nothing, so its dequantize and both
passes of its 8x8 IDCT are skipped; a DC unit of length 0 also skips the
second-level IDCT and its two barriers. A tile with no coded unit therefore
runs **no transform stage at all**. That is the shape `WARP_SKIP` and
`STATIC_MV` will have when the inter hook lands — the `bool intra` derivation
in `reconstruct.comp` marked `INTER HOOK` is where the mode test goes, and the
coefficient short-circuit it wanted is already there — and it is already the
shape of a static region under rolling refresh.

The effect is not separable from section 2's numbers by construction (the
skip *is* what makes the QP ladder slope), but the QP 63 row isolates it: a
frame whose units are almost all uncoded reconstructs in 0.20 ms of Pass B
against 1.76 ms before, a factor of 8.8.

---

## 5. Two display stores from one dispatch

A frame needing two display formats used to reconstruct every tile twice.
That is the shape a 4:2:0 stream with a coded alpha plane already has — the
two-plane store has nowhere to put alpha — and the shape the **reference ring
slot** will have when the inter path lands, because the slot and the display
image are two stores of the same samples. Specialization constant 3,
`kOutSecond`, does both from one dispatch and defaults to `kOutNone`, so a
one-store pipeline compiles to exactly the kernel it did before.

2048 tiles, 4:2:0 with a coded alpha plane, QP 24, Pass B only:

| | RADV | lavapipe |
|---|---|---|
| two dispatches (ycbcr420, then rgba8) | 2.271 ms | 473–667 ms |
| one dispatch, both stores | **1.685 ms** | **246–257 ms** |
| | **−26 %** | **−46 to −63 %** |

Cheaper on both, by about what the second reconstruction costs minus the
second store's bandwidth — and much more than that on lavapipe, where the
reconstruction is the whole cost and the store is nearly free.
`NXVC_VKD_FLAG_SPLIT_STORES` keeps the two-dispatch path for measurement.

---

## 6. The Adreno 650 estimate

**No Adreno 650 was measured.** Assumptions, stated because they do all the
work:

* **A1** — compute and serialisation scale by **20–30x** from the 7900 XTX,
  the range the brief gives. It is a throughput ratio applied to a kernel
  whose problem is serialisation, so it is a floor, not a bracket.
* **A2** — memory is **25 GB/s, shared** (PAPER 3.1). A 7900 XTX has 960, so
  bandwidth invisible on RADV is 38x more expensive there.
* **A3** — the frame is 2 x 2048x2048 at 4:2:0, 2048 tiles, 90 Hz: **11.1 ms**.
* **A4** — lavapipe bounds nothing, but it separates two things RADV cannot.
  Where a change helps on RADV and not on lavapipe it was occupancy; where it
  helps on both it moved work or bytes.

### The two terms

Bandwidth per frame at 25 GB/s, QP 24, two-plane 4:2:0 store:

| | dense | sparse |
|---|---|---|
| bitstream read | 1.3 MB | 1.3 MB |
| coefficient write + read | 51.2 MB | **23.2 MB** |
| lengths, modes, tile records | 0.4 MB | 1.4 MB |
| output write | 12.6 MB | 12.6 MB |
| **total** | 65.5 MB = **2.6 ms** | 38.5 MB = **1.5 ms** |

At QP 36 the sparse total is 15.3 MB = 0.6 ms against the same 2.6 ms dense.

Scaled compute, Pass B only, QP 24:

| schedule | RADV before → after | at 20x | at 30x |
|---|---|---|---|
| `INTRA_DIR` off | 0.255 → 0.241 ms | 5.1 → **4.8 ms** | 7.7 → **7.2 ms** |
| 0 | 1.780 → 1.183 ms | 35.6 → **23.7 ms** | 53.4 → **35.5 ms** |
| 1 | 1.310 → 0.885 ms | 26.2 → **17.7 ms** | 39.3 → **26.6 ms** |
| 3 | 1.041 → 0.732 ms | 20.8 → **14.6 ms** | 31.2 → **22.0 ms** |

and Pass A:

| QP | RADV before → after | at 20x | at 30x |
|---|---|---|---|
| 36 | 0.34 → 0.54 ms | 6.8 → 10.8 ms | 10.2 → 16.2 ms |
| 24 | 1.12 → 1.26 ms | 22.4 → 25.2 ms | 33.6 → 37.8 ms |

### What it says

* **The compute term dominates the bandwidth term by an order of magnitude in
  every row.** ADR 0025 point 4 called sparse transfer "the larger lever" and
  it is not: even dense, the whole memory system is 2.6 ms of an 11.1 ms
  budget, against a wavefront at 15–36 ms. The sparse layout's real
  contribution was the per-unit length, which is a *compute* saving that
  happens to arrive with a bandwidth saving attached.
* **A v1 stream (`INTRA_DIR` off) is close, and Pass A is now the problem.**
  At QP 36: Pass B 4.8–7.2 ms plus Pass A 6.8–16.2 ms is 11.6–23.4 ms against
  11.1. That is a different problem from a version ago and a more tractable
  one — Pass A's three `barrier()`s per scheduling round are an implementation
  choice, not a syntax property.
* **`INTRA_DIR` still does not fit that part.** The cheapest schedule is
  14.6 ms of Pass B alone at the optimistic end. It was 4–8x over budget and
  is now 2–4x over. Tool bit 17 stays a negotiated desktop-decoder tool
  exactly as ADR 0025 decided.
* **Sparse is still worth having there**, not as the headline lever but
  because 23.2 MB against 51.2 MB is 1.1 ms of a memory system the display
  controller and the reprojection shader also use — and unlike the compute
  terms it is a measured ratio rather than a scaled one.
* **What to measure first on a real device**, in order: Pass A with
  `INTRA_DIR` off at QP 36 (the number the Pico 4 stream depends on), the
  tile-sort delta (the one knob whose sign is unknown), and sparse against
  dense, which A2 says should flip sign from the −5 % it costs on RADV.

---

## 7. Conformance

Run after every step, on both ICDs:

| test | RADV | lavapipe |
|---|---|---|
| `vk.passA.*` (7 tests, both read-pointer modes, both layouts, subgroup 32 and 64) | pass | pass |
| `vk.passB.*` (54 cases, GPU against the CPU model) | 0 mismatching pixels | 0 |
| `vk.decoder.conformance` (152 streams, 23 skipped) | **0 mismatching samples** | **0** |
| `vk.decoder.cli` (`nxvc-vkdec` vs `nxv-dec`, byte for byte) | pass | — |
| whole `^vk\.` suite | 28/28 | 28/28 |

Two checks are new and worth naming, because they are what makes "Pass A
writes exactly these slots and no others" a fact rather than a hope:

* `nxvc-passA-test` runs GPU-against-model in **both** layouts and seeds the
  coefficient buffer, on the GPU and in the model alike, with `0x5555`. Under
  the sparse layout Pass A no longer zeroes the region, so any slot it writes
  that the model does not — or the reverse — shows up as a mismatch;
* `nxvc-passB-test` builds scan-order scenes with the same sentinel past every
  `LAST`, so a Pass B that reads past a published length fails.

The CPU models carry the same `sparse` switch as the kernels and track them
line for line. `passB_model.cpp` deliberately does **not** carry the mode-0
fast path: leaving the model on the general path makes `vk.passB.*` a test of
the claim that the two are bit-identical, rather than an assumption of it.

---

## 8. Method

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=ON \
      -DNXVC_VK_HEADERS_DIR=/path/to/vulkan-sdk/include
cmake --build build-ref -j4
chrt -i 0 taskset -c 20-23 nice -n 19 ./build-ref/bin/test_vk_decoder_conformance --bench 30
VK_DRIVER_FILES=$NXQ_SCRATCH/warp/icd/lvp_icd.x86_64.json NXVC_VKD_DEVICE=llvmpipe \
  chrt -i 0 taskset -c 20-23 nice -n 19 ./build-ref/bin/test_vk_decoder_conformance --bench 6
```

Every process ran under `chrt -i 0 taskset -c 20-23 nice -n 19`, `-j4`. The
"before" column is commit `e4e85af` built and benched the same way, from a
clean tree under `$NXQ_SCRATCH/tourney-sparse/base`.

**Devices.** AMD Radeon RX 7900 XTX (RADV NAVI31, Mesa 25.x, wave64) and
llvmpipe (LLVM 21, 256-bit) **pinned to four cores**. The four-core pin makes
lavapipe about twice the figures `vk/decoder/README.md` used to quote; before
and after were measured identically, so the deltas hold, but the absolute
lavapipe numbers here are not comparable to older ones.

**Noise.** RADV's run-to-run spread at best-of-30 is about ±10 % and every
RADV figure quoted as a single number is the median of three runs. lavapipe's
spread at best-of-3 and best-of-6 is about ±20 %, which is why its figures are
ranges and why nothing is concluded from it that is smaller than 25 %.

Raw harness output: `$NXQ_SCRATCH/tourney-sparse/{before,after}-{radv,lvp,lvp6}.txt`.

**No compression measurement was run**, and none is applicable: this package
changes no syntax and produces byte-identical output, so BD-rate against any
anchor is unchanged by definition and the conformance suite is what proves it.
