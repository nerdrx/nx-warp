// Decode-time governor: five ordered knobs, 3-frame step down, 2 s step up.
// SPDX-License-Identifier: Apache-2.0
#include "rc_test_util.h"

#include "nxrc/nxrc.hpp"

using namespace nxrc;

namespace {
constexpr float kPeriod = 1e6f / 90.0f;          // 11111 us at 90 Hz
constexpr float kTarget = 0.40f * kPeriod;       // 4444 us

void feed(Governor& g, float ratio, int frames, bool miss = false) {
    for (int i = 0; i < frames; ++i) g.update(ratio * kTarget, kPeriod, miss);
}
} // namespace

int main() {
    const GovernorConfig cfg;

    rct::begin("target is 40% of the frame period");
    CHECK_NEAR(cfg.target_fraction * kPeriod, 4444.4, 1.0);

    rct::begin("a quiet decoder leaves the knobs alone");
    {
        Governor g;
        feed(g, 0.5f, 100);
        CHECK_EQ(g.state().knob_level, 0);
        CHECK_EQ(int(g.state().refresh_hz), 90);
    }

    rct::begin("three frames over 110% step down once, not twice");
    {
        Governor g;
        feed(g, 1.2f, 2);
        CHECK_EQ(g.state().knob_level, 0);      // two is not enough
        feed(g, 1.2f, 1);
        CHECK_EQ(g.state().knob_level, 1);      // the third does it
        feed(g, 1.2f, 2);
        CHECK_EQ(g.state().knob_level, 1);      // the counter was reset
        feed(g, 1.2f, 1);
        CHECK_EQ(g.state().knob_level, 2);
    }

    rct::begin("the over-run counter resets in the dead band");
    {
        Governor g;
        feed(g, 1.2f, 2);
        feed(g, 1.0f, 1);                       // dead band: 70% .. 110%
        CHECK_EQ(g.over_run(), 0);
        feed(g, 1.2f, 2);
        CHECK_EQ(g.state().knob_level, 0);
    }

    rct::begin("one frame at 150% steps down immediately");
    {
        Governor g;
        feed(g, 1.6f, 1);
        CHECK_EQ(g.state().knob_level, 1);
        feed(g, 1.6f, 1);
        CHECK_EQ(g.state().knob_level, 2);
    }

    rct::begin("step up needs 180 consecutive frames under 70%");
    {
        Governor g;
        feed(g, 1.6f, 3);
        CHECK_EQ(g.state().knob_level, 3);
        feed(g, 0.5f, 179);
        CHECK_EQ(g.state().knob_level, 3);
        feed(g, 0.5f, 1);
        CHECK_EQ(g.state().knob_level, 2);
        feed(g, 0.5f, 180);
        CHECK_EQ(g.state().knob_level, 1);
    }

    rct::begin("a deadline miss cancels progress towards a step up");
    {
        Governor g;
        feed(g, 1.6f, 1);
        CHECK_EQ(g.state().knob_level, 1);
        feed(g, 0.5f, 179);
        g.update(0.5f * kTarget, kPeriod, /*deadline_miss=*/true);
        CHECK_EQ(g.under_run(), 0);
        feed(g, 0.5f, 179);
        CHECK_EQ(g.state().knob_level, 1);      // still not there
        feed(g, 0.5f, 1);
        CHECK_EQ(g.state().knob_level, 0);
    }

    rct::begin("the asymmetry is real: down in 3 frames, up in 180");
    {
        Governor g;
        int down_at = -1, up_at = -1;
        for (int i = 0; i < 10 && down_at < 0; ++i) {
            g.update(1.2f * kTarget, kPeriod);
            if (g.state().knob_level == 1) down_at = i + 1;
        }
        for (int i = 0; i < 400 && up_at < 0; ++i) {
            g.update(0.5f * kTarget, kPeriod);
            if (g.state().knob_level == 0) up_at = i + 1;
        }
        CHECK_EQ(down_at, 3);
        CHECK_EQ(up_at, 180);
        CHECK(up_at > 50 * down_at);
    }

    rct::begin("the knobs engage in the order of PAPER.md 4.7");
    {
        Governor g;
        const KnobState& s = g.state();
        CHECK(!s.drop_enh_class_c);

        g.update(1.6f * kTarget, kPeriod);          // knob 1
        CHECK_EQ(s.knob_level, 1);
        CHECK(s.drop_enh_class_c);
        CHECK(!s.class_c_base_only);
        CHECK_NEAR(s.fovea_region_scale, 1.0, 1e-6);

        g.update(1.6f * kTarget, kPeriod);          // knob 2
        CHECK(s.class_c_base_only);
        CHECK(s.class_c_dc_plane);
        CHECK(!s.drop_enh_class_b);

        g.update(1.6f * kTarget, kPeriod);          // knob 3
        CHECK_NEAR(s.fovea_region_scale, 0.90, 1e-6);
        CHECK(!s.drop_enh_class_b);

        g.update(1.6f * kTarget, kPeriod);          // knob 4
        CHECK(s.drop_enh_class_b);
        CHECK_EQ(int(s.res_floor_class_b), 2);
        CHECK_EQ(int(s.refresh_hz), 90);

        g.update(1.6f * kTarget, kPeriod);          // knob 5
        CHECK_EQ(s.knob_level, 5);
        CHECK_EQ(int(s.refresh_hz), 72);

        g.update(1.6f * kTarget, kPeriod);          // saturated
        CHECK_EQ(s.knob_level, 5);
    }

    rct::begin("knobs unwind in reverse order");
    {
        Governor g;
        for (int i = 0; i < 5; ++i) g.update(1.6f * kTarget, kPeriod);
        CHECK_EQ(g.state().knob_level, 5);
        feed(g, 0.5f, 180);
        CHECK_EQ(g.state().knob_level, 4);
        CHECK_EQ(int(g.state().refresh_hz), 90);
        CHECK(g.state().drop_enh_class_b);
        feed(g, 0.5f, 180);
        CHECK_EQ(g.state().knob_level, 3);
        CHECK(!g.state().drop_enh_class_b);
        CHECK_NEAR(g.state().fovea_region_scale, 0.90, 1e-6);
    }

    rct::begin("a step up never happens at the bottom");
    {
        Governor g;
        feed(g, 0.1f, 1000);
        CHECK_EQ(g.state().knob_level, 0);
    }

    rct::begin("the governor works at 72 Hz too");
    {
        Governor g;
        const float p72 = 1e6f / 72.0f;
        for (int i = 0; i < 3; ++i) g.update(0.41f * p72, p72);   // 102% of target
        CHECK_EQ(g.state().knob_level, 0);                       // inside the band
        for (int i = 0; i < 3; ++i) g.update(0.45f * p72, p72);   // 112%
        CHECK_EQ(g.state().knob_level, 1);
    }

    rct::begin("reset clears everything");
    {
        Governor g;
        for (int i = 0; i < 3; ++i) g.update(1.6f * kTarget, kPeriod);
        CHECK_EQ(g.state().knob_level, 3);
        g.reset();
        CHECK_EQ(g.state().knob_level, 0);
        CHECK_EQ(g.over_run(), 0);
        CHECK_EQ(g.under_run(), 0);
        CHECK_EQ(int(g.state().refresh_hz), 90);
    }

    return rct::finish("rc.governor");
}
