// 8x8 integer DCT (Loeffler-derived, 9-bit constants) and the resampling
// kernels.  Everything here is normative; see docs/SYNTAX.md sections 5 and 7.
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

// The 4-point transform (tool bit 19, XFORM_4X4_SPLIT).  Its constants are the
// 8-point even part's, rescaled by sqrt(2) so that the 4-point flow graph has
// the same gain 2^10 per dimension the 8-point one has:
//   kD0 = round(512*sqrt(2)*cos(pi/4)) = 512   (a shift, not a multiply)
//   kD1 = round(512*sqrt(2)*cos(pi/8)) = 669
//   kD2 = round(512*sqrt(2)*sin(pi/8)) = 277
constexpr i32 kD0 = 512;
constexpr i32 kD1 = 669;
constexpr i32 kD2 = 277;

// Forward: samples (residual) -> coefficients.  Two passes, >>6 then >>14,
// with the first-pass result clamped to int16.
void fdct8x8(const i32 src[64], i16 dst[64]);
// Inverse: dequantized coefficients -> residual.  Two passes, >>7 then >>13,
// with the first-pass result clamped to int16.
void idct8x8(const i32 src[64], i32 dst[64]);

// The 4x4 pair, on one quadrant of a split block.  Its shift chain differs
// from the 8x8's by one: forward >>6 then >>13 (total 19, not 20) and inverse
// >>7 then >>14 (total 21).  The round trip is still unit gain, but the extra
// factor of 2 on the coefficients puts a 4x4 coefficient on the same scale as
// the 8x8 coefficient of the same residual amplitude, so one quantizer step
// table and one set of weights serve both.  See docs/SYNTAX.md 6.7.
void fdct4x4(const i32 src[16], i16 dst[16]);
void idct4x4(const i32 src[16], i32 dst[16]);

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
