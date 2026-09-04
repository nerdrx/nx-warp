// rc.refresh - invariants of the per-tile refresh scheduler.
//
// The scheduler is allowed to make the periphery stale.  It is not allowed
// to make the fovea stale, to make text stale, to let any tile drift for
// longer than k_max_frames, to override the rolling intra refresh or the
// reference-eligibility rule, or to give two different answers to the same
// question.  Those six are this file.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxrc/refresh.hpp"
#include "nxrc/nxrc.hpp"
#include "nxrc/synth.hpp"
#include "nxfov/foveation.hpp"
#include "rc_test_util.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace nxrc;

namespace {

// A test frame: a foveation map, a class per tile, a residual per tile.
struct Frame {
    nxfov::LensParams   lens;
    nxfov::FoveationMap fov;
    std::vector<uint8_t> cls;
    std::vector<uint8_t> intra_due, ref_bad;
    TileStats stats;
    std::vector<float> cplx;
    float ppd_center = 22.0f;

    void build(bool eye_tracked = true) {
        lens = nxfov::pico4_eye();
        nxfov::FoveationConfig c;
        nxfov::Gaze g; g.valid = eye_tracked; g.latency_ms = 40.0f;
        fov = nxfov::foveation_map(lens, c, eye_tracked ? &g : nullptr);
        ppd_center = nxfov::ppd_center(lens);
        const size_t n = fov.size();
        cls.assign(n, uint8_t(TileClass::Texture));
        intra_due.assign(n, 0u);
        ref_bad.assign(n, 0u);
        cplx.assign(n, 6.0f);
        stats.resize(n);
        for (size_t i = 0; i < n; ++i) {
            stats.mean_luma[i] = 128.0f;
            stats.log_var[i]   = 7.0f;
            // Enough gradient energy to give a texture-like R.
            stats.jxx[i] = 200000.0f; stats.jxy[i] = 0.0f; stats.jyy[i] = 200000.0f;
            stats.ui_stencil[i] = 0u;
        }
        // A block of text tiles and a block of edge tiles, both out in the
        // periphery where the scheduler would most like to skip them.
        for (size_t i = 0; i < n; ++i) {
            if (fov.ecc_deg[i] > 20.0f) {
                if ((i % 7) == 0) cls[i] = uint8_t(TileClass::Text);
                else if ((i % 7) == 1) cls[i] = uint8_t(TileClass::Edge);
                else if ((i % 7) == 2) cls[i] = uint8_t(TileClass::Flat);
            }
        }
    }

