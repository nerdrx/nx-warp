# 3. Conventions

This clause fixes the arithmetic. Every formula in clauses 4 to 6 and in
Annex A is to be evaluated exactly as specified here, with no reassociation, no
widening and no reordering that changes a rounding decision. The codec's
bit-exactness rests entirely on this clause.

## 3.1 Byte order and bit packing

1. All multi-byte integer fields in headers are **little endian**
   [SYNTAX 1, TRANSPORT: Conventions].
2. Bit fields packed inside a `u32` header word are listed and numbered
   **LSB first**: bits `[a:b]` of a word means the field occupying bit
   positions `b` through `a` inclusive, with bit 0 the least significant bit of
   the little-endian word.
3. The **one exception** to little-endian order is the rANS renormalisation
   pair, which is read as `hi, lo` — big-endian — because the encoder writes it
   while walking backwards. This is a wire fact, not a preference
   [SYNTAX 9.5].
4. The 5-bit values of a transmitted probability table are packed **MSB
   first**, contexts in index order and symbols in symbol order
   [SYNTAX 9.4].
5. Bypass bit chunks wider than 8 bits are emitted **most significant chunk
   first** [SYNTAX 9.5].
6. TLV records in the extension area are padded to a multiple of 4 bytes with
   zero bytes [SYNTAX 2.1].

## 3.2 Operators

In order of decreasing precedence. Where this differs from C, the difference is
stated.

| Operator | Meaning |
|---|---|
| `a[i]` | Element `i` of array `a` |
| `-a` | Negation |
| `~a` | Bitwise complement |
| `a * b`, `a / b`, `a % b` | Multiplication; integer division; remainder |
| `a + b`, `a - b` | Addition, subtraction |
| `a << n`, `a >> n` | Shifts, see clause 3.3 |
| `a < b`, `a <= b`, `a > b`, `a >= b` | Comparison, yielding 0 or 1 |
| `a == b`, `a != b` | Equality |
| `a & b`, `a ^ b`, `a \| b` | Bitwise and, exclusive or, or |
| `a && b`, `a \|\| b` | Logical and, or |
| `a ? b : c` | Conditional |
| `a = b` | Assignment |

`2^n` denotes exponentiation, never exclusive or. Exclusive or is always
written `^` between operands of an integer type and is never applied to a
literal exponent.

## 3.3 Shifts

`x >> n` is:

* an **arithmetic** right shift when `x` is signed: the sign bit is
  replicated, and the result is `floor(x / 2^n)` for every `x`, including
  negative `x`. `(-1) >> 1 == -1`. This is the semantics SPIR-V's
  `OpShiftRightArithmetic` defines exactly [R-3], and it is why source
  coordinates may be negative without a special case (clause 6.5).
* a **logical** right shift when `x` is unsigned.

`x << n` is a logical left shift. Shifting a value out of the range of its type
is not permitted to occur in a conforming decoding process; where a formula
could reach that, the specification states the bound that prevents it.

`n` MUST be in `[0, 31]` for a 32-bit operand. SPIR-V leaves out-of-range shift
amounts undefined [R-3], so every shift amount in this specification is either
a compile-time constant or is explicitly bounded at the point of use.

**Rounding shift.** The idiom `(x + (1 << (s - 1))) >> s` is the only rounding
form used. It rounds half away from zero for positive `x` and half toward
positive infinity for negative `x`, deterministically, because the shift is
arithmetic. Rounding is written out in full in every formula; there is no
implicit rounding anywhere in this document set.

## 3.4 The integer-only rule (normative)

Within the decoding process:

1. **All arithmetic is `int32`.** Intermediate values are 32-bit signed
   integers unless a formula states otherwise, and every formula in this
   document set is accompanied, at its point of definition, by the bound that
   keeps it inside `int32`.
