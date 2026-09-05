# XFORM_FAST: a multiply-free 8x8 transform, and what it is worth

Branch `exp/xform-fast`. A **decoder-cost** package: it adds one stream tool
bit (28), replaces the 8x8 transform behind it with one that contains no
multiply at all, and pays for that in rate. Whether the trade is worth taking
depends entirely on the device, and the answer this document reaches is
**no for the shipped targets, and only conditionally yes for a Pico profile**.

Everything below is measured. Method, hardware and the raw harness output are
at the end.

---

## 0. The result, in one table

| | Loeffler 8x8 (6.1-6.3) | XFORM_FAST (6.7) |
|---|---|---|
| multiplies per 8x8 inverse transform | 288 | **0** |
| multiplies per 8x8, dequantizer included | 416 | **192** |
| adds per 8x8, dequantizer included | 736 | 704 |
| shifts per 8x8, dequantizer included | 320 | 416 |
| BD-rate, 4:4:4, PSNR-Y | — | **+3.65 %** |
| BD-rate, 4:2:0, PSNR-Y | — | **+3.57 %** |
| BD-rate, 4:4:4 / 4:2:0, SSIM-Y | — | +2.32 % / +2.26 % |
| Pass B, 2048 tiles, RADV (RX 7900 XTX) | 0.430 ms | **0.413 ms** (−4.0 %) |
| Pass B, 512 tiles, lavapipe (2 cores) | 47.8 ms | **54.5 ms** (+13.9 %) |
| forward-inverse round trip, RMS / max | 0.347 / 2 | **0.276 / 1** |
| transform coding gain lost vs the true DCT | 0.000 dB | 0.043 dB |
| conformance mismatches, RADV and lavapipe | — | **0** |

The transform is multiply-free, more accurate than the Loeffler pair at zero
quantisation, and costs 3.6 % of rate. On the two devices available it buys
4 % of Pass B on a discrete GPU and *loses* 14 % on a software rasteriser.

---

## 1. What the tool is

`docs/SYNTAX.md` 6.7 is the normative description; this is the shape of it.

The flow graph is the **H.264/AVC High-profile 8x8 butterfly** (H.264
8.5.13.2): 32 adds and 10 shifts per 8-point 1D transform, and not one
multiply. It was picked over a binDCT lifting structure for three reasons: it
is exactly orthogonal (the Gram matrix of its columns is diagonal with no
residue at all, which a lifting structure does not give you for free), it is
the most deployed integer transform in existence and therefore the best
understood, and its basis is within 0.032 of the true DCT basis in normalised
terms, which costs 0.043 dB of coding gain on an AR(1) source at rho = 0.95.

It is **not orthonormal**: the squared column norms are 8 (k = 0, 4), 289/32
(k odd) and 5 (k = 2, 6). The 2D gain at position `(u, v)` is `g_u g_v`, which
factorises, so the correction is a per-position scale that goes into the
**dequantizer**, not into the transform:

```
t = min((qstep[qp] * w[i] * xfs[i] + 8192) >> 14, 63744)
c = clamp16((q * t + 8) >> 4)                       // unchanged from 6.5
```

with `xfs[i] = round(1024 * 8 / (g_u g_v))`, six distinct values between 907
and 1638. This is free: the step table is built once per plane and the
per-coefficient work is the one multiply and one shift 6.5 already specifies.
It is also the reason the quantiser interface did not have to change — a
weighting matrix, a QP and a level mean exactly what they meant before.

Two details are worth stating because they are where a reimplementation would
go wrong:

* **The numerator is 8, and it is 8 because of an int32 bound.** The scale
  wants to be `K^2/(g_u g_v)` for the largest power of two `K^2` that keeps
  `q * t` inside int32 at `qstep[63]`, `w = 32` and the largest `xfs`. 8 is
  that power; 16 would need `t` up to 148000 and `q * t` up to 4.9e9. With 8
  the unclamped worst case is 74126, still outside, which is what the clamp to
  63744 is for — it binds for exactly **7 of the 12288** `(qp, w, position)`
  combinations, all at QP 62-63 with a weight of 28 or more at the four
  positions in `{2,6} x {2,6}`, and is the identity everywhere else.
  `ref.transform` asserts both the count and the bound.
* **The shift chain is `<< 3` then `>> 6`,** not a symmetric pair. The row
  pass shifts in by 3 and does not shift back, so the transpose buffer carries
  three fractional bits — the same trick 6.3's 7/13 split uses. It is still
  clamped to int16, so a GPU may still keep it in `int16` LDS.

