// NX Warp -- floating-point oracle. TESTS ONLY.
//
// Nothing here may ever be reachable from a decode. Its sole purpose is to
// bound the error of the integer pipeline: the oracle evaluates the *exact*
// homography per pixel with the *exact* Catmull-Rom / bilinear kernel in
// double precision, and the tests assert that the normative integer result
// stays inside the tolerance documented in docs/WARP.md section 9.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxvc/warp.h"

#include <cmath>

namespace nxvc::warp::oracle {

namespace {

double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Exact Catmull-Rom (a = -1/2) basis, unquantised.
void catmull_rom_weights(double t, double w[4]) {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t + t2 - 0.5 * t3;
    w[1] = 1.0 - 2.5 * t2 + 1.5 * t3;
    w[2] = 0.5 * t + 2.0 * t2 - 1.5 * t3;
    w[3] = -0.5 * t2 + 0.5 * t3;
}

double fetch(const RefImage& ref, int32_t x, int32_t y, int32_t ch) {
    x = clampi(x, 0, ref.width - 1);
    y = clampi(y, 0, ref.height - 1);
    return static_cast<double>(
        ref.data[static_cast<size_t>(y) * ref.stride + static_cast<size_t>(x) * ref.channels + ch]);
}

}  // namespace

void source_coord(const double Hd[9],
                  int32_t ox,
                  int32_t oy,
                  int32_t x,
                  int32_t y,
                  const int32_t mv_qpel[2],
                  double* sx,
                  double* sy) {
    const double cx = static_cast<double>(x - ox);
    const double cy = static_cast<double>(y - oy);
    const double den = Hd[6] * cx + Hd[7] * cy + Hd[8];
    *sx = (Hd[0] * cx + Hd[1] * cy + Hd[2]) / den + ox + mv_qpel[0] / 4.0;
    *sy = (Hd[3] * cx + Hd[4] * cy + Hd[5]) / den + oy + mv_qpel[1] / 4.0;
}

void source_coord_q(const Homography& H,
                    int32_t x,
                    int32_t y,
                    const int32_t mv_qpel[2],
                    double* sx,
                    double* sy) {
    const double cx = static_cast<double>(x - H.ox);
    const double cy = static_cast<double>(y - H.oy);
    const double ns = static_cast<double>(1 << kQNum);
    const double ds = static_cast<double>(1 << kQDen);
    const double den = (static_cast<double>(H.h[6]) * cx + static_cast<double>(H.h[7]) * cy +
                        static_cast<double>(H.h[8])) /
                       ds;
    *sx = ((static_cast<double>(H.h[0]) * cx + static_cast<double>(H.h[1]) * cy +
            static_cast<double>(H.h[2])) /
           ns) / den +
          H.ox + mv_qpel[0] / 4.0;
    *sy = ((static_cast<double>(H.h[3]) * cx + static_cast<double>(H.h[4]) * cy +
            static_cast<double>(H.h[5])) /
           ns) / den +
          H.oy + mv_qpel[1] / 4.0;
}

void warp_tile_float(const RefImage& ref,
                     int32_t tile_x,
                     int32_t tile_y,
                     const double Hd[9],
                     int32_t ox,
                     int32_t oy,
                     const int32_t mv_qpel[2],
                     Filter filter,
                     Mode mode,
                     double* out_tile,
                     int32_t out_stride) {
    for (int32_t v = 0; v < kTile; ++v) {
        for (int32_t u = 0; u < kTile; ++u) {
            const int32_t px = tile_x + u;
            const int32_t py = tile_y + v;
            double sx, sy;
            if (mode == kModeStatic) {
                sx = static_cast<double>(px) + mv_qpel[0] / 4.0;
                sy = static_cast<double>(py) + mv_qpel[1] / 4.0;
            } else {
                source_coord(Hd, ox, oy, px, py, mv_qpel, &sx, &sy);
            }
            const double fx0 = std::floor(sx);
            const double fy0 = std::floor(sy);
            const int32_t ix = static_cast<int32_t>(fx0);
            const int32_t iy = static_cast<int32_t>(fy0);
            const double tx = sx - fx0;
            const double ty = sy - fy0;

            double wx[4], wy[4];
            int taps, base;
            if (filter == kFilterCatmullRom) {
                catmull_rom_weights(tx, wx);
                catmull_rom_weights(ty, wy);
                taps = 4;
                base = -1;
            } else {
                wx[0] = 1.0 - tx; wx[1] = tx;
                wy[0] = 1.0 - ty; wy[1] = ty;
                taps = 2;
                base = 0;
            }

            double* dst = out_tile + static_cast<size_t>(v) * out_stride +
                          static_cast<size_t>(u) * ref.channels;
            for (int32_t ch = 0; ch < ref.channels; ++ch) {
                double acc = 0.0;
                for (int j = 0; j < taps; ++j) {
                    double row = 0.0;
                    for (int i = 0; i < taps; ++i) {
                        row += wx[i] * fetch(ref, ix + base + i, iy + base + j, ch);
                    }
                    acc += wy[j] * row;
                }
                dst[ch] = clampd(acc, 0.0, static_cast<double>(ref.max_value));
            }
        }
    }
}

}  // namespace nxvc::warp::oracle
