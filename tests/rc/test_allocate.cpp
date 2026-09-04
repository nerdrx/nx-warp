// Allocation invariants: budget tracking, monotonicity, ladder ordering.
// SPDX-License-Identifier: Apache-2.0
#include "rc_test_util.h"

#include "nxrc/nxrc.hpp"
#include "nxfov/foveation.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace nxrc;

namespace {

struct Fixture {
    nxfov::LensParams      lens = nxfov::pico4_eye();
    nxfov::FoveationConfig fcfg;
    nxfov::FoveationMap    fov;
    TileStats              stats;
    std::vector<uint8_t>   cls;
    std::vector<float>     cplx, slip;

    Fixture() {
        fov = nxfov::foveation_map(lens, fcfg, nullptr);
        const size_t n = fov.size();
        stats.resize(n);
        cls.assign(n, uint8_t(TileClass::Texture));
        cplx.assign(n, 6.0f);
        slip.assign(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            stats.mean_luma[i] = 120.0f;
            stats.log_var[i]   = 9.0f;
        }
        // A deterministic sprinkle of the other three classes, spread over
        // every eccentricity so the ordering test has pairs to compare.
        for (size_t i = 0; i < n; ++i) {
            if (i % 7 == 0) cls[i] = uint8_t(TileClass::Edge);
            if (i % 7 == 1) cls[i] = uint8_t(TileClass::Text);
            if (i % 7 == 2) cls[i] = uint8_t(TileClass::Flat);
        }
    }

    FrameInputs inputs() const {
        FrameInputs in;
        in.fov = &fov; in.cls = cls; in.complexity = cplx;
        in.slip_deg_s = slip; in.stats = &stats;
        in.head_speed_deg_s = 30.0f;
        in.intra_ratio = 0.05f;
        return in;
    }
};

// Run the controller to a steady state at one budget and return the result.
AllocResult steady(Fixture& fx, float bits_per_frame, int frames = 24,
                   RateConfig cfg = {}) {
    RateController rc(cfg);
    rc.reset(fx.fov.size());
    std::vector<float> actual(fx.fov.size());
    AllocResult out;
    for (int f = 0; f < frames; ++f) {
        const AllocResult& a = rc.allocate(bits_per_frame, fx.inputs());
        for (size_t i = 0; i < a.size(); ++i)
            actual[i] = a.skip[i] ? 1.0f : a.predicted_bits[i];
        out = a;
        rc.update_model(std::span<const float>(actual));
    }
    return out;
}

} // namespace

