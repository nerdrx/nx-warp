// nxvc-warpsim -- paper 2.11 items 1 and 2, answered early and on synthetic
// content.
//
// Builds a procedural equirectangular panorama (multi-octave noise, hard
// edges, fine checkerboard, grid lines and real 5x7 text), renders ground
// truth frames for a head-rotation pose log at 90 Hz by ray-sampling the
// panorama in double precision, and then measures what the warp predictor can
// and cannot do:
//
//   (a) single-step  -- PSNR of warp(true frame N-1) against true frame N
//   (b) chain        -- PSNR of a 2 s warp-only chain, no residual at all,
//                       bilinear against Catmull-Rom (the resampling blur
//                       decay of paper 2.2)
//   (c) residual     -- what is left for the codec to code: mean |e|, the
//                       fraction of near-zero pixels (skip candidates), and a
//                       first-order entropy estimate in bits per pixel
//
// The ground truth renderer is completely independent of the warp: it goes
// pose -> ray -> sphere -> panorama, in double, with no homography anywhere.
// That is the point -- the warp is being scored against real reprojection,
// not against itself.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nxvc/warp.h"

using namespace nxvc::warp;

// ---------------------------------------------------------------------------
// Panorama
// ---------------------------------------------------------------------------

// 5x7 glyphs, same shapes as tools/quality/capture/font5x7.py, so the text in
// this material matches the text in the quality harness's material.
static const char* const kGlyphChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,-+:/%()*=!?#";
static const unsigned char kGlyphRows[][7] = {
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
    {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    {0x0e,0x11,0x10,0x17,0x11,0x11,0x0e}, {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
    {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0c},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    {0x0e,0x11,0x10,0x0e,0x01,0x11,0x0e}, {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1b,0x11}, {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e},
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},
    {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}, {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c},
    {0x00,0x00,0x00,0x00,0x0c,0x0c,0x08}, {0x00,0x00,0x00,0x1f,0x00,0x00,0x00},
    {0x00,0x04,0x04,0x1f,0x04,0x04,0x00}, {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00},
    {0x01,0x02,0x02,0x04,0x08,0x08,0x10}, {0x19,0x1a,0x04,0x08,0x10,0x16,0x16},
    {0x04,0x08,0x10,0x10,0x10,0x08,0x04}, {0x04,0x02,0x01,0x01,0x01,0x02,0x04},
    {0x00,0x15,0x0e,0x1f,0x0e,0x15,0x00}, {0x00,0x00,0x1f,0x00,0x1f,0x00,0x00},
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, {0x0e,0x11,0x01,0x02,0x04,0x00,0x04},
    {0x0a,0x1f,0x0a,0x0a,0x1f,0x0a,0x00},
};

static uint32_t hash3(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = seed ^ 0x9e3779b9u;
    h ^= static_cast<uint32_t>(x) * 0x85ebca6bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(y) * 0xc2b2ae35u;
    h *= 0x27d4eb2fu;
    h ^= h >> 15;
    return h;
}

static double value_noise(double x, double y, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const double fx = x - xi, fy = y - yi;
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    auto n = [&](int a, int b) { return (hash3(a, b, seed) >> 8) / 16777215.0; };
    const double a = n(xi, yi), b = n(xi + 1, yi), c = n(xi, yi + 1), d = n(xi + 1, yi + 1);
    return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}

struct Panorama {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    uint8_t at(int x, int y) const {
        // Longitude wraps, latitude clamps.
        x = ((x % w) + w) % w;
        y = std::clamp(y, 0, h - 1);
        return px[static_cast<size_t>(y) * w + x];
    }
    // Bilinear sample in double. Ground truth only.
    double sample(double x, double y) const {
        const double fx0 = std::floor(x), fy0 = std::floor(y);
        const int ix = static_cast<int>(fx0), iy = static_cast<int>(fy0);
        const double tx = x - fx0, ty = y - fy0;
        const double a = at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx;
        const double b = at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx;
        return a * (1 - ty) + b * ty;
    }
};

static void draw_text(Panorama& p, int x0, int y0, const char* s, int scale, uint8_t fg) {
    int cx = x0;
    for (const char* c = s; *c; ++c) {
        const char* q = std::strchr(kGlyphChars, *c);
        if (q) {
            const size_t gi = static_cast<size_t>(q - kGlyphChars);
            for (int r = 0; r < 7; ++r) {
                for (int b = 0; b < 5; ++b) {
                    if (kGlyphRows[gi][r] & (1 << (4 - b))) {
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx) {
                                const int px = cx + b * scale + sx, py = y0 + r * scale + sy;
                                if (px >= 0 && px < p.w && py >= 0 && py < p.h)
                                    p.px[static_cast<size_t>(py) * p.w + px] = fg;
                            }
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

static Panorama make_panorama(int w, int h) {
    Panorama p;
    p.w = w;
    p.h = h;
    p.px.assign(static_cast<size_t>(w) * h, 0);

    const int nthr = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> gts;
    auto band = [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        for (int x = 0; x < w; ++x) {
            const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
            // Multi-octave value noise: "textured geometry".
            double n = 0.0, amp = 0.5, f = 8.0;
            for (int o = 0; o < 5; ++o) {
                n += amp * value_noise(u * f, v * f, 1234u + o);
                amp *= 0.5;
                f *= 2.07;
            }
            double val = 40.0 + 150.0 * n;
            // Fine checkerboard band: the pathological case for resampling.
            if (y > h * 0.30 && y < h * 0.42) {
                val = ((x >> 1) + (y >> 1)) & 1 ? 235.0 : 20.0;
            }
            // Hard-edged blocks: sharp geometry.
            if (((x / 97) + (y / 89)) % 7 == 0) val = std::min(250.0, val + 70.0);
            // Grid lines every 32 px: 1 px structures.
            if ((x % 32) == 0 || (y % 32) == 0) val = 230.0;
            // Smooth gradient band.
            if (y > h * 0.72 && y < h * 0.82) val = 20.0 + 200.0 * u;
            p.px[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(std::clamp(val, 0.0, 255.0));
        }
    }
    };
    {
        const int chunk = (h + nthr - 1) / nthr;
        for (int t = 0; t < nthr; ++t) {
            const int ya = t * chunk, yb = std::min(h, ya + chunk);
            if (ya < yb) gts.emplace_back(band, ya, yb);
        }
        for (auto& th : gts) th.join();
    }

    // Text panels around the equator, at three sizes.
    const char* lines[] = {"NX WARP PHASE 2 PREDICTOR TEST",
                           "THE QUICK BROWN FOX 0123456789",
                           "RESAMPLING BLUR DECAY 2 SECONDS",
                           "CATMULL-ROM VS BILINEAR (90 HZ)",
                           "HEAD LOCKED UI TEXT SAMPLE #42!"};
    for (int band = 0; band < 6; ++band) {
        const int x0 = (w / 6) * band + 20;
        for (int i = 0; i < 5; ++i) {
            const int scale = 1 + (band % 3);
            draw_text(p, x0, h / 2 - 90 + i * 10 * scale, lines[i], scale, 250);
        }
    }
    return p;
}

// ---------------------------------------------------------------------------
// Ground truth rendering: pose -> ray -> sphere -> panorama. No homography.
// ---------------------------------------------------------------------------

static void quat_to_mat(const Quat& q, double m[9]) {
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz); m[1] = 2 * (xy - wz);     m[2] = 2 * (xz + wy);
    m[3] = 2 * (xy + wz);     m[4] = 1 - 2 * (xx + zz); m[5] = 2 * (yz - wx);
    m[6] = 2 * (xz - wy);     m[7] = 2 * (yz + wx);     m[8] = 1 - 2 * (xx + yy);
}

struct Frame {
    int w = 0, h = 0;
    std::vector<uint16_t> px;  // one channel, 8-bit values in uint16
};

// `ss` is the supersampling factor: ss*ss box-filtered samples per output
// pixel. This is not cosmetic. If the panorama carries more angular detail
// than the eye buffer can hold and we point-sample it, every ground truth
// frame is full of aliasing that reshuffles on any sub-pixel pose change.
// That noise is unpredictable by construction, so an unfiltered renderer
// scores the predictor at ~16 dB no matter how good the predictor is and the
// experiment measures nothing but its own sampling bug. ss is chosen from the
// ratio of panorama to eye angular resolution.
static void render(const Panorama& pano, const Quat& q, const Fov& fov, Frame& out, int threads,
                   int ss) {
    const double tl = std::tan(fov.angle_left), tr = std::tan(fov.angle_right);
    const double tu = std::tan(fov.angle_up), td = std::tan(fov.angle_down);
    double R[9];
    quat_to_mat(q, R);
    const double inv_ss = 1.0 / ss;
    const double norm = 1.0 / (ss * ss);

    auto row_range = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < out.w; ++x) {
                double acc = 0.0;
                for (int sy = 0; sy < ss; ++sy) {
                    for (int sx = 0; sx < ss; ++sx) {
                        // Sub-sample centres inside the pixel footprint. At
                        // ss == 1 this reduces to (x+0.5, y+0.5), which is the
                        // convention make_K() folds in.
                        const double fx = x + (sx + 0.5) * inv_ss;
                        const double fy = y + (sy + 0.5) * inv_ss;
                        const double a = tl + (tr - tl) * (fx / out.w);
                        const double b = tu - (tu - td) * (fy / out.h);
                        // Camera-space ray (-Z forward).
                        const double vx = a, vy = b, vz = -1.0;
                        // To world.
                        const double wx = R[0] * vx + R[1] * vy + R[2] * vz;
                        const double wy = R[3] * vx + R[4] * vy + R[5] * vz;
                        const double wz = R[6] * vx + R[7] * vy + R[8] * vz;
                        const double len = std::sqrt(wx * wx + wy * wy + wz * wz);
                        const double lon = std::atan2(wx / len, -wz / len);  // -pi..pi
                        const double lat = std::asin(std::clamp(wy / len, -1.0, 1.0));
                        const double pu = (lon / (2 * 3.14159265358979) + 0.5) * pano.w - 0.5;
                        const double pv = (0.5 - lat / 3.14159265358979) * pano.h - 0.5;
                        acc += pano.sample(pu, pv);
                    }
                }
                out.px[static_cast<size_t>(y) * out.w + x] =
                    static_cast<uint16_t>(std::clamp(acc * norm + 0.5, 0.0, 255.0));
            }
        }
    };

    if (threads <= 1) {
        row_range(0, out.h);
        return;
    }
    std::vector<std::thread> ts;
    const int chunk = (out.h + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int y0 = t * chunk, y1 = std::min(out.h, y0 + chunk);
        if (y0 < y1) ts.emplace_back(row_range, y0, y1);
    }
    for (auto& t : ts) t.join();
}

// ---------------------------------------------------------------------------
// Whole-frame warp built out of 64x64 tile calls (exactly what the decoder
// does: no MV, no residual, WARP_SKIP everywhere).
// ---------------------------------------------------------------------------

static void warp_frame(const Frame& ref, const Homography& H, Filter f, Frame& out, int threads) {
    RefImage ri;
    ri.data = ref.px.data();
    ri.width = ref.w;
    ri.height = ref.h;
    ri.stride = ref.w;
    ri.channels = 1;
    ri.max_value = 255;
    const int32_t zero[2] = {0, 0};
    const int tiles_y = out.h / kTile;
    const int tiles_x = out.w / kTile;

    auto do_rows = [&](int r0, int r1) {
        std::vector<uint16_t> tile(static_cast<size_t>(kTile) * kTile);
        for (int ty = r0; ty < r1; ++ty) {
            for (int tx = 0; tx < tiles_x; ++tx) {
                warp_tile(ri, tx * kTile, ty * kTile, H, zero, f, kModeWarp, tile.data(), kTile);
                for (int v = 0; v < kTile; ++v) {
                    std::memcpy(&out.px[static_cast<size_t>(ty * kTile + v) * out.w + tx * kTile],
                                &tile[static_cast<size_t>(v) * kTile], kTile * sizeof(uint16_t));
                }
            }
        }
    };
    if (threads <= 1) {
        do_rows(0, tiles_y);
        return;
    }
    std::vector<std::thread> ts;
    const int chunk = (tiles_y + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int r0 = t * chunk, r1 = std::min(tiles_y, r0 + chunk);
        if (r0 < r1) ts.emplace_back(do_rows, r0, r1);
    }
    for (auto& t : ts) t.join();
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

struct Metrics {
    double psnr = 0;         // full frame
    double psnr_centre = 0;  // central 75 %, i.e. excluding the disocclusion strip
    double mean_abs = 0;
    double skip_frac = 0;    // |e| <= 2, a proxy for WARP_SKIP candidates
    double entropy_bpp = 0;  // first-order entropy of the residual
};

static Metrics measure(const Frame& a, const Frame& b) {
    Metrics m;
    double se = 0, se_c = 0, sabs = 0;
    long n = 0, nc = 0, nskip = 0;
    std::vector<long> hist(1024, 0);
    const int mx = a.w / 8, my = a.h / 8;
    for (int y = 0; y < a.h; ++y) {
        for (int x = 0; x < a.w; ++x) {
            const size_t i = static_cast<size_t>(y) * a.w + x;
            const double d = static_cast<double>(a.px[i]) - static_cast<double>(b.px[i]);
            se += d * d;
            sabs += std::fabs(d);
            ++n;
            if (std::fabs(d) <= 2.0) ++nskip;
            hist[static_cast<size_t>(std::clamp(static_cast<int>(d) + 512, 0, 1023))]++;
            if (x >= mx && x < a.w - mx && y >= my && y < a.h - my) {
                se_c += d * d;
                ++nc;
            }
        }
    }
    auto psnr = [](double mse) {
        return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    };
    m.psnr = psnr(se / n);
    m.psnr_centre = psnr(se_c / std::max(1L, nc));
    m.mean_abs = sabs / n;
    m.skip_frac = static_cast<double>(nskip) / n;
    double H = 0;
    for (long c : hist) {
        if (c > 0) {
            const double p = static_cast<double>(c) / n;
            H -= p * std::log2(p);
        }
    }
    m.entropy_bpp = H;
    return m;
}

// ---------------------------------------------------------------------------
// Pose trajectories
// ---------------------------------------------------------------------------

static Quat quat_ypr(double yaw, double pitch, double roll) {
    const double cy = std::cos(yaw * 0.5), sy = std::sin(yaw * 0.5);
    const double cp = std::cos(pitch * 0.5), sp = std::sin(pitch * 0.5);
    const double cr = std::cos(roll * 0.5), sr = std::sin(roll * 0.5);
    Quat q;
    q.w = cr * cp * cy + sr * sp * sy;
    q.x = cr * sp * cy + sr * cp * sy;
    q.y = cr * cp * sy - sr * sp * cy;
    q.z = sr * cp * cy - cr * sp * sy;
    return q;
}

struct Traj {
    const char* name;
    double peak_deg_s;  // reported, informational
    // Reciprocating so the content stays in frame over a 2 s chain, which is
    // what a blur-decay measurement needs; a monotonic 300 deg/s turn empties
    // the frame in 300 ms and the chain PSNR would measure disocclusion.
    double amp_yaw, amp_pitch, amp_roll, period_s;
};

// Amplitudes are deliberately capped at 10 degrees. A sustained 300 deg/s turn
// replaces the entire frame in about 300 ms, so a 2 s warp-only chain under one
// would be measuring disocclusion (which the encoder answers with INTRA) rather
// than resampling blur (which is what the chain exists to measure). Keeping the
// excursion inside the centre crop isolates the blur; the rate is reached by
// shortening the period instead. `peak_deg_s` here is nominal -- the actual
// peak angular rate is measured from the pose log and reported.
static const Traj kTrajectories[] = {
    // name, nominal deg/s, amp yaw, amp pitch, amp roll (deg), period (s)
    {"still",        0.0,   0.0,  0.0,  0.0,  1.00},
    {"slow drift",  30.0,   8.0,  2.5,  0.0,  1.70},
    {"medium turn",100.0,  10.0,  3.0,  1.5,  0.63},
    {"fast turn",  300.0,  10.0,  3.0,  2.0,  0.21},
    {"roll",       100.0,   0.0,  0.0, 10.0,  0.63},
    {"mixed",      200.0,   8.0,  5.0,  3.0,  0.26},
};


static Quat pose_at(const Traj& t, int frame, double hz) {
    const double kDeg = 3.14159265358979 / 180.0;
    const double ph = 2 * 3.14159265358979 * (frame / hz) / t.period_s;
    return quat_ypr(t.amp_yaw * kDeg * std::sin(ph), t.amp_pitch * kDeg * std::sin(ph * 0.7 + 1.0),
                    t.amp_roll * kDeg * std::sin(ph * 1.3 + 2.0));
}

// Peak angular rate of the trajectory in deg/s, measured from the pose log.
static double measured_peak_rate(const Traj& t, double hz, int nframes) {
    double peak = 0.0;
    for (int f = 1; f <= nframes; ++f) {
        const Quat a = pose_at(t, f - 1, hz), b = pose_at(t, f, hz);
        // Angle of the relative rotation: 2*acos(|dot|).
        double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        d = std::clamp(std::fabs(d), 0.0, 1.0);
        peak = std::max(peak, 2.0 * std::acos(d) * 180.0 / 3.14159265358979 * hz);
    }
    return peak;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    int W = 512, Hh = 512;
    int frames = 60;
    int chain = 180;  // 2 s at 90 Hz
    int pano_w = 8192, pano_h = 4096;
    int threads = 4;
    int ss = 0;  // 0 = choose from the panorama/eye resolution ratio
    bool quiet = false;
    std::string out_md;
    const double hz = 90.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto ni = [&]() { return (i + 1 < argc) ? std::atoi(argv[++i]) : 0; };
        if (a == "--width") W = ni();
        else if (a == "--height") Hh = ni();
        else if (a == "--frames") frames = ni();
        else if (a == "--chain") chain = ni();
        else if (a == "--pano") { pano_w = ni(); pano_h = pano_w / 2; }
        else if (a == "--threads") threads = ni();
        else if (a == "--ss") ss = ni();
        else if (a == "--quiet") quiet = true;
        else if (a == "--out" && i + 1 < argc) out_md = argv[++i];
        else if (a == "--help") {
            std::printf("nxvc-warpsim [--width W] [--height H] [--frames N] [--chain N]\n"
                        "             [--pano W] [--threads N] [--out RESULTS.md] [--quiet]\n");
            return 0;
        }
    }
    if (W % kTile || Hh % kTile) {
        std::fprintf(stderr, "width and height must be multiples of %d\n", kTile);
        return 1;
    }

    if (!quiet) std::fprintf(stderr, "panorama %dx%d ...\n", pano_w, pano_h);
    Panorama pano = make_panorama(pano_w, pano_h);

    Fov fov;  // ~95 deg horizontal, Pico 4 class
    fov.angle_left = -0.83; fov.angle_right = 0.83;
    fov.angle_up = 0.90;    fov.angle_down = -0.90;

    // Band-limit the ground truth: average over the output pixel's footprint
    // in the panorama, or the "true" frames carry aliasing that no predictor
    // can follow and the whole experiment measures its own sampling bug.
    const double fov_deg = (fov.angle_right - fov.angle_left) * 180.0 / 3.14159265358979;
    const double pano_ppd = pano_w / 360.0;
    const double eye_ppd = W / fov_deg;
    if (ss <= 0) ss = std::clamp(static_cast<int>(std::ceil(pano_ppd / eye_ppd)), 1, 8);
    if (!quiet)
        std::fprintf(stderr,
                     "panorama %.1f px/deg, eye %.1f px/deg -> %dx%d supersampling\n",
                     pano_ppd, eye_ppd, ss, ss);

    Frame prev{W, Hh, std::vector<uint16_t>(static_cast<size_t>(W) * Hh)};
    Frame cur{W, Hh, std::vector<uint16_t>(static_cast<size_t>(W) * Hh)};
    Frame pred{W, Hh, std::vector<uint16_t>(static_cast<size_t>(W) * Hh)};
    Frame chain_bil{W, Hh, std::vector<uint16_t>(static_cast<size_t>(W) * Hh)};
    Frame chain_cr{W, Hh, std::vector<uint16_t>(static_cast<size_t>(W) * Hh)};
    Frame tmp{W, Hh, std::vector<uint16_t>(static_cast<size_t>(W) * Hh)};

    struct Row {
        std::string traj;
        double deg_s;
        Metrics bil, cr;
        double chain_bil_1s, chain_cr_1s, chain_bil_2s, chain_cr_2s;
        double chain_bil_30f, chain_cr_30f;
    };
    std::vector<Row> rows;
    // Chain decay curves, printed separately: [traj][filter][sample]
    struct Curve { std::string traj; std::vector<double> bil, cr; };
    std::vector<Curve> curves;

    for (const Traj& t : kTrajectories) {
        if (!quiet) std::fprintf(stderr, "trajectory '%s' ...\n", t.name);
        Row row;
        row.traj = t.name;
        row.deg_s = measured_peak_rate(t, hz, std::max(frames, chain));
        Curve cv;
        cv.traj = t.name;

        // One pass over the trajectory. The ground truth for frame f is
        // rendered exactly once and shared by the single-step measurement and
        // by both warp-only chains; rendering is by far the dominant cost, so
        // this is what makes a well-oversampled 2048^2 run affordable.
        double acc_psnr[2] = {0, 0}, acc_pc[2] = {0, 0}, acc_ma[2] = {0, 0};
        double acc_sk[2] = {0, 0}, acc_en[2] = {0, 0};
        int nacc = 0;

        const int last = std::max(frames, chain);
        render(pano, pose_at(t, 0, hz), fov, prev, threads, ss);
        chain_bil.px = prev.px;  // both chains start from the true frame 0
        chain_cr.px = prev.px;

        for (int f = 1; f <= last; ++f) {
            render(pano, pose_at(t, f, hz), fov, cur, threads, ss);
            Homography H;
            if (!derive_homography(pose_at(t, f - 1, hz), fov, pose_at(t, f, hz), fov, W, Hh, &H))
                break;

            // (a) single-step: warp the TRUE previous frame.
            if (f <= frames) {
                for (int fi = 0; fi < 2; ++fi) {
                    warp_frame(prev, H, fi ? kFilterCatmullRom : kFilterBilinear, pred, threads);
                    const Metrics m = measure(pred, cur);
                    acc_psnr[fi] += m.psnr;
                    acc_pc[fi] += m.psnr_centre;
                    acc_ma[fi] += m.mean_abs;
                    acc_sk[fi] += m.skip_frac;
                    acc_en[fi] += m.entropy_bpp;
                }
                ++nacc;
            }

            // (b) chains: warp each chain's OWN previous output. No residual
            // is ever applied, so this is pure accumulated resampling loss.
            if (f <= chain) {
                warp_frame(chain_bil, H, kFilterBilinear, tmp, threads);
                chain_bil.px.swap(tmp.px);
                cv.bil.push_back(measure(chain_bil, cur).psnr_centre);

                warp_frame(chain_cr, H, kFilterCatmullRom, tmp, threads);
                chain_cr.px.swap(tmp.px);
                cv.cr.push_back(measure(chain_cr, cur).psnr_centre);
            }

            prev.px = cur.px;
        }
        if (nacc == 0) continue;
        Metrics* dst[2] = {&row.bil, &row.cr};
        for (int fi = 0; fi < 2; ++fi) {
            dst[fi]->psnr = acc_psnr[fi] / nacc;
            dst[fi]->psnr_centre = acc_pc[fi] / nacc;
            dst[fi]->mean_abs = acc_ma[fi] / nacc;
            dst[fi]->skip_frac = acc_sk[fi] / nacc;
            dst[fi]->entropy_bpp = acc_en[fi] / nacc;
        }

        auto at_or = [](const std::vector<double>& v, size_t i) {
            return (v.empty() || i >= v.size()) ? 0.0 : v[i];
        };
        row.chain_bil_30f = at_or(cv.bil, 29);
        row.chain_cr_30f = at_or(cv.cr, 29);
        row.chain_bil_1s = at_or(cv.bil, 89);
        row.chain_cr_1s = at_or(cv.cr, 89);
        row.chain_bil_2s = at_or(cv.bil, 179);
        row.chain_cr_2s = at_or(cv.cr, 179);
        curves.push_back(std::move(cv));
        rows.push_back(std::move(row));
    }

    // --- report -------------------------------------------------------------
    std::string md;
    char buf[1024];
    auto out = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        md += buf;
    };

    out("<!-- generated by nxvc-warpsim; do not edit by hand -->\n");
    out("# NX Warp: predictor measurements\n\n");
    out("Synthetic equirectangular panorama %dx%d (multi-octave noise, hard edges, a\n", pano_w,
        pano_h);
    out("1:1 checkerboard band, 32 px grid lines, smooth gradients, 5x7 text at three\n");
    out("sizes). Eye %dx%d, 95 deg horizontal FOV, %g Hz, single luma channel.\n", W, Hh, hz);
    out("Ground truth is box-filtered with %dx%d supersampling (panorama %.1f px/deg,\n", ss, ss,
        pano_ppd);
    out("eye %.1f px/deg) so the reference frames are band-limited.\n", eye_ppd);
    out("Ground truth is rendered pose -> ray -> sphere -> panorama in double precision,\n");
    out("independent of the warp. The predictor is WARP_SKIP everywhere: global\n");
    out("homography only, no motion vectors, no residual.\n\n");

    out("## (a) Single-step prediction: warp(true N-1) vs true N\n\n");
    out("Full-frame PSNR includes the disocclusion strip on the leading edge, which the\n");
    out("encoder codes as INTRA; the centre columns exclude a 1/8 border on each side\n");
    out("and are the number that describes the predictor itself.\n\n");
    out("| trajectory | measured peak deg/s | full bil | full CR | centre bil | centre CR | "
        "mean abs err (CR) | skip frac (\\|e\\|<=2) | residual H0 bpp |\n");
    out("|---|---|---|---|---|---|---|---|---|\n");
    for (const Row& r : rows) {
        out("| %s | %.0f | %.2f | %.2f | %.2f | %.2f | %.2f | %.1f %% | %.2f |\n",
            r.traj.c_str(), r.deg_s, r.bil.psnr, r.cr.psnr, r.bil.psnr_centre, r.cr.psnr_centre,
            r.cr.mean_abs, 100.0 * r.cr.skip_frac, r.cr.entropy_bpp);
    }
    out("\nPSNR in dB, mean abs err in 8-bit LSB, H0 is the first-order entropy of the\n");
    out("residual (an upper bound on what a good entropy coder would spend on it).\n");

    out("\n## (b) Warp-only chain (no residual at all), centre-crop PSNR\n\n");
    out("| trajectory | 30 frames bil | 30 frames CR | 1 s bil | 1 s CR | 2 s bil | 2 s CR |\n");
    out("|---|---|---|---|---|---|---|\n");
    for (const Row& r : rows) {
        out("| %s | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |\n", r.traj.c_str(),
            r.chain_bil_30f, r.chain_cr_30f, r.chain_bil_1s, r.chain_cr_1s, r.chain_bil_2s,
            r.chain_cr_2s);
    }
    out("\nAll values in dB. The paper's threshold for the Full profile filter is\n");
    out("35 dB held for 30 frames on textured content.\n");

    out("\n## (c) Chain decay curve, Catmull-Rom, dB by frame\n\n");
    out("| trajectory | f1 | f5 | f10 | f30 | f60 | f90 | f120 | f180 |\n");
    out("|---|---|---|---|---|---|---|---|---|\n");
    const size_t pts[] = {0, 4, 9, 29, 59, 89, 119, 179};
    for (const Curve& c : curves) {
        out("| %s |", c.traj.c_str());
        for (size_t p : pts) out(" %.2f |", c.cr.empty() ? 0.0 : c.cr[std::min(p, c.cr.size() - 1)]);
        out("\n");
    }

    if (!out_md.empty()) {
        FILE* f = std::fopen(out_md.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", out_md.c_str());
            return 1;
        }
        std::fwrite(md.data(), 1, md.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "wrote %s (%zu bytes)\n", out_md.c_str(), md.size());
    }
    if (!quiet) std::fputs(md.c_str(), stdout);
    return 0;
}
