// Integer DCT of edge 8, 16 and 32 (Loeffler-derived 8-point core plus the
// even/odd recursion of docs/SYNTAX.md 6.2) and the resampling kernels.
// Everything here is normative; see docs/SYNTAX.md sections 6 and 8.
#pragma once
#include "common.h"

namespace nxvc {

// 9-bit constants: round(512 * cos(k*pi/16)) / round(512 * sin(...)).
constexpr i32 kC4 = 362;  // 512*cos(pi/4)
constexpr i32 kC2 = 473;  // 512*cos(pi/8)
constexpr i32 kS2 = 196;  // 512*sin(pi/8)
constexpr i32 kA1 = 502;  // 512*cos(pi/16)
constexpr i32 kA3 = 426;  // 512*cos(3pi/16)
constexpr i32 kA5 = 284;  // 512*sin(3pi/16)
constexpr i32 kA7 = 100;  // 512*sin(pi/16)

// 4x4 constants, from the same construction: the 1D inverse matrix is
// M[n][k] = round(1024 * c_k * cos(pi*(2n+1)*k/8)) with c_0 = 1/2 and
// c_k = 1/sqrt(2), so the 1D gain is exactly 2^10, the same as the 8x8's.
// Only three distinct magnitudes appear.  See docs/SYNTAX.md 6.8.
constexpr i32 kD0 = 512;  // 1024 * 1/2
constexpr i32 kD1 = 669;  // round(1024 * cos(pi/8)  / sqrt(2))
constexpr i32 kD2 = 277;  // round(1024 * cos(3pi/8) / sqrt(2))

// The odd half of the length-16 and length-32 transforms, entry [n][j] =
// round(512 * cos(pi * (2n+1) * (2j+1) / (2N))).  SYNTAX.md 6.2.1; regenerated
// and checked against that formula by tests/ref/test_transform.cpp.
extern const i16 kOdd16[8][8];
extern const i16 kOdd32[16][16];

// ------------------------------------------------------- the family
// ONE transform family over four edges.  The 4 comes from the detail package
// (per-block split, tool bit 19, SYNTAX.md 6.8) and the 16 and 32 from the
// transform package (per-tile size, tool bit 27, SYNTAX.md 6.7); they are the
// same construction and are declared once so there is exactly one dequantiser
// scale to get right.
//
// **Invariant: the quantiser sees orthonormal coefficients at every size**,
// at the v1 reference scale of 2^10 per dimension.  That is not the same as
// "the same 2D gain at every size" -- the unnormalized graph grows by sqrt(2)
// per doubling, so the 2D gains are 2^20, 2^20, 2^21, 2^22 -- and it is the
// property one qstep table depends on.  `ref.transform_gain` measures it
// against a floating-point DCT at every size, to within 0.1 %.  A slip here
// is silent: a factor of two shifts the effective QP by 6 and every rate and
// PSNR number stays plausible.  docs/MERGE-PLAN.md 4.4.
constexpr int kMaxBlock = 32;
constexpr int kMinBlock = 4;
inline bool block_size_ok(int n) {
    return n == 4 || n == 8 || n == 16 || n == 32;
}

// Forward: samples (residual) -> coefficients, `n` x `n`, raster order.
// Inverse: dequantized coefficients -> residual.  Two passes each, with the
// first-pass result clamped to int16.  Shifts per size: SYNTAX.md 6.3.
void fdct_block(const i32 *src, i16 *dst, int n);
void idct_block(const i32 *src, i32 *dst, int n);

// The named pairs, all of them exactly fdct_block/idct_block at one size.
inline void fdct8x8(const i32 src[64], i16 dst[64]) { fdct_block(src, dst, 8); }
inline void idct8x8(const i32 src[64], i32 dst[64]) { idct_block(src, dst, 8); }
inline void fdct4x4(const i32 src[16], i16 dst[16]) { fdct_block(src, dst, 4); }
inline void idct4x4(const i32 src[16], i32 dst[16]) { idct_block(src, dst, 4); }

// Bilinear resampling.  `sx`,`sy` are Q4 source coordinates.  Integer only.
i32 bilinear_q4(const u8 *src, int w, int h, int stride, i32 sx, i32 sy);
i32 bilinear_q4_i32(const i32 *src, int w, int h, int stride, i32 sx, i32 sy);

// Upsample a w x h plane by `factor` (2 or 4) into dst (w*factor x h*factor).
void upsample(const u8 *src, int w, int h, int sstride, u8 *dst, int dstride,
              int factor);
// Box-downsample by `factor`.
void downsample(const u8 *src, int w, int h, int sstride, u8 *dst, int dstride,
                int factor);

}  // namespace nxvc
