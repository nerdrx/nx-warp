# ADR 0026: The Pass A to Pass B coefficient buffer is sparse

Status: Accepted, 2026-09-04
Relates to: ADR 0011 (Vulkan compute over fixed function), ADR 0025 point 4,
PAPER 3.2.5

## Context

The GPU decoder is two dispatches with a coefficient buffer between them
(PAPER 3.2.1). That buffer was dense: a `res_level` 0 4:2:0 tile reserves 6240
int16 and Pass A wrote all of them -- zeroing the region, then scattering the
coded coefficients into it at their raster positions -- so a 2048-tile frame
moved 25.6 MB out of Pass A and 25.6 MB into Pass B whatever the stream said.
PAPER 3.2.5 named a sparse layout as the first optimisation and deferred it
"because the dense layout keeps Pass A trivial".

The measurement that forced the question is in vk/decoder/README.md: Pass B's
cost had no slope against payload at all, over a 28x range of payload, because
the bytes it read did not depend on the payload and the wavefront ran its steps
whether the blocks carried coefficients or not.

## Decision

Change two things about the layout, and nothing else.

1. Inside a coding unit, coefficient `k` is stored at slot `k` -- **scan
   order** -- rather than at `scan_index(scan_id, k)`. The unit's base and its
   reserved width do not move, so `coef_stride` and every plane base are
   unchanged.
2. Pass A publishes one byte per coding unit holding **`LAST + 1`**, 0 for an
   uncoded unit, and writes slots `[0, LAST]` only. Nothing is zeroed, and
   nothing past `LAST` is written or read.

`LAST` is already in the syntax (SYNTAX.md 9.2) and the scan is already
normative (5), so this is a re-indexing of numbers the bitstream already
carries: same stream, same coefficients, same pixels, no tool bit, no version
bump. The layout is entirely between the two passes and the C ABI does not
change. `NXVC_VKD_FLAG_DENSE_COEF` keeps the old layout for measurement;
`NXVC_VKD_FLAG_COEF_STATS` reports the exact bytes.

The per-unit length is a specialisation constant in Pass B and a push constant
in Pass A: in Pass B the test sits inside the innermost coefficient loop and
costs 0.10 ms if left dynamic, while in Pass A it selects one store address and
one zeroing loop and a pipeline rebuild would cost more than it saves.

## Consequences

**Traffic follows the payload**, 2048 tiles 4:2:0, written by Pass A and read
by Pass B:

| QP | payload | dense | sparse |
|---|---|---|---|
| 63 | 0.10 MB | 25.6 MB | 0.87 MB |
| 36 | 0.16 MB | 25.6 MB | 0.93 MB |
| 24 | 1.42 MB | 25.6 MB | 11.6 MB |
| 12 | 2.87 MB | 25.6 MB | 13.6 MB |

The floor is the length words themselves, 66 uints per tile, 0.54 MB per frame.

**The bytes were not the point.** On an RX 7900 XTX the sparse layout costs
Pass B about 5 percent at QP 12 and QP 24 and buys nothing: 960 GB/s was never
the constraint, and checking every coefficient against its unit's length is
real work. On lavapipe, where compute is scarce and the coefficient fetch is a
cache hit either way, it costs more. ADR 0025 point 4 called sparse transfer
"the larger lever" and that was wrong; the scaled Adreno estimate puts the
whole memory system at 2.6 ms of an 11.1 ms budget dense, against a wavefront
at 15 to 36 ms.

**The length was the point.** A unit whose length is 0 coded nothing, so its
dequantize and both passes of its 8x8 IDCT are skipped, and a DC unit whose
length is 0 skips the second-level IDCT and its two barriers. That, with a
companion fast path for planes whose intra modes are all mode 0, took Pass B's
fixed cost from 890 ns per tile to 248 ns and gave it a real slope against
payload for the first time.

**A tile with no coded unit now runs no transform stage at all.** That is the
shape `WARP_SKIP` and `STATIC_MV` will have when the inter hook lands, and it
is already the shape of a static region under rolling refresh.

**Pass A got slightly more expensive** at low rates, 124 to 170 ns per tile of
fixed cost. Part is the length words and their atomicOr (accumulated in LDS and
flushed once per tile, so the frame's atomics stay local); most is the loss of
the zeroing loop, which used to write the coefficient region in whole cache
lines and so warmed it for the scattered stores that followed. Pass B gained
642 ns per tile against it.

**Both CPU models carry the same switch** and are checked in both layouts:
nxvc-passA-test runs GPU-against-model in each, seeding the coefficient buffer
with a sentinel so "Pass A wrote exactly these slots and no others" is a direct
check; nxvc-passB-test builds scan-order scenes with the same sentinel past
every `LAST`. vk.decoder.conformance is green on RADV and lavapipe.
