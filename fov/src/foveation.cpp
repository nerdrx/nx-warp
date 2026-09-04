// SPDX-License-Identifier: Apache-2.0
#include "nxfov/foveation.hpp"

#include <algorithm>
#include <cmath>

namespace nxfov {

namespace {
constexpr float kPi      = 3.14159265358979323846f;
constexpr float kRad2Deg = 180.0f / kPi;
constexpr float kDeg2Rad = kPi / 180.0f;

inline int tile_count(int px, int tile) { return (px + tile - 1) / tile; }
} // namespace

LensParams pico4_eye() {
    // 2160 px over +/- 0.8568 tangent (+/- 40.6 deg) gives
    // k = 2160 / 1.7136 = 1260.5 px per unit tangent, i.e. 22.0 ppd on axis,
    // and a corner (diagonal) eccentricity of 50.5 deg.
    LensParams l;
    l.tan_left = -0.8568f; l.tan_right = 0.8568f;
    l.tan_down = -0.8568f; l.tan_up    = 0.8568f;
    l.width_px = 2160; l.height_px = 2160; l.tile_size = 64;
    return l;
}

LensParams pico4_eye_wide() {
    LensParams l = pico4_eye();
    const float t = std::tan(50.0f * kDeg2Rad);
    l.tan_left = -t; l.tan_right = t; l.tan_down = -t; l.tan_up = t;
    return l;
}

float ppd_needed(float e_deg, const FoveationConfig& cfg) {
    const float e = e_deg < 0.0f ? 0.0f : e_deg;
    return cfg.ppd_fovea / (1.0f + e / cfg.e2_deg);
}

float ppd_center(const LensParams& lens) {
    const float kx = float(lens.width_px)  / (lens.tan_right - lens.tan_left);
    const float ky = float(lens.height_px) / (lens.tan_up    - lens.tan_down);
    // Conservative: the axis with the fewest pixels per unit tangent decides.
    return std::min(kx, ky) * kDeg2Rad;
}

float ppd_render(const LensParams& lens, float theta_deg) {
    const float c = std::cos(theta_deg * kDeg2Rad);
    return ppd_center(lens) / std::max(c * c, 1e-4f);
}

float level_scale(uint8_t level) {
    switch (level) {
        case 0: return 1.0f;
        case 1: return 0.5f;
        default: return 0.25f;
    }
}

uint8_t r8_from_level(uint8_t level) {
    switch (level) {
        case 0: return 255;
        case 1: return 128;
        default: return 64;
    }
}

uint8_t level_from_r8(uint8_t r8) {
    if (r8 >= 192) return 0;
    if (r8 >= 96)  return 1;
    return 2;
}

FoveationMap foveation_map(const LensParams& lens,
                           const FoveationConfig& cfg,
                           const Gaze* gaze) {
    FoveationMap m;
    m.tiles_x = tile_count(lens.width_px,  lens.tile_size);
    m.tiles_y = tile_count(lens.height_px, lens.tile_size);
    const size_t n = size_t(m.tiles_x) * size_t(m.tiles_y);

    m.r8.resize(n); m.level.resize(n);
    m.ecc_deg.resize(n); m.ecc_raw.resize(n);
    m.theta_deg.resize(n); m.ratio.resize(n); m.weight.resize(n);

    const float ppd_c = ppd_center(lens);

    // Gaze direction in tan space; lens axis when there is no tracker.
    const bool  tracked = (gaze != nullptr && gaze->valid);
    const float gx = tracked ? gaze->tan_x : 0.0f;
    const float gy = tracked ? gaze->tan_y : 0.0f;

    // Direction vectors are (tan_x, tan_y, 1) normalised.
    const float gnorm = 1.0f / std::sqrt(gx * gx + gy * gy + 1.0f);
    const float gvx = gx * gnorm, gvy = gy * gnorm, gvz = gnorm;

    const float scale   = std::max(cfg.region_scale, 0.05f);
    const float box_x   = cfg.box_x_deg * scale;
    const float box_y   = cfg.box_y_deg * scale;
    const float pad_deg = tracked
        ? (cfg.pad_per_ms * gaze->latency_ms + cfg.pad_tracker_deg) * scale
        : 0.0f;

    const float gx_deg = std::atan(gx) * kRad2Deg;
    const float gy_deg = std::atan(gy) * kRad2Deg;

    for (int ty = 0; ty < m.tiles_y; ++ty) {
        for (int tx = 0; tx < m.tiles_x; ++tx) {
            const int i = ty * m.tiles_x + tx;

            // Tile centre in pixels, clamped to the image (the last tile row
            // and column may be partial).
            const float cx = std::min(float(tx * lens.tile_size + lens.tile_size * 0.5f),
                                      float(lens.width_px)  - 0.5f);
            const float cy = std::min(float(ty * lens.tile_size + lens.tile_size * 0.5f),
                                      float(lens.height_px) - 0.5f);

            const float u = cx / float(lens.width_px);
            const float v = cy / float(lens.height_px);
            const float tanx = lens.tan_left + u * (lens.tan_right - lens.tan_left);
            // Image y grows downward; tan_up is at v = 0.
            const float tany = lens.tan_up   + v * (lens.tan_down  - lens.tan_up);

            // Off-axis angle from the lens axis.
            const float r2 = tanx * tanx + tany * tany;
            const float theta = std::atan(std::sqrt(r2)) * kRad2Deg;

            // Angle from the gaze direction (great circle).
            const float tnorm = 1.0f / std::sqrt(r2 + 1.0f);
            const float dot = (tanx * gvx + tany * gvy + gvz) * tnorm;
            const float e = std::acos(std::clamp(dot, -1.0f, 1.0f)) * kRad2Deg;

            float e_eff;
            if (tracked) {
                e_eff = std::max(0.0f, e - pad_deg);
            } else {
                // Elliptical eye box on the lens axis: everything inside the
                // box behaves as if the gaze were on it.
                const float ex = std::atan(tanx) * kRad2Deg - gx_deg;
                const float ey = std::atan(tany) * kRad2Deg - gy_deg;
                const float q = std::sqrt((ex / box_x) * (ex / box_x) +
                                          (ey / box_y) * (ey / box_y));
                e_eff = (q <= 1.0f) ? 0.0f : e * (1.0f - 1.0f / q);
            }

            const float need   = cfg.margin * ppd_needed(e_eff, cfg);
            const float ct     = std::cos(theta * kDeg2Rad);
            const float render = ppd_c / std::max(ct * ct, 1e-4f);
            const float s_raw  = need / render;

            uint8_t lvl;
            if      (s_raw >= cfg.thresh_half)    lvl = 0;
            else if (s_raw >= cfg.thresh_quarter) lvl = 1;
            else                                  lvl = 2;

            const float w = std::clamp(
                std::pow(1.0f + e_eff / cfg.e2_deg, -cfg.weight_exp),
                cfg.weight_floor, 1.0f);

            m.level[i]     = lvl;
            m.r8[i]        = r8_from_level(lvl);
            m.ecc_deg[i]   = e_eff;
            m.ecc_raw[i]   = e;
            m.theta_deg[i] = theta;
            m.ratio[i]     = s_raw;
            m.weight[i]    = w;
        }
    }
    return m;
}

} // namespace nxfov
