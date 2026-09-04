#include "transform.h"

namespace nxvc {

// ------------------------------------------------------------------ 1D DCT
// Exactly ((s * kC4) + 256) >> 9, computed without an int32 overflow.
//
// In the inverse flow graph the odd-part rotation operand `P +- Q` reaches
// +-8.6e7 on legal (if pathological) int16 input, and 8.6e7 * 362 is 3.1e10 --
// outside int32.  Split the operand as s = 512*hi + lo with hi = s >> 9
// (arithmetic) and lo = s & 511; then
//
//     (s*362 + 256) >> 9 == hi*362 + ((lo*362 + 256) >> 9)
//
// because 512*hi*362 is an exact multiple of 512, so the shift distributes.
// This is a two-word product, not an approximation: it is bit-identical to the
// mathematical value for every int32 `s` the graph can produce, so no
// conformance vector changes.  Both partial products are small: |hi*362| <=
// 6.1e7 and lo*362 <= 1.9e5.  A GPU implementation may use the same identity
// or a 64-bit multiply; the result is defined to be the exact value.
static inline i32 mul_c4_rnd9(i32 s) {
    const i32 hi = s >> 9, lo = s & 511;
    return hi * kC4 + ((lo * kC4 + 256) >> 9);
}

// Inverse 1D transform, gain 2^10 relative to the orthonormal DCT-III.
static inline void idct8_1d(const i32 *x, i32 *y) {
    // even part
    i32 t0 = (x[0] + x[4]) * kC4;
    i32 t1 = (x[0] - x[4]) * kC4;
    i32 t2 = x[2] * kS2 - x[6] * kC2;
    i32 t3 = x[2] * kC2 + x[6] * kS2;
    i32 e0 = t0 + t3, e3 = t0 - t3;
    i32 e1 = t1 + t2, e2 = t1 - t2;
    // odd part
    i32 A = x[1] * kA1 + x[7] * kA7;
    i32 B = x[1] * kA7 - x[7] * kA1;
    i32 C = x[3] * kA3 + x[5] * kA5;
    i32 D = x[3] * kA5 - x[5] * kA3;
    i32 O0 = A + C;
    i32 O3 = B - D;
    i32 P = A - C, Q = B + D;
    i32 O1 = mul_c4_rnd9(P + Q);
    i32 O2 = mul_c4_rnd9(P - Q);
    y[0] = e0 + O0; y[7] = e0 - O0;
    y[1] = e1 + O1; y[6] = e1 - O1;
    y[2] = e2 + O2; y[5] = e2 - O2;
    y[3] = e3 + O3; y[4] = e3 - O3;
}

// Forward 1D transform: the exact transpose of the flow graph above.
static inline void fdct8_1d(const i32 *y, i32 *x) {
    i32 e0 = y[0] + y[7], O0 = y[0] - y[7];
    i32 e1 = y[1] + y[6], O1 = y[1] - y[6];
    i32 e2 = y[2] + y[5], O2 = y[2] - y[5];
    i32 e3 = y[3] + y[4], O3 = y[3] - y[4];
    i32 P = mul_c4_rnd9(O1 + O2);
    i32 Q = mul_c4_rnd9(O1 - O2);
    i32 A = O0 + P, C = O0 - P;
    i32 B = O3 + Q, D = Q - O3;
    x[1] = A * kA1 + B * kA7;
    x[7] = A * kA7 - B * kA1;
    x[3] = C * kA3 + D * kA5;
    x[5] = C * kA5 - D * kA3;
    i32 t0 = e0 + e3, t3 = e0 - e3;
    i32 t1 = e1 + e2, t2 = e1 - e2;
    x[0] = (t0 + t1) * kC4;
    x[4] = (t0 - t1) * kC4;
    x[2] = t2 * kS2 + t3 * kC2;
    x[6] = t3 * kS2 - t2 * kC2;
}

// ------------------------------------------------- the 16- and 32-point odd
// kernels.  K_N[j][m] = round(512 * cos((2m+1)(2j+1) pi / (2N))) is the odd
// half of T_N, i.e. row 2j+1 restricted to its first N/2 columns.  The even
// half needs no table: it *is* T_{N/2}, so the recursion below calls itself.
// docs/SYNTAX.md 6.1.
constexpr i32 kOdd16[8][8] = {
    { 510,  490,  452,  396,  325,  241,  149,   50},
    { 490,  325,   50, -241, -452, -510, -396, -149},
    { 452,   50, -396, -490, -149,  325,  510,  241},
    { 396, -241, -490,   50,  510,  149, -452, -325},
    { 325, -452, -149,  510,  -50, -490,  241,  396},
    { 241, -510,  325,  149, -490,  396,   50, -452},
    { 149, -396,  510, -452,  241,   50, -325,  490},
    {  50, -149,  241, -325,  396, -452,  490, -510},
};
constexpr i32 kOdd32[16][16] = {
    { 511,  506,  497,  482,  463,  439,  411,  379,
      344,  305,  263,  219,  172,  124,   75,   25},
    { 506,  463,  379,  263,  124,  -25, -172, -305,
     -411, -482, -511, -497, -439, -344, -219,  -75},
    { 497,  379,  172,  -75, -305, -463, -511, -439,
     -263,  -25,  219,  411,  506,  482,  344,  124},
    { 482,  263,  -75, -379, -511, -411, -124,  219,
      463,  497,  305,  -25, -344, -506, -439, -172},
    { 463,  124, -305, -511, -344,   75,  439,  482,
      172, -263, -506, -379,   25,  411,  497,  219},
    { 439,  -25, -463, -411,   75,  482,  379, -124,
     -497, -344,  172,  506,  305, -219, -511, -263},
    { 411, -172, -511, -124,  439,  379, -219, -506,
      -75,  463,  344, -263, -497,  -25,  482,  305},
    { 379, -305, -439,  219,  482, -124, -506,   25,
      511,   75, -497, -172,  463,  263, -411, -344},
    { 344, -411, -263,  463,  172, -497,  -75,  511,
      -25, -506,  124,  482, -219, -439,  305,  379},
    { 305, -482,  -25,  497, -263, -344,  463,   75,
     -506,  219,  379, -439, -124,  511, -172, -411},
    { 263, -511,  219,  305, -506,  172,  344, -497,
      124,  379, -482,   75,  411, -463,   25,  439},
    { 219, -497,  411,  -25, -379,  506, -263, -172,
      482, -439,   75,  344, -511,  305,  124, -463},
    { 172, -439,  506, -344,   25,  305, -497,  463,
     -219, -124,  411, -511,  379,  -75, -263,  482},
    { 124, -344,  482, -506,  411, -219,  -25,  263,
     -439,  511, -463,  305,  -75, -172,  379, -497},
    {  75, -219,  344, -439,  497, -511,  482, -411,
      305, -172,   25,  124, -263,  379, -463,  506},
    {  25,  -75,  124, -172,  219, -263,  305, -344,
      379, -411,  439, -463,  482, -497,  506, -511},
};

// K_N as a flat row-major array of (N/2)^2 entries.
static inline const i32 *odd_kernel(int n) {
    return n == 16 ? &kOdd16[0][0] : &kOdd32[0][0];
}

// Inverse 1D transform of length `n` (8, 16 or 32), gain 512*sqrt(n/2)
// relative to the orthonormal DCT-III.  The even-indexed coefficients drive
// the next size down; the odd-indexed ones drive K_N.  SYNTAX.md 6.2.
//
// Range: with |x| <= 32768 (the dequantizer's clamp), |y| <= 32768 *
// max_n sum_k |T_N[k][n]|, which is 8.9e7, 1.8e8 and 3.5e8 for n = 8, 16, 32
// -- all inside int32, and all reached by the saturation vector.
static void idct_1d(int n, const i32 *x, i32 *y) {
    if (n == kBlock) {
        idct8_1d(x, y);
        return;
    }
    const int h = n >> 1;
    const i32 *k = odd_kernel(n);
    i32 xe[kMaxXform / 2] = {}, e[kMaxXform / 2] = {};
    for (int j = 0; j < h; ++j) xe[j] = x[2 * j];
    idct_1d(h, xe, e);
    for (int m = 0; m < h; ++m) {
        i32 o = 0;
        for (int j = 0; j < h; ++j) o += k[j * h + m] * x[2 * j + 1];
        y[m] = e[m] + o;
        y[n - 1 - m] = e[m] - o;
    }
}

// Forward 1D transform: the exact transpose of the flow graph above.
static void fdct_1d(int n, const i32 *y, i32 *x) {
    if (n == kBlock) {
        fdct8_1d(y, x);
        return;
    }
    const int h = n >> 1;
    const i32 *k = odd_kernel(n);
    i32 E[kMaxXform / 2] = {}, O[kMaxXform / 2] = {}, xe[kMaxXform / 2] = {};
    for (int m = 0; m < h; ++m) {
        E[m] = y[m] + y[n - 1 - m];
        O[m] = y[m] - y[n - 1 - m];
    }
    fdct_1d(h, E, xe);
    for (int j = 0; j < h; ++j) {
        x[2 * j] = xe[j];
        i32 o = 0;
        for (int m = 0; m < h; ++m) o += k[j * h + m] * O[m];
        x[2 * j + 1] = o;
    }
}

// The 2D transforms.  The 2D gain is exactly 2^(17 + log2 n) at every size,
// so the two shifts of a direction always sum to that; the second-pass shift
// is the same at every size and the first-pass shift grows by one per size
// doubling, which keeps the transposed intermediate at a constant scale.
// SYNTAX.md 6.3.  For n = 8 these are the version 1 shifts 6/14 and 7/13.
void fdct_2d(int n, const i32 *src, i16 *dst) {
    const int shift1 = 3 + xform_log2(n);
    const i32 rnd1 = 1 << (shift1 - 1);
    i32 tmp[kMaxXform * kMaxXform];
    i32 in[kMaxXform], out[kMaxXform];
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = src[r * n + c];
        fdct_1d(n, in, out);
        for (int c = 0; c < n; ++c)
            tmp[c * n + r] = clamp16((out[c] + rnd1) >> shift1);  // transposed
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = tmp[r * n + c];
        fdct_1d(n, in, out);
        for (int c = 0; c < n; ++c)
            dst[c * n + r] = (i16)clamp16((out[c] + 8192) >> 14);
    }
}

