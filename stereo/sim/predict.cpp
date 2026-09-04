#include "predict.h"

#include <algorithm>
#include <cstring>

namespace nxs {
namespace {

i32 g_filt[16][4];
bool g_filt_init = false;

void init_filter() {
    if (g_filt_init) return;
    for (int p = 0; p < 16; ++p) {
        double t = p / 16.0;
        double c[4];
        c[0] = -0.5 * t + t * t - 0.5 * t * t * t;
        c[1] = 1.0 - 2.5 * t * t + 1.5 * t * t * t;
        c[2] = 0.5 * t + 2.0 * t * t - 1.5 * t * t * t;
        c[3] = -0.5 * t * t + 0.5 * t * t * t;
        i32 q[4], sum = 0;
        for (int i = 0; i < 4; ++i) {
            q[i] = static_cast<i32>(std::floor(c[i] * 64.0 + 0.5));
            sum += q[i];
        }
        // Force the row to sum to 64 by correcting the largest tap, so that a
        // constant region reproduces exactly and the predictor has no DC bias.
        int big = 1;
        for (int i = 0; i < 4; ++i)
            if (q[i] > q[big]) big = i;
        q[big] += 64 - sum;
        for (int i = 0; i < 4; ++i) g_filt[p][i] = q[i];
    }
    g_filt_init = true;
}

inline i64 rdiv(i64 n, i64 d) {
    // Round half away from zero.  d is always positive here (the denominator of
    // the homography is the projected depth of a forward-facing ray).
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

}  // namespace

const i32 (*filter_table())[4] {
    init_filter();
    return g_filt;
}

i32 sample_q4(const Image& img, i32 x_q4, i32 y_q4) {
    init_filter();
    const i32 xi = x_q4 >> 4, yi = y_q4 >> 4;
    const i32 fx = x_q4 & 15, fy = y_q4 & 15;
    // Integer position: phase 0 is {0,64,0,0} in both directions, so this is a
    // copy.  Kept as a fast path because the encoder's integer search hits it
    // for every candidate.
    if ((fx | fy) == 0) return img.clamped(xi, yi);
    const i32* cx = g_filt[fx];
    const i32* cy = g_filt[fy];
    i32 rows[4];
    for (int j = 0; j < 4; ++j) {
        const int yy = yi - 1 + j;
        i32 acc = 0;
        for (int i = 0; i < 4; ++i) acc += cx[i] * img.clamped(xi - 1 + i, yy);
        rows[j] = (acc + 32) >> 6;
    }
    i32 acc = 0;
    for (int j = 0; j < 4; ++j) acc += cy[j] * rows[j];
    return clampi((acc + 32) >> 6, 0, 255);
}

WarpQ quantize_warp(const Mat3& r_prev, const Mat3& r_cur, double f, double cx, double cy) {
    // Centred pixel coordinates, so K is diag(f, f, 1) and no term of H is
    // larger than about f.
    Mat3 k;
    k(0, 0) = f;
    k(1, 1) = f;
    k(2, 2) = 1.0;
    Mat3 kinv = inverse(k);
    Mat3 h = mul(mul(k, mul(transpose(r_prev), r_cur)), kinv);
    // Normalise so h22 == 1 before scaling.
    double n = h(2, 2);
    WarpQ w;
    w.cx = cx;
    w.cy = cy;
    const double s = static_cast<double>(1 << kWarpShift);
    for (int i = 0; i < 9; ++i) {
        double v = h.m[i] / n * s;
        w.h[i] = static_cast<i32>(std::floor(v + 0.5));
    }
    w.h[8] = 1 << kWarpShift;
    return w;
}

void warp_point_q6(const WarpQ& w, i32 x, i32 y, i32* sx_q6, i32* sy_q6) {
    const i64 X = x - static_cast<i64>(w.cx);
    const i64 Y = y - static_cast<i64>(w.cy);
    const i64 nx = static_cast<i64>(w.h[0]) * X + static_cast<i64>(w.h[1]) * Y + w.h[2];
    const i64 ny = static_cast<i64>(w.h[3]) * X + static_cast<i64>(w.h[4]) * Y + w.h[5];
    const i64 den = static_cast<i64>(w.h[6]) * X + static_cast<i64>(w.h[7]) * Y + w.h[8];
    const i64 d = den > 0 ? den : 1;
    *sx_q6 = static_cast<i32>(rdiv(nx * 64, d) + static_cast<i64>(w.cx) * 64);
    *sy_q6 = static_cast<i32>(rdiv(ny * 64, d) + static_cast<i64>(w.cy) * 64);
}

void warp_tile(const Image& ref, const WarpQ& w, int tx, int ty, i32 mv_x_q2, i32 mv_y_q2,
               std::vector<i32>* out) {
    out->assign(kTile * kTile, 0);
    // Four corner divisions per tile, then integer bilinear inside.
    i32 cx4[4], cy4[4];
    const int x0 = tx * kTile, y0 = ty * kTile;
    warp_point_q6(w, x0, y0, &cx4[0], &cy4[0]);
    warp_point_q6(w, x0 + kTile, y0, &cx4[1], &cy4[1]);
    warp_point_q6(w, x0, y0 + kTile, &cx4[2], &cy4[2]);
    warp_point_q6(w, x0 + kTile, y0 + kTile, &cx4[3], &cy4[3]);

    for (int j = 0; j < kTile; ++j) {
        for (int i = 0; i < kTile; ++i) {
            const i64 a = static_cast<i64>(kTile - i) * (kTile - j);
            const i64 b = static_cast<i64>(i) * (kTile - j);
            const i64 c = static_cast<i64>(kTile - i) * j;
            const i64 d = static_cast<i64>(i) * j;
            const i64 sxq6 = (a * cx4[0] + b * cx4[1] + c * cx4[2] + d * cx4[3] + 2048) >> 12;
            const i64 syq6 = (a * cy4[0] + b * cy4[1] + c * cy4[2] + d * cy4[3] + 2048) >> 12;
            // Warp is Q.6, MV is Q.2; sum in Q.6 then round to Q.4 (1/16 pel).
            const i64 xq6 = sxq6 + static_cast<i64>(mv_x_q2) * 16;
            const i64 yq6 = syq6 + static_cast<i64>(mv_y_q2) * 16;
            const i32 xq4 = static_cast<i32>((xq6 + 2) >> 2);
            const i32 yq4 = static_cast<i32>((yq6 + 2) >> 2);
            (*out)[j * kTile + i] = sample_q4(ref, xq4, yq4);
        }
    }
}

void shift_tile(const Image& ref, int tx, int ty, i32 dx_q2, i32 dy_q2, std::vector<i32>* out) {
    out->assign(kTile * kTile, 0);
    const int x0 = tx * kTile, y0 = ty * kTile;
    for (int j = 0; j < kTile; ++j)
        for (int i = 0; i < kTile; ++i)
            (*out)[j * kTile + i] =
                sample_q4(ref, ((x0 + i) << 4) + dx_q2 * 4, ((y0 + j) << 4) + dy_q2 * 4);
}

void intra_plane(const Image& img, int tx, int ty, std::vector<i32>* out) {
    out->assign(kTile * kTile, 0);
    const int x0 = tx * kTile, y0 = ty * kTile;
    // Centred coordinates make the normal equations diagonal.
    const double c = (kTile - 1) * 0.5;
    double s = 0, sx = 0, sy = 0, sxx = 0, syy = 0;
    for (int j = 0; j < kTile; ++j)
        for (int i = 0; i < kTile; ++i) {
            double v = img.at(x0 + i, y0 + j);
            double u = i - c, w = j - c;
            s += v;
            sx += v * u;
            sy += v * w;
            sxx += u * u;
            syy += w * w;
        }
    const double n = kTile * kTile;
    double a = s / n;
    double bx = sxx > 0 ? sx / sxx : 0.0;
    double by = syy > 0 ? sy / syy : 0.0;
    for (int j = 0; j < kTile; ++j)
        for (int i = 0; i < kTile; ++i)
            (*out)[j * kTile + i] =
                clampi(static_cast<i32>(std::floor(a + bx * (i - c) + by * (j - c) + 0.5)), 0, 255);
}

i64 tile_sad(const Image& img, int tx, int ty, const std::vector<i32>& pred) {
    const int x0 = tx * kTile, y0 = ty * kTile;
    i64 s = 0;
    for (int j = 0; j < kTile; ++j)
        for (int i = 0; i < kTile; ++i)
            s += std::abs(static_cast<i32>(img.at(x0 + i, y0 + j)) - pred[j * kTile + i]);
    return s;
}

i64 tile_sse(const Image& img, int tx, int ty, const std::vector<i32>& pred) {
    const int x0 = tx * kTile, y0 = ty * kTile;
    i64 s = 0;
    for (int j = 0; j < kTile; ++j)
        for (int i = 0; i < kTile; ++i) {
            i64 d = static_cast<i32>(img.at(x0 + i, y0 + j)) - pred[j * kTile + i];
            s += d * d;
        }
    return s;
}

namespace {

// Orthonormal 8x8 DCT-II.  A stand-in for the codec's integer transform; the
// bit model only needs the coefficient distribution to be right.
void dct8x8(const double in[64], double out[64]) {
    static double c[8][8];
    static bool init = false;
    if (!init) {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = (u == 0 ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0)) *
                          std::cos((2 * x + 1) * u * M_PI / 16.0);
        init = true;
    }
    double tmp[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            double s = 0;
            for (int x = 0; x < 8; ++x) s += c[u][x] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
            double s = 0;
            for (int y = 0; y < 8; ++y) s += c[v][y] * tmp[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

// Signed Exp-Golomb order 0 length for a non-zero level.
double se_bits(i64 level) {
    const i64 k = 2 * std::abs(level) - (level > 0 ? 1 : 0);
    int msb = 0;
    for (i64 v = k + 1; v > 1; v >>= 1) ++msb;
    return 2.0 * msb + 1.0;
}

}  // namespace

double tile_bits(const Image& img, int tx, int ty, const std::vector<i32>& pred, double q) {
    const int x0 = tx * kTile, y0 = ty * kTile;
    double bits = 0;
    double blk[64], co[64];
    for (int by = 0; by < kTile; by += 8) {
        for (int bx = 0; bx < kTile; bx += 8) {
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    blk[j * 8 + i] = static_cast<double>(img.at(x0 + bx + i, y0 + by + j)) -
                                     pred[(by + j) * kTile + bx + i];
            dct8x8(blk, co);
            double bb = 2.0;  // per-block flags
            for (int k = 0; k < 64; ++k) {
                i64 lv = static_cast<i64>(std::floor(co[k] / q + 0.5));
                bb += lv == 0 ? 0.08 : se_bits(lv);
            }
            bits += bb;
        }
    }
    return bits;
}

namespace {

void idct8x8(const double in[64], double out[64]) {
    static double c[8][8];
    static bool init = false;
    if (!init) {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = (u == 0 ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0)) *
                          std::cos((2 * x + 1) * u * M_PI / 16.0);
        init = true;
    }
    double tmp[64];
    for (int v = 0; v < 8; ++v)
        for (int y = 0; y < 8; ++y) {
            double s = 0;
            for (int u = 0; u < 8; ++u) s += c[u][y] * in[v * 8 + u];
            tmp[v * 8 + y] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int v = 0; v < 8; ++v) s += c[v][x] * tmp[v * 8 + y];
            out[x * 8 + y] = s;
        }
}

}  // namespace

Image reconstruct_frame(const Image& cur, const Image& prev, const WarpQ& w, double q, int range) {
    Image out(cur.w, cur.h);
    const int tiles_x = cur.w / kTile, tiles_y = cur.h / kTile;
    std::vector<i32> pred, best, ip;
    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            // Integer + quarter-pel refinement of the warp vector.
            warp_tile(prev, w, tx, ty, 0, 0, &best);
            i64 bs = tile_sad(cur, tx, ty, best);
            i32 bx = 0, by = 0;
            for (int dy = -range; dy <= range; ++dy)
                for (int dx = -range; dx <= range; ++dx) {
                    if (!dx && !dy) continue;
                    warp_tile(prev, w, tx, ty, dx * 4, dy * 4, &pred);
                    i64 s = tile_sad(cur, tx, ty, pred);
                    if (s < bs) { bs = s; bx = dx * 4; by = dy * 4; }
                }
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    warp_tile(prev, w, tx, ty, bx + dx, by + dy, &pred);
                    i64 s = tile_sad(cur, tx, ty, pred);
                    if (s < bs) { bs = s; bx += dx; by += dy; }
                }
            warp_tile(prev, w, tx, ty, bx, by, &best);
            // DC-plane intra fallback where the warp has nothing to offer.
            intra_plane(cur, tx, ty, &ip);
            if (tile_sad(cur, tx, ty, ip) < bs) best = ip;

