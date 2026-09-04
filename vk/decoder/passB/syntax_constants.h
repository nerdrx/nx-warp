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
// [SYN] 4.2 / [REF] tile_geom(): chroma_size is clamped up to one block.
NXVW_CONST kMinCodedSize = 8;
// [SYN] 4.1: res_level 3 is reserved.
NXVW_CONST kMaxResLevel = 2;

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
// [SYN] 6.3: rows then columns, both passes writing transposed.  The clamp16
// after pass 1 is NORMATIVE, so the transpose buffer may be int16.
// NOTE PAPER 1.4 says "7 bits after the first dimension, 12 after the
// second"; SYNTAX.md and ref/ both use 7 then 13.  SYNTAX.md wins.
NXVW_CONST kIdctRound1 = 64;
NXVW_CONST kIdctShift1 = 7;
NXVW_CONST kIdctRound2 = 4096;
NXVW_CONST kIdctShift2 = 13;

// ----------------------------------------------------- planar DC intra
// [SYN] 7.2: a block mean sits at the block centre (bx*8 + 3.5, by*8 + 3.5),
// so the Q4 source coordinate is ux = 2x - 7, uy = 2y - 7.  Positions outside
// the outermost centres clamp to the edge.
// [PAPER] 3.2.4 / 6.4.
NXVW_CONST kPlanarMul = 2;
NXVW_CONST kPlanarOff = -7;

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

#endif  // NXVW_PASSB_SYNTAX_CONSTANTS_H
