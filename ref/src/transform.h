// The integer DCT family (8x8, 16x16, 32x32) and the resampling kernels.
// Everything here is normative; see docs/SYNTAX.md sections 6 and 8.
#pragma once
#include "common.h"

namespace nxvc {

// The whole family is one matrix, sampled at three sizes (SYNTAX.md 6.1):
//
//     T_N[k][n] = round(512 * c_k * cos((2n+1) k pi / (2N))),  c_0 = 1/sqrt2
//
// The 9-bit constants below are T_8, which is what version 1 already codes
// with.  Because the scale 512 does not depend on N, the *even* rows of
// T_2N are bit for bit the rows of T_N, which is what lets a 16- or 32-point
// transform be one butterfly on top of the next size down and reuse the
// existing 8-point flow graph unchanged as its base case.
constexpr i32 kC4 = 362;  // 512*cos(pi/4)
constexpr i32 kC2 = 473;  // 512*cos(pi/8)
constexpr i32 kS2 = 196;  // 512*sin(pi/8)
constexpr i32 kA1 = 502;  // 512*cos(pi/16)
constexpr i32 kA3 = 426;  // 512*cos(3pi/16)
constexpr i32 kA5 = 284;  // 512*sin(3pi/16)
constexpr i32 kA7 = 100;  // 512*sin(pi/16)

// Forward: samples (residual) -> coefficients.  `n` is 8, 16 or 32; both
// arrays hold n*n values in raster order inside the block.  Two passes,
// >> (3 + log2 n) then >> 14, with the first-pass result clamped to int16.
void fdct_2d(int n, const i32 *src, i16 *dst);
// Inverse: dequantized coefficients -> residual.  Two passes,
// >> (4 + log2 n) then >> 13, with the first-pass result clamped to int16.
void idct_2d(int n, const i32 *src, i32 *dst);

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
