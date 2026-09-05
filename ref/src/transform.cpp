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

// ================================================== XFORM_FAST (tool bit 28)
//
// The multiply-free 8x8 transform.  Its flow graph is the H.264/AVC High
// profile 8x8 butterfly (ITU-T H.264 8.5.13.2): every operation is an add, a
// subtract, or a shift by 1 or 2.  There is no multiply anywhere in the
// inverse, and none in the forward either.
//
// The graph's basis is EXACTLY orthogonal -- the Gram matrix of the eight
// columns is diagonal, with no residue at all -- but it is not orthonormal.
// Writing M for the inverse graph's matrix (column k = the graph applied to
// the unit vector e_k), the squared column norms are
//
//     ||M[:,k]||^2  =  8       for k = 0, 4
//                      289/32  for k = 1, 3, 5, 7
//                      5       for k = 2, 6
//
// so M = N diag(g) with N orthonormal and g_k the three norms above.  A 2D
// transform therefore has gain g_u * g_v at coefficient position (u, v), and
// the correction is a per-position scale that FACTORISES -- which is why it
// can be folded into the dequantizer, where it is free: the step table is
// built once per plane and the per-coefficient work (one multiply and one
// shift) is exactly what the Loeffler path already does.
//
// SCALE.  Dequantized coefficients on this path are on the scale
//
//     dq[u][v] = c[u][v] * 8 / (g_u * g_v)          c = orthonormal DCT-II
//
// i.e. kXfsScale[i]/1024 = 8/(g_u g_v), between 0.886 and 1.600.  The "8" is
// the largest power of two that keeps the largest scale small enough for the
// int32 bound below; it also means the fast path's dequantized values carry
// at least as many fractional bits as the Loeffler path's, so the extra
// rounding this fold could have cost is zero at 60 of the 64 positions and
// negative (finer) at the other four.
//
// RANGE, dequantizer.  With qstep <= 23170 and w <= 32 (SYNTAX 6.5) the
// unclamped step is at most (23170*32*1638 + 8192) >> 14 = 74126, which with
// the legal level bound |q| <= 32767 would give q*t = 2.43e9 -- outside int32.
// The step is therefore clamped to kXfsTMax = 63744, which bounds
// q*t + 8 <= 32767*63744 + 8 = 2088699656 < 2^31 - 1.  The clamp binds for
// exactly 7 of the 12288 (qp, w, position) combinations -- QP 62 and 63 with
// a weight of 28 or more at the four positions (u,v) in {2,6}x{2,6} -- and is
// a no-op everywhere else.  Both sides apply it, so it is bit-exact.
//
// RANGE, inverse transform.  The row pass runs on dq << 3 with |dq| <= 32768,
// and the largest absolute row sum of M is 7.375, so the row outputs are
// bounded by 8 * 32768 * 7.375 = 1.93e6.  They are clamped to int16 for the
// transpose exactly as the Loeffler path clamps its own, which bounds the
// column pass by 32768 * 7.375 = 2.42e5.  Nothing on this path comes within
// three decimal orders of int32, and there is no place where a product can:
// there are no products.
//
// SHIFT CHAIN.  The row pass left-shifts by 3 and does not shift back, so the
// transpose buffer carries three fractional bits (the same trick as the
// Loeffler path's 7/13 split); the column pass ends with (x + 32) >> 6.  The
// total is dq * 2^3 * g_u * g_v / 2^6 = dq * g_u g_v / 8, which with the
// dequantizer scale above is exactly the orthonormal inverse.  A dequantized
// DC of 1024 reconstructs a flat 128, as on the Loeffler path.

// Q10 per-position dequantizer scale, round(1024 * 8 / (g_u * g_v)).
const u16 kXfsScale[64] = {
    1024,  964, 1295,  964, 1024,  964, 1295,  964,
     964,  907, 1219,  907,  964,  907, 1219,  907,
    1295, 1219, 1638, 1219, 1295, 1219, 1638, 1219,
     964,  907, 1219,  907,  964,  907, 1219,  907,
    1024,  964, 1295,  964, 1024,  964, 1295,  964,
     964,  907, 1219,  907,  964,  907, 1219,  907,
    1295, 1219, 1638, 1219, 1295, 1219, 1638, 1219,
     964,  907, 1219,  907,  964,  907, 1219,  907,
};