2. **No floating point.** Not `float`, not `double`, not `fp16`, not
   `RelaxedPrecision`. Vulkan permits 2.5 ULP for `fp32` division, FMA
   contraction differs between compilers, and vendors round differently; a
   single ULP difference in a sampling coordinate flips a rounding decision and
   the mismatch then propagates through every later frame [PAPER 2.2].
3. **No 64-bit integer opcodes.** Where a 64-bit intermediate is genuinely
   required — the homography numerator of clause 6.7 — it is computed as an
   explicit `(hi, lo)` pair of `uint32` via extended multiply
   (`OpUMulExtended` / `OpSMulExtended`, core SPIR-V, no `shaderInt64`
   capability) [PAPER 2.2, warp/include/nxvc/warp.h @ 9083dd1].
4. **No division and no remainder** on any per-sample, per-coefficient or
   per-tile path. `OpSDiv`, `OpUDiv`, `OpSRem`, `OpSMod`, `OpUMod` do not
   appear in a conforming decoder's normative shaders [PAPER 3.7]. There are
   exactly **two** exceptions, both stated where they occur and both bounded:
   * **probability-table normalisation** (clause 6.6.2), which runs 12 x 8
     times per frame and uses a truncating divide [SYNTAX 9.4];
   * **the corner divide** of the warp (clause 6.7), which is not a division
     opcode at all but a fixed 32-iteration restoring division built from
     shifts, comparisons and subtractions, evaluated four times per tile
     [PAPER 2.2].
5. **Every load is bounds-clamped in the decoder.** `robustBufferAccess`
   behaviour differs across vendors (zero versus garbage) and the codec MUST
   NOT depend on it [PAPER 3.7]. Sample fetches outside a picture use
   clamp-to-edge (clause 6.5.2).
6. **Clamping ranges are normative**, so that overflow cannot differ by vendor.
   Where a clamp appears in a formula it is part of the format, not a defensive
   measure that an implementation may hoist or elide.

A clause that appears to require a float, a division or a 64-bit opcode outside
these two exceptions is defective. Report it rather than implementing it.

## 3.5 Functions

```
clamp(v, lo, hi)   = lo   if v < lo
                     hi   if v > hi
                     v    otherwise

clamp16(v)         = clamp(v, -32768, 32767)

min(a, b), max(a, b)    as usual

abs(v)             = v >= 0 ? v : -v
sign(v)            = v > 0 ? 1 : (v < 0 ? -1 : 0)

floor_div(a, b)    is not defined and is not used; every division-like
                   operation in this document set is a shift
```

`clamp16` appears in three normative places — after the first inverse
transform pass, after dequantisation, and on reconstructed DC-plane values —
and in each it is load-bearing rather than defensive: it is what bounds the
next stage's intermediates to `int32` and what permits a GPU implementation to
hold the transpose buffer in `int16` local memory [SYNTAX 6.3, decision 11].

## 3.6 Descriptors

Clause 4 declares each syntax element with a descriptor naming how it is read.

### Byte-stream descriptors (headers)

These read from the byte-aligned header stream. The read position advances by
whole bytes; a header field never straddles a header structure.

| Descriptor | Meaning |
|---|---|
| `f(n)` | Unsigned integer of `n` bits. When `n` is 8, 16 or 32 and the element occupies whole bytes, the bytes are little endian. When the element is a bit field of a larger word, `n` is its width and the word is little endian with bits numbered LSB first |
| `s(n)` | Signed integer of `n` bits, two's complement, otherwise as `f(n)` |
| `b(n)` | An opaque byte array of `n` bytes, carried without interpretation |

**`u(n)`, `ue(v)` and `se(v)` are deliberately not used.** NX Warp has no
variable-length header syntax and no bit-oriented header parsing: every header
element is at a fixed byte offset with a fixed width, which is what allows a
GPU to parse tile headers without a bit reader [PAPER 1.2]. The only
Exp-Golomb code in the format is the order-3 escape suffix inside the entropy
payload, which is coded as bypass bits and is written `eg3(v)` below. Readers
familiar with H.264 should note that `se(v)`-style parsing appears nowhere.

