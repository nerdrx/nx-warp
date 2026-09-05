// NX Warp Pass B -- normative constants, shared by the GLSL kernel and the
// CPU model.
//
// EVERY value in this file is a bitstream-normative constant.  It is written
// in a dialect that both GLSL 4.60 and C++ accept so that the kernel and the
// model can never drift apart, and so that a re-alignment against the CPU
// reference is a diff of this one file.
//
// Provenance of each block is marked:
//   [SYN]   docs/SYNTAX.md, the normative specification.
//   [REF]   ref/ (ref/src/transform.{h,cpp}, tables.cpp, codec.cpp), which is
//           the implementation of that specification.
//   [PAPER] docs/PAPER.md, only where the other two are silent.
//
// This file was first written against ref/ alone, before docs/SYNTAX.md
// existed, and then re-aligned against SYNTAX.md section by section.  Every
// value below agrees with both; where SYNTAX.md and PAPER.md disagree, the
// disagreement is called out at the constant.

#ifndef NXVW_PASSB_SYNTAX_CONSTANTS_H
#define NXVW_PASSB_SYNTAX_CONSTANTS_H

// [minor 6] Whether the two XFORM_LARGE scans exist in this translation unit.
// The CPU model always carries them; the GLSL kernel carries them only in its
// XFORM_LARGE build variant, because nxvw_scan_pos() is called once per
// COEFFICIENT and two more compares in there are not free -- on an Adreno 650
// they were most of a 13 % instruction-count rise on a stream that sets no
// tool bit.  See vk/decoder/passB/reconstruct.comp.
#ifdef __cplusplus
#define NXVW_SCAN_LARGE 1
#elif defined(NXVW_XFORM_LARGE)
#define NXVW_SCAN_LARGE NXVW_XFORM_LARGE
#else
#define NXVW_SCAN_LARGE 1
#endif

#ifdef __cplusplus
#include <cstdint>
#define NXVW_ARR(T, name, n) constexpr T name[n] = {
#define NXVW_ARR_END \
    }                \
    ;
