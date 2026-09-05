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

// Forward: samples (residual) -> coefficients.  Two passes, >>6 then >>14,
// with the first-pass result clamped to int16.
void fdct8x8(const i32 src[64], i16 dst[64]);
// Inverse: dequantized coefficients -> residual.  Two passes, >>7 then >>13,
// with the first-pass result clamped to int16.
void idct8x8(const i32 src[64], i32 dst[64]);

// ------------------------------------------------------- XFORM_FAST (bit 28)
// A multiply-free 8x8 integer transform: the H.264/AVC High-profile 8x8
// butterfly, whose only operations are adds and shifts by 1 and 2.  Its basis
// is exactly orthogonal but not orthonormal -- three distinct column norms,
// sqrt(8), sqrt(289/32) and sqrt(5) -- so the norm correction lives in the
// dequantizer as a per-position Q10 scale, `kXfsScale`.  docs/SYNTAX.md 6.7.

// Q10 dequantizer scale, normative.  t = min((qstep*w*kXfsScale[i] + 8192)
// >> 14, kXfsTMax).
extern const u16 kXfsScale[64];
// The largest dequantizer step the fast path may produce.  See SYNTAX 6.7.
constexpr i32 kXfsTMax = 63744;
// Q15 forward scale, encoder only (informative).
extern const u16 kXfsFwdScale[64];

// Forward: samples (residual) -> coefficients on the fast path's own scale.
void fdct8x8_fast(const i32 src[64], i16 dst[64]);
// Inverse: dequantized coefficients -> residual.  Row pass on `dq << 3` with
// no shift, column pass with `(x + 32) >> 6`; both clamped to int16.
void idct8x8_fast(const i32 src[64], i32 dst[64]);

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