There is **no `mulC4` here.** 6.3's two-word identity exists because the
Loeffler odd-part rotation leaves int32 on legal input; a graph with no
products cannot. The worst case anywhere on the fast path is 1.93e6.

The tool replaces the 8x8 transform **everywhere the stream uses one** —
residual blocks and the DC plane's second level — so a decoder that implements
it needs one transform, not two. That is the whole point.

---

## 2. Operation counts

Per 8x8 block, counting the inverse transform as the reference and the shader
implement it (16 1D transforms, the dequantizer, and the rounding and clamping
stages between the passes). `mulC4` is counted as the two products it actually
performs.

| | multiplies | adds | shifts | clamps |
|---|---|---|---|---|
| Loeffler 1D (x16) | 288 | 480 | 64 | — |
| XFORM_FAST 1D (x16) | **0** | 512 | 160 | — |
| dequantizer, Loeffler (64 coefficients) | 128 | 128 | 128 | 64 |
| dequantizer, XFORM_FAST | 192 | 128 | 128 | 64 |
| pass shifts + input shift | — | 128 / 64 | 128 / 128 | 128 |
| **Loeffler, total** | **416** | **736** | **320** | 192 |
| **XFORM_FAST, total** | **192** | **704** | **416** | 192 |

Read that honestly. The *transform* is multiply-free; the *path* is not,
because the dequantizer still multiplies a level by a step, and the fold costs
it one extra multiply per coefficient (`qstep * w * xfs` instead of
`qstep * w`). That extra multiply is avoidable — the product `w * xfs[i]` is
position-only and could be hoisted into a per-plane step table, or folded into
the weighting matrix the host uploads, which is exact by associativity — and
was left in place here because it keeps the shader a line-for-line match of
`ref/` and because it is 64 multiplies out of 192, not the difference between
the two devices below.

The net is **2.2x fewer multiplies**, 4 % fewer adds, and 30 % more shifts.

---

## 3. Rate

`tools/quality/compare.py`, `vr-mixed-1024-v2` truncated to 12 frames,
2048x1024 side-by-side, QP 16/20/24/28/32 against `x264-intra` at QP
24/28/32/36/40. Both columns are the same encoder, the same RD trellis and the
same shipped defaults (directional intra, 16 contexts, sign data hiding); the
only difference is the tool bit.

**Direct BD-rate, XFORM_FAST against the Loeffler ladder:**

| sequence | BD-rate PSNR-Y | BD-rate SSIM-Y | BD-PSNR-Y |
|---|---|---|---|
| vr-mixed-1024-v2, 4:4:4 | **+3.65 %** | +2.32 % | −0.376 dB |
| vr-mixed-1024-v2, 4:2:0 | **+3.57 %** | +2.26 % | −0.404 dB |

**Against the Phase 1 anchor**, which is how the gate is quoted elsewhere:

| sequence | Loeffler | XFORM_FAST | delta |
|---|---|---|---|
| 4:4:4, BD-rate vs x264-intra, PSNR-Y | +114.98 % | +124.20 % | +9.22 pp |
| 4:2:0, BD-rate vs x264-intra, PSNR-Y | +101.56 % | +110.39 % | +8.84 pp |

**Point by point**, which is where the shape of the cost shows:

| QP | 4:4:4 rate | 4:4:4 PSNR-Y | 4:2:0 rate | 4:2:0 PSNR-Y |
|---|---|---|---|---|
| 16 | +2.30 % | −0.263 dB | +2.45 % | −0.224 dB |
| 20 | +2.14 % | −0.230 dB | +2.81 % | −0.179 dB |
| 24 | +0.53 % | −0.299 dB | +1.00 % | −0.296 dB |
| 28 | +1.31 % | −0.175 dB | +1.47 % | −0.161 dB |
| 32 | +1.76 % | −0.083 dB | +1.13 % | −0.076 dB |

The cost is a **basis cost, not a precision cost**, and the numbers say so
three ways. The round trip at zero quantisation is *better* than the Loeffler
pair (RMS 0.276 against 0.347, maximum 1 against 2), so the transform is not
losing accuracy. The coding-gain loss against the true DCT is 0.043 dB, which
is about 0.7 % of rate at 6 dB per bit — the same order as the measured 2 %
rate delta at matched QP. And the loss does not grow at low QP, which is what
a precision problem would do. What is left is that the AVC basis is a slightly
worse decorrelator than the Loeffler one on this content, and the encoder pays
for it in bits at every operating point.

The RD trellis was given the per-position distortion weights the fold
requires (`(1024/xfs[i])^2` on the squared error, since the coefficients now
live on a per-position scale). Without them the trellis is wrong by up to 2.6x
between the cheapest and the dearest position; with them the measured cost
above is what is left.

