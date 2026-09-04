// SPDX-License-Identifier: Apache-2.0
//
// Tile statistics and the four-class content classifier of PAPER.md 4.6.1.

#include "nxrc/nxrc.hpp"

#include <algorithm>
#include <cmath>

namespace nxrc {

const char* class_name(uint8_t c) {
    switch (c) {
        case uint8_t(TileClass::Flat):    return "flat";
        case uint8_t(TileClass::Texture): return "texture";
        case uint8_t(TileClass::Edge):    return "edge";
        case uint8_t(TileClass::Text):    return "text";
        default: return "?";
    }
}

void TileStats::resize(size_t n) {
    mean_luma.assign(n, 0.0f);
    log_var.assign(n, 0.0f);
    jxx.assign(n, 0.0f);
    jxy.assign(n, 0.0f);
    jyy.assign(n, 0.0f);
    ui_stencil.assign(n, 0u);
}

float tile_coherence(float jxx, float jxy, float jyy) {
    const float tr = jxx + jyy;
    if (tr <= 1e-6f) return 0.0f;
    const float d  = jxx - jyy;
    const float rt = std::sqrt(d * d + 4.0f * jxy * jxy);
    return std::clamp(rt / tr, 0.0f, 1.0f);
}

float tile_gradient_energy(float jxx, float jyy, int tile_pixels) {
    if (tile_pixels <= 0) return 0.0f;
    return (jxx + jyy) / float(tile_pixels);
}

float tile_frequency_ratio(float grad_energy, float log_var) {
    const float var = std::exp2(log_var) - 1.0f;
    return grad_energy / std::max(var, 1e-3f);
}

// ------------------------------------------------------------------ stats --

void compute_one_tile_stats(const uint8_t* luma, int stride, int n,
                            float& mean, float& log_var,
                            float& jxx, float& jxy, float& jyy) {
    double s = 0.0, ss = 0.0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double v = luma[size_t(y) * size_t(stride) + size_t(x)];
            s += v; ss += v * v;
        }
    const double np = double(n) * double(n);
    mean = float(s / np);
    const double var = std::max(0.0, ss / np - (s / np) * (s / np));
    log_var = float(std::log2(var + 1.0));

    double a = 0.0, b = 0.0, c = 0.0;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int xm = std::max(x - 1, 0), xp = std::min(x + 1, n - 1);
            const int ym = std::max(y - 1, 0), yp = std::min(y + 1, n - 1);
            const double gx = 0.5 * (double(luma[size_t(y) * stride + xp]) -
                                     double(luma[size_t(y) * stride + xm]));
            const double gy = 0.5 * (double(luma[size_t(yp) * stride + x]) -
                                     double(luma[size_t(ym) * stride + x]));
            a += gx * gx; b += gx * gy; c += gy * gy;
        }
    }
    jxx = float(a); jxy = float(b); jyy = float(c);
}

void compute_tile_stats(const uint8_t* luma, int stride,
                        int width, int height, int tile_size,
                        TileStats& out) {
    const int tx_n = (width  + tile_size - 1) / tile_size;
    const int ty_n = (height + tile_size - 1) / tile_size;
    out.resize(size_t(tx_n) * size_t(ty_n));

    for (int ty = 0; ty < ty_n; ++ty) {
        for (int tx = 0; tx < tx_n; ++tx) {
            const int i  = ty * tx_n + tx;
            const int x0 = tx * tile_size, y0 = ty * tile_size;
            const int w  = std::min(tile_size, width  - x0);
            const int h  = std::min(tile_size, height - y0);

            double s = 0.0, ss = 0.0, a = 0.0, b = 0.0, c = 0.0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t p = size_t(y0 + y) * size_t(stride) + size_t(x0 + x);
                    const double v = luma[p];
                    s += v; ss += v * v;
                    const int xm = x0 + std::max(x - 1, 0);
                    const int xp = x0 + std::min(x + 1, w - 1);
                    const int ym = y0 + std::max(y - 1, 0);
                    const int yp = y0 + std::min(y + 1, h - 1);
                    const double gx = 0.5 * (double(luma[size_t(y0 + y) * stride + xp]) -
                                             double(luma[size_t(y0 + y) * stride + xm]));
                    const double gy = 0.5 * (double(luma[size_t(yp) * stride + x0 + x]) -
                                             double(luma[size_t(ym) * stride + x0 + x]));
                    a += gx * gx; b += gx * gy; c += gy * gy;
                }
            }
            const double np = std::max(1.0, double(w) * double(h));
            out.mean_luma[i] = float(s / np);
            const double var = std::max(0.0, ss / np - (s / np) * (s / np));
            out.log_var[i]   = float(std::log2(var + 1.0));
            // Normalise the tensor to a full tile so the thresholds do not
            // depend on the partial-tile size.
            const double k = (double(tile_size) * double(tile_size)) / np;
            out.jxx[i] = float(a * k);
            out.jxy[i] = float(b * k);
            out.jyy[i] = float(c * k);
        }
    }
}

