// Tile classification on synthetic material.
// SPDX-License-Identifier: Apache-2.0
#include "rc_test_util.h"

#include "nxrc/nxrc.hpp"
#include "nxrc/synth.hpp"

#include <vector>

using namespace nxrc;

namespace {

// Build a TileStats of one tile from a 64x64 luma block.
TileStats one(const std::vector<uint8_t>& t, bool stencil = false) {
    TileStats s; s.resize(1);
    compute_one_tile_stats(t.data(), 64, 64, s.mean_luma[0], s.log_var[0],
                           s.jxx[0], s.jxy[0], s.jyy[0]);
    s.ui_stencil[0] = stencil ? 1u : 0u;
    return s;
}

uint8_t classify(const TileStats& s, const ClassifyConfig& c = {}) {
    std::vector<uint8_t> out;
    classify_tiles(s, c, {}, out);
    return out[0];
}

} // namespace

int main() {
    std::vector<uint8_t> t(64 * 64);

    // ---- the four canonical classes ------------------------------------
    rct::begin("flat: constant");
    synth::flat_const(t.data(), 64, 128);
    CHECK_EQ(int(classify(one(t))), int(TileClass::Flat));

    rct::begin("flat: smooth gradient (large variance, no structure)");
    synth::flat_gradient(t.data(), 64, 40, 90);
    CHECK_EQ(int(classify(one(t))), int(TileClass::Flat));

    rct::begin("flat: full-range gradient must not read as edge");
    synth::flat_gradient(t.data(), 64, 0, 255);
    CHECK_EQ(int(classify(one(t))), int(TileClass::Flat));

    rct::begin("edge: straight step at several angles");
    for (float deg : {0.0f, 15.0f, 45.0f, 70.0f, 90.0f}) {
        synth::hard_edge(t.data(), 64, deg);
        CHECK_MSG(classify(one(t)) == uint8_t(TileClass::Edge),
                  "angle " + std::to_string(deg) + " gave class " +
                  class_name(classify(one(t))));
    }

    rct::begin("edge: low-contrast step is still an edge");
    synth::hard_edge(t.data(), 64, 20.0f, 110, 150);
    CHECK_EQ(int(classify(one(t))), int(TileClass::Edge));

    rct::begin("texture: white noise at several amplitudes");
    for (int amp : {20, 40, 60, 90, 120}) {
        synth::noise_texture(t.data(), 64, 1, amp);
        CHECK_MSG(classify(one(t)) == uint8_t(TileClass::Texture),
                  "amp " + std::to_string(amp) + " gave class " +
                  class_name(classify(one(t))));
    }

    rct::begin("text: glyph field");
    synth::text_glyphs(t.data(), 64);
    CHECK_EQ(int(classify(one(t))), int(TileClass::Text));

    rct::begin("text: the UI stencil always wins");
    synth::flat_const(t.data(), 64, 128);
    CHECK_EQ(int(classify(one(t, /*stencil=*/true))), int(TileClass::Text));

    rct::begin("text: high-contrast oriented micro-structure");
    synth::oriented_stripes(t.data(), 64, 0.0f, 4);
    CHECK_EQ(int(classify(one(t))), int(TileClass::Text));

    // ---- the discriminator itself --------------------------------------
    rct::begin("R separates white noise from everything else");
    {
        // White noise sits at R = 1 by construction, whatever its amplitude.
        for (int amp : {30, 60, 120}) {
            synth::noise_texture(t.data(), 64, 9, amp);
            TileStats s = one(t);
            const float g = tile_gradient_energy(s.jxx[0], s.jyy[0], 64 * 64);
            const float r = tile_frequency_ratio(g, s.log_var[0]);
            CHECK_MSG(r > 0.93f, "white noise amp " + std::to_string(amp) +
                                 " has R = " + std::to_string(r));
        }
        synth::text_glyphs(t.data(), 64);
        TileStats s = one(t);
        const float g = tile_gradient_energy(s.jxx[0], s.jyy[0], 64 * 64);
        CHECK_LT(tile_frequency_ratio(g, s.log_var[0]), 0.93f);
    }

    rct::begin("coherence separates a single edge from noise");
    {
        synth::hard_edge(t.data(), 64, 30.0f);
        TileStats e = one(t);
        synth::noise_texture(t.data(), 64, 4, 60);
        TileStats x = one(t);
        CHECK(tile_coherence(e.jxx[0], e.jxy[0], e.jyy[0]) > 0.45f);
        CHECK(tile_coherence(x.jxx[0], x.jxy[0], x.jyy[0]) < 0.45f);
    }

    // ---- partial tiles --------------------------------------------------
    rct::begin("compute_tile_stats handles a partial edge tile");
    {
        // 100x70 image: the right column and bottom row of tiles are partial.
        std::vector<uint8_t> img(100 * 70);
        for (int y = 0; y < 70; ++y)
            for (int x = 0; x < 100; ++x) img[y * 100 + x] = uint8_t(x < 50 ? 20 : 220);
        TileStats s;
        compute_tile_stats(img.data(), 100, 100, 70, 64, s);
        CHECK_EQ(int(s.size()), 4);   // 2x2 tiles
        std::vector<uint8_t> cls;
        classify_tiles(s, {}, {}, cls);
        // Tile (0,0) contains the step at x=50; tile (1,0) is all bright.
        CHECK_EQ(int(cls[0]), int(TileClass::Edge));
        CHECK_EQ(int(cls[1]), int(TileClass::Flat));
    }

    // ---- hysteresis -----------------------------------------------------
    rct::begin("hysteresis holds a class through a threshold nudge");
    {
        ClassifyConfig c;
        TileStats s; s.resize(1);
        // Park the tile just inside Flat on gradient energy.
        s.mean_luma[0] = 128.0f;
        s.log_var[0]   = 8.0f;
        const float g_just_flat = c.flat_gradient * 0.999f;
        s.jxx[0] = g_just_flat * 64 * 64 * 0.5f;
        s.jyy[0] = g_just_flat * 64 * 64 * 0.5f;
        s.jxy[0] = 0.0f;
        std::vector<uint8_t> out;
        classify_tiles(s, c, {}, out);
        CHECK_EQ(int(out[0]), int(TileClass::Flat));

        // Nudge it just over the line.  With no history it flips; with a
        // Flat history the band holds it.
        const float g_just_over = c.flat_gradient * 1.05f;
        s.jxx[0] = g_just_over * 64 * 64 * 0.5f;
        s.jyy[0] = g_just_over * 64 * 64 * 0.5f;
        std::vector<uint8_t> fresh, held;
        classify_tiles(s, c, {}, fresh);
        const uint8_t prev = uint8_t(TileClass::Flat);
        classify_tiles(s, c, std::span<const uint8_t>(&prev, 1), held);
        CHECK_MSG(fresh[0] != uint8_t(TileClass::Flat), "no-history tile should flip");
        CHECK_EQ(int(held[0]), int(TileClass::Flat));
    }

    rct::begin("hysteresis can be switched off");
    {
        ClassifyConfig c; c.hysteresis = false;
        TileStats s; s.resize(1);
        s.mean_luma[0] = 128.0f; s.log_var[0] = 8.0f;
        const float g = c.flat_gradient * 1.05f;
        s.jxx[0] = g * 64 * 64 * 0.5f; s.jyy[0] = g * 64 * 64 * 0.5f; s.jxy[0] = 0;
        const uint8_t prev = uint8_t(TileClass::Flat);
        std::vector<uint8_t> out;
        classify_tiles(s, c, std::span<const uint8_t>(&prev, 1), out);
        CHECK(out[0] != uint8_t(TileClass::Flat));
    }

    return rct::finish("rc.classify");
}