---

## 4. Decoder cost

Pass B only, as a specialization-constant variant of `reconstruct.comp`
(constant 5, `kXformFast`). Same kernel, same thread mapping, same barriers;
`nxvc-passB-test --bench` runs both pipelines back to back in one process, so
the two columns of a row share a device state.

### RADV, RX 7900 XTX (NAVI31), 2048 tiles (2048x2048 luma), 4:2:0, res_level 0

| | Loeffler | XFORM_FAST | delta |
|---|---|---|---|
| steady state (6 of 8 runs, identical) | 0.430 ms | **0.413 ms** | **−4.0 %** |
| best of 8 | 0.282 ms | 0.286 ms | +1.4 % |

The box was shared with other work during this measurement and the first runs
of each series show clock ramping; the steady-state figure is the one to
believe, and it is −4.0 % on six consecutive runs that agreed to the
microsecond. **Pass B on a discrete GPU is not transform-bound.** The frame is
56 MB of traffic at 129 GB/s effective, and the 224 multiplies per block this
saves, out of a kernel that also does a bilinear prediction, a resample and a
colour conversion for every pixel, is not where the time goes.

### lavapipe (llvmpipe, LLVM 21, 2 cores), 512 tiles

| | Loeffler | XFORM_FAST | delta |
|---|---|---|---|
| best of 3 | 47.8 ms | 54.5 ms | **+13.9 %** |
| the other two | 58.9 / 60.9 ms | 65.6 / 62.6 ms | +11.4 % / +2.8 % |

**Consistently slower, and the reason is instructive.** llvmpipe vectorises
the kernel to AVX2, where a 32-bit integer multiply and a 32-bit integer add
are both one instruction on one port. Trading 224 multiplies for 112 extra
shifts and roughly the same number of adds is a straight loss on a machine
where multiply is not special. A multiply-free transform is only fast on a
machine that charges extra for multiplies.

That is exactly the Adreno case the tool was proposed for — `passB/README.md`
records `K3 idct` at 11 ms for a 2048x4096 frame on the Pico, where integer
multiply throughput is a quarter of add throughput — but **this branch could
not measure a Pico**, and the two devices it could measure disagree in sign.
That is the central weakness of the result and it is stated here rather than
buried: the case for the tool rests on a device nobody in this experiment
benchmarked.

### Conformance

`nxvc-passB-test` grew 29 XFORM_FAST cases covering both chroma formats, both
colour paths, QP 0/13/24/47/63, every `res_level`, transform skip on and off
(which bypasses the transform and must therefore be unaffected), a coded alpha
plane, and saturating coefficients at QP 63 with matrix 3, which is where the
step clamp and the transpose clamp both bind.

| device | cases | mismatching pixels |
|---|---|---|
| RADV (RX 7900 XTX) | 83 | **0** |
| lavapipe (llvmpipe) | 80 | **0** |

lavapipe runs three fewer because the 4:4:4-plus-alpha configurations exceed
its 32 KB shared-memory limit, which is the harness's existing skip.

The **whole-decoder** conformance harness agrees, which is the check that
matters most: it decodes every committed vector and 73 synthetic encoder
streams on the GPU and compares the result against the reference decoder's
MD5, so the four `v5x_xfast*` vectors and nine new `syn_xfast_*` streams are
checked end to end, entropy decode included.

| device | streams checked | failures |
|---|---|---|
| RADV (RX 7900 XTX) | 168 | **0** |
| lavapipe (llvmpipe) | 168 | **0** |

Tool bit 28 had to be added to the harness's `kPhase1Tools` for this: the
Vulkan decoder implements the tool, so its streams are decoded and compared
rather than counted as a refusal.

On the CPU side the reference grew four conformance vectors
(`v57_xfast420_qp24`, `v58_xfast444_qp16`, `v59_xfast_default444`,
`v60_xfast_qp63_wm2`) and one rejection vector
(`r30_lossless_xform_fast`, `LOSSLESS` and `XFORM_FAST` together, which is
`BITSTREAM` because a lossless stream runs no transform). Every pre-existing
vector's MD5 is byte-identical: the manifest diff is four added lines and one
added rejection line, nothing else. `ctest`: 53/53. The Python bindings grew
the bit, the exclusion and the config field, and their suite is 470/470 —
which also means `nxvc.bitstream` parses the new vectors' headers
independently of the C code.

---

## 5. Recommendation

**Do not take it for the shipped profiles, and do not take it for a Pico
profile on this evidence either — but keep the branch.**

The case against, in order of weight:

1. **3.6 % of rate is expensive for this codec.** The Phase 1 gate is already
   at +101 % to +115 % against x264-intra; spending another 3.6 % to save
   0.017 ms of Pass B on the only real GPU that could be measured is not a
   trade anyone would take.
2. **The two devices disagree in sign.** −4 % on RADV, +14 % on lavapipe. A
   tool whose benefit inverts between a discrete GPU and a software rasteriser
   needs the target device on the bench before it is a decision, and the
   target device was not available here.
3. **Pass B is not transform-bound where it was measured.** On RADV the
   wavefront of directional intra costs 3-4x what the whole transform does
   (`passB/README.md`: 0.24 ms to 1.18 ms when `INTRA_DIR` turns on). Removing
   every multiply from the transform cannot move a number the transform does
   not dominate.

The case for keeping it:

1. **It is cheap to carry.** One tool bit, one specialization constant, one
   `if` in three places in the kernel, and a 64-entry table. It changes no
   existing stream and no existing vector, and the default is off.
2. **The transform itself is not the problem.** It is more accurate than the
   Loeffler pair at zero quantisation and loses 0.043 dB of coding gain; the
   3.6 % is the AVC basis being a slightly worse decorrelator, not any defect
   in the integer design. If a smaller-BD-rate multiply-free basis is wanted,
   the place to look is a binDCT lifting structure with norms closer together,
   not a repair of this one.
3. **The Pico number is the missing measurement, and it is a cheap one to
   take.** `nxvc-passB-test` builds for Android already
   (`build-vkdec-android`). If the Adreno 650 shows the 2-3x multiply penalty
   the ISA documents, this tool turns Pass B's transform stage into
   approximately free and 3.6 % of rate becomes arguable for a headset-only
   profile. If it shows what lavapipe showed, the branch should be closed.

**The one thing to do next** is run `nxvc-passB-test --bench` on a Pico 4.
Until that number exists this is an experiment with a clean implementation and
an unproven premise.

---

## 6. Method

```
worktree   exp/xform-fast off main @ a41f9e8
build      cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNXWARP_BUILD_VK=ON
           chrt -i 0 taskset -c 16-17 nice -n 19, -j2
sequence   $NXQ_SCRATCH/seq/vr-mixed-1024-v2.{yuv444p,yuv420p}.yuv truncated
           to 12 frames (a truncated file, not compare.py --frames)
rate       compare.py --anchors x264-intra --qp 16,20,24,28,32
                      --anchor-qp 24,28,32,36,40 --no-vmaf
BD-rate    nxq.bdrate.bd_rate, cubic, Loeffler ladder as the anchor
GPU        RADV NAVI31 (RX 7900 XTX); lavapipe via
           VK_DRIVER_FILES=.../warp/icd/lvp_icd.x86_64.json
           headers -DNXVW_VK_INCLUDE=.../tools/local/include
```

The box was running other agents' builds throughout, which is why the RADV
timings are quoted as a steady state over consecutive identical runs rather
than as a single best-of number, and why the lavapipe series is quoted with
all three runs shown.

### What changed

```
include/nxvc/nxvc.h              tool bit 28, cfg.xform_fast (appended, ABI safe)
ref/src/transform.{h,cpp}        idct8x8_fast, fdct8x8_fast, kXfsScale, kXfsFwdScale
ref/src/codec.cpp                dequant_step_x, the four transform call sites,
                                 per-position RD distortion weights
ref/src/codec_impl.inc           the tool bit, both directions; LOSSLESS exclusion
ref/tools/nxv-enc.cpp            --xform-fast
tests/ref/test_transform.cpp     basis, orthogonality, norms, gain, round trip,
                                 the step clamp, the scale-1024 identity
tests/ref/vectors.cpp            v57-v60 and r30
vk/decoder/passB/syntax_constants.h  the shared constants and the table
vk/decoder/passB/reconstruct.comp    specialization constant 5, idct8_1d_fast,
                                     idctPass1/idctPass2, dequantStepX
vk/decoder/passB/passB_model.{h,cpp} the same, in the CPU oracle
vk/decoder/passB/tools/nxvc-passB-test.cpp  29 cases, and both benches
vk/decoder/nxvc_vkdec{,_parse}.cpp   the tool bit, and the pipeline key
tests/vk-decoder/conformance/...     bit 28 in kPhase1Tools, 9 synthetic streams
python/src/nxvc/{_ffi,bitstream}.py  the bit, RESERVED_MASK, the exclusion
docs/SYNTAX.md                   2.3, 6.7, Appendix A 53 and 54
vk/decoder/passB/README.md       the variant
```
