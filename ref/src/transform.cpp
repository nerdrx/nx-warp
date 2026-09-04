#include "transform.h"

namespace nxvc {

// The odd halves of the length-16 and length-32 transforms.  Entry [n][j] is
// round(512 * cos(pi * (2n+1) * (2j+1) / (2N))); tests/ref/test_transform.cpp
// regenerates both from that formula.  Max row absolute sum: 2613 and 5215.
const i16 kOdd16[8][8] = {
    {  510,   490,   452,   396,   325,   241,   149,    50},
    {  490,   325,    50,  -241,  -452,  -510,  -396,  -149},
    {  452,    50,  -396,  -490,  -149,   325,   510,   241},
    {  396,  -241,  -490,    50,   510,   149,  -452,  -325},
    {  325,  -452,  -149,   510,   -50,  -490,   241,   396},
    {  241,  -510,   325,   149,  -490,   396,    50,  -452},
    {  149,  -396,   510,  -452,   241,    50,  -325,   490},
    {   50,  -149,   241,  -325,   396,  -452,   490,  -510},
};
const i16 kOdd32[16][16] = {
    {  511,   506,   497,   482,   463,   439,   411,   379,   344,   305,   263,   219,   172,   124,    75,    25},
    {  506,   463,   379,   263,   124,   -25,  -172,  -305,  -411,  -482,  -511,  -497,  -439,  -344,  -219,   -75},
    {  497,   379,   172,   -75,  -305,  -463,  -511,  -439,  -263,   -25,   219,   411,   506,   482,   344,   124},
    {  482,   263,   -75,  -379,  -511,  -411,  -124,   219,   463,   497,   305,   -25,  -344,  -506,  -439,  -172},
    {  463,   124,  -305,  -511,  -344,    75,   439,   482,   172,  -263,  -506,  -379,    25,   411,   497,   219},
    {  439,   -25,  -463,  -411,    75,   482,   379,  -124,  -497,  -344,   172,   506,   305,  -219,  -511,  -263},
    {  411,  -172,  -511,  -124,   439,   379,  -219,  -506,   -75,   463,   344,  -263,  -497,   -25,   482,   305},
    {  379,  -305,  -439,   219,   482,  -124,  -506,    25,   511,    75,  -497,  -172,   463,   263,  -411,  -344},
    {  344,  -411,  -263,   463,   172,  -497,   -75,   511,   -25,  -506,   124,   482,  -219,  -439,   305,   379},
    {  305,  -482,   -25,   497,  -263,  -344,   463,    75,  -506,   219,   379,  -439,  -124,   511,  -172,  -411},
    {  263,  -511,   219,   305,  -506,   172,   344,  -497,   124,   379,  -482,    75,   411,  -463,    25,   439},
    {  219,  -497,   411,   -25,  -379,   506,  -263,  -172,   482,  -439,    75,   344,  -511,   305,   124,  -463},
    {  172,  -439,   506,  -344,    25,   305,  -497,   463,  -219,  -124,   411,  -511,   379,   -75,  -263,   482},
    {  124,  -344,   482,  -506,   411,  -219,   -25,   263,  -439,   511,  -463,   305,   -75,  -172,   379,  -497},
    {   75,  -219,   344,  -439,   497,  -511,   482,  -411,   305,  -172,    25,   124,  -263,   379,  -463,   506},
    {   25,   -75,   124,  -172,   219,  -263,   305,  -344,   379,  -411,   439,  -463,   482,  -497,   506,  -511},
};

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

// ------------------------------------------------- the even/odd recursion
// A length-2M DCT-III splits into the length-M DCT-III of the even-indexed
// coefficients plus a dense M x M rotation of the odd-indexed ones
// (SYNTAX.md 6.2.1).  Written with the 512-scaled constants of `kOdd*`, the
// even half needs no rescaling at all: the length-2M transform simply has
// sqrt(2) times the gain of the length-M one it is built from.  So
//
//     gain(8) = 2^10,  gain(16) = 2^10 * sqrt(2),  gain(32) = 2^11
//
// per dimension, and the two-dimensional gains are the exact powers 2^20,
// 2^21 and 2^22 that the shift chains of `kInvShift*` undo.
#define NXVC_EVEN_ODD_INVERSE(HALF, INNER, ODD)                            \
    i32 xe[HALF], e[HALF];                                                 \
    for (int k = 0; k < HALF; ++k) xe[k] = x[2 * k];                       \
    INNER(xe, e);                                                          \
    for (int n = 0; n < HALF; ++n) {                                       \
        i32 o = 0;                                                         \
        for (int j = 0; j < HALF; ++j) o += x[2 * j + 1] * ODD[n][j];      \
        y[n] = e[n] + o;                                                   \
        y[2 * HALF - 1 - n] = e[n] - o;                                    \
    }

// The exact transpose of the above: the butterfly first, then the length-M
// forward transform on the sums and the transposed rotation on the
// differences.
#define NXVC_EVEN_ODD_FORWARD(HALF, INNER, ODD)                            \
    i32 u[HALF], v[HALF], xe[HALF];                                        \
    for (int n = 0; n < HALF; ++n) {                                       \
        u[n] = y[n] + y[2 * HALF - 1 - n];                                 \
        v[n] = y[n] - y[2 * HALF - 1 - n];                                 \
    }                                                                      \
    INNER(u, xe);                                                          \
    for (int k = 0; k < HALF; ++k) x[2 * k] = xe[k];                       \
    for (int j = 0; j < HALF; ++j) {                                       \
        i32 o = 0;                                                         \
        for (int n = 0; n < HALF; ++n) o += v[n] * ODD[n][j];              \
        x[2 * j + 1] = o;                                                  \
    }

// |even| <= 1.1e8 and |odd| <= 32767 * 2613 = 8.6e7, so |y| <= 2.0e8.
static inline void idct16_1d(const i32 *x, i32 *y) {
    NXVC_EVEN_ODD_INVERSE(8, idct8_1d, kOdd16)
}
static inline void fdct16_1d(const i32 *y, i32 *x) {
    NXVC_EVEN_ODD_FORWARD(8, fdct8_1d, kOdd16)
}
// |even| <= 2.0e8 and |odd| <= 32767 * 5215 = 1.7e8, so |y| <= 3.7e8.
static inline void idct32_1d(const i32 *x, i32 *y) {
    NXVC_EVEN_ODD_INVERSE(16, idct16_1d, kOdd32)
}
static inline void fdct32_1d(const i32 *y, i32 *x) {
    NXVC_EVEN_ODD_FORWARD(16, fdct16_1d, kOdd32)
}

#undef NXVC_EVEN_ODD_INVERSE
#undef NXVC_EVEN_ODD_FORWARD

// ------------------------------------------------------- the 2D transforms
// Indexed by log2(n) - 3.  Both passes of both directions write transposed,
// so `dst` comes out in the opposite order to `src` and two passes restore
// it.  The shifts of a column sum to log2 of that size's 2D gain, which is
// what makes every size unit gain; SYNTAX.md 6.3.
//
// The first-pass shift grows by one per size because the value entering it
// grows by exactly a factor of two per size (one more butterfly level), so
// all three sizes leave the same margin under the int16 clamp of the
// transpose buffer -- which is what lets a GPU hold that buffer in int16 LDS
// at every size.
static const int kInvShift1[3] = {7, 7, 8};
static const int kInvShift2[3] = {13, 14, 14};
static const int kFwdShift1[3] = {6, 7, 8};
static const int kFwdShift2[3] = {14, 14, 14};

static inline int size_index(int n) { return n == 8 ? 0 : (n == 16 ? 1 : 2); }

static inline void idct_1d(const i32 *x, i32 *y, int n) {
    if (n == 8) idct8_1d(x, y);
    else if (n == 16) idct16_1d(x, y);
    else idct32_1d(x, y);
}
static inline void fdct_1d(const i32 *y, i32 *x, int n) {
    if (n == 8) fdct8_1d(y, x);
    else if (n == 16) fdct16_1d(y, x);
    else fdct32_1d(y, x);
}

void idct_block(const i32 *src, i32 *dst, int n) {
    const int s1 = kInvShift1[size_index(n)], s2 = kInvShift2[size_index(n)];
    i32 tmp[kMaxBlock * kMaxBlock];
    i32 in[kMaxBlock], out[kMaxBlock];
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = src[r * n + c];
        idct_1d(in, out, n);
        for (int c = 0; c < n; ++c)
            tmp[c * n + r] = clamp16((out[c] + (1 << (s1 - 1))) >> s1);
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = tmp[r * n + c];
        idct_1d(in, out, n);
        for (int c = 0; c < n; ++c)
            dst[c * n + r] = clamp16((out[c] + (1 << (s2 - 1))) >> s2);
    }
}

void fdct_block(const i32 *src, i16 *dst, int n) {
    const int s1 = kFwdShift1[size_index(n)], s2 = kFwdShift2[size_index(n)];
    i32 tmp[kMaxBlock * kMaxBlock];
    i32 in[kMaxBlock], out[kMaxBlock];
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = src[r * n + c];
        fdct_1d(in, out, n);
        for (int c = 0; c < n; ++c)
            tmp[c * n + r] = clamp16((out[c] + (1 << (s1 - 1))) >> s1);
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = tmp[r * n + c];
        fdct_1d(in, out, n);
        for (int c = 0; c < n; ++c)
            dst[c * n + r] = (i16)clamp16((out[c] + (1 << (s2 - 1))) >> s2);
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