    RefreshInputs inputs(int frame, float pressure = 4.0f) const {
        RefreshInputs in;
        in.fov = &fov;
        in.cls = cls;
        in.complexity = cplx;
        in.stats = &stats;
        in.intra_due = intra_due;
        in.ref_ineligible = ref_bad;
        in.frame_index = frame;
        in.spatial_pressure = pressure;
        in.ppd_center = ppd_center;
        return in;
    }
};

RefreshConfig loose() {
    RefreshConfig c;
    c.fps = 72.0f;
    c.gate_hi = 8.0f;          // deliberately wide: push the scheduler hard
    c.gate_slew_up = 1e9f;     // no slew, so a sweep sees the extremes
    return c;
}

// ---- the fovea is never below full rate ----------------------------------
void test_fovea_full_rate() {
    rct::begin("fovea always full rate");
    Frame fr; fr.build();

    // Every budget from "code one tile in twenty" up to "code everything",
    // and the pressure-coupled mode at maximum pressure.
    for (float target : { 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, -1.0f }) {
        RefreshConfig cfg = loose();
        cfg.target_coded_fraction = target;
        RefreshScheduler s(cfg);
        s.reset(fr.fov.size());
        for (int f = 0; f < 40; ++f) {
            const RefreshResult& r = s.schedule(fr.inputs(f));
            for (size_t i = 0; i < r.size(); ++i) {
                if (fr.fov.ecc_deg[i] > cfg.fovea_full_deg) continue;
                if (fr.cplx[i] < cfg.static_mad) continue;   // static, exempt
                CHECK_MSG(r.divisor[i] == 1,
                          "foveal tile at divisor " + std::to_string(r.divisor[i]) +
                          " with target " + std::to_string(target));
                CHECK_MSG(r.force_skip[i] == 0, "foveal tile skipped");
            }
        }
    }
}

// ---- text is never skipped unless static ---------------------------------
void test_text_never_skipped() {
    rct::begin("text never skipped unless static");
    Frame fr; fr.build();

    RefreshConfig cfg = loose();
    cfg.target_coded_fraction = 0.05f;
    RefreshScheduler s(cfg);
    s.reset(fr.fov.size());
    for (int f = 0; f < 40; ++f) {
        const RefreshResult& r = s.schedule(fr.inputs(f));
        for (size_t i = 0; i < r.size(); ++i) {
            if (fr.cls[i] != uint8_t(TileClass::Text)) continue;
            CHECK_MSG(r.divisor[i] == 1, "moving text tile stepped");
            CHECK_MSG(r.force_skip[i] == 0, "moving text tile skipped");
        }
    }

    // The exemption: a text tile whose warped residual is under skip_sad is
    // already WARP_SKIP in the allocator, so the scheduler is free to let it
    // sit - it has no residual to withhold.
    std::fill(fr.cplx.begin(), fr.cplx.end(), 0.2f);
    RefreshScheduler s2(cfg);
    s2.reset(fr.fov.size());
    const RefreshResult& r2 = s2.schedule(fr.inputs(0));
    bool any = false;
    for (size_t i = 0; i < r2.size(); ++i)
        if (fr.cls[i] == uint8_t(TileClass::Text) && r2.divisor[i] > 1) any = true;
    CHECK_MSG(any, "a static text tile must be allowed to rest");
}

// ---- every tile refreshed at least every K frames ------------------------
void test_age_bound() {
    rct::begin("age bound");
    for (uint8_t K : { uint8_t(2), uint8_t(3), uint8_t(4), uint8_t(6), uint8_t(8) }) {
        Frame fr; fr.build();
        RefreshConfig cfg = loose();
        cfg.k_max_frames = K;
        cfg.target_coded_fraction = 0.02f;   // as aggressive as it is allowed
        RefreshScheduler s(cfg);
        s.reset(fr.fov.size());

        std::vector<int> since(fr.fov.size(), 0);
        int worst = 0;
        for (int f = 0; f < 200; ++f) {
            const RefreshResult& r = s.schedule(fr.inputs(f));
            for (size_t i = 0; i < r.size(); ++i) {
                if (r.force_skip[i]) ++since[i]; else since[i] = 0;
                worst = std::max(worst, since[i]);
                CHECK_MSG(r.age[i] < K,
                          "age " + std::to_string(r.age[i]) + " >= K=" +
                          std::to_string(int(K)));
                CHECK_MSG(since[i] < K,
                          "tile " + std::to_string(i) + " skipped " +
                          std::to_string(since[i]) + " frames in a row, K=" +
                          std::to_string(int(K)));
                CHECK_MSG(r.divisor[i] <= K, "divisor above the age bound");
            }
        }
        CHECK_MSG(worst > 0 || K <= 1, "nothing was ever skipped: test is inert");
    }
}

// ---- the rolling intra refresh and reference eligibility win --------------
//
// A tile the encoder must code - because it is this frame's slice of the
// 1/180 rolling intra refresh (PAPER.md 6.6), or because its 3x3
// acknowledged neighbourhood failed and it is going intra anyway
// (TRANSPORT.md 9) - is never turned into a WARP_SKIP by the scheduler.  The
// scheduler only ever adds skips; it never removes a mandatory code.
void test_mandatory_wins() {
    rct::begin("rolling refresh and reference eligibility win");
    Frame fr; fr.build();
    RefreshConfig cfg = loose();
    cfg.target_coded_fraction = 0.02f;
    RefreshScheduler s(cfg);
    s.reset(fr.fov.size());

    long checked = 0;
    for (int f = 0; f < 60; ++f) {
        // A different 1/180 slice each frame, plus a moving band of tiles
        // whose neighbourhood is not acknowledged.
        std::fill(fr.intra_due.begin(), fr.intra_due.end(), 0u);
        std::fill(fr.ref_bad.begin(), fr.ref_bad.end(), 0u);
        const size_t n = fr.fov.size();
        for (size_t j = 0; j < n / 180 + 1; ++j)
            fr.intra_due[(size_t(f) * 13 + j * 180) % n] = 1u;
        for (size_t j = 0; j < 40; ++j)
            fr.ref_bad[(size_t(f) * 37 + j * 7) % n] = 1u;

        const RefreshResult& r = s.schedule(fr.inputs(f));
        for (size_t i = 0; i < n; ++i) {
            if (!fr.intra_due[i] && !fr.ref_bad[i]) continue;
            ++checked;
            CHECK_MSG(r.force_skip[i] == 0, "mandatory tile was skipped");
            CHECK_MSG(r.mandatory[i] == 1, "mandatory tile not marked");
        }
    }
    CHECK(checked > 1000);

    // A scene cut codes every tile.
    Frame fr2; fr2.build();
    RefreshScheduler s2(cfg);
    s2.reset(fr2.fov.size());
    for (int f = 0; f < 10; ++f) s2.schedule(fr2.inputs(f));
    RefreshInputs in = fr2.inputs(11);
    in.scene_cut = true;
    const RefreshResult& rc2 = s2.schedule(in);
    CHECK_EQ(rc2.skipped, 0);
    for (size_t i = 0; i < rc2.size(); ++i)
        CHECK_MSG(rc2.force_skip[i] == 0, "tile skipped on a scene cut");
}

// ---- determinism ---------------------------------------------------------
void test_determinism() {
    rct::begin("determinism");
    Frame fr; fr.build();
    RefreshConfig cfg = loose();
    cfg.target_coded_fraction = 0.35f;

    std::vector<std::vector<uint8_t>> a, b;
    for (int pass = 0; pass < 2; ++pass) {
        RefreshScheduler s(cfg);
        s.reset(fr.fov.size());
        auto& out = pass ? b : a;
        for (int f = 0; f < 50; ++f) {
            const RefreshResult& r = s.schedule(fr.inputs(f));
            out.push_back(r.force_skip);
            out.push_back(r.divisor);
        }
    }
    CHECK_EQ(a.size(), b.size());
    bool same = true;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) same = false;
    CHECK_MSG(same, "two identical runs disagreed");

