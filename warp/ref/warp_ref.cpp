// NX Warp -- bit-exact CPU reference for the pose-warped predictor.
//
// This file is the normative implementation. It is deliberately written the
// way the GLSL kernel is written: no int64, no float, no division outside
// nxvc_warp_div(), arithmetic shifts spelled out. Every expression here has a
// line-for-line counterpart in warp/glsl/warp_tile.comp.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxvc/warp.h"

namespace nxvc::warp {

// ---------------------------------------------------------------------------
// Emulated 64-bit arithmetic on (hi, lo) uint32 pairs.
//
// The GLSL twin uses umulExtended()/imulExtended(), which are core GLSL 4.00
// and lower to OpSUMulExtended/OpSMulExtended -- no shaderInt64 capability.
// Here we spell out the same schoolbook 16x16 decomposition so the C++ path
// exercises identical logic rather than hiding behind the host's 64-bit ALU.
// ---------------------------------------------------------------------------

U64 nxvc_umul_ext(uint32_t a, uint32_t b) {
    const uint32_t a0 = a & 0xffffu, a1 = a >> 16;
    const uint32_t b0 = b & 0xffffu, b1 = b >> 16;
    const uint32_t p00 = a0 * b0;
    const uint32_t p01 = a0 * b1;
    const uint32_t p10 = a1 * b0;
    const uint32_t p11 = a1 * b1;
    // mid cannot overflow: (2^16-1) + 2*(2^16-1) < 2^18.
    const uint32_t mid = (p00 >> 16) + (p01 & 0xffffu) + (p10 & 0xffffu);
    U64 r;
    r.lo = (p00 & 0xffffu) | (mid << 16);
    r.hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
    return r;
}

U64 nxvc_imul_ext(int32_t a, int32_t b) {
    // Unsigned product of the two's-complement bit patterns, then the standard
    // sign correction: signed_hi = unsigned_hi - (a<0 ? b : 0) - (b<0 ? a : 0).
    U64 r = nxvc_umul_ext(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
    if (a < 0) r.hi -= static_cast<uint32_t>(b);
    if (b < 0) r.hi -= static_cast<uint32_t>(a);
    return r;
}

U64 nxvc_add64(U64 a, U64 b) {
    U64 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u);
    return r;
}

U64 nxvc_neg64(U64 a) {
    U64 r;
    r.lo = ~a.lo + 1u;
    r.hi = ~a.hi + (r.lo == 0u ? 1u : 0u);
    return r;
}

U64 nxvc_shl64(U64 a, uint32_t n) {
    U64 r;
    if (n == 0u) return a;
    r.hi = (a.hi << n) | (a.lo >> (32u - n));
    r.lo = a.lo << n;
    return r;
}

U64 nxvc_from_i32(int32_t v) {
    U64 r;
    r.lo = static_cast<uint32_t>(v);
    r.hi = v < 0 ? 0xffffffffu : 0u;
    return r;
}

// Fixed 32-iteration restoring division.
//
// Precondition n.hi < d (the caller guarantees it; warp_tile_corners()
// saturates instead of calling when it does not hold). The loop invariant
// rem < d then holds at every step, and because d < 2^30 the shifted remainder
// (rem << 1 | bit) stays below 2^31 and never wraps.
uint32_t nxvc_warp_div(U64 n, uint32_t d) {
    uint32_t rem = n.hi;
    uint32_t q = 0u;
    for (int k = 31; k >= 0; --k) {
        rem = (rem << 1) | ((n.lo >> static_cast<uint32_t>(k)) & 1u);
        // Branchless in the shader; written as a branch here for readability.
        // Both forms produce identical results.
        if (rem >= d) {
            rem -= d;
            q |= (1u << static_cast<uint32_t>(k));
        }
    }
    return q;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Arithmetic (sign-propagating) right shift. C++20 mandates this for signed
// operands; the shader uses OpShiftRightArithmetic, which is the same.
static inline int32_t sar(int32_t v, int n) {
    return v >> n;
}

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

const int8_t kCatmullRom[16][4] = {
    {   0,  64,   0,   0},  // f= 0
    {  -2,  64,   2,   0},  // f= 1
    {  -3,  61,   6,   0},  // f= 2
    {  -4,  59,  10,  -1},  // f= 3
    {  -5,  56,  15,  -2},  // f= 4
    {  -5,  51,  20,  -2},  // f= 5
    {  -5,  47,  25,  -3},  // f= 6
    {  -4,  41,  30,  -3},  // f= 7
    {  -4,  36,  36,  -4},  // f= 8
    {  -3,  30,  41,  -4},  // f= 9
    {  -3,  25,  47,  -5},  // f=10
    {  -2,  20,  51,  -5},  // f=11
    {  -2,  15,  56,  -5},  // f=12
    {  -1,  10,  59,  -4},  // f=13
    {   0,   6,  61,  -3},  // f=14
    {   0,   2,  64,  -2},  // f=15
};

Homography identity_homography(int32_t ox, int32_t oy) {
    Homography H{};
    H.h[0] = 1 << kQNum;
    H.h[1] = 0;
    H.h[2] = 0;
    H.h[3] = 0;
    H.h[4] = 1 << kQNum;
    H.h[5] = 0;
    H.h[6] = 0;
    H.h[7] = 0;
    H.h[8] = 1 << kQDen;
    H.ox = ox;
    H.oy = oy;
    return H;
}

// ---------------------------------------------------------------------------
// Step 1: the four tile-corner source coordinates.
//
//   num_x = h00*cx + h01*cy + h02        (Q10.21, 64-bit accumulator)
//   num_y = h10*cx + h11*cy + h12        (Q10.21)
//   den   = h20*cx + h21*cy + h22        (Q2.29,  fits int32 by construction)
//   X_q6  = round( (num_x << kDivShift) / den )  +  (ox << 6)
//
// One divide per numerator, four corners: eight divides per tile, none per
// pixel.
// ---------------------------------------------------------------------------

static inline int32_t corner_component(int32_t h_a, int32_t h_b, int32_t h_c,
                                       int32_t cx, int32_t cy,
                                       int32_t den, int32_t origin) {
    U64 num = nxvc_add64(nxvc_add64(nxvc_imul_ext(h_a, cx), nxvc_imul_ext(h_b, cy)),
                         nxvc_from_i32(h_c));

    const bool neg = (num.hi & 0x80000000u) != 0u;
    U64 mag = neg ? nxvc_neg64(num) : num;
    mag = nxvc_shl64(mag, static_cast<uint32_t>(kDivShift));

    // Round half away from zero: add den/2 to the magnitude before dividing.
    mag = nxvc_add64(mag, nxvc_from_i32(den >> 1));

    const uint32_t ud = static_cast<uint32_t>(den);
    int32_t v;
    if (mag.hi >= ud) {
        // Cannot happen for any homography that passes derive_homography()'s
        // validation; saturate deterministically rather than diverge.
        v = neg ? -kCornerClamp : kCornerClamp;
    } else {
        const uint32_t q = nxvc_warp_div(mag, ud);
        v = neg ? -static_cast<int32_t>(q) : static_cast<int32_t>(q);
    }
    v += origin << kQCorner;
    return clamp_i32(v, -kCornerClamp, kCornerClamp);
}

TileCorners warp_tile_corners(const Homography& H, int32_t tile_x, int32_t tile_y, Mode mode) {
    TileCorners c{};
    if (mode == kModeStatic) {
        // STATIC_MV: the identity predictor, exactly. No homography, no divide.
        for (int i = 0; i < 4; ++i) {
            c.x[i] = (tile_x + ((i & 1) ? kTile : 0)) << kQCorner;
            c.y[i] = (tile_y + ((i >> 1) ? kTile : 0)) << kQCorner;
        }
        return c;
    }
    for (int i = 0; i < 4; ++i) {
        const int32_t cx = tile_x + ((i & 1) ? kTile : 0) - H.ox;
        const int32_t cy = tile_y + ((i >> 1) ? kTile : 0) - H.oy;

        // den fits int32: |h20|,|h21| <= 2^31/2^12 by validation and
        // |cx|,|cy| <= 2^15 for any picture we stream.
        U64 d64 = nxvc_add64(nxvc_add64(nxvc_imul_ext(H.h[6], cx), nxvc_imul_ext(H.h[7], cy)),
                             nxvc_from_i32(H.h[8]));
        int32_t den = static_cast<int32_t>(d64.lo);
        const bool den_ok = (d64.hi == (den < 0 ? 0xffffffffu : 0u)) && den >= kDenMin &&
                            den < kDenMax;
        if (!den_ok) {
            // Behind the camera or outside the validated envelope. Saturate.
            c.x[i] = kCornerClamp;
            c.y[i] = kCornerClamp;
            continue;
        }
        c.x[i] = corner_component(H.h[0], H.h[1], H.h[2], cx, cy, den, H.ox);
        c.y[i] = corner_component(H.h[3], H.h[4], H.h[5], cx, cy, den, H.oy);
    }
    return c;
}

// ---------------------------------------------------------------------------
// Step 2: bilinear interpolation of the corner coordinates inside the tile.
//
// Two rounded steps rather than one, so that the intermediate stays small:
//
//   top = (X00*(64-u) + X10*u + 32) >> 6     <= 2^19,  intermediate <= 2^25
//   bot = (X01*(64-u) + X11*u + 32) >> 6     <= 2^19
//   X   = (top*(64-v) + bot*v   + 32) >> 6   <= 2^19,  intermediate <= 2^25
//
// A single-step form (multiply out, one >>12) would need the corners clamped
// to +-2^18 == +-4096 pel to stay inside int32, and a 4096-wide eye plus a few
// hundred pixels of warp displacement already passes that. The extra shift per
// axis buys 8x the coordinate range for two adds.
//
// Rounding is add-half then arithmetic shift, twice, so the interpolation
// error against the exact bilinear value is at most one Q.6 step (1/64 pel).
// Integer adds and multiplies by small constants only; no 64-bit anywhere.
// ---------------------------------------------------------------------------

static inline int32_t bilerp_corner(const int32_t v00, const int32_t v10,
                                    const int32_t v01, const int32_t v11,
                                    int32_t u, int32_t v) {
    const int32_t top = sar(v00 * (kTile - u) + v10 * u + (kTile / 2), 6);
    const int32_t bot = sar(v01 * (kTile - u) + v11 * u + (kTile / 2), 6);
    return sar(top * (kTile - v) + bot * v + (kTile / 2), 6);
}

// ---------------------------------------------------------------------------
// Step 4: the sampling filters.
// ---------------------------------------------------------------------------

static inline int32_t fetch(const RefImage& ref, int32_t x, int32_t y, int32_t ch) {
    // Clamp-to-edge border policy, applied per tap on integer sample indices.
    x = clamp_i32(x, 0, ref.width - 1);
    y = clamp_i32(y, 0, ref.height - 1);
    return static_cast<int32_t>(ref.data[static_cast<size_t>(y) * ref.stride +
                                         static_cast<size_t>(x) * ref.channels + ch]);
}

static inline int32_t sample_bilinear(const RefImage& ref, int32_t ix, int32_t iy,
                                      int32_t fx, int32_t fy, int32_t ch) {
    const int32_t gx = 16 - fx, gy = 16 - fy;
    const int32_t acc = gx * gy * fetch(ref, ix, iy, ch) +
                        fx * gy * fetch(ref, ix + 1, iy, ch) +
                        gx * fy * fetch(ref, ix, iy + 1, ch) +
                        fx * fy * fetch(ref, ix + 1, iy + 1, ch);
    return sar(acc + 128, 8);  // weights sum to 256
}

static inline int32_t sample_catmullrom(const RefImage& ref, int32_t ix, int32_t iy,
                                        int32_t fx, int32_t fy, int32_t ch) {
    const int8_t* wx = kCatmullRom[fx];
    const int8_t* wy = kCatmullRom[fy];
    int32_t acc = 0;
    for (int j = 0; j < 4; ++j) {
        // Horizontal pass at full precision, no intermediate rounding.
        // |row| <= 72*max_value, |acc| <= 72*72*max_value < 2^23 at 10 bits.
        int32_t row = 0;
        for (int i = 0; i < 4; ++i) {
            row += static_cast<int32_t>(wx[i]) * fetch(ref, ix - 1 + i, iy - 1 + j, ch);
        }
        acc += static_cast<int32_t>(wy[j]) * row;
    }
    return sar(acc + 2048, 12);  // 64*64 == 4096
}

// ---------------------------------------------------------------------------
// The predictor.
// ---------------------------------------------------------------------------

void warp_tile(const RefImage& ref,
               int32_t tile_x,
               int32_t tile_y,
               const Homography& H,
               const int32_t mv_qpel[2],
               Filter filter,
               Mode mode,
               uint16_t* out_tile,
               int32_t out_stride) {
    const TileCorners c = warp_tile_corners(H, tile_x, tile_y, mode);

    // mv is Q.2; promoting it to Q.6 is a shift by kQCorner - kQMv == 4.
    const int32_t mvx_q6 = mv_qpel[0] << (kQCorner - kQMv);
    const int32_t mvy_q6 = mv_qpel[1] << (kQCorner - kQMv);

    for (int32_t v = 0; v < kTile; ++v) {
        for (int32_t u = 0; u < kTile; ++u) {
            int32_t xq6 = bilerp_corner(c.x[0], c.x[1], c.x[2], c.x[3], u, v) + mvx_q6;
            int32_t yq6 = bilerp_corner(c.y[0], c.y[1], c.y[2], c.y[3], u, v) + mvy_q6;

            // Q.6 -> Q.4, round half up (paper 2.2 step 4: "(c + 2) >> 2").
            const int32_t xq4 = sar(xq6 + 2, kQCorner - kQSample);
            const int32_t yq4 = sar(yq6 + 2, kQCorner - kQSample);

            const int32_t ix = sar(xq4, kQSample);
            const int32_t iy = sar(yq4, kQSample);
            const int32_t fx = xq4 & 15;
            const int32_t fy = yq4 & 15;

            uint16_t* dst = out_tile + static_cast<size_t>(v) * out_stride +
                            static_cast<size_t>(u) * ref.channels;
            for (int32_t ch = 0; ch < ref.channels; ++ch) {
                const int32_t p = (filter == kFilterCatmullRom)
                                      ? sample_catmullrom(ref, ix, iy, fx, fy, ch)
                                      : sample_bilinear(ref, ix, iy, fx, fy, ch);
                dst[ch] = static_cast<uint16_t>(clamp_i32(p, 0, ref.max_value));
            }
        }
    }
}

}  // namespace nxvc::warp