void idct_2d(int n, const i32 *src, i32 *dst) {
    const int shift1 = 4 + xform_log2(n);
    const i32 rnd1 = 1 << (shift1 - 1);
    i32 tmp[kMaxXform * kMaxXform];
    i32 in[kMaxXform], out[kMaxXform];
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = src[r * n + c];
        idct_1d(n, in, out);
        for (int c = 0; c < n; ++c)
            tmp[c * n + r] = clamp16((out[c] + rnd1) >> shift1);
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = tmp[r * n + c];
        idct_1d(n, in, out);
        for (int c = 0; c < n; ++c)
            dst[c * n + r] = clamp16((out[c] + 4096) >> 13);
    }
}

// -------------------------------------------------------------- resampling
template <typename T>
static inline i32 bilinear_impl(const T *src, int w, int h, int stride, i32 sx,
                                i32 sy) {
    i32 x0 = sx >> 4, y0 = sy >> 4;
    i32 fx = sx & 15, fy = sy & 15;
    i32 x1 = x0 + 1, y1 = y0 + 1;
    x0 = clamp_i32(x0, 0, w - 1);
    x1 = clamp_i32(x1, 0, w - 1);
    y0 = clamp_i32(y0, 0, h - 1);
    y1 = clamp_i32(y1, 0, h - 1);
    i32 p00 = src[y0 * stride + x0], p01 = src[y0 * stride + x1];
    i32 p10 = src[y1 * stride + x0], p11 = src[y1 * stride + x1];
    i32 wx0 = 16 - fx, wy0 = 16 - fy;
    return (p00 * wx0 * wy0 + p01 * fx * wy0 + p10 * wx0 * fy + p11 * fx * fy +
            128) >> 8;
}

