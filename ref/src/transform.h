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

// 4x4 constants, from the same construction: the 1D inverse matrix is
// M[n][k] = round(1024 * c_k * cos(pi*(2n+1)*k/8)) with c_0 = 1/2 and
// c_k = 1/sqrt(2), so the 1D gain is exactly 2^10, the same as the 8x8's.
// Only three distinct magnitudes appear.  See docs/SYNTAX.md 6.7.
constexpr i32 kD0 = 512;  // 1024 * 1/2
constexpr i32 kD1 = 669;  // round(1024 * cos(pi/8)  / sqrt(2))
constexpr i32 kD2 = 277;  // round(1024 * cos(3pi/8) / sqrt(2))

// Forward: samples (residual) -> coefficients.  Two passes, >>6 then >>14,
// with the first-pass result clamped to int16.
void fdct8x8(const i32 src[64], i16 dst[64]);
// Inverse: dequantized coefficients -> residual.  Two passes, >>7 then >>13,
// with the first-pass result clamped to int16.
void idct8x8(const i32 src[64], i32 dst[64]);

// The 4x4 pair, tool bit 19 (XFORM_4X4_SPLIT).  Same 1D gain (2^10) and
// therefore the same shift chain and the same quantiser scale as the 8x8, so
// no second dequantiser and no second weighting-matrix family exist.
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
