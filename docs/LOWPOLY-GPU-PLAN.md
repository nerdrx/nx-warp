# The piecewise-planar tile mode on a GPU decoder

**Status:** a plan. Nothing here is implemented. The reference codec implements
the mode (SYNTAX.md 13.13, `nxvc_config::planar`, `nxv-enc --planar`); this
document says what a Vulkan decoder would do with it and what the Adreno 650 in
a Pico 4 permits, so that the syntax that shipped is one a kernel can actually
run.

The syntax was written against these constraints rather than measured against
them afterwards, which is the same order `int_rdoq` was done in: the tool that
crosses is the one whose *arithmetic* was chosen to cross.

## 1. What the mode asks of a decoder

Per coded colour plane of edge `size`, per sample:

```
r     = label[(y >> shift) * M + (x >> shift)]
value = dc_offset + d0[r] + ((dh[r] * (2*x - size + 1)) >> log2(size))
                          + ((dv[r] * (2*y - size + 1)) >> log2(size))
sample = clamp(value, 0, maxval)
```

with `d0/dh/dv` dequantised once per region per plane by
`dequant(c, dequant_step(dc_qp_of(qp), 16))`.

Four properties matter, and all four are deliberate:

1. **Integer.** No float appears anywhere in the reconstruction, so a device
   and a host libm cannot disagree — the property ADR 0028 and `int_rdoq` were
   both built around.
2. **No dependency between samples.** Not raster order, not a scan, not a
   neighbour. Any lane may compute any sample.
3. **No dependency between tiles.** A planar tile predicts from nothing.
4. **No entropy decode.** The body is raw bytes: a header byte, a bit-packed
   label map, and `3 * R * planes` signed bytes.

(4) is why the label map is raw in this version even though a context-coded map
measures about four times smaller (LOWPOLY-MODE.md 3.2). A compressed map puts
a serial decode in front of a reconstruction whose entire argument is that it
has none. If it is ever added it should be a second tool bit, so that a decoder
can accept the mode and decline the compressed map.

## 2. The kernel

One workgroup per planar tile, in the decoder's Pass B slot. It replaces Pass A
(entropy) and Pass B (transform + prediction) for that tile with a single pass.

```
layout(local_size_x = 32) in;              // TPG <= 32, see 3
push constants (<= 128 B):
    uint  tile_base;      // byte offset of this tile's body in the payload SSBO
    uint  plane_geom;     // size and log2(size) for luma and chroma, packed
    int   qstep_luma, qstep_chroma;   // dequant_step(dc_qp_of(qp), 16)
    uint  dc_off_maxval;  // packed 2 x u16
    uint  flags;          // chroma format, plane count
```

Per workgroup:

1. **Lane 0..0 reads the header byte**, publishes `R`, `M` and `shift` through
   shared memory (a single `barrier()`); every lane reads them.
2. **The map goes to shared memory as bytes.** 16 or 64 bytes at
   `granularity == 1`, 8 or 16 at 0 — 64 bytes worst case, which is nothing.
   Unpacking is per lane, one byte each, no scan.
3. **The coefficients are dequantised into shared memory**: `3 * R * planes`
   bytes in, at most 4 x 3 x 3 = 36 `int`s out. One lane per (region, plane,
   term) — 36 lanes at most, so a 32-lane group does it in two rounds.
4. **`barrier()`.** The only one in the kernel.
5. **Every lane walks its share of the samples**: 4096 luma samples over 32
   lanes is 128 samples a lane, each a shared-memory label read, three
   multiply-adds and a clamp. Chroma is 1024 samples per plane at 4:2:0.

No subgroup operations. No scan. No atomics. No second barrier. The whole tile
is one dispatch's worth of arithmetic with a 64-byte shared array and a 36-int
one.

## 3. The Adreno 650 constraints this was written against

| constraint | what it forces |
|---|---|
| **push constants ≤ 128 B** | the tile's own parameters must fit, or be read from an SSBO. The block above is 24 bytes; everything per-tile that varies (the QP, the geometry) is packed rather than passed per plane |
| **no subgroup scans** (`subgroupInclusiveAdd` and friends are unreliable across the vendor's driver versions; vk/decoder/README.md records the same finding for Pass A) | the mode must have nothing to scan. It does not: there is no prefix, no run length, no variable-length field inside the body. The label map is fixed-width and indexed, which is what makes this true |
| **threads per group ≤ 32 for the decoder's existing passes** | the kernel must be correct at 32 lanes and must not assume a wave width. It is: the sample loop is a strided `for` over `gl_LocalInvocationIndex`, and correctness does not depend on how many lanes there are |
| shared memory is precious next to Pass A's tables | 100 bytes total here, against Pass A's kilobytes |
| 16-bit storage is available but not free | the map is bytes and the coefficients are bytes; both are read as `uint` words and shifted, so the kernel needs no 8-bit or 16-bit storage extension at all |

**The thing that would break it** is a per-sample variable-length field, and
the syntax has none. That is the single design constraint worth restating for
anyone extending the mode: a per-region line refinement (LOWPOLY-MODE.md 5) is
fine because it is fixed-width and per region; a context-coded map is not,
without its own tool bit and its own pass.

## 4. Where it sits in the frame

The decoder already dispatches per tile with a mode in the tile record. A
planar tile:

* is **skipped by Pass A** entirely — it has no rANS payload, so it consumes no
  lane, no round and no table;
* is **skipped by Pass B's transform path** — no coefficients, no IDCT;
* runs this kernel instead, writing the same reconstruction buffer Pass B
  writes, in the same layout;
* contributes to the reference picture like any other coded tile.

So the integration is a third branch in the existing per-tile dispatch, not a
new stage. The expected cost is **below any other coded tile and above a skip**
— an argument, not a measurement, and it stays an argument until someone runs
it.

## 5. What to measure first, when it is built

1. **Cost per tile** against a coded tile and a skipped one at the same tile
   count, on the Adreno, which is the only device whose numbers decide
   anything.
2. **Whether the encoder-side fit belongs on the GPU too.** The reference fit
   is `double` k-planes. Nothing about it has to be: the reassignment is a
   per-cell argmin and the plane fit is a 3x3 normal-equation solve, both of
   which have integer forms. That is a second project and it should not start
   before the decoder side exists.
3. **Temporal stability**, which LOWPOLY-MODE.md 8 lists as unmeasured and
   which is the property most likely to sink the mode in a headset: a
   segmentation that flickers frame to frame on static content trades block
   noise for something worse. The fix, if it is needed, is to predict the map
   from the co-located tile's previous map — which is a *new dependency between
   frames* and would have to be paid for in this kernel's shape.