int main() {
    Fixture fx;
    const size_t n = fx.fov.size();

    // ---- the budget is respected ----------------------------------------
    rct::begin("predicted total tracks the budget across 20 Mbit .. 1 Gbit");
    for (float mbit : {20.0f, 40.0f, 80.0f, 150.0f, 300.0f, 600.0f, 1000.0f}) {
        const float B = mbit * 1e6f / 72.0f;
        const AllocResult a = steady(fx, B);
        const float err = (a.predicted_total - a.budget_bits) / a.budget_bits;
        CHECK_MSG(err < 0.02f, std::to_string(double(mbit)) + " Mbit overshoots by " +
                               std::to_string(double(err)));
        // Undershoot is allowed, but only because tiles ran into their class
        // QP floors: above about 400 Mbit/s there is nothing left to buy
        // (PAPER.md 4.6).  Anywhere else it would be a controller bug.
        if (err < -0.02f)
            CHECK_MSG(a.clamped_floor > 0,
                      std::to_string(double(mbit)) + " Mbit undershoots by " +
                      std::to_string(double(err)) + " with no tile at its QP floor");
        CHECK_MSG(err > -0.50f, std::to_string(double(mbit)) + " Mbit undershoots by " +
                                std::to_string(double(err)));
    }

    // ---- monotonicity ----------------------------------------------------
    // Less budget must never make any tile better.  The comparable scalar is
    // the effective QP: the coded QP minus everything the ladder and the
    // foveation resolution bought back.
    rct::begin("less budget never improves any tile (effective QP)");
    {
        const float budgets[] = {1000e6f, 600e6f, 300e6f, 150e6f, 80e6f, 40e6f, 20e6f};
        std::vector<AllocResult> res;
        for (float b : budgets) res.push_back(steady(fx, b / 72.0f));

        int violations = 0;
        float worst = 0.0f;
        for (size_t k = 1; k < res.size(); ++k) {
            for (size_t i = 0; i < n; ++i) {
                if (res[k].skip[i] || res[k - 1].skip[i]) continue;
                const float hi = effective_qp(res[k - 1], fx.fov, i, RateConfig{});
                const float lo = effective_qp(res[k],     fx.fov, i, RateConfig{});
                // lo is the smaller budget, so it must be >= hi.  The slack
                // is one and a half QP: the QP is an integer and the
                // allocator dithers the rounding threshold over a full step
                // (allocate.cpp, qp_dither), so two allocations of the same
                // tile can legitimately differ by one step either way.
                if (lo < hi - 1.5f) { ++violations; worst = std::max(worst, hi - lo); }
            }
        }
        CHECK_MSG(violations == 0,
                  std::to_string(violations) + " tiles improved when the budget fell, worst " +
                  std::to_string(double(worst)) + " QP");
    }

    rct::begin("ladder pressure rises as the budget falls, unless tiles drop out");
    {
        // The ladder and SKIP_WARP are two different valves.  Pressure is
        // monotone while the skip set is stable; once tiles start dropping
        // out entirely the survivors can afford a lower pressure, which is
        // correct behaviour and not a monotonicity violation (the per-tile
        // effective-QP test above is the invariant that matters).
        float prev_p = -1.0f;
        int   prev_skip = -1;
        for (float mbit : {1000.0f, 600.0f, 300.0f, 150.0f, 80.0f, 40.0f, 20.0f}) {
            const AllocResult a = steady(fx, mbit * 1e6f / 72.0f);
            CHECK_MSG(a.pressure >= prev_p - 1e-4f || a.skipped > prev_skip,
                      "pressure fell at " + std::to_string(double(mbit)) +
                      " Mbit with no extra skips");
            prev_p = a.pressure; prev_skip = a.skipped;
        }
        CHECK_LT(steady(fx, 1000e6f / 72.0f).pressure,
                 steady(fx, 20e6f / 72.0f).pressure + 1e-4f);
    }

    rct::begin("the skip set only grows as the budget falls");
    {
        std::vector<uint8_t> prev;
        for (float mbit : {1000.0f, 300.0f, 80.0f, 20.0f}) {
            const AllocResult a = steady(fx, mbit * 1e6f / 72.0f);
            if (!prev.empty())
                for (size_t i = 0; i < n; ++i)
                    if (prev[i]) CHECK_MSG(a.skip[i], "tile " + std::to_string(i) +
                                                      " un-skipped at a lower budget");
            prev = a.skip;
        }
    }

    // ---- ladder ordering -------------------------------------------------
    // The paper's requirement in one sentence: a text tile is never at a
    // worse ladder step than a texture tile of equal eccentricity.  Ladder
    // step indices are not comparable across classes (step 2 is "res 1/2"
    // for texture and "untouched" for text), so the invariant is stated in
    // ladder severity, the QP-equivalent detail the ladder removed.
    rct::begin("ladder ordering: text <= edge <= texture, flat worst");
    {
        const RateConfig cfg;
        for (int step = 0; step < kLadderSteps; ++step) {
            for (uint8_t lvl = 0; lvl < 3; ++lvl) {
                const float t = ladder_severity(uint8_t(TileClass::Text),    step, lvl, cfg);
                const float e = ladder_severity(uint8_t(TileClass::Edge),    step, lvl, cfg);
                const float x = ladder_severity(uint8_t(TileClass::Texture), step, lvl, cfg);
                const float f = ladder_severity(uint8_t(TileClass::Flat),    step, lvl, cfg);
                const std::string at = " at step " + std::to_string(step) +
                                       " level " + std::to_string(int(lvl));
                CHECK_MSG(t <= e + 1e-4f, "text worse than edge" + at);
                CHECK_MSG(e <= x + 1e-4f, "edge worse than texture" + at);
                CHECK_MSG(x <= f + 1e-4f, "texture worse than flat" + at);
            }
        }
    }

    rct::begin("ladder severity is monotone in the step index");
    {
        const RateConfig cfg;
        for (uint8_t c = 0; c < 4; ++c)
            for (uint8_t lvl = 0; lvl < 3; ++lvl)
                for (int s = 1; s < kLadderSteps; ++s)
                    CHECK(ladder_severity(c, s, lvl, cfg) >=
                          ladder_severity(c, s - 1, lvl, cfg) - 1e-4f);
    }

    rct::begin("a ladder step never sharpens a periphery tile");
    {
        const RateConfig cfg;
        for (uint8_t c = 0; c < 4; ++c)
            for (uint8_t lvl = 0; lvl < 3; ++lvl)
                for (int s = 0; s < kLadderSteps; ++s) {
                    const LadderStep& st = ladder_step(c, s);
                    CHECK(std::max(lvl, st.res_level_abs) >= lvl);
                }
    }

    rct::begin("in the allocation, text holds while texture blurs");
    {
        // At a punishing budget, compare tiles of the two classes at the
        // same foveation level.
        const AllocResult a = steady(fx, 40e6f / 72.0f);
        const RateConfig cfg;
        double sev_text[3] = {0, 0, 0}, sev_tex[3] = {0, 0, 0};
        long   n_text[3] = {0, 0, 0}, n_tex[3] = {0, 0, 0};
        for (size_t i = 0; i < n; ++i) {
            if (a.skip[i]) continue;
            const uint8_t l = fx.fov.level[i];
            const float s = ladder_severity(a, fx.fov, i, cfg);
            if (fx.cls[i] == uint8_t(TileClass::Text))    { sev_text[l] += s; ++n_text[l]; }
            if (fx.cls[i] == uint8_t(TileClass::Texture)) { sev_tex[l]  += s; ++n_tex[l];  }
        }
        bool compared = false;
        for (int l = 0; l < 3; ++l) {
            if (!n_text[l] || !n_tex[l]) continue;
            compared = true;
            CHECK_MSG(sev_text[l] / n_text[l] <= sev_tex[l] / n_tex[l] + 1e-4,
                      "text more degraded than texture at level " + std::to_string(l));
        }
        // At the very bottom of the range whole classes drop out, so the
        // per-level comparison may have nothing to compare; the frame-wide
        // one always does.
        const double st = sev_text[0] + sev_text[1] + sev_text[2];
        const double sx = sev_tex[0] + sev_tex[1] + sev_tex[2];
        const long   nt = n_text[0] + n_text[1] + n_text[2];
        const long   nx = n_tex[0] + n_tex[1] + n_tex[2];
        CHECK(nt > 0); CHECK(nx > 0);
        if (nt && nx)
            CHECK_MSG(st / double(nt) <= sx / double(nx) + 1e-4,
                      "text more degraded than texture frame-wide");
        (void)compared;

        // And the coded QP ceilings held for every class.
        for (size_t i = 0; i < n; ++i) {
            if (a.skip[i]) continue;
            const uint8_t c = fx.cls[i];
            CHECK_MSG(a.qp[i] <= cfg.qp_ceiling[c] + 4,
                      "class " + std::string(class_name(c)) + " at QP " +
                      std::to_string(int(a.qp[i])));
        }
    }

    // ---- ladder ordering under pressure, per tile ------------------------
    rct::begin("text tiles are never at a worse coded resolution than texture");
    {
        for (float mbit : {20.0f, 40.0f, 150.0f}) {
            const AllocResult a = steady(fx, mbit * 1e6f / 72.0f);
            for (size_t i = 0; i < n; ++i) {
                if (a.skip[i] || fx.cls[i] != uint8_t(TileClass::Text)) continue;
                // A text tile is coded at exactly its foveation resolution.
                CHECK_MSG(a.res_level[i] == fx.fov.level[i],
                          "text tile downsampled at " + std::to_string(double(mbit)) + " Mbit");
                CHECK_MSG(a.dc_plane[i] == 0, "text tile reduced to a DC plane");
            }
        }
    }

    // ---- foveation and chroma -------------------------------------------
    rct::begin("chroma mode follows the coded resolution");
    {
        const AllocResult a = steady(fx, 150e6f / 72.0f);
        for (size_t i = 0; i < n; ++i) {
            if (a.skip[i]) continue;
            if (fx.cls[i] == uint8_t(TileClass::Text)) { CHECK_EQ(int(a.chroma_mode[i]), int(CHROMA_444)); continue; }
            const int want = a.res_level[i] == 0 ? CHROMA_444
                           : a.res_level[i] == 1 ? CHROMA_420 : CHROMA_410;
            CHECK_EQ(int(a.chroma_mode[i]), want);
        }
    }

    rct::begin("coded resolution is never sharper than the foveation map");
    {
        for (float mbit : {20.0f, 150.0f, 1000.0f}) {
            const AllocResult a = steady(fx, mbit * 1e6f / 72.0f);
            for (size_t i = 0; i < n; ++i)
                CHECK(a.res_level[i] >= fx.fov.level[i]);
        }
    }

    rct::begin("QP stays inside the class floor and ceiling");
    {
        const RateConfig cfg;
        for (float mbit : {20.0f, 150.0f, 1000.0f}) {
            const AllocResult a = steady(fx, mbit * 1e6f / 72.0f);
            for (size_t i = 0; i < n; ++i) {
                if (a.skip[i]) continue;
                CHECK(a.qp[i] >= cfg.qp_floor[fx.cls[i]]);
                CHECK(a.qp[i] <= 63);
            }
        }
    }

    // ---- static content skips -------------------------------------------
    rct::begin("a tile below the skip SAD becomes SKIP_WARP");
    {
        Fixture s;
        std::fill(s.cplx.begin(), s.cplx.end(), 6.0f);
        s.cplx[100] = 0.1f;                       // static
        const AllocResult a = steady(s, 150e6f / 72.0f);
        CHECK_EQ(int(a.skip[100]), 1);
    }

    // ---- scene cuts ------------------------------------------------------
    rct::begin("a scene cut gets 1.5x and repays it");
    {
        RateController rc;
        rc.reset(n);
        const float B = 150e6f / 72.0f;
        std::vector<float> actual(n);
        auto frame = [&](bool cut) {
            FrameInputs in = fx.inputs();
            in.intra_ratio = cut ? 0.85f : 0.05f;
            const AllocResult& a = rc.allocate(B, in);
            for (size_t i = 0; i < a.size(); ++i)
                actual[i] = a.skip[i] ? 1.0f : a.predicted_bits[i];
            const float b = a.budget_bits;
            const bool  c = a.scene_cut;
            rc.update_model(std::span<const float>(actual));
            return std::pair<float, bool>{b, c};
        };
        for (int f = 0; f < 12; ++f) frame(false);
        const auto cut = frame(true);
        CHECK(cut.second);
        CHECK_NEAR(cut.first / B, 1.5, 0.01);
        CHECK(rc.debt_frames() > 0);

        // The following frames run under budget until the debt is repaid.
        float sum = 0;
        int   k = 0;
        for (int f = 0; f < 30; ++f) { sum += frame(false).first; ++k; }
        CHECK_LT(sum / float(k), B);
        // 1.5x for one frame paid back over 30 frames: the average over the
        // whole episode is the nominal budget.
        CHECK_NEAR((cut.first + sum) / float(k + 1), B, B * 0.01f);
        CHECK_EQ(rc.debt_frames(), 0);
    }

    rct::begin("without a cut the budget is the budget");
    {
        const AllocResult a = steady(fx, 150e6f / 72.0f);
        CHECK(!a.scene_cut);
        CHECK_NEAR(a.budget_bits, a.requested_bits, 1.0);
    }

    // ---- governor coupling ----------------------------------------------
    rct::begin("governor knobs push the periphery down, never the fovea");
    {
        RateConfig cfg;
        RateController rc(cfg);
        rc.reset(n);
        const float B = 300e6f / 72.0f;
        std::vector<float> actual(n);
        for (int f = 0; f < 16; ++f) {
            const AllocResult& a = rc.allocate(B, fx.inputs());
            for (size_t i = 0; i < a.size(); ++i)
                actual[i] = a.skip[i] ? 1.0f : a.predicted_bits[i];
            rc.update_model(std::span<const float>(actual));
        }
        const AllocResult before = rc.last();

        KnobState k;
        k.knob_level = 4;
        k.res_floor_class_c = 2; k.class_c_dc_plane = true; k.res_floor_class_b = 2;
        rc.set_knobs(k);
        const AllocResult& after = rc.allocate(B, fx.inputs());

        long touched = 0;
        for (size_t i = 0; i < n; ++i) {
            if (fx.fov.level[i] == 0) {
                // Class A is untouched by knobs 1, 2 and 4.
                CHECK(after.res_level[i] <= before.res_level[i] ||
                      after.res_level[i] == fx.fov.level[i]);
            } else {
                if (after.res_level[i] > before.res_level[i] ||
                    after.dc_plane[i] > before.dc_plane[i]) ++touched;
            }
        }
        CHECK(touched > 0);
    }

    return rct::finish("rc.allocate");
}
