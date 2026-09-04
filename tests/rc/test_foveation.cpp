// Foveation map shape: centre at s = 1, periphery at 1/4, eye box, gaze.
// SPDX-License-Identifier: Apache-2.0
#include "rc_test_util.h"

#include "nxfov/foveation.hpp"

#include <algorithm>
#include <cmath>

using namespace nxfov;

namespace {
int centre_index(const FoveationMap& m) {
    return (m.tiles_y / 2) * m.tiles_x + m.tiles_x / 2;
}
} // namespace

int main() {
    const LensParams lens = pico4_eye();
    FoveationConfig cfg;

    // ---- the lens model -------------------------------------------------
    rct::begin("Pico 4 default reproduces PAPER.md 5.1.2");
    CHECK_NEAR(ppd_center(lens), 22.0, 0.2);
    // ppd_render = ppd_center / cos^2(theta)
    CHECK_NEAR(ppd_render(lens, 18.0f), 24.3, 0.3);
    CHECK_NEAR(ppd_render(lens, 35.0f), 32.8, 0.5);
    CHECK_NEAR(ppd_render(lens, 50.0f), 53.2, 1.0);

    rct::begin("acuity model");
    CHECK_NEAR(ppd_needed(0.0f, cfg), 60.0, 1e-3);
    CHECK_NEAR(ppd_needed(2.3f, cfg), 30.0, 1e-3);
    CHECK_NEAR(ppd_needed(14.0f, cfg), 8.44, 0.05);

    // ---- fixed foveation ------------------------------------------------
    const FoveationMap fx = foveation_map(lens, cfg, nullptr);

    rct::begin("map geometry");
    CHECK_EQ(fx.tiles_x, 34);
    CHECK_EQ(fx.tiles_y, 34);
    CHECK_EQ(int(fx.size()), 34 * 34);

    rct::begin("centre tile is s = 1");
    {
        const int c = centre_index(fx);
        CHECK_EQ(int(fx.level[c]), 0);
        CHECK_EQ(int(fx.r8[c]), 255);
        CHECK_NEAR(level_scale(fx.level[c]), 1.0, 1e-6);
        CHECK_NEAR(fx.ecc_deg[c], 0.0, 1e-3);   // inside the eye box
    }

    rct::begin("corner tiles are s = 1/4");
    {
        const int tl = 0;
        const int tr = fx.tiles_x - 1;
        const int bl = (fx.tiles_y - 1) * fx.tiles_x;
        const int br = fx.tiles_y * fx.tiles_x - 1;
        for (int c : {tl, tr, bl, br}) {
            CHECK_EQ(int(fx.level[c]), 2);
            CHECK_EQ(int(fx.r8[c]), 64);
            CHECK_NEAR(level_scale(fx.level[c]), 0.25, 1e-6);
        }
        CHECK_NEAR(fx.ecc_raw[br], 50.0, 1.5);   // the corner is ~50 deg off axis
    }

    rct::begin("levels are monotone along a row out of the centre");
    {
        const int y = fx.tiles_y / 2;
        uint8_t prev = 0;
        for (int x = fx.tiles_x / 2; x < fx.tiles_x; ++x) {
            const uint8_t l = fx.level[y * fx.tiles_x + x];
            CHECK_MSG(l >= prev, "level fell at x = " + std::to_string(x));
            prev = l;
        }
    }

    rct::begin("all three ladder levels are present, and only those");
    {
        long lv[3] = {0, 0, 0};
        for (uint8_t l : fx.level) { CHECK(l <= 2); ++lv[l]; }
        CHECK(lv[0] > 0); CHECK(lv[1] > 0); CHECK(lv[2] > 0);
        // PAPER.md 5.1.3 predicts about half the samples for fixed foveation.
        const double frac = (lv[0] + lv[1] / 4.0 + lv[2] / 16.0) / double(fx.size());
        CHECK_MSG(frac > 0.35 && frac < 0.60,
                  "sample fraction " + std::to_string(frac));
    }

    rct::begin("R8 round-trips through the level");
    for (uint8_t l = 0; l < 3; ++l) CHECK_EQ(int(level_from_r8(r8_from_level(l))), int(l));

    // ---- the eye box ----------------------------------------------------
    rct::begin("20x15 deg eye box: e' is zero inside, positive outside");
    {
        // Sample along the horizontal and the vertical from the centre.
        const int cy = fx.tiles_y / 2, cx = fx.tiles_x / 2;
        bool saw_zero_h = false, saw_pos_h = false;
        for (int x = cx; x < fx.tiles_x; ++x) {
            const int i = cy * fx.tiles_x + x;
            if (fx.ecc_deg[i] == 0.0f) saw_zero_h = true;
            else saw_pos_h = true;
        }
        CHECK(saw_zero_h); CHECK(saw_pos_h);
        // The box is wider than it is tall, so the vertical leaves it first.
        int last_zero_x = cx, last_zero_y = cy;
        for (int x = cx; x < fx.tiles_x; ++x)
            if (fx.ecc_deg[cy * fx.tiles_x + x] == 0.0f) last_zero_x = x;
        for (int y = cy; y < fx.tiles_y; ++y)
            if (fx.ecc_deg[y * fx.tiles_x + cx] == 0.0f) last_zero_y = y;
        CHECK_MSG((last_zero_x - cx) > (last_zero_y - cy),
                  "eye box should be wider than tall");
    }

    rct::begin("a smaller eye box shrinks the s = 1 region");
    {
        FoveationConfig small = cfg;
        small.region_scale = 0.5f;
        const FoveationMap fs = foveation_map(lens, small, nullptr);
        long full_big = 0, full_small = 0;
        for (size_t i = 0; i < fx.size(); ++i) {
            if (fx.level[i] == 0) ++full_big;
            if (fs.level[i] == 0) ++full_small;
        }
        CHECK_LT(full_small, full_big);
        // Every tile is at least as degraded as before: the knob never
        // sharpens anything.
        for (size_t i = 0; i < fx.size(); ++i) CHECK(fs.level[i] >= fx.level[i]);
    }

    // ---- eye tracked ----------------------------------------------------
    rct::begin("gaze moves the s = 1 region");
    {
        Gaze g; g.valid = true; g.latency_ms = 40.0f;
        g.tan_x = 0.45f; g.tan_y = 0.0f;          // ~24 deg to the right
        const FoveationMap fg = foveation_map(lens, cfg, &g);

        // The tile nearest the gaze direction must be s = 1 ...
        size_t best = 0;
        for (size_t i = 1; i < fg.size(); ++i)
            if (fg.ecc_raw[i] < fg.ecc_raw[best]) best = i;
        CHECK_EQ(int(fg.level[best]), 0);
        CHECK_LT(fg.ecc_raw[best], 2.0f);
        // ... and it must be to the right of the image centre.
        CHECK_LT(int(fg.tiles_x / 2), int(best % size_t(fg.tiles_x)));

        // The far left edge, now ~55 deg from gaze, must be at 1/4.
        CHECK_EQ(int(fg.level[size_t(fg.tiles_y / 2) * fg.tiles_x]), 2);
    }

    rct::begin("the pad is 0.05 deg/ms + 1 deg and widens the fovea");
    {
        Gaze slow; slow.valid = true; slow.latency_ms = 0.0f;
        Gaze fast; fast.valid = true; fast.latency_ms = 80.0f;
        const FoveationMap fa = foveation_map(lens, cfg, &slow);
        const FoveationMap fb = foveation_map(lens, cfg, &fast);
        long n_a = 0, n_b = 0;
        for (size_t i = 0; i < fa.size(); ++i) {
            n_a += (fa.level[i] == 0);
            n_b += (fb.level[i] == 0);
            // More latency never sharpens less.
            CHECK(fb.level[i] <= fa.level[i]);
        }
        CHECK_LT(n_a, n_b);
        // 80 ms -> 0.05*80 + 1 = 5 deg of pad.
        const size_t c = size_t(centre_index(fa));
        CHECK_NEAR(fa.ecc_deg[c] - fb.ecc_deg[c], std::min(fa.ecc_deg[c], 5.0f), 0.2);
    }

    rct::begin("eye-tracked foveation costs fewer samples than fixed");
    {
        Gaze g; g.valid = true; g.latency_ms = 40.0f;
        const FoveationMap fg = foveation_map(lens, cfg, &g);
        auto frac = [](const FoveationMap& m) {
            double s = 0;
            for (uint8_t l : m.level) s += double(level_scale(l)) * double(level_scale(l));
            return s / double(m.size());
        };
        CHECK_LT(frac(fg), frac(fx));
    }

    // ---- the rate-control weight ----------------------------------------
    rct::begin("eccentricity weight is 1 at the centre and floors in the periphery");
    {
        CHECK_NEAR(fx.weight[centre_index(fx)], 1.0, 1e-4);
        for (float w : fx.weight) { CHECK(w <= 1.0f + 1e-4f); CHECK(w >= 0.15f - 1e-4f); }
        CHECK_NEAR(fx.weight[fx.tiles_y * fx.tiles_x - 1], 0.15, 0.02);
    }

    rct::begin("a wider render FOV lowers ppd_center");
    CHECK_LT(ppd_center(pico4_eye_wide()), ppd_center(lens));

    return rct::finish("rc.foveation");
}
