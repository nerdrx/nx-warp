// nxvc-stereosim: shared types.  See docs/STEREO.md and stereo/RESULTS.md.
//
// Everything here is deterministic: no floating-point reductions whose order
// depends on threading, no RNG that is not explicitly seeded, no time or
// address dependence.  The sim is single-threaded on purpose.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nxs {

using i32 = int32_t;
using i64 = int64_t;
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

// The codec's Lite/Full profiles both use 64x64 tiles in the shipped bitstream
// (PAPER 6.2).  The sim measures at 64x64 and can be re-run at 32.
constexpr int kTile = 64;

inline i32 clampi(i32 v, i32 lo, i32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- images

// 8-bit luma plane.  The sim is luma-only; chroma residual tracks luma closely
// on rendered content and would not change the mode decision, which is what we
// are measuring.  Documented as a limitation in RESULTS.md.
struct Image {
    int w = 0, h = 0;
    std::vector<u8> px;

    Image() = default;
    Image(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_, 0) {}

    u8& at(int x, int y) { return px[static_cast<size_t>(y) * w + x]; }
    u8 at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }

    // Clamp-to-edge fetch.  This is the codec's border rule: the predictor is
    // always dense, so out-of-frame reference reads replicate the edge instead
    // of producing a hole (PAPER 2.2).
    u8 clamped(int x, int y) const {
        return px[static_cast<size_t>(clampi(y, 0, h - 1)) * w + clampi(x, 0, w - 1)];
    }
};

// Per-pixel eye-space depth in metres.  Infinity (no hit) is stored as kFarZ.
constexpr float kFarZ = 1.0e6f;

struct DepthMap {
    int w = 0, h = 0;
    std::vector<float> z;

    DepthMap() = default;
    DepthMap(int w_, int h_) : w(w_), h(h_), z(static_cast<size_t>(w_) * h_, kFarZ) {}

    float& at(int x, int y) { return z[static_cast<size_t>(y) * w + x]; }
    float at(int x, int y) const { return z[static_cast<size_t>(y) * w + x]; }
};

// ---------------------------------------------------------------- vec/mat

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalize(Vec3 a) {
    double l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1.0 / l) : a;
}

// Row-major 3x3.
struct Mat3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    double& operator()(int r, int c) { return m[r * 3 + c]; }
    double operator()(int r, int c) const { return m[r * 3 + c]; }

    static Mat3 identity() { return Mat3{}; }
};

inline Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a(i, k) * b(k, j);
            r(i, j) = s;
        }
    return r;
}

inline Vec3 mul(const Mat3& a, Vec3 v) {
    return {a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z,
            a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z,
            a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z};
}

inline Mat3 transpose(const Mat3& a) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r(i, j) = a(j, i);
    return r;
}

inline Mat3 rot_x(double rad) {
    Mat3 r;
    double c = std::cos(rad), s = std::sin(rad);
    r(1, 1) = c;  r(1, 2) = -s;
    r(2, 1) = s;  r(2, 2) = c;
    return r;
}
inline Mat3 rot_y(double rad) {
    Mat3 r;
    double c = std::cos(rad), s = std::sin(rad);
    r(0, 0) = c;  r(0, 2) = s;
    r(2, 0) = -s; r(2, 2) = c;
    return r;
}
inline Mat3 rot_z(double rad) {
    Mat3 r;
    double c = std::cos(rad), s = std::sin(rad);
    r(0, 0) = c;  r(0, 1) = -s;
    r(1, 0) = s;  r(1, 1) = c;
    return r;
}

inline Mat3 inverse(const Mat3& a) {
    double d = a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) -
               a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
               a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
    Mat3 r;
    double id = 1.0 / d;
    r(0, 0) = (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) * id;
    r(0, 1) = (a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2)) * id;
    r(0, 2) = (a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1)) * id;
    r(1, 0) = (a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2)) * id;
    r(1, 1) = (a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0)) * id;
    r(1, 2) = (a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2)) * id;
    r(2, 0) = (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0)) * id;
    r(2, 1) = (a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1)) * id;
    r(2, 2) = (a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0)) * id;
    return r;
}

// ---------------------------------------------------------------- hashing

// FNV-1a over bytes.  Used for scene noise and for the determinism test's
// image digests.  Not cryptographic; it only has to be stable.
inline u64 fnv1a(const void* data, size_t n, u64 h = 1469598103934665603ull) {
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline double hash01(int a, int b, int c) {
    u32 h = hash_u32(static_cast<u32>(a) * 73856093U ^ static_cast<u32>(b) * 19349663U ^
                     static_cast<u32>(c) * 83492791U);
    return static_cast<double>(h & 0xffffffU) / 16777215.0;
}

}  // namespace nxs