// Q15 per-position forward scale, round(32768 * 8 / (g_u^2 * g_v^2)).  The
// forward graph produces c * g_u g_v; this brings it to the dq scale above.
// Encoder only, informative.
const u16 kXfsFwdScale[64] = {
    4096, 3628, 6554, 3628, 4096, 3628, 6554, 3628,
    3628, 3214, 5805, 3214, 3628, 3214, 5805, 3214,
    6554, 5805,10486, 5805, 6554, 5805,10486, 5805,
    3628, 3214, 5805, 3214, 3628, 3214, 5805, 3214,
    4096, 3628, 6554, 3628, 4096, 3628, 6554, 3628,
    3628, 3214, 5805, 3214, 3628, 3214, 5805, 3214,
    6554, 5805,10486, 5805, 6554, 5805,10486, 5805,
    3628, 3214, 5805, 3214, 3628, 3214, 5805, 3214,
};

// Inverse 1D: 32 adds and 10 shifts, no multiply.  H.264 8.5.13.2.
static inline void idct8_1d_fast(const i32 *d, i32 *y) {
    const i32 d0 = d[0], d1 = d[1], d2 = d[2], d3 = d[3];
    const i32 d4 = d[4], d5 = d[5], d6 = d[6], d7 = d[7];
    const i32 e0 = d0 + d4;
    const i32 e1 = -d3 + d5 - d7 - (d7 >> 1);
    const i32 e2 = d0 - d4;
    const i32 e3 = d1 + d7 - d3 - (d3 >> 1);
    const i32 e4 = (d2 >> 1) - d6;
    const i32 e5 = -d1 + d7 + d5 + (d5 >> 1);
    const i32 e6 = d2 + (d6 >> 1);
    const i32 e7 = d3 + d5 + d1 + (d1 >> 1);
    const i32 f0 = e0 + e6, f1 = e1 + (e7 >> 2);
    const i32 f2 = e2 + e4, f3 = e3 + (e5 >> 2);
    const i32 f4 = e2 - e4, f5 = (e3 >> 2) - e5;
    const i32 f6 = e0 - e6, f7 = e7 - (e1 >> 2);
    y[0] = f0 + f7; y[7] = f0 - f7;
    y[1] = f2 + f5; y[6] = f2 - f5;
    y[2] = f4 + f3; y[5] = f4 - f3;
    y[3] = f6 + f1; y[4] = f6 - f1;
}

// Forward 1D: the transpose of the graph above, and numerically exactly M^T.
static inline void fdct8_1d_fast(const i32 *s, i32 *d) {
    const i32 a0 = s[0] + s[7], a1 = s[1] + s[6];
    const i32 a2 = s[2] + s[5], a3 = s[3] + s[4];
    const i32 a4 = s[0] - s[7], a5 = s[1] - s[6];
    const i32 a6 = s[2] - s[5], a7 = s[3] - s[4];
    const i32 b0 = a0 + a3, b1 = a1 + a2;
    const i32 b2 = a0 - a3, b3 = a1 - a2;
    d[0] = b0 + b1;
    d[4] = b0 - b1;
    d[2] = b2 + (b3 >> 1);
    d[6] = (b2 >> 1) - b3;
    const i32 b4 = a5 + a6 + ((a4 >> 1) + a4);
    const i32 b5 = a4 - a7 - ((a6 >> 1) + a6);
    const i32 b6 = a4 + a7 - ((a5 >> 1) + a5);
    const i32 b7 = a5 - a6 + ((a7 >> 1) + a7);
    d[1] = b4 + (b7 >> 2);
    d[3] = b5 + (b6 >> 2);
    d[5] = b6 - (b5 >> 2);
    d[7] = (b4 >> 2) - b7;
}

void idct8x8_fast(const i32 src[64], i32 dst[64]) {
    i32 tmp[64];
    i32 in[8], out[8];
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = src[r * 8 + c] << 3;
        idct8_1d_fast(in, out);
        for (int c = 0; c < 8; ++c) tmp[c * 8 + r] = clamp16(out[c]);
    }
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        idct8_1d_fast(in, out);
        for (int c = 0; c < 8; ++c)
            dst[c * 8 + r] = clamp16((out[c] + 32) >> 6);
    }
}

void fdct8x8_fast(const i32 src[64], i16 dst[64]) {
    // Encoder only.  `src` is a residual, |src| <= 255, so the first pass is
    // bounded by 64 * 255 * 7.375 and the transpose buffer by 4 * 255 * 8.
    i32 tmp[64];
    i32 in[8], out[8];
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = src[r * 8 + c] << 6;
        fdct8_1d_fast(in, out);
        for (int c = 0; c < 8; ++c)
            tmp[c * 8 + r] = clamp16((out[c] + 8) >> 4);
    }
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        fdct8_1d_fast(in, out);
        for (int c = 0; c < 8; ++c) {
            // (u, v) = (c, r): the store is transposed.
            const i64 s = (i64)out[c] * kXfsFwdScale[c * 8 + r];
            dst[c * 8 + r] = (i16)clamp16((i32)((s + (1 << 16)) >> 17));
        }
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
