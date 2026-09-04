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

void fdct8x8(const i32 src[64], i16 dst[64]) {
    i32 tmp[64];
    i32 in[8], out[8];
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = src[r * 8 + c];
        fdct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            tmp[c * 8 + r] = clamp16((out[c] + 32) >> 6);  // transposed
    }
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        fdct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            dst[c * 8 + r] = (i16)clamp16((out[c] + 8192) >> 14);
    }
}

void idct8x8(const i32 src[64], i32 dst[64]) {
    i32 tmp[64];
    i32 in[8], out[8];
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = src[r * 8 + c];
        idct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            tmp[c * 8 + r] = clamp16((out[c] + 64) >> 7);
    }
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        idct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            dst[c * 8 + r] = clamp16((out[c] + 4096) >> 13);
    }
}

// ------------------------------------------------------------- 4-point DCT
// Inverse 1D transform, gain 2^10 relative to the orthonormal DCT-III -- the
// same gain the 8-point graph above has, which is what kD0..kD2 were rescaled
// for.  Every product is small: |x| <= 32768 and 32768*669 = 2.2e7, so unlike
// the 8-point odd part this graph needs no two-word product.
static inline void idct4_1d(const i32 *x, i32 *y) {
    i32 t0 = (x[0] + x[2]) * kD0;
    i32 t1 = (x[0] - x[2]) * kD0;
    i32 t2 = x[1] * kD2 - x[3] * kD1;
    i32 t3 = x[1] * kD1 + x[3] * kD2;
    y[0] = t0 + t3; y[3] = t0 - t3;
    y[1] = t1 + t2; y[2] = t1 - t2;
}

// Forward 1D transform: the exact transpose of the flow graph above.
static inline void fdct4_1d(const i32 *y, i32 *x) {
    i32 t0 = y[0] + y[3], t3 = y[0] - y[3];
    i32 t1 = y[1] + y[2], t2 = y[1] - y[2];
    x[0] = (t0 + t1) * kD0;
    x[2] = (t0 - t1) * kD0;
    x[1] = t3 * kD1 + t2 * kD2;
    x[3] = t3 * kD2 - t2 * kD1;
}

void fdct4x4(const i32 src[16], i16 dst[16]) {
    i32 tmp[16];
    i32 in[4], out[4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) in[c] = src[r * 4 + c];
        fdct4_1d(in, out);
        for (int c = 0; c < 4; ++c)
            tmp[c * 4 + r] = clamp16((out[c] + 32) >> 6);  // transposed
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) in[c] = tmp[r * 4 + c];
        fdct4_1d(in, out);
        for (int c = 0; c < 4; ++c)
            dst[c * 4 + r] = (i16)clamp16((out[c] + 4096) >> 13);
    }
}

void idct4x4(const i32 src[16], i32 dst[16]) {
    i32 tmp[16];
    i32 in[4], out[4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) in[c] = src[r * 4 + c];
        idct4_1d(in, out);
        for (int c = 0; c < 4; ++c)
            tmp[c * 4 + r] = clamp16((out[c] + 64) >> 7);
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) in[c] = tmp[r * 4 + c];
        idct4_1d(in, out);
        for (int c = 0; c < 4; ++c)
            dst[c * 4 + r] = clamp16((out[c] + 8192) >> 14);
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