    // reset() really resets: a third run after a reset matches the first.
    RefreshScheduler s3(cfg);
    s3.reset(fr.fov.size());
    for (int f = 0; f < 7; ++f) s3.schedule(fr.inputs(f));
    s3.reset(fr.fov.size());
    for (int f = 0; f < 50; ++f) {
        const RefreshResult& r = s3.schedule(fr.inputs(f));
        CHECK_MSG(r.force_skip == a[size_t(f) * 2], "state survived reset()");
    }
}

// ---- the gate is a monotone control --------------------------------------
//
// The budget knob is a bisection over the gate, which is only a search if
// the duty cycle is monotone in it.  Also: the gate must never be able to
// buy anything at all inside the fovea, so the duty cycle has a floor.
void test_gate_monotone() {
    rct::begin("gate monotonicity");
    Frame fr; fr.build();
    RefreshConfig cfg = loose();
    RefreshScheduler s(cfg);
    s.reset(fr.fov.size());
    const RefreshInputs in = fr.inputs(0);

    float prev = 1e9f;
    for (float g = 0.01f; g <= 8.0f; g *= 1.25f) {
        double d = 0;
        for (size_t i = 0; i < fr.fov.size(); ++i)
            d += 1.0 / double(s.admissible_divisor(in, i, g));
        const float duty = float(d / double(fr.fov.size()));
        CHECK_MSG(duty <= prev + 1e-6f, "duty cycle rose with the gate");
        prev = duty;
    }
    CHECK_MSG(prev > 0.0f, "duty cycle must have a floor");

    // A tighter target really does code more tiles than a looser one.
    auto duty_for = [&](float target) {
        RefreshConfig c = cfg;
        c.target_coded_fraction = target;
        RefreshScheduler ss(c);
        ss.reset(fr.fov.size());
        float acc = 0;
        for (int f = 0; f < 30; ++f) acc = ss.schedule(fr.inputs(f)).duty_cycle;
        return acc;
    };
    CHECK_LT(duty_for(0.4f), duty_for(0.9f));
}

// ---- the temporal ladder waits for the spatial one -----------------------
//
// Default (pressure-coupled) mode: below RefreshConfig::engage_pressure the
// spatial ladder still has free steps left, and the temporal ladder must not
// spend a frame of staleness it does not have to.  RATECONTROL.md 8.4.
void test_pressure_coupling() {
    rct::begin("pressure coupling");
    Frame fr; fr.build();
    RefreshConfig cfg;                     // both targets off: pressure mode
    cfg.fps = 72.0f;
    cfg.gate_slew_up = 1e9f;

    auto duty_at = [&](float p) {
        RefreshScheduler s(cfg);
        s.reset(fr.fov.size());
        float d = 1.0f;
        for (int f = 0; f < 20; ++f) d = s.schedule(fr.inputs(f, p)).duty_cycle;
        return d;
    };
    const float d0 = duty_at(0.0f), d2 = duty_at(2.0f), d4 = duty_at(4.0f);
    CHECK_NEAR(d0, d2, 1e-6);              // nothing happens below engage
    CHECK_LT(d4, d2);                      // and something does above it
    CHECK_MSG(d0 > 0.97f, "the temporal ladder engaged at zero pressure");
}

