// SPDX-License-Identifier: Apache-2.0
#include "scene.hpp"
#include "nxrc/synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace rcsim {

namespace {

inline uint32_t rng(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline float    frand(uint32_t& s) { return float(rng(s) & 0xFFFFFF) / float(0x1000000); }

struct MatStats { float mean, log_var, jxx, jxy, jyy; float a_true, sad; bool stencil; };

// Measured once from real 64x64 tiles at startup.
MatStats g_mat[MAT_COUNT];
bool     g_measured = false;

void measure_materials() {
    if (g_measured) return;
    std::vector<uint8_t> t(64 * 64);
    auto grab = [&](Material m, float a_true, float sad, bool stencil) {
        MatStats& s = g_mat[m];
        nxrc::compute_one_tile_stats(t.data(), 64, 64, s.mean, s.log_var,
                                     s.jxx, s.jxy, s.jyy);
        s.a_true = a_true; s.sad = sad; s.stencil = stencil;
    };
    // Band-limited noise, the realistic texture shape.
    auto bl = [&](uint32_t seed, int amp, int k) {
        std::vector<float> w(64 * 64);
        uint32_t s = seed;
        for (auto& v : w) v = float(int(rng(s) % uint32_t(2 * amp + 1)) - amp);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                float acc = 0; int c = 0;
                for (int dy = -k; dy <= k; ++dy)
                    for (int dx = -k; dx <= k; ++dx) {
                        const int yy = std::clamp(y + dy, 0, 63);
                        const int xx = std::clamp(x + dx, 0, 63);
                        acc += w[yy * 64 + xx]; ++c;
                    }
                t[y * 64 + x] = uint8_t(std::clamp(int(std::lround(128 + acc / c)), 0, 255));
            }
    };

    nxrc::synth::flat_gradient(t.data(), 64, 60, 110);  grab(MAT_SKY,     6000.0f,  1.4f, false);
    bl(11, 90, 2);                                       grab(MAT_WALL,   26000.0f,  4.0f, false);
    bl(23, 110, 1);                                      grab(MAT_GROUND, 48000.0f,  6.0f, false);
    nxrc::synth::hard_edge(t.data(), 64, 22.0f);         grab(MAT_OUTLINE,18000.0f,  5.0f, false);
    nxrc::synth::text_glyphs(t.data(), 64);              grab(MAT_UI,     24000.0f,  2.5f, true);
    nxrc::synth::noise_texture(t.data(), 64, 5, 55);     grab(MAT_FOLIAGE,90000.0f, 14.0f, false);
    g_measured = true;
}

} // namespace

const char* Scene::material_name(uint8_t m) const {
    static const char* kN[] = {"sky", "wall", "ground", "outline", "ui", "foliage"};
    return m < MAT_COUNT ? kN[m] : "?";
}

nxfov::FoveationMap stereo_map(const nxfov::FoveationMap& one, int eyes) {
    nxfov::FoveationMap m = one;
    for (int e = 1; e < eyes; ++e) {
        m.r8.insert(m.r8.end(), one.r8.begin(), one.r8.end());
        m.level.insert(m.level.end(), one.level.begin(), one.level.end());
        m.ecc_deg.insert(m.ecc_deg.end(), one.ecc_deg.begin(), one.ecc_deg.end());
        m.ecc_raw.insert(m.ecc_raw.end(), one.ecc_raw.begin(), one.ecc_raw.end());
        m.theta_deg.insert(m.theta_deg.end(), one.theta_deg.begin(), one.theta_deg.end());
        m.ratio.insert(m.ratio.end(), one.ratio.begin(), one.ratio.end());
        m.weight.insert(m.weight.end(), one.weight.begin(), one.weight.end());
    }
    m.tiles_y = one.tiles_y * eyes;
    return m;
}

void Scene::build(int tx, int ty, int n_eyes, uint32_t seed) {
    measure_materials();
    tiles_x = tx; tiles_y = ty; eyes = n_eyes;
    n = size_t(tx) * size_t(ty) * size_t(n_eyes);

    material.assign(n, MAT_WALL);
    a_true.assign(n, 0.0f);
    stats.resize(n);
    complexity.assign(n, 0.0f);
    slip.assign(n, 0.0f);
    cuts = {120, 260};

    uint32_t s = seed ? seed : 1u;
    for (int e = 0; e < n_eyes; ++e) {
        for (int y = 0; y < ty; ++y) {
            for (int x = 0; x < tx; ++x) {
                const size_t i = size_t(e) * tx * ty + size_t(y) * tx + x;
                const float fy = float(y) / float(ty - 1);
                const float fx = float(x) / float(tx - 1);

                uint8_t m;
                if (fy < 0.30f)                       m = MAT_SKY;
                else if (fy > 0.72f)                  m = MAT_GROUND;
                else if (frand(s) < 0.18f)            m = MAT_OUTLINE;
                else if (fx > 0.62f && fy > 0.42f && fy < 0.66f) m = MAT_FOLIAGE;
                else                                  m = MAT_WALL;

                // A UI panel: 6x4 tiles, low and slightly right of centre,
                // the classic health/ammo/menu block.
                const int ux0 = tx / 2 - 1, uy0 = int(ty * 0.62f);
                if (x >= ux0 && x < ux0 + 6 && y >= uy0 && y < uy0 + 4) m = MAT_UI;

                material[i] = m;
                a_true[i] = g_mat[m].a_true * (0.75f + 0.5f * frand(s));
            }
        }
    }
}

