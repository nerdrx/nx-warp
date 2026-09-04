// NX Warp Pass B -- normative constants, shared by the GLSL kernel and the
// CPU model.
//
// EVERY value in this file is a bitstream-normative constant.  It is written
// in a dialect that both GLSL 4.60 and C++ accept so that the kernel and the
// model can never drift apart, and so that a re-alignment against the CPU
// reference is a diff of this one file.
//
// Provenance of each block is marked:
//   [REF]   taken from ref/ (ref/src/transform.{h,cpp}, ref/src/tables.cpp,
//           ref/src/codec.cpp).  ref/ is normative and overrides the paper.
//   [PAPER] taken from docs/PAPER.md where ref/ has nothing to say.
//
// docs/SYNTAX.md did not exist when this was written; ref/ did, and ref/ is
// the implementation of that document, so ref/ was used as the source of
// truth wherever the two could disagree.

#ifndef NXVW_PASSB_SYNTAX_CONSTANTS_H
#define NXVW_PASSB_SYNTAX_CONSTANTS_H

#ifdef __cplusplus
#include <cstdint>
#define NXVW_ARR(T, name, n) constexpr T name[n] = {
#define NXVW_ARR_END \
    }                \
    ;
#define NXVW_CONST constexpr int
namespace nxvw {
#else
#define NXVW_ARR(T, name, n) const T name[n] = T[n](
#define NXVW_ARR_END );
#define NXVW_CONST const int
#endif

// ------------------------------------------------------------- geometry
// [REF] common.h kTile / kBlock, tile_geom().
NXVW_CONST kTile = 64;
NXVW_CONST kBlock = 8;
// [REF] tile_geom(): chroma_size is clamped up to one block.
NXVW_CONST kMinCodedSize = 8;
// [REF] decoder rejects res_level > 2.
NXVW_CONST kMaxResLevel = 2;

// ------------------------------------------------------------ quantizer
// [REF] tables.cpp kQStep: round(16 * 2^(qp/6)), Q4 (16 == step 1.0).
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

// [REF] codec.cpp dequant_step(): t = (kQStep[qp] * w + kQStepRound) >> kQStepShift
NXVW_CONST kQStepRound = 8;
NXVW_CONST kQStepShift = 4;
// [REF] codec.cpp dequant(): clamp16((q * t + kDequantRound) >> kDequantShift)
NXVW_CONST kDequantRound = 8;
NXVW_CONST kDequantShift = 4;
// [REF] reconstruct_plane(): the DC plane is coded at qp - 6, floored at 0,
// with a flat weight of 16 (i.e. w == 1.0).
NXVW_CONST kDcQpOffset = 6;
NXVW_CONST kFlatWeight = 16;
// [REF] common.h clamp16.
NXVW_CONST kI16Min = -32768;
NXVW_CONST kI16Max = 32767;

// [REF] tables.cpp kWeight[4][64], flattened: matrix m occupies [m*64 .. m*64+63]
// in raster order inside the 8x8 block.
//   0 flat, 1 luma roll-off, 2 periphery roll-off, 3 chroma.
// [REF] resolve_matrices(): luma uses matrix m, chroma uses matrix 3 unless
// m == 0 in which case chroma also uses matrix 0.  A custom matrix is 128
// bytes (64 luma then 64 chroma), each entry clamped to [1, 32].
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
// [REF] transform.h: Loeffler-Ligtenberg-Moschytz factorization with our own
// 9-bit constants, round(512 * cos/sin(k*pi/16)).
// [PAPER] 1.4 "our own 9-bit integer constants and a defined two-stage shift".
NXVW_CONST kC4 = 362;  // 512*cos(pi/4)
NXVW_CONST kC2 = 473;  // 512*cos(pi/8)
NXVW_CONST kS2 = 196;  // 512*sin(pi/8)
NXVW_CONST kA1 = 502;  // 512*cos(pi/16)
NXVW_CONST kA3 = 426;  // 512*cos(3pi/16)
NXVW_CONST kA5 = 284;  // 512*sin(3pi/16)
NXVW_CONST kA7 = 100;  // 512*sin(pi/16)

// [REF] transform.cpp idct8_1d(): the rotation inside the odd butterfly is
// brought back to 9-bit gain before the final adds.
NXVW_CONST kOddRound = 256;
NXVW_CONST kOddShift = 9;
// [REF] transform.cpp idct8x8(): first dimension (rows) then second (columns).
// NOTE the paper (1.4) says "7 bits after the first dimension, 12 after the
// second"; ref uses 7 then 13.  ref wins.
NXVW_CONST kIdctRound1 = 64;
NXVW_CONST kIdctShift1 = 7;
NXVW_CONST kIdctRound2 = 4096;
NXVW_CONST kIdctShift2 = 13;

// ----------------------------------------------------- planar DC intra
// [REF] reconstruct_plane(): pred(x,y) = bilinear over the nb x nb array of
// block means at Q4 source coordinates (2x - 7, 2y - 7).  With 8x8 blocks that
// places the sample grid on the block centres.
// [PAPER] 3.2.4 / 6.4 "bilinear interpolation between the four nearest block
// DCs (planar-like)".
NXVW_CONST kPlanarMul = 2;
NXVW_CONST kPlanarOff = -7;

// [REF] transform.cpp bilinear_impl(): Q4 coordinates, integer weights,
// (p00*wx0*wy0 + p01*fx*wy0 + p10*wx0*fy + p11*fx*fy + 128) >> 8, source
// coordinates clamped to the plane.
NXVW_CONST kBilinFracBits = 4;   // Q4
NXVW_CONST kBilinOne = 16;       // 1 << kBilinFracBits
NXVW_CONST kBilinRound = 128;
NXVW_CONST kBilinShift = 8;

// [REF] transform.cpp upsample() / codec_impl.inc upsampled(): half-phase
// mapping source = (out + 0.5)/factor - 0.5 in Q4, i.e.
//   mul = 16 / factor, off = mul/2 - 8, sx = mul*x + off.
// factor 2 gives the 3/4, 1/4 taps of PAPER 1.3; factor 4 gives 4*x - 6.
// (No separate table: the two taps fall out of the Q4 bilinear above.)

// -------------------------------------------------------------- colour
// [REF] codec.cpp Geometry::dc_offset() / maxval(): under the YCoCg-R colour
// transform the two chroma planes are 9-bit, DC-centred at 256; everything
// else is 8-bit centred at 128.
// [PAPER] 1.3 "needs one extra bit on the chroma planes".
NXVW_CONST kDcOffset8 = 128;
NXVW_CONST kMaxval8 = 255;
NXVW_CONST kDcOffsetChromaCT = 256;
NXVW_CONST kMaxvalChromaCT = 511;
// [REF] nxvc_ycocgr_inverse():
//   t = Y - (Cg >> 1); G = Cg + t; B = t - (Co >> 1); R = B + Co
// with Co, Cg already de-offset by kDcOffsetChromaCT.
// [PAPER] 1.3, Malvar and Sullivan 2003 lifting form.

// colour_transform values, matching nxvc_color_transform in include/nxvc/nxvc.h.
NXVW_CONST kCtNone = 0;
NXVW_CONST kCtYCoCgR = 1;

// tile mode values, matching nxvc_tile_mode.
NXVW_CONST kModeSkipWarp = 0;
NXVW_CONST kModeSkipStatic = 1;
NXVW_CONST kModeInter = 2;
NXVW_CONST kModeIntra = 3;

// alpha_mode values [REF] codec.cpp / PAPER 1.2.
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
