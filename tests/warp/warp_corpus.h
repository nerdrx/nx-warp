// NX Warp -- shared deterministic test corpus.
//
// Everything here is integer and compiler-independent by construction so that
// the same corpus is produced under -O0 and -O3, by g++ and clang++, and by
// the GPU harness. Do not introduce floating point into the corpus *inputs*;
// pose generation goes through fixed integer angle steps and only the
// (encoder-side, double precision) homography derivation sees a double.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef NXVC_WARP_TEST_CORPUS_H
#define NXVC_WARP_TEST_CORPUS_H

#include <cmath>
#include <cstdint>
#include <vector>

#include "nxvc/warp.h"

namespace nxvc::warp::test {

// splitmix64 -- fixed, portable, no library dependency.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return static_cast<uint32_t>(next() >> 32); }
    // Uniform in [lo, hi].
    int32_t range(int32_t lo, int32_t hi) {
        return lo + static_cast<int32_t>(u32() % static_cast<uint32_t>(hi - lo + 1));
    }
};

// A reference picture with a deterministic mix of high-frequency noise, hard
// edges and smooth ramps -- the three things that expose interpolation bugs.
struct Picture {
    std::vector<uint16_t> data;
    RefImage img;
};

inline Picture make_picture(int w, int h, int channels, int max_value, uint64_t seed) {
    Picture p;
    p.data.assign(static_cast<size_t>(w) * h * channels, 0);
    Rng rng(seed);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < channels; ++c) {
                const uint32_t r = rng.u32();
                int32_t v;
                switch ((x / 17 + y / 13 + c) % 3) {
                    case 0: v = static_cast<int32_t>(r % (max_value + 1u)); break;         // noise
                    case 1: v = ((x ^ y) & 8) ? max_value : 0; break;                      // edges
                    default: v = ((x * 3 + y * 5 + c * 7) * max_value / (w + h)) % (max_value + 1);
                }
                p.data[(static_cast<size_t>(y) * w + x) * channels + c] =
                    static_cast<uint16_t>(v);
            }
        }
    }
    p.img.data = p.data.data();
    p.img.width = w;
    p.img.height = h;
    p.img.stride = w * channels;
    p.img.channels = channels;
    p.img.max_value = max_value;
    return p;
}

// A smooth, band-limited picture. Used where the integer-vs-float pixel
// tolerance has to be meaningful: on white noise a 1/32-pel coordinate
// difference is a full-scale sample difference and bounds nothing.
inline Picture make_smooth_picture(int w, int h, int channels, int max_value) {
    Picture p;
    p.data.assign(static_cast<size_t>(w) * h * channels, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < channels; ++c) {
                const double fx = 2.0 * 3.14159265358979 * x / 64.0;
                const double fy = 2.0 * 3.14159265358979 * y / 48.0;
                const double s = 0.5 + 0.25 * std::sin(fx + c) + 0.2 * std::cos(fy - c);
                int32_t v = static_cast<int32_t>(s * max_value);
                v = v < 0 ? 0 : (v > max_value ? max_value : v);
                p.data[(static_cast<size_t>(y) * w + x) * channels + c] =
                    static_cast<uint16_t>(v);
            }
        }
    }
    p.img.data = p.data.data();
    p.img.width = w;
    p.img.height = h;
    p.img.stride = w * channels;
    p.img.channels = channels;
    p.img.max_value = max_value;
    return p;
}

// One corpus entry: everything warp_tile() needs.
struct Case {
    Homography H;
    double Hd[9];
    int32_t tile_x, tile_y;
    int32_t mv[2];
    Filter filter;
    Mode mode;
    // Provenance, for diagnostics.
    double yaw_prev, pitch_prev, roll_prev, yaw_cur, pitch_cur, roll_cur;
};