void Scene::step(int f, uint32_t seed) {
    uint32_t s = seed ^ uint32_t(f * 2654435761u);

    const bool is_cut = std::find(cuts.begin(), cuts.end(), f) != cuts.end();
    intra_ratio = is_cut ? 0.85f : 0.05f + 0.05f * frand(s);

    // Head motion: a slow sweep with two fast turns.
    head_speed_deg_s = 20.0f + 40.0f * std::fabs(std::sin(float(f) * 0.021f));
    if ((f > 150 && f < 175) || (f > 300 && f < 320)) head_speed_deg_s = 170.0f;

    // A moving object sweeping horizontally through the mid band.
    const float obj_x = 0.5f * float(tiles_x) *
                        (1.0f + std::sin(float(f) * 0.035f)) * 0.9f;
    const float obj_y = float(tiles_y) * 0.52f;

    for (int e = 0; e < eyes; ++e) {
        for (int y = 0; y < tiles_y; ++y) {
            for (int x = 0; x < tiles_x; ++x) {
                const size_t i = size_t(e) * tiles_x * tiles_y + size_t(y) * tiles_x + x;
                const uint8_t m = material[i];
                const MatStats& g = g_mat[m];

                // Statistics with a little frame-to-frame jitter so the
                // classifier's hysteresis is exercised.
                const float j = 1.0f + 0.04f * (frand(s) - 0.5f);
                stats.mean_luma[i] = g.mean * j;
                stats.log_var[i]   = g.log_var + 0.10f * (frand(s) - 0.5f);
                stats.jxx[i]       = g.jxx * j;
                stats.jxy[i]       = g.jxy * j;
                stats.jyy[i]       = g.jyy * j;
                stats.ui_stencil[i]= g.stencil ? 1u : 0u;

                // Complexity: the warped SAD.  Head motion raises it
                // everywhere, the moving object raises it locally, and a cut
                // makes the whole frame intra-sized.
                const float dx = float(x) - obj_x, dy = float(y) - obj_y;
                const float d  = std::sqrt(dx * dx + dy * dy);
                const float obj = 6.0f * std::exp(-d * d / 18.0f);
                float c = g.sad * (1.0f + head_speed_deg_s / 90.0f) + obj;
                if (m == MAT_UI) c = g.sad;                 // head-locked HUD, small updates
                if (is_cut && m != MAT_UI) c *= 9.0f;
                complexity[i] = c * (0.85f + 0.3f * frand(s));

                // Retinal slip: the object's own motion, plus foliage.
                float sl = 45.0f * std::exp(-d * d / 18.0f);
                if (m == MAT_FOLIAGE) sl += 18.0f;
                slip[i] = sl;

                if (is_cut) a_true[i] = g.a_true * (0.75f + 0.5f * frand(s));
            }
        }
    }
}

bool Scene::load_dump(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    // Format: one line per tile, "mean log_var jxx jxy jyy sad slip stencil".
    std::vector<float> m, lv, xx, xy, yy, sad, sl;
    std::vector<uint8_t> st;
    float a, b, c, d, e2, f2, g2; int h;
    while (in >> a >> b >> c >> d >> e2 >> f2 >> g2 >> h) {
        m.push_back(a); lv.push_back(b); xx.push_back(c); xy.push_back(d);
        yy.push_back(e2); sad.push_back(f2); sl.push_back(g2);
        st.push_back(uint8_t(h));
    }
    if (m.size() != n) {
        std::fprintf(stderr, "rcsim: dump has %zu tiles, scene has %zu; ignoring\n",
                     m.size(), n);
        return false;
    }
    stats.mean_luma = m; stats.log_var = lv;
    stats.jxx = xx; stats.jxy = xy; stats.jyy = yy;
    stats.ui_stencil = st;
    complexity = sad; slip = sl;
    return true;
}

} // namespace rcsim