// ---- the gate relaxes slowly ---------------------------------------------
void test_gate_slew() {
    rct::begin("gate slew");
    Frame fr; fr.build();
    RefreshConfig cfg;
    cfg.fps = 72.0f;
    cfg.gate_slew_up = 1.35f;
    RefreshScheduler s(cfg);
    s.reset(fr.fov.size());

    // Drive it wide open, then slam the target shut and watch the gate come
    // down in bounded multiplicative steps rather than in one frame.
    for (int f = 0; f < 30; ++f) s.schedule(fr.inputs(f, 4.0f));
    const float hot = s.gate();
    CHECK(hot > cfg.gate_lo * 2.0f);
    float last = hot;
    for (int f = 30; f < 90; ++f) {
        s.schedule(fr.inputs(f, 0.0f));
        CHECK_MSG(s.gate() >= last / (cfg.gate_slew_up * 1.001f),
                  "gate relaxed faster than the slew allows");
        last = s.gate();
    }
    CHECK_NEAR(last, cfg.gate_lo, 1e-4);   // and it does get all the way back
}

// ---- the phase permutation spreads the load ------------------------------
void test_phase_spread() {
    rct::begin("refresh phase spread");
    // For each k, the phases of 4096 consecutive tile ids must cover every
    // residue roughly evenly, or the scheduler produces a refresh wave and a
    // per-frame bit spike.
    for (uint8_t k : { uint8_t(2), uint8_t(3), uint8_t(4), uint8_t(6) }) {
        std::vector<int> hist(k, 0);
        for (size_t i = 0; i < 4096; ++i) ++hist[refresh_phase(i, k)];
        const int want = 4096 / k;
        for (uint8_t j = 0; j < k; ++j)
            CHECK_MSG(std::abs(hist[j] - want) <= want / 4,
                      "phase " + std::to_string(int(j)) + " of k=" +
                      std::to_string(int(k)) + " got " + std::to_string(hist[j]));
    }
    CHECK_EQ(refresh_phase(1234, 1), 0u);
    // Deterministic.
    CHECK_EQ(refresh_phase(777, 3), refresh_phase(777, 3));
}

// ---- the allocator honours a forced skip ---------------------------------
void test_allocator_honours_skip() {
    rct::begin("allocator honours force_warp_skip");
    Frame fr; fr.build();
    const size_t n = fr.fov.size();

    std::vector<float> slip(n, 5.0f);
    std::vector<uint8_t> force(n, 0u);
    for (size_t i = 0; i < n; i += 3) force[i] = 1u;

    RateController rc;
    rc.reset(n);
    FrameInputs in;
    in.fov = &fr.fov;
    in.cls = fr.cls;
    in.complexity = fr.cplx;
    in.slip_deg_s = slip;
    in.stats = &fr.stats;

    const AllocResult& a0 = rc.allocate(150e6f / 72.0f, in);
    const int base_skipped = a0.skipped;

    RateController rc2;
    rc2.reset(n);
    in.force_warp_skip = force;
    const AllocResult& a1 = rc2.allocate(150e6f / 72.0f, in);

    for (size_t i = 0; i < n; ++i)
        if (force[i]) CHECK_MSG(a1.skip[i] == 1, "forced tile was still coded");
    CHECK(a1.skipped > base_skipped);
    CHECK(a1.skipped_temporal > 0);

    // The bits of the skipped tiles go back into the pot: the survivors are
    // coded at a strictly better QP, they are not simply dropped.
    double q0 = 0, q1 = 0; long n0 = 0, n1 = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!a0.skip[i]) { q0 += a0.qp[i]; ++n0; }
        if (!a1.skip[i]) { q1 += a1.qp[i]; ++n1; }
    }
    CHECK_MSG(n1 > 0 && n0 > 0, "no coded tiles");
    CHECK_MSG(q1 / double(n1) <= q0 / double(n0) + 1e-6,
              "freed bits did not reach the survivors");
}

} // namespace

int main() {
    test_fovea_full_rate();
    test_text_never_skipped();
    test_age_bound();
    test_mandatory_wins();
    test_determinism();
    test_gate_monotone();
    test_pressure_coupling();
    test_gate_slew();
    test_phase_spread();
    test_allocator_honours_skip();
    return rct::finish("rc.refresh");
}