            const int x0 = tx * kTile, y0 = ty * kTile;
            double blk[64], co[64], rec[64];
            for (int by8 = 0; by8 < kTile; by8 += 8)
                for (int bx8 = 0; bx8 < kTile; bx8 += 8) {
                    for (int j = 0; j < 8; ++j)
                        for (int i = 0; i < 8; ++i)
                            blk[j * 8 + i] =
                                static_cast<double>(cur.at(x0 + bx8 + i, y0 + by8 + j)) -
                                best[(by8 + j) * kTile + bx8 + i];
                    dct8x8(blk, co);
                    for (int k = 0; k < 64; ++k)
                        co[k] = std::floor(co[k] / q + 0.5) * q;  // quantise, dequantise
                    idct8x8(co, rec);
                    for (int j = 0; j < 8; ++j)
                        for (int i = 0; i < 8; ++i)
                            out.at(x0 + bx8 + i, y0 + by8 + j) = static_cast<u8>(clampi(
                                static_cast<i32>(std::floor(
                                    best[(by8 + j) * kTile + bx8 + i] + rec[j * 8 + i] + 0.5)),
                                0, 255));
                }
        }
    }
    return out;
}

double mode_side_bits(const char* mode) {
    // 3 bits of mode in every case (PAPER 2.3), plus what the mode carries.
    const std::string m(mode);
    if (m == "INTRA") return 3.0 + 21.0;      // DC-plane: DC 8 bits, two slopes ~6.5 each
    if (m == "WARP_SKIP") return 3.0 + 1.0;   // no vector, no residual flag
    if (m == "WARP_MV") return 3.0 + 8.0;     // Exp-Golomb MV delta, typically small
    if (m == "STEREO") return 3.0 + 6.0;      // disparity delta from the depth seed
    if (m == "STEREO_MV") return 3.0 + 10.0;  // disparity plus a refinement vector
    return 3.0;
}

}  // namespace nxs
