// Bit-model convergence: a_t <- a_t * (actual/predicted)^0.6.
// SPDX-License-Identifier: Apache-2.0
#include "rc_test_util.h"

#include "nxrc/nxrc.hpp"
#include "nxfov/foveation.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace nxrc;

namespace {

struct Rig {
    nxfov::FoveationMap  fov;
    TileStats            stats;
    std::vector<uint8_t> cls;
    std::vector<float>   cplx, slip, a_true, actual;
    RateController       rc;

    explicit Rig(float spread = 3.0f) {
        fov = nxfov::foveation_map(nxfov::pico4_eye(), {}, nullptr);
        const size_t n = fov.size();
        stats.resize(n);
        cls.assign(n, uint8_t(TileClass::Texture));
        cplx.assign(n, 6.0f);
        slip.assign(n, 0.0f);
        a_true.assign(n, 0.0f);
        actual.assign(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            stats.mean_luma[i] = 120.0f;
            stats.log_var[i]   = 9.0f;
            // Ground truth spans `spread` either side of the model's a_init.
            const float t = float(i % 97) / 96.0f;         // 0 .. 1
            a_true[i] = 32768.0f * std::pow(spread, 2.0f * t - 1.0f);
        }
        rc.reset(n);
    }

    FrameInputs inputs() {
        FrameInputs in;
        in.fov = &fov; in.cls = cls; in.complexity = cplx;
        in.slip_deg_s = slip; in.stats = &stats;
        in.head_speed_deg_s = 0.0f; in.intra_ratio = 0.05f;
        return in;
    }

    // One frame; returns the mean relative per-tile prediction error.
    float frame(float budget) {
        const AllocResult& a = rc.allocate(budget, inputs());
        double err = 0; long k = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a.skip[i]) { actual[i] = 1.0f; continue; }
            const float s_fov  = nxfov::level_scale(fov.level[i]);
            const float s_code = nxfov::level_scale(a.res_level[i]);
            float g = (s_code * s_code) / (s_fov * s_fov);
            if (a.wm_id[i] == WM_PERIPH) g *= rc.config().gain_wm_periph;
            if (a.dc_plane[i])           g *= rc.config().gain_dc_plane;
            actual[i] = a_true[i] * s_fov * s_fov * g * std::exp2(-float(a.qp[i]) / 6.0f);
            if (a.predicted_bits[i] > 1.0f) {
                err += std::fabs(actual[i] - a.predicted_bits[i]) / a.predicted_bits[i];
                ++k;
            }
        }
        rc.update_model(std::span<const float>(actual));
        return k ? float(err / double(k)) : 0.0f;
    }
};

} // namespace

int main() {
    const float B = 150e6f / 72.0f;

    rct::begin("the model converges from a 3x mismatch in a few frames");
    {
        Rig r(3.0f);
        std::vector<float> e;
        for (int f = 0; f < 40; ++f) e.push_back(r.frame(B));
        CHECK_MSG(e[0] > 0.25f, "frame 0 should start badly, got " + std::to_string(double(e[0])));
        // PAPER.md 4.6: "it converges in 2 to 3 frames".
        CHECK_MSG(e[3] < 0.5f * e[0], "no convergence by frame 3: " +
                                      std::to_string(double(e[3])) + " vs " +
                                      std::to_string(double(e[0])));
        CHECK_MSG(e[10] < 0.05f, "frame 10 error " + std::to_string(double(e[10])));
        CHECK_MSG(e[39] < 0.02f, "frame 39 error " + std::to_string(double(e[39])));
        // Monotone-ish: the error never grows back.
        CHECK_LE(e[20], e[5]);
    }

    rct::begin("the model recovers the true per-tile cost, not just the total");
    {
        Rig r(3.0f);
        for (int f = 0; f < 60; ++f) r.frame(B);
        auto model = r.rc.model();
        double worst = 0;
        for (size_t i = 0; i < model.size(); ++i) {
            if (r.rc.last().skip[i]) continue;
            worst = std::max(worst, double(std::fabs(std::log2(model[i] / r.a_true[i]))));
        }
        CHECK_MSG(worst < 0.2, "worst per-tile model error " + std::to_string(worst) +
                               " octaves");
    }

    rct::begin("the per-frame update ratio is clamped to 4x");
    {
        Rig r(1.0f);
        for (int f = 0; f < 10; ++f) r.frame(B);
        const float a0 = r.rc.model()[0];
        // A single absurd measurement must move the model by at most 4^0.6.
        std::vector<float> huge(r.fov.size());
        for (size_t i = 0; i < huge.size(); ++i)
            huge[i] = r.rc.last().skip[i] ? 1.0f : r.rc.last().predicted_bits[i] * 1000.0f;
        r.rc.update_model(std::span<const float>(huge));
        const float a1 = r.rc.model()[0];
        CHECK_LE(a1 / a0, std::pow(4.0f, 0.6f) + 1e-3f);
        CHECK(a1 > a0);
    }

    rct::begin("the model tolerates a scene cut and re-converges");
    {
        Rig r(2.0f);
        for (int f = 0; f < 30; ++f) r.frame(B);
        const float settled = r.frame(B);
        CHECK_LT(settled, 0.03f);
        // The content changes completely.
        for (size_t i = 0; i < r.a_true.size(); ++i) r.a_true[i] *= 4.0f;
        const float after = r.frame(B);
        CHECK(after > settled);
        for (int f = 0; f < 12; ++f) r.frame(B);
        CHECK_MSG(r.frame(B) < 0.05f, "no re-convergence after a content change");
    }

    rct::begin("an overrun becomes debt on the next frame");
    {
        Rig r(1.0f);
        for (int f = 0; f < 8; ++f) r.frame(B);
        const float before = r.rc.debt_bits();
        std::vector<float> big(r.fov.size());
        for (size_t i = 0; i < big.size(); ++i)
            big[i] = r.rc.last().skip[i] ? 1.0f : r.rc.last().predicted_bits[i] * 2.0f;
        r.rc.update_model(std::span<const float>(big));
        CHECK(r.rc.debt_bits() > before);
        CHECK(r.rc.debt_frames() > 0);
    }

    rct::begin("the uint32 overload matches the float one");
    {
        Rig a(2.0f), b(2.0f);
        for (int f = 0; f < 6; ++f) { a.frame(B); b.frame(B); }
        std::vector<uint32_t> u(a.actual.size());
        for (size_t i = 0; i < u.size(); ++i) u[i] = uint32_t(a.actual[i]);
        std::vector<float> fl(u.begin(), u.end());
        a.rc.update_model(std::span<const uint32_t>(u));
        b.rc.update_model(std::span<const float>(fl));
        for (size_t i = 0; i < a.rc.model().size(); ++i)
            CHECK_NEAR(a.rc.model()[i], b.rc.model()[i], 1e-3);
    }

    return rct::finish("rc.model");
}
