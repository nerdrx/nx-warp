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

// The odd half of the length-16 and length-32 transforms, entry [n][j] =
// round(512 * cos(pi * (2n+1) * (2j+1) / (2N))).  SYNTAX.md 6.2.1; regenerated
// and checked against that formula by tests/ref/test_transform.cpp.
extern const i16 kOdd16[8][8];
extern const i16 kOdd32[16][16];

// Transform edges the format defines, and the largest of them.
constexpr int kMaxBlock = 32;
inline bool block_size_ok(int n) { return n == 8 || n == 16 || n == 32; }

// Forward: samples (residual) -> coefficients, `n` x `n`, raster order.
// Inverse: dequantized coefficients -> residual.  Two passes each, with the
// first-pass result clamped to int16.  Shifts per size: SYNTAX.md 6.3.
void fdct_block(const i32 *src, i16 *dst, int n);
void idct_block(const i32 *src, i32 *dst, int n);

// The 8x8 pair, unchanged: exactly fdct_block/idct_block with n == 8.
inline void fdct8x8(const i32 src[64], i16 dst[64]) { fdct_block(src, dst, 8); }
inline void idct8x8(const i32 src[64], i32 dst[64]) { idct_block(src, dst, 8); }

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