// --------------------------------------------------------------- classify --

namespace {

// Order matters and is part of the specification:
//   1. UI stencil    -> Text   (the reliable route; PAPER.md 5.2)
//   2. no structure  -> Flat
//   3. hard micro-structure at full contrast, not white noise -> Text
//   4. one dominant gradient orientation -> Edge
//   5. otherwise     -> Texture
uint8_t classify_one(float log_var, float coh, float grad, float r,
                     bool stencil, const ClassifyConfig& cfg,
                     uint8_t prev, bool have_prev) {
    if (stencil) return uint8_t(TileClass::Text);

    const bool hy = have_prev && cfg.hysteresis;
    const bool was_flat = hy && prev == uint8_t(TileClass::Flat);
    const bool was_text = hy && prev == uint8_t(TileClass::Text);
    const bool was_edge = hy && prev == uint8_t(TileClass::Edge);

    // A tile already in a class keeps it until it crosses the far side of
    // the band; a tile outside has to cross the near side.
    const float gm = cfg.gradient_margin;

    const float flat_g = cfg.flat_gradient  * (was_flat ? (1.0f + gm) : 1.0f);
    const float flat_a = cfg.flat_activity  + (was_flat ? cfg.activity_margin : 0.0f);
    if (grad < flat_g || log_var < flat_a)
        return uint8_t(TileClass::Flat);

    const float text_g = cfg.text_gradient * (was_text ? (1.0f - gm) : 1.0f);
    const float text_a = cfg.text_activity - (was_text ? cfg.activity_margin : 0.0f);
    if (grad >= text_g && log_var >= text_a && r <= cfg.text_r_max)
        return uint8_t(TileClass::Text);

    const float edge_c = cfg.edge_coherence -
                         ((was_edge || was_text) ? cfg.coherence_margin : 0.0f);
    if (coh >= edge_c) return uint8_t(TileClass::Edge);

    return uint8_t(TileClass::Texture);
}

} // namespace

void classify_tiles(const TileStats& stats, const ClassifyConfig& cfg,
                    std::span<const uint8_t> prev,
                    std::vector<uint8_t>& out_class) {
    const size_t n = stats.size();
    out_class.resize(n);
    const bool have_prev = prev.size() == n;
    const bool have_sten = stats.ui_stencil.size() == n;
    const int  tile_px   = 64 * 64;

    for (size_t i = 0; i < n; ++i) {
        const float coh  = tile_coherence(stats.jxx[i], stats.jxy[i], stats.jyy[i]);
        const float grad = tile_gradient_energy(stats.jxx[i], stats.jyy[i], tile_px);
        const float r    = tile_frequency_ratio(grad, stats.log_var[i]);
        out_class[i] = classify_one(stats.log_var[i], coh, grad, r,
                                    have_sten && stats.ui_stencil[i] != 0,
                                    cfg, have_prev ? prev[i] : 0u, have_prev);
    }
}

} // namespace nxrc