inline Quat quat_from_ypr(double yaw, double pitch, double roll) {
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

// The reference angular envelope: 3.3 degrees per frame is 297 deg/s at 90 Hz,
// which paper 2.2 calls a fast head turn. Errors are quoted against this.
inline constexpr double kFastDegPerFrame = 3.3;

// Draw one random case. The rotation delta has a *fixed magnitude* of
// `deg_per_frame` about a uniformly random axis, so the corpus probes the
// envelope boundary rather than averaging over its interior.
inline bool make_case(Rng& rng, int frame_w, int frame_h, Case* out,
                      double deg_per_frame = kFastDegPerFrame) {
    const double kDeg = 3.14159265358979 / 180.0;
    // Absolute orientation anywhere in a wide envelope; delta bounded.
    out->yaw_prev = rng.range(-1800, 1800) * 0.1 * kDeg;
    out->pitch_prev = rng.range(-600, 600) * 0.1 * kDeg;
    out->roll_prev = rng.range(-300, 300) * 0.1 * kDeg;
    const double a = rng.range(0, 3600) * 0.1 * kDeg;
    const double b = rng.range(0, 3600) * 0.1 * kDeg;
    const double d = deg_per_frame * kDeg;
    out->yaw_cur = out->yaw_prev + d * std::cos(a) * std::cos(b);
    out->pitch_cur = out->pitch_prev + d * std::sin(a) * std::cos(b);
    out->roll_cur = out->roll_prev + d * std::sin(b);

    Fov fov;
    fov.angle_left = -0.83 - rng.range(0, 10) * 0.005;
    fov.angle_right = 0.83 + rng.range(0, 10) * 0.005;
    fov.angle_up = 0.90 + rng.range(0, 10) * 0.005;
    fov.angle_down = -0.90 - rng.range(0, 10) * 0.005;

    const Quat qp = quat_from_ypr(out->yaw_prev, out->pitch_prev, out->roll_prev);
    const Quat qc = quat_from_ypr(out->yaw_cur, out->pitch_cur, out->roll_cur);
    if (!derive_homography(qp, fov, qc, fov, frame_w, frame_h, &out->H)) return false;
    exact_homography(qp, fov, qc, fov, frame_w, frame_h, out->Hd);

    // Tile origins are multiples of 64 within the picture.
    const int tiles_x = frame_w / kTile;
    const int tiles_y = frame_h / kTile;
    out->tile_x = rng.range(0, tiles_x - 1) * kTile;
    out->tile_y = rng.range(0, tiles_y - 1) * kTile;

    out->mv[0] = rng.range(-256, 256);  // +-64 px at 1/4 pel
    out->mv[1] = rng.range(-256, 256);
    out->filter = (rng.u32() & 1u) ? kFilterCatmullRom : kFilterBilinear;
    // STATIC_MV is roughly 1 case in 8, matching head-locked content share.
    out->mode = (rng.u32() % 8u == 0u) ? kModeStatic : kModeWarp;
    return true;
}

// Draw one random case with NO floating point anywhere: the nine int32 are
// synthesised directly inside their formats. This is what the determinism and
// GPU-diff corpora use, so that a hash mismatch can only come from the
// normative integer path and never from libm or from constant folding of a
// double expression at a different precision.
//
// Envelopes (frame <= 1024, so |cx| <= 576):
//   h00,h11 = 2^21 +- 2^17   (scale within +-6 %)
//   h01,h10 = +- 2^17        (shear/roll up to 6 %)
//   h02,h12 = +- 200 * 2^21  (translation up to 200 px, i.e. ~850 deg/s)
//   h20,h21 = +- 200000      (perspective; keeps den inside [2^28, 2^30))
inline void make_case_int(Rng& rng, int frame_w, int frame_h, Case* out) {
    Homography& H = out->H;
    H.ox = frame_w / 2;
    H.oy = frame_h / 2;
    H.h[0] = (1 << kQNum) + rng.range(-(1 << 17), 1 << 17);
    H.h[1] = rng.range(-(1 << 17), 1 << 17);
    H.h[2] = rng.range(-200, 200) * (1 << kQNum);
    H.h[3] = rng.range(-(1 << 17), 1 << 17);
    H.h[4] = (1 << kQNum) + rng.range(-(1 << 17), 1 << 17);
    H.h[5] = rng.range(-200, 200) * (1 << kQNum);
    H.h[6] = rng.range(-200000, 200000);
    H.h[7] = rng.range(-200000, 200000);
    H.h[8] = 1 << kQDen;

    const int tiles_x = frame_w / kTile;
    const int tiles_y = frame_h / kTile;
    out->tile_x = rng.range(0, tiles_x - 1) * kTile;
    out->tile_y = rng.range(0, tiles_y - 1) * kTile;
    out->mv[0] = rng.range(-256, 256);
    out->mv[1] = rng.range(-256, 256);
    out->filter = (rng.u32() & 1u) ? kFilterCatmullRom : kFilterBilinear;
    out->mode = (rng.u32() % 8u == 0u) ? kModeStatic : kModeWarp;
    for (int i = 0; i < 9; ++i) out->Hd[i] = 0.0;
}

// FNV-1a over bytes. Stable across compilers and optimisation levels.
struct Hash {
    uint64_t h = 1469598103934665603ull;
    void byte(uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    }
    void u16(uint16_t v) {
        byte(static_cast<uint8_t>(v & 0xff));
        byte(static_cast<uint8_t>(v >> 8));
    }
    void i32(int32_t v) {
        for (int i = 0; i < 4; ++i) byte(static_cast<uint8_t>((static_cast<uint32_t>(v) >> (8 * i)) & 0xff));
    }
};

}  // namespace nxvc::warp::test

#endif  // NXVC_WARP_TEST_CORPUS_H