#define NXVW_CONST constexpr int
#define NXVW_FN inline int
namespace nxvw {
#else
#define NXVW_ARR(T, name, n) const T name[n] = T[n](
#define NXVW_ARR_END );
#define NXVW_CONST const int
#define NXVW_FN int
#endif

// ------------------------------------------------------------- geometry
// [SYN] 4.2 tile geometry.  [REF] common.h kTile / kBlock, tile_geom().
NXVW_CONST kTile = 64;
NXVW_CONST kBlock = 8;
NXVW_CONST kBlockLog2 = 3;
// [SYN] 4.2 / [REF] tile_geom(): chroma_size is clamped up to one block.
NXVW_CONST kMinCodedSize = 8;
// [SYN] 4.1: res_level 3 is reserved.
NXVW_CONST kMaxResLevel = 2;

// ------------------------------------------------------- inverse scans
// [sparse] Pass A stores a unit's coefficients in SCAN order and tells Pass B
// how many of them there are (passA/syntax_constants.h section 8).  Pass B
// wants them by raster position, so it needs the inverse of each scan:
// kInv*[raster index] = scan position.  These are the exact inverses of
// passA's kZigzag8 / kZigzag4 and are checked against them by
// vk.passB.scan_inverse.  kScanRaster8 (transform skip) and kScanSmall
// (ncoef 4 or 1) are the identity in both directions and are not tabulated.
NXVW_CONST kScanZigzag8 = 0;
NXVW_CONST kScanRaster8 = 1;
NXVW_CONST kScanZigzag4 = 2;
NXVW_CONST kScanSmall = 3;
// [minor 6] XFORM_4X4_SPLIT: four concatenated 4x4 sub-blocks, each in its own
// zigzag.  [REF] ref/src/tables.cpp kScan4Split, passA's kScan4Split id.
NXVW_CONST kScan4Split = 4;
// [minor 6] XFORM_LARGE (tool bit 27): the zigzags of a 16x16 and a 32x32
// block.  They are NOT tabulated.  A 1024-entry const array indexed by a
// runtime value is a private-memory allocation on Adreno
// (docs/ADRENO-RULES.md), and the zigzag is a rule rather than a list
// (ref/src/common.h build_zigzag), so the one direction Pass B needs --
// raster position -> scan position -- is written as that rule's closed form
// below.  Mirrors passA's kScanZigzag16 / kScanZigzag32.
NXVW_CONST kScanZigzag16 = 5;
NXVW_CONST kScanZigzag32 = 6;

NXVW_ARR(int, kInvZigzag8, 64)
     0,  1,  5,  6, 14, 15, 27, 28,  2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,  9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63
NXVW_ARR_END

NXVW_ARR(int, kInvZigzag4, 16)
     0,  1,  5,  6,  2,  4,  7, 12,  3,  8, 11, 13,  9, 10, 14, 15
NXVW_ARR_END

// [minor 6] The inverse of the 4x4-split scan, COMPUTED rather than tabulated.
// The split scan is four of kZigzag4 laid over the quadrants of an 8x8 array,
// so a raster position's scan position is its sub-block index times sixteen
// plus the 4x4 inverse zigzag of its position inside that sub-block -- which
// reuses kInvZigzag4 and adds no third table.
//
// It is not a micro-optimisation.  A module-scope const array indexed by a
// runtime value is a private-memory allocation on Adreno, and adding a third
// one took Pass B's scratch from 342 to 616 B and its overall register
// footprint from 6 to 104 -- a factor of four of the pass.
// docs/ADRENO-RULES.md.
NXVW_FN nxvw_inv_scan4split(int raster) {
    int row = raster >> 3, col = raster & 7;
    int sb = (row >= 4 ? 2 : 0) + (col >= 4 ? 1 : 0);
    return sb * 16 + kInvZigzag4[(row & 3) * 4 + (col & 3)];
}

// [minor 6] The number of scan positions before diagonal `s` of an
// `edge` x `edge` zigzag: the triangle from either end.  [REF]
// ref/src/common.h build_zigzag(), and passA's nxs_zigzag_before() -- the two
// sides of the same permutation, so they are written the same way.
NXVW_FN nxvw_zigzag_before(int edge, int s) {
    if (s <= edge - 1) return s * (s + 1) / 2;
    int t = 2 * edge - 1 - s;
    return edge * edge - t * (t + 1) / 2;
}
// Raster index -> scan position of an `edge` x `edge` zigzag, `lg` =
// log2(edge).  The two call sites below pass literal edges, so the divide and
// the modulo fold to a shift and a mask and no runtime-indexed table exists.
NXVW_FN nxvw_zigzag_raster_to_pos(int edge, int lg, int raster) {
    int u = raster >> lg, v = raster & (edge - 1);
    int s = u + v;
    int lo = s - edge + 1 > 0 ? s - edge + 1 : 0;
    int hi = s < edge - 1 ? s : edge - 1;
    int idx = (s & 1) == 0 ? hi - u : u - lo;
    return nxvw_zigzag_before(edge, s) + idx;
}

// [REF] common.h scan_table(): which scan a unit of `ncoef` coefficients uses.
// Mirrors passA's nxs_scan_id().  [minor 6] The 256- and 1024-coefficient
// units are XFORM_LARGE's; transform skip is mutually exclusive with a
// transform size other than 8x8 (SYNTAX.md 6.7), so the tskip arm only ever
// sees 64.
NXVW_FN nxvw_scan_id(int ncoef, int tskip) {
#if NXVW_SCAN_LARGE
    if (ncoef == 1024) return kScanZigzag32;
    if (ncoef == 256) return kScanZigzag16;
#endif
    if (ncoef == 64) return tskip != 0 ? kScanRaster8 : kScanZigzag8;
    if (ncoef == 16) return kScanZigzag4;
    return kScanSmall;
}

// Scan position of the coefficient whose raster index inside the unit is
// `pos`, i.e. the slot Pass A wrote it to under the sparse layout.
NXVW_FN nxvw_scan_pos(int scan_id, int pos) {
    if (scan_id == kScanZigzag8) return kInvZigzag8[pos];
    if (scan_id == kScanZigzag4) return kInvZigzag4[pos];
    if (scan_id == kScan4Split) return nxvw_inv_scan4split(pos);
#if NXVW_SCAN_LARGE
    if (scan_id == kScanZigzag16) return nxvw_zigzag_raster_to_pos(16, 4, pos);
    if (scan_id == kScanZigzag32) return nxvw_zigzag_raster_to_pos(32, 5, pos);
#endif
    return pos;
}

// ------------------------------------------------------------ quantizer
// [SYN] 6.5 qstep[qp] = round(16 * 2^(qp/6)), Q4 (16 == step 1.0).
// [PAPER] 1.5 "step = 2^(QP/6), QP 0..63".
NXVW_ARR(int, kQStep, 64)
      16,    18,    20,    23,    25,    29,    32,    36,
      40,    45,    51,    57,    64,    72,    81,    91,
     102,   114,   128,   144,   161,   181,   203,   228,
     256,   287,   323,   362,   406,   456,   512,   575,
     645,   724,   813,   912,  1024,  1149,  1290,  1448,
    1625,  1825,  2048,  2299,  2580,  2896,  3251,  3649,
    4096,  4598,  5161,  5793,  6502,  7298,  8192,  9195,
   10321, 11585, 13004, 14596, 16384, 18390, 20643, 23170
NXVW_ARR_END

// [SYN] 6.5 t = (qstep[qp] * w[i] + 8) >> 4, at most 46340.
NXVW_CONST kQStepRound = 8;
NXVW_CONST kQStepShift = 4;
// [SYN] 6.5 c = clamp16((q * t + 8) >> 4).  Levels are in [-32767, 32767].
NXVW_CONST kDequantRound = 8;
NXVW_CONST kDequantShift = 4;
// [SYN] 6.5 / 7.1 and [REF] codec.cpp dc_qp_of(): the DC plane is quantized
// at HALF the QP index with a flat weight.
//
// [nxvc_vk_decoder glue, marked edit] this was `max(0, qp - 6)` -- the rule
// that held when Pass B was written.  docs/SYNTAX.md 7.1 line 540 and
// ref/src/codec.cpp `dc_qp_of(qp) { return qp >> 1; }` now both say qp >> 1,
// and every conformance vector in tests/vectors decodes wrong under the old
// rule.  kDcQpOffset is gone rather than retuned so no caller can keep using
// a subtractive offset.
NXVW_FN nxvw_dc_qp(int qp) { return qp >> 1; }
NXVW_CONST kFlatWeight = 16;
// [SYN] 6.3 clamp16, normative after both transform passes.
NXVW_CONST kI16Min = -32768;
NXVW_CONST kI16Max = 32767;

// [SYN] 6.5 built-in weighting matrices, with s = u + v (u vertical):
//   0: 16 everywhere
//   1: min(32, 16 + s)            2: min(32, 16 + 2s)
//   3: min(32, 16 + s + (s >> 1))
// Flattened here: matrix m occupies [m*64 .. m*64+63] in raster order.
// Luma and alpha use matrix m; chroma uses matrix 3's formula unless m == 0,
// in which case chroma is flat too.  A custom matrix is 128 bytes (64 luma
// then 64 chroma), each entry clamped to [1, 32].
NXVW_ARR(int, kWeightFlat, 256)
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,