i32 bilinear_q4(const u8 *src, int w, int h, int stride, i32 sx, i32 sy) {
    return bilinear_impl<u8>(src, w, h, stride, sx, sy);
}
i32 bilinear_q4_i32(const i32 *src, int w, int h, int stride, i32 sx, i32 sy) {
    return bilinear_impl<i32>(src, w, h, stride, sx, sy);
}

void upsample(const u8 *src, int w, int h, int sstride, u8 *dst, int dstride,
              int factor) {
    // Half-phase mapping: source = (out + 0.5)/factor - 0.5, in Q4.
    //   factor 2: sx = 8*x - 4      factor 4: sx = 4*x - 6
    const i32 mul = 16 / factor;
    const i32 off = mul / 2 - 8;
    for (int y = 0; y < h * factor; ++y) {
        i32 sy = mul * y + off;
        for (int x = 0; x < w * factor; ++x) {
            i32 sx = mul * x + off;
            dst[y * dstride + x] = (u8)clamp_i32(
                bilinear_q4(src, w, h, sstride, sx, sy), 0, 255);
        }
    }
}

void downsample(const u8 *src, int w, int h, int sstride, u8 *dst, int dstride,
                int factor) {
    const i32 n = factor * factor;
    const i32 rnd = n / 2;
    const int shift = factor == 2 ? 2 : 4;
    for (int y = 0; y < h / factor; ++y)
        for (int x = 0; x < w / factor; ++x) {
            i32 s = 0;
            for (int j = 0; j < factor; ++j)
                for (int i = 0; i < factor; ++i)
                    s += src[(y * factor + j) * sstride + x * factor + i];
            dst[y * dstride + x] = (u8)((s + rnd) >> shift);
        }
}

}  // namespace nxvc