### Entropy-payload descriptors

These read from the interleaved rANS payload of a tile (clause 6.6). They may
be executed only inside the schedule of clause 6.6.4.

| Descriptor | Meaning |
|---|---|
| `ae(c)` | One symbol in `[0, 15]` decoded with the frequency table of context `c` |
| `bp(n)` | `n` bypass bits, `1 <= n <= 8`, MSB first. Wider values are split into chunks of at most 8 bits, most significant chunk first |
| `eg3(v)` | The order-3 Exp-Golomb escape suffix of clause 6.6.6, read entirely as `bp` chunks |

### Derived elements

An element written `= expr` in a syntax table is **not read from the
bitstream**: it is derived from elements already read, and is listed so that
clause 5 can give it semantics and so that clause 6 can refer to it by name.

## 3.7 Fixed-point formats

The format uses a small, closed set of Q-formats. `Qm.n` means a signed two's
complement value with `n` fractional bits; `Q.n` means the integer part is
whatever the stated range allows.

| Format | Where | Value of 1.0 | Range / bound |
|---|---|---|---|
| Q4 | Quantiser steps `qstep[qp]`, weighting matrix entries `w[i]` | 16 | `qstep` in `[16, 23170]`; `w` in `[1, 32]` [SYNTAX 6.5] |
| Q4 | Resampling source coordinates `sx`, `sy` (1/16 sample) | 16 | Clamped by the plane extent [SYNTAX 8] |
| Q8 | Probability-table delta multipliers `kDeltaMul` | 256 | `[16, 3444]` (Annex A.3) |
| Q15 | Transport pose quaternion components | 32768 | `[-32768, 32767]` [TRANSPORT 3.3] |
| Q8 | Transport pose position, millimetres | 256 | `int32` [TRANSPORT 3.3] |
| Q.2 | Per-tile motion vectors (quarter sample) | 4 | `mv_x`, `mv_y` in `[-128, 127]`, i.e. `[-32, 31.75]` samples |
| Q.6 | Warp corner source coordinates (1/64 sample) | 64 | Saturated to `+-2^18` [warp.h @ 9083dd1] |
| Q10.21 | Homography rows 0 and 1 | `2^21` | `+-1024.0` [warp.h @ 9083dd1] [pending WARP.md] |
| Q2.29 | Homography row 2; `h22` is exactly `1 << 29` | `2^29` | Denominator constrained to `[2^28, 2^30)` [warp.h @ 9083dd1] [pending WARP.md] |
| Q.4 | Warp sampling positions (1/16 sample) | 16 | Derived, see clause 6.7 |

Two different Q4 uses appear above (quantiser steps and resampling
coordinates). They never meet in one expression.

**Note.** `docs/PAPER.md` 2.2 states the homography is quantised to Q8.24 with
`h22 = 2^24`. `docs/STEREO.md` 5 demonstrates that **this overflows**: with
centred coordinates the largest coefficient is of order `f` (940 at the Pico 4's
streamed width) and `940 * 2^24` exceeds `int32` by seven bits. `docs/STEREO.md`
uses centred coordinates on a common **Q10.21** scale with `h22 = 2^21`, which
bounds every coefficient by `2^31 / 2^21 = 1024 > f`. The implementation in
`warp/` also uses Q10.21 for rows 0–1, but Q2.29 with `h22 = 2^29` for row 2 —
a *split* scale, where `docs/STEREO.md` recommends a common one. So there are
now three published formats for the same matrix and no normative document.
Recorded as Annex C issue **C-2**. [pending WARP.md]

## 3.8 Pseudo-code

Pseudo-code blocks use the operators of clause 3.2, C-style control flow, and
`for i in a .. b` inclusive of both bounds. Variables are `int32` unless
declared otherwise. Arrays are zero-based. Assignment to an array element that
was never written is a defect in the clause, not an implementation choice.