    16, 17, 18, 19, 20, 21, 22, 23,
    17, 18, 19, 20, 21, 22, 23, 24,
    18, 19, 20, 21, 22, 23, 24, 25,
    19, 20, 21, 22, 23, 24, 25, 26,
    20, 21, 22, 23, 24, 25, 26, 27,
    21, 22, 23, 24, 25, 26, 27, 28,
    22, 23, 24, 25, 26, 27, 28, 29,
    23, 24, 25, 26, 27, 28, 29, 30,

    16, 18, 20, 22, 24, 26, 28, 30,
    18, 20, 22, 24, 26, 28, 30, 32,
    20, 22, 24, 26, 28, 30, 32, 32,
    22, 24, 26, 28, 30, 32, 32, 32,
    24, 26, 28, 30, 32, 32, 32, 32,
    26, 28, 30, 32, 32, 32, 32, 32,
    28, 30, 32, 32, 32, 32, 32, 32,
    30, 32, 32, 32, 32, 32, 32, 32,

    16, 17, 19, 20, 22, 23, 25, 26,
    17, 19, 20, 22, 23, 25, 26, 28,
    19, 20, 22, 23, 25, 26, 28, 29,
    20, 22, 23, 25, 26, 28, 29, 31,
    22, 23, 25, 26, 28, 29, 31, 32,
    23, 25, 26, 28, 29, 31, 32, 32,
    25, 26, 28, 29, 31, 32, 32, 32,
    26, 28, 29, 31, 32, 32, 32, 32
NXVW_ARR_END

// ----------------------------------------------------------- 8x8 IDCT
// [SYN] 6.1 nine-bit constants, round(512 * cos/sin(k*pi/16)), on the
// Loeffler-Ligtenberg-Moschytz flow graph (1989, expired).
// [PAPER] 1.4 "our own 9-bit integer constants and a defined two-stage shift".
NXVW_CONST kC4 = 362;  // 512*cos(pi/4)
NXVW_CONST kC2 = 473;  // 512*cos(pi/8)
NXVW_CONST kS2 = 196;  // 512*sin(pi/8)
NXVW_CONST kA1 = 502;  // 512*cos(pi/16)
NXVW_CONST kA3 = 426;  // 512*cos(3pi/16)
NXVW_CONST kA5 = 284;  // 512*sin(3pi/16)
NXVW_CONST kA7 = 100;  // 512*sin(pi/16)

// [SYN] 6.2: the rotation inside the odd butterfly is brought back to 9-bit
// gain before the final adds.
NXVW_CONST kOddRound = 256;
NXVW_CONST kOddShift = 9;

// [SYN] 6.3 mulC4: `(s * C4 + 256) >> 9` written so it never leaves int32.
//
// [nxvc_vk_decoder glue, marked edit] the kernel used to compute this as a
// plain `(s * kC4 + kOddRound) >> kOddShift`.  Dequantized coefficients are
// clamped to int16, which bounds |P +- Q| at 8.62e7, and 8.62e7 * 362 is
// 3.12e10 -- outside int32.  docs/SYNTAX.md 6.3 defines the result as the
// exact mathematical value and gives this two-word identity for it, which
// ref/src/transform.cpp mul_c4_rnd9() implements.  Without it every stream
// whose dequantized coefficients saturate decodes differently from the
// reference (tests/vectors/v35_saturate420).
//
// Exact because 512 * hi * C4 is a multiple of 512, so the shift distributes;
// both partial products stay small (|hi * C4| <= 6.1e7, lo * C4 <= 1.9e5).
// `>>` on a signed int is arithmetic in GLSL and in C++20, and `& 511` takes
// the low bits of the two's-complement value, so the split is the same on
// both sides.
NXVW_FN nxvw_mul_c4_rnd9(int s) {
    int hi = s >> kOddShift;
    int lo = s & (kOddRound * 2 - 1);
    return hi * kC4 + ((lo * kC4 + kOddRound) >> kOddShift);
}
// [SYN] 6.3: rows then columns, both passes writing transposed.  The clamp16
// after pass 1 is NORMATIVE, so the transpose buffer may be int16.
// NOTE PAPER 1.4 says "7 bits after the first dimension, 12 after the
// second"; SYNTAX.md and ref/ both use 7 then 13.  SYNTAX.md wins.
NXVW_CONST kIdctRound1 = 64;
NXVW_CONST kIdctShift1 = 7;
NXVW_CONST kIdctRound2 = 4096;
NXVW_CONST kIdctShift2 = 13;

// ------------------------------------------- [minor 6] 16- and 32-point IDCT
// XFORM_LARGE, tool bit 27.  [SYN] 6.2.1, [REF] ref/src/transform.cpp
// even_odd_inverse / kOdd16 / kOdd32.
//
// A length-2M DCT-III is the length-M DCT-III of the even-indexed
// coefficients plus a dense M x M rotation of the odd-indexed ones.  Written
// with these 512-scaled constants the even half needs no rescaling at all, so
// the 1D gain grows by sqrt(2) per doubling: 2^10 at 4 and 8, 2^10*sqrt(2) at
// 16, 2^11 at 32, and the 2D gains are the exact powers 2^20, 2^21 and 2^22
// that the shift chain below undoes.  **The quantiser therefore sees
// orthonormal coefficients at every size and one qstep table serves all
// four** -- the invariant is NOT that every size hits the same 2D gain.
//
// entry [n][j] == round(512 * cos(pi * (2n+1) * (2j+1) / (2N))), flattened
// row-major.  Max row absolute sum 2613 and 5215, so with the int16 clamp of
// 6.3 on both passes' inputs the odd half reaches 8.6e7 at 16 and 1.7e8 at
// 32, and |y| stays inside int32 without the two-word identity the 8-point
// core's `P +- Q` needs.
NXVW_ARR(int, kOdd16, 64)
      510,   490,   452,   396,   325,   241,   149,    50,
      490,   325,    50,  -241,  -452,  -510,  -396,  -149,
      452,    50,  -396,  -490,  -149,   325,   510,   241,
      396,  -241,  -490,    50,   510,   149,  -452,  -325,
      325,  -452,  -149,   510,   -50,  -490,   241,   396,
      241,  -510,   325,   149,  -490,   396,    50,  -452,
      149,  -396,   510,  -452,   241,    50,  -325,   490,
       50,  -149,   241,  -325,   396,  -452,   490,  -510
NXVW_ARR_END

NXVW_ARR(int, kOdd32, 256)
      511,   506,   497,   482,   463,   439,   411,   379,
      344,   305,   263,   219,   172,   124,    75,    25,
      506,   463,   379,   263,   124,   -25,  -172,  -305,
     -411,  -482,  -511,  -497,  -439,  -344,  -219,   -75,
      497,   379,   172,   -75,  -305,  -463,  -511,  -439,
     -263,   -25,   219,   411,   506,   482,   344,   124,
      482,   263,   -75,  -379,  -511,  -411,  -124,   219,
      463,   497,   305,   -25,  -344,  -506,  -439,  -172,
      463,   124,  -305,  -511,  -344,    75,   439,   482,
      172,  -263,  -506,  -379,    25,   411,   497,   219,
      439,   -25,  -463,  -411,    75,   482,   379,  -124,
     -497,  -344,   172,   506,   305,  -219,  -511,  -263,
      411,  -172,  -511,  -124,   439,   379,  -219,  -506,
      -75,   463,   344,  -263,  -497,   -25,   482,   305,
      379,  -305,  -439,   219,   482,  -124,  -506,    25,
      511,    75,  -497,  -172,   463,   263,  -411,  -344,
      344,  -411,  -263,   463,   172,  -497,   -75,   511,
      -25,  -506,   124,   482,  -219,  -439,   305,   379,
      305,  -482,   -25,   497,  -263,  -344,   463,    75,
     -506,   219,   379,  -439,  -124,   511,  -172,  -411,
      263,  -511,   219,   305,  -506,   172,   344,  -497,
      124,   379,  -482,    75,   411,  -463,    25,   439,
      219,  -497,   411,   -25,  -379,   506,  -263,  -172,
      482,  -439,    75,   344,  -511,   305,   124,  -463,
      172,  -439,   506,  -344,    25,   305,  -497,   463,
     -219,  -124,   411,  -511,   379,   -75,  -263,   482,
      124,  -344,   482,  -506,   411,  -219,   -25,   263,
     -439,   511,  -463,   305,   -75,  -172,   379,  -497,
       75,  -219,   344,  -439,   497,  -511,   482,  -411,
      305,  -172,    25,   124,  -263,   379,  -463,   506,
       25,   -75,   124,  -172,   219,  -263,   305,  -344,
      379,  -411,   439,  -463,   482,  -497,   506,  -511
NXVW_ARR_END

// [REF] transform.cpp kInvShift1 / kInvShift2, indexed by log2(n) - 2:
//   4 and 8 -> 7, 13     16 -> 7, 14     32 -> 8, 14
// The first-pass shift grows with the value entering it -- one more butterfly
// level per doubling -- so every size leaves the same margin under the int16
// clamp of the transpose buffer, which is what lets that buffer stay int16 in
// LDS at every size.  4 and 8 share a chain because the 4-point graph has one
// butterfly level fewer AND one fewer sqrt(2) of gain, and the two cancel.
// `lb` is log2 of the block edge: 3, 4 or 5.
NXVW_FN nxvw_idct_shift1(int lb) { return lb >= 5 ? 8 : kIdctShift1; }
NXVW_FN nxvw_idct_shift2(int lb) { return lb >= 4 ? 14 : kIdctShift2; }
NXVW_FN nxvw_idct_round1(int lb) { return 1 << (nxvw_idct_shift1(lb) - 1); }
NXVW_FN nxvw_idct_round2(int lb) { return 1 << (nxvw_idct_shift2(lb) - 1); }

// [SYN] 6.7 the transform edge a plane uses: the tile's `8 << xform_size`,
// capped by the plane's own coded extent, so no combination of res_level,
// chroma format and xform_size asks for a block larger than the plane.  Both
// are powers of two, so this is stated in logs and `nb = size >> lb` is the
// block grid.  [REF] ref/src/common.h block_edge_for().
NXVW_FN nxvw_block_log2(int xform_size, int size) {
    int ls = 3;
    while ((1 << ls) < size) ++ls;
    int lb = 3 + xform_size;
    return lb < ls ? lb : ls;
}

// [SYN] 6.5 / [REF] codec.cpp block_weight(): ONE transmitted 8x8 matrix
// serves every size.  An n x n block replicates it -- entry (u, v) is entry
// (u >> k, v >> k) of the 8x8 with k = log2(n) - 3 -- and a split 4x4
// sub-block subsamples it, so the roll-off covers the same fraction of the
// frequency plane at every size and no second matrix is transmitted.  At
// lb == 3 this is the identity and the expression reduces to `i`.
NXVW_FN nxvw_block_weight_index(int i, int lb) {
    int k = lb - 3;
    return ((i >> lb) >> k) * 8 + ((i & ((1 << lb) - 1)) >> k);
}

// ----------------------------------------------------- planar DC intra
// [SYN] 7.2: a block mean sits at the block centre (bx*8 + 3.5, by*8 + 3.5),
// so the Q4 source coordinate is ux = 2x - 7, uy = 2y - 7.  Positions outside
// the outermost centres clamp to the edge.
// [PAPER] 3.2.4 / 6.4.
NXVW_CONST kPlanarMul = 2;
NXVW_CONST kPlanarOff = -7;
// [SYN] 7.2 in general form: a block mean of an n x n grid sits at the block
// centre (bx*n + (n-1)/2, ...), so the Q4 source coordinate of sample x is
//   (16*x - 8*(n - 1) + (n >> 1)) >> log2(n)
// with an arithmetic shift and `+ (n >> 1)` as the rounding term of 3.3.  At
// n == 8 it is exactly 2*x - 7 for every x, which is the v1 formula and why
// kPlanarMul / kPlanarOff are still what the 8x8 kernel computes; at 16 and
// 32 the exact coordinate has a half-Q4 fraction (the block centre falls
// between two Q4 positions) and the rounding term settles it the same way in
// both directions.  [REF] ref/src/codec.cpp planar_from_means().
NXVW_FN nxvw_planar_q4(int v, int bs, int lb) {
    return (16 * v - 8 * (bs - 1) + (bs >> 1)) >> lb;
}

// ------------------------------------------------- directional intra [v3]
// [SYN] 7.4, tool bit 17.  Each 8x8 block carries one of nine modes; mode 0
// IS the DC-plane prediction of 7.2, which makes INTRA_DIR a strict superset
// of v1.  [REF] ref/src/common.h kIntra*, ref/src/codec.cpp predict_block().
NXVW_CONST kIntraDcPlane = 0;  // the v1 bilinear DC-plane predictor
NXVW_CONST kIntraDc = 1;       // mean of the 8 top and 8 left neighbours
NXVW_CONST kIntraPlanar = 2;   // HEVC-style planar
NXVW_CONST kIntraH = 3;        // horizontal
NXVW_CONST kIntraV = 4;        // vertical
NXVW_CONST kIntraDdl = 5;      // diagonal down-left, 45 deg
NXVW_CONST kIntraDdr = 6;      // diagonal down-right, 45 deg
NXVW_CONST kIntraVr = 7;       // vertical-right, 26.6 deg
NXVW_CONST kIntraHd = 8;       // horizontal-down, 63.4 deg
// [minor 6] INTRA_CFL (tool bit 24), CHROMA ONLY: a linear model of the
// co-located reconstructed luma, fitted to the block's own reconstructed
// neighbours.  [REF] ref/src/common.h kIntraCfl, codec.cpp cfl_fit /
// cfl_luma, docs/SYNTAX.md 7.7.
NXVW_CONST kIntraCfl = 9;
NXVW_CONST kNumIntraModes = 9;

// [SYN] 7.4: the reference arrays run A[-1..15] and L[-1..15] with A[-1] ==
// L[-1] == the corner, so 17 entries each with the corner at index 0.
NXVW_CONST kIntraRefs = 17;

// [SYN] 7.4 / 7.6: the wavefront schedule.  Block (bx, by) reads its left,
// above and ABOVE-RIGHT neighbours (mode DDL reaches A[15]), so the
// independent set is 2*by + bx and a res_level-0 luma plane is a 22-step
// wavefront at 4.5 % occupancy -- 69 barriers for a 4:4:4 tile.
//
// SYNTAX.md 7.6 prices two restrictions that shorten it, and ref/ implements
// both behind -DNXVC_DIR_SCHED_EXPERIMENT with exactly this bit encoding
// (ref/src/codec.cpp build_refs()), so a stream produced with NXVC_DIR_SCHED
// = k decodes bit-exactly under kDirSched == k:
//
//   bit 0  drop the above-right reference   15 steps,  6.7 %,  +0.24 % rate
//   bit 1  confine to 32x32 sub-tiles       10 steps, 10.0 %,  +1.6  % rate
//   both                                     7 steps, 14.3 %,  +1.8  % rate
//
// 7.4 as written -- kDirSchedFull -- is the normative derivation and the only
// one a conformant encoder emits today.  The others are selectable so the
// decode cost of each can be measured against those rate numbers.
NXVW_CONST kDirSchedFull = 0;
NXVW_CONST kDirSchedNoAboveRight = 1;
NXVW_CONST kDirSchedSubTile = 2;
NXVW_CONST kDirSchedBoth = 3;
// The sub-tile is 4x4 blocks = 32x32 samples, so the predicate is a compare
// of the block indices shifted by this.
NXVW_CONST kDirSubTileLog2 = 2;
NXVW_CONST kDirSubTile = 4;  // 1 << kDirSubTileLog2, blocks per sub-tile edge

// Threads the kernel gives one 8x8 block during the INTRA_DIR wavefront.  The
// transform's four-per-block mapping is not forced on the wavefront: the
// residual is in shared memory by then, so the step is free to spread over as
// many threads as it has blocks.  16 threads own two uints each -- rows 2j,
// 2j+1 of one column pair -- which keeps every shared word owned by exactly
// one thread, and 16 blocks x 16 threads is the 256-thread workgroup.
NXVW_CONST kDirLanesPerBlock = 16;

// [SYN] 7.4 predictor rounding.  Every mode but 0 is a weighted average of
// references already in [0, maxval], so no clamp is needed and none is
// applied.
NXVW_CONST kIntraDcRound = 8;
NXVW_CONST kIntraDcShift = 4;    // (sum of 16 references + 8) >> 4
NXVW_CONST kIntraPlanarRound = 8;
NXVW_CONST kIntraPlanarShift = 4;
NXVW_CONST kIntraTap3Round = 2;  // (a + 2b + c + 2) >> 2
NXVW_CONST kIntraTap3Shift = 2;
NXVW_CONST kIntraTap2Round = 1;  // (a + b + 1) >> 1
NXVW_CONST kIntraTap2Shift = 1;

// [minor 6] INTRA_CFL.  The fit reads the block's 2n reconstructed neighbours
// -- 16 at the 8x8 block -- takes the mean of the two smallest and the two
// largest co-located luma values, and divides by their difference through
// kCflRecip[d] = round(2^15 / d).  The slope is Q8 and clamped to +-8.0.
// [REF] ref/src/common.h kCflRecip / kCflAlpha*, docs/SYNTAX.md 7.7.
NXVW_CONST kCflPairs = 16;
NXVW_CONST kCflAlphaBits = 8;
NXVW_CONST kCflAlphaMax = 2048;   // 8 << kCflAlphaBits
NXVW_CONST kCflRecipShift = 7;    // Q15 product -> Q8 slope
NXVW_CONST kCflRecipRound = 64;
NXVW_CONST kCflPredRound = 128;   // (alpha * dl + 128) >> 8

// [minor 6] XFORM_4X4_SPLIT.  The inverse 4-point DCT of 6.8; the 1D inverse
// matrix rows are +-{kD0, kD1, kD0, kD2} and +-{kD0, kD2, -kD0, -kD1}, so the
// 1D gain is 2^10 exactly as the 8-point core's and the two-pass shift chain
// is the SAME one the 8x8 uses (kIdctShift1/2).  [REF] ref/src/transform.h
// kD0/kD1/kD2 and transform.cpp kInvShift1[0] == 7, kInvShift2[0] == 13.
NXVW_CONST kD0 = 512;
NXVW_CONST kD1 = 669;
NXVW_CONST kD2 = 277;

// [SYN] 8, the one resampling kernel: Q4 coordinates, integer weights,
// (p00*wx0*wy0 + p01*fx*wy0 + p10*wx0*fy + p11*fx*fy + 128) >> 8, source
// coordinates clamped to the plane.  ONE rounding step: a decoder must not
// implement it as two separable passes.
NXVW_CONST kBilinFracBits = 4;   // Q4
NXVW_CONST kBilinOne = 16;       // 1 << kBilinFracBits
NXVW_CONST kBilinRound = 128;
NXVW_CONST kBilinShift = 8;

// [SYN] 8: for an upsample by factor 1, 2, 4 or 8,
//   mul = 16 / factor, off = mul/2 - 8, sx = mul*x + off,
// the half-phase alignment source = (x + 0.5)/factor - 0.5.  Factor 2 gives
// the 3/4, 1/4 taps of PAPER 1.3.  A plane is upsampled to its full extent in
// ONE step (SYN 4.2); the 4:2:0 chroma-to-luma-resolution step of SYN 5.2 is a
// separate, later operation and only exists on the packed display outputs.

// -------------------------------------------------------------- colour
// [SYN] 4.3 sample domains: under colour_transform 1 the two chroma planes
// are 9-bit with offset 256; every other plane is 8-bit with offset 128.
NXVW_CONST kDcOffset8 = 128;
NXVW_CONST kMaxval8 = 255;
NXVW_CONST kDcOffsetChromaCT = 256;
NXVW_CONST kMaxvalChromaCT = 511;
// [SYN] 5.1 inverse: Co = plane1 - 256; Cg = plane2 - 256;
//   t = Y - (Cg >> 1); G = Cg + t; B = t - (Co >> 1); R = B + Co.
// `>>` is arithmetic.  Malvar and Sullivan 2003.

// colour_transform values, matching nxvc_color_transform in include/nxvc/nxvc.h.
NXVW_CONST kCtNone = 0;
NXVW_CONST kCtYCoCgR = 1;

// [SYN] 4.1 tile mode values, matching nxvc_tile_mode in include/nxvc/nxvc.h.
// v1 (Phase 1, SYN 12) codes INTRA only; the rest are the inter hook.
NXVW_CONST kModeWarpSkip = 0;
NXVW_CONST kModeStaticMv = 1;
NXVW_CONST kModeWarpMv = 2;
NXVW_CONST kModeIntra = 3;
NXVW_CONST kModeStereo = 4;

// [SYN] 4.1 alpha_mode; 7.3 says modes 0 and 1 code no coefficients at all.
NXVW_CONST kAlphaOpaque = 0;
NXVW_CONST kAlphaConstant = 1;
NXVW_CONST kAlphaCoded = 2;

// ------------------------------------------------------ 10-bit display
// [PAPER] 1.3: the client stores references in display format, RGBA8 or
// RGB10A2.  v1 codes 8-bit samples (ref rejects bit_depth != 8), so the
// RGB10A2 output path replicates the top bits: v10 = (v8 << 2) | (v8 >> 6),
// alpha2 = a8 >> 6.  This is a *display* mapping, not a bitstream constant;
// a real 10-bit profile will widen the sample path instead.
NXVW_CONST kOutRgba8 = 0;
NXVW_CONST kOutRgb10A2 = 1;
// Two-plane 4:2:0 YCbCr passthrough: a full-resolution R8_UINT luma image plus
// an interleaved half-resolution R8G8_UINT CbCr image, with NO colour
// transform.  This is what the WiVRn NX client's decoder output and the Linux
// server input already are, and what the reference slots hold on the headset
// (4 slots x 2 eyes at RGBA8 would be 134 MB on a Pico 4; at 4:2:0 it is 50).
// It requires colour_transform == kCtNone and a 4:2:0 stream.  Default output
// on the Android target; see NXVW_PASSB_DEFAULT_OUT below.
NXVW_CONST kOutYcbcr420 = 2;
// "no second store", specialization constant 3's default.
NXVW_CONST kOutNone = -1;

#if defined(__ANDROID__)
NXVW_CONST kOutDefault = 2;  // kOutYcbcr420
#else
NXVW_CONST kOutDefault = 0;  // kOutRgba8
#endif

#ifdef __cplusplus
}  // namespace nxvw
#endif

#undef NXVW_ARR
#undef NXVW_ARR_END
#undef NXVW_CONST
#undef NXVW_SCAN_LARGE

#endif  // NXVW_PASSB_SYNTAX_CONSTANTS_H
