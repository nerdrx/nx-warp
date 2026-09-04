// SPDX-License-Identifier: Apache-2.0
#include "nxrc/synth.hpp"

#include <algorithm>
#include <cmath>

namespace nxrc::synth {

namespace {
inline uint32_t rng(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
inline uint8_t clip8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// 5x7 stencils for a handful of glyphs, one bit per pixel, MSB unused.
const uint8_t kGlyphs[10][7] = {
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
};
} // namespace

void flat_const(uint8_t* out, int n, int value) {
    std::fill(out, out + size_t(n) * size_t(n), clip8(value));
}

void flat_gradient(uint8_t* out, int n, int lo, int hi) {
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float t = float(x + y) / float(2 * (n - 1));
            out[size_t(y) * n + x] = clip8(int(std::lround(lo + t * (hi - lo))));
        }
}

void hard_edge(uint8_t* out, int n, float deg, int lo, int hi) {
    const float r  = deg * 3.14159265358979f / 180.0f;
    const float nx = std::cos(r), ny = std::sin(r);
    const float c  = 0.5f * (n - 1) * (nx + ny);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            out[size_t(y) * n + x] = (nx * x + ny * y >= c) ? clip8(hi) : clip8(lo);
}

void noise_texture(uint8_t* out, int n, uint32_t seed, int amp, int base) {
    uint32_t s = seed ? seed : 1u;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int v = int(rng(s) % uint32_t(2 * amp + 1)) - amp;
            out[size_t(y) * n + x] = clip8(base + v);
        }
}

void text_glyphs(uint8_t* out, int n, uint32_t seed) {
    std::fill(out, out + size_t(n) * size_t(n), uint8_t(235)); // paper white
    uint32_t s = seed ? seed : 1u;
    const int cw = 6, ch = 9;                 // 5x7 glyph on a 6x9 cell
    for (int gy = 0; gy + ch <= n; gy += ch) {
        for (int gx = 0; gx + cw <= n; gx += cw) {
            const uint8_t* g = kGlyphs[rng(s) % 10];
            for (int r = 0; r < 7; ++r)
                for (int c2 = 0; c2 < 5; ++c2)
                    if (g[r] & (1u << (4 - c2)))
                        out[size_t(gy + r) * n + (gx + c2)] = 20; // ink
        }
    }
}

void oriented_stripes(uint8_t* out, int n, float deg, int period) {
    const float r  = deg * 3.14159265358979f / 180.0f;
    const float nx = std::cos(r), ny = std::sin(r);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float p = nx * x + ny * y;
            const int   k = int(std::floor(p / float(period)));
            out[size_t(y) * n + x] = (k & 1) ? 235 : 20;
        }
}

} // namespace nxrc::synth
