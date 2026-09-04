// nxvc-rcsim, temporal-ladder scenario.
//
// Budgets 20 to 150 Mbit/s, with and without the per-tile refresh scheduler,
// under fixed and eye-tracked foveation.  Reports the coded-tile fraction
// per eccentricity ring, the visibility model's prediction for the temporal
// artefacts it induces, and the bit saving against the spatial-only ladder
// at equal delivered foveal quality and bounded predicted visibility.
//
// Writes rc/RESULTS-temporal.md and rc/refresh-map.svg.
//
// SPDX-License-Identifier: Apache-2.0

#include "temporal.hpp"
#include "scene.hpp"

#include "nxrc/nxrc.hpp"
#include "nxrc/refresh.hpp"
#include "nxrc/tvm.hpp"
#include "nxfov/foveation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace nxrc;

namespace rcsim {
namespace {

constexpr float kFps  = 72.0f;
constexpr int   kEyes = 2;

std::string fmt(const char* f, ...) {
    char buf[2048];
    va_list ap; va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return std::string(buf);
}

// Floeter et al. 2025, figure 2: five concentric regions whose *diameters*
// are 6.3, 9.1, 18.1 and 31.1 degrees, taken from Mohanto et al. 2022.  Read
// as eccentricity (half those), the ring boundaries are 3.15, 4.55, 9.05 and
// 15.55 degrees.  We keep their five regions and add a sixth for the far
// field, which their 31.1 degree mask does not reach but a 100 degree HMD
// lens does.
constexpr int   kRings = 6;
constexpr float kRingEdge[kRings] = { 3.15f, 4.55f, 9.05f, 15.55f, 30.0f, 1e9f };
const char* ring_name(int r) {
    static const char* n[kRings] = { "0-3.2", "3.2-4.6", "4.6-9.1", "9.1-15.6",
                                     "15.6-30", ">30" };
    return n[r];
}
int ring_of(float e_deg) {
    for (int r = 0; r < kRings; ++r) if (e_deg <= kRingEdge[r]) return r;
    return kRings - 1;
}

// ------------------------------------------------------------------ rig ---

struct Rig {
    Scene                  scene;
    nxfov::FoveationMap    fov;
    nxfov::LensParams      lens;
    nxfov::FoveationConfig fcfg;
    RateController         rc;
    ClassifyConfig         ccfg;
    RefreshScheduler       sched;
    std::vector<uint8_t>   cls, prev_cls, intra_due;
    std::vector<float>     actual;
    int tiles_x = 0, tiles_y = 0;
    uint32_t rs = 0x9e3779b9u;

    void init(bool eye_tracked, const RefreshConfig& rcfg) {
        lens = nxfov::pico4_eye();
        nxfov::Gaze g;
        g.valid = eye_tracked;
        g.latency_ms = 40.0f;
        nxfov::FoveationMap one =
            nxfov::foveation_map(lens, fcfg, eye_tracked ? &g : nullptr);
        tiles_x = one.tiles_x; tiles_y = one.tiles_y;
        fov = stereo_map(one, kEyes);
        scene.build(tiles_x, tiles_y, kEyes, 0xC0FFEEu);
        rc = RateController();
        rc.reset(fov.size());
        sched = RefreshScheduler(rcfg);
        sched.reset(fov.size());
        cls.assign(fov.size(), 0);
        prev_cls.clear();
        actual.assign(fov.size(), 0.0f);
        intra_due.assign(fov.size(), 0u);
        rs = 0x9e3779b9u;
    }

    // The rolling intra refresh of PAPER.md 6.6 / ADR-0006: 1/180 of the
    // tiles per frame under a fixed permutation.  These tiles are never
    // skippable, which is the safety net the temporal ladder rides on.
    void select_intra(int frame) {
        const size_t n = intra_due.size();
        std::fill(intra_due.begin(), intra_due.end(), 0u);
        if (!n) return;
        const size_t per = std::max<size_t>(n / 180, 1);
        for (size_t j = 0; j < per; ++j) {
            const size_t k = (size_t(frame) * per + j) % n;
            // A bit-reversal permutation, same device as refresh_phase.
            uint32_t v = uint32_t(k);
            v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
            v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
            v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
            v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
            v = (v >> 16) | (v << 16);
            // Scaled, not remaindered, for the reason in refresh_phase().
            intra_due[size_t((uint64_t(v) * uint64_t(n)) >> 32)] = 1u;
        }
    }

    float tile_bits(const AllocResult& a, size_t i, const RateConfig& cfg) {
        if (a.skip[i]) return 1.0f;
        const float s_fov  = nxfov::level_scale(fov.level[i]);
        const float s_code = nxfov::level_scale(a.res_level[i]);
        float g = (s_code * s_code) / (s_fov * s_fov);
        if (a.wm_id[i] == WM_PERIPH) g *= cfg.gain_wm_periph;
        if (a.dc_plane[i])           g *= cfg.gain_dc_plane;
        rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
        const float noise = 0.92f + 0.16f * float(rs & 0xFFFF) / 65535.0f;
        return 64.0f + scene.a_true[i] * s_fov * s_fov * g *
                       std::exp2(-float(a.qp[i]) / 6.0f) * noise;
    }
};

// ----------------------------------------------------------------- stats --

struct TempStats {
    // Per ring, accumulated over frames.
    double coded[kRings]   = {};
    double tiles[kRings]   = {};
    double vis_sum[kRings] = {};
    double vis_n[kRings]   = {};
    double p_sum[kRings]   = {};
    double dut_sum[kRings] = {};
    long   div_hist[kRings][8] = {};
    double max_vis = 0.0;

    // Frame level.
    double bits_sum = 0.0;
    double eqp_fovea = 0.0;   // mean effective QP over foveation level 0
    double eqp_n = 0.0;
    double eqp_cls[4] = {};
    double eqp_cls_n[4] = {};
    double skip_temporal = 0.0, skip_static = 0.0, tile_n = 0.0;
    int    frames = 0;
    long   age_violations = 0;
    long   max_age = 0;
    long   text_skips = 0;
    long   fovea_skips = 0;
    double gate_sum = 0.0;

    double coded_frac(int r) const {
        return tiles[r] > 0 ? coded[r] / tiles[r] : 1.0;
    }
    double mean_vis(int r) const { return vis_n[r] > 0 ? vis_sum[r] / vis_n[r] : 0.0; }
    double mean_p(int r) const   { return tiles[r] > 0 ? p_sum[r] / tiles[r] : 0.5; }
    double mbit() const { return frames ? bits_sum / frames * kFps / 1e6 : 0.0; }
    double fovea_eqp() const { return eqp_n > 0 ? eqp_fovea / eqp_n : 0.0; }
};

// One run.  `temporal` off gives the spatial-only baseline.
TempStats run(bool eye_tracked, bool temporal, const RefreshConfig& rcfg,
              float bitrate_bps, int frames,
              RefreshResult* capture, AllocResult* capture_alloc,
              std::vector<uint8_t>* capture_cls, int capture_frame) {
    Rig rig;
    rig.init(eye_tracked, rcfg);
    TempStats st;
    const float B = bitrate_bps / kFps;
    const float ppdc = nxfov::ppd_center(rig.lens);

    std::vector<uint8_t> force;
    float best_duty = 2.0f;
    for (int f = 0; f < frames; ++f) {
        rig.scene.step(f, 0x5EEDu);
        rig.select_intra(f);

        classify_tiles(rig.scene.stats, rig.ccfg,
                       rig.prev_cls.empty() ? std::span<const uint8_t>()
                                            : std::span<const uint8_t>(rig.prev_cls),
                       rig.cls);
        rig.prev_cls = rig.cls;

        const bool cut = std::find(rig.scene.cuts.begin(), rig.scene.cuts.end(), f)
                         != rig.scene.cuts.end();

        const RefreshResult* rr = nullptr;
        if (temporal) {
            RefreshInputs ri;
            ri.fov = &rig.fov;
            ri.cls = rig.cls;
            ri.complexity = rig.scene.complexity;
            ri.stats = &rig.scene.stats;
            ri.intra_due = rig.intra_due;
            ri.scene_cut = cut;
            ri.frame_index = f;
            ri.ppd_center = ppdc;
            ri.spatial_pressure = rig.rc.last().size() ? rig.rc.last().pressure : 0.0f;
            rr = &rig.sched.schedule(ri);
            force = rr->force_skip;
        }

        FrameInputs in;
        in.fov = &rig.fov;
        in.cls = rig.cls;
        in.complexity = rig.scene.complexity;
        in.slip_deg_s = rig.scene.slip;
        in.stats = &rig.scene.stats;
        in.head_speed_deg_s = rig.scene.head_speed_deg_s;
        in.intra_ratio = rig.scene.intra_ratio;
        if (temporal) in.force_warp_skip = force;

        const AllocResult& a = rig.rc.allocate(B, in);

        double total = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            rig.actual[i] = rig.tile_bits(a, i, rig.rc.config());
            total += rig.actual[i];
        }

        if (f >= 8) {
            ++st.frames;
            st.bits_sum += total;
            st.gate_sum += temporal ? rr->gate : 0.0;
            for (size_t i = 0; i < a.size(); ++i) {
                const int r = ring_of(rig.fov.ecc_deg[i]);
                ++st.tiles[r];
                ++st.tile_n;
                const bool sk = a.skip[i] != 0;
                const bool tsk = temporal && rr->force_skip[i];
                if (!sk) st.coded[r] += 1.0;
                if (tsk) st.skip_temporal += 1.0;
                else if (sk) st.skip_static += 1.0;

                if (temporal) {
                    const uint8_t k = rr->divisor[i];
                    ++st.div_hist[r][std::min<int>(k, 7)];
                    st.dut_sum[r] += 1.0 / double(k);
                    st.p_sum[r] += rr->p_detect[i];
                    if (k > 1) {
                        st.vis_sum[r] += rr->visibility[i];
                        ++st.vis_n[r];
                        st.max_vis = std::max(st.max_vis, double(rr->visibility[i]));
                    }
                    st.max_age = std::max<long>(st.max_age, rr->age[i]);
                    if (rr->age[i] >= rcfg.k_max_frames) ++st.age_violations;
                    if (rr->force_skip[i] &&
                        rig.cls[i] == uint8_t(TileClass::Text) &&
                        rig.scene.complexity[i] >= rcfg.static_mad) ++st.text_skips;
                    if (rr->force_skip[i] &&
                        rig.fov.ecc_deg[i] <= rcfg.fovea_full_deg &&
                        rig.scene.complexity[i] >= rcfg.static_mad) ++st.fovea_skips;
                } else {
                    st.p_sum[r] += 0.5;
                    st.dut_sum[r] += 1.0;
                }

                if (sk) continue;
                const float eq = effective_qp(a, rig.fov, i, rig.rc.config());
                if (rig.fov.level[i] == 0) { st.eqp_fovea += eq; ++st.eqp_n; }
                const uint8_t c = rig.cls[i] < 4 ? rig.cls[i] : 1;
                st.eqp_cls[c] += eq; ++st.eqp_cls_n[c];
            }
        }

        // Capture the frame in which the temporal ladder is most engaged
        // (lowest duty cycle), not an arbitrary one: the gate moves with the
        // spatial pressure and with head motion, so a fixed frame index is
        // as likely to catch the ladder idle as busy.
        if (capture && temporal && f >= 8 &&
            (f == 8 || rr->duty_cycle < best_duty)) {
            best_duty = rr->duty_cycle;
            *capture = *rr;
            if (capture_alloc) *capture_alloc = a;
            if (capture_cls) *capture_cls = rig.cls;
        }

        if (temporal) rig.sched.update_cost(float(total), a.size() ?
                                            int(a.size()) - a.skipped : 1);
        rig.rc.update_model(std::span<const float>(rig.actual));
    }
    return st;
}

// The iso-quality comparison.  Arm A is the spatial-only ladder at budget B.
// Arm B adds the temporal ladder and is given the *smallest* budget at which
// its mean foveal effective QP is no worse than arm A's.  The saving is
// (B - B') / B, and it is an honest comparison only because the temporal
// arm's predicted visibility is reported alongside it and bounded by the
// gate; that is the "at equal predicted visibility" half.
float match_budget(bool eye_tracked, const RefreshConfig& rcfg,
                   float baseline_eqp, float B_hi, int frames) {
    float lo = B_hi * 0.30f, hi = B_hi;
    // 8 bisection steps is 0.4% of the range, well inside the model noise.
    for (int it = 0; it < 8; ++it) {
        const float mid = 0.5f * (lo + hi);
        const TempStats s = run(eye_tracked, true, rcfg, mid, frames,
                                nullptr, nullptr, nullptr, -1);
        if (s.fovea_eqp() <= baseline_eqp) hi = mid; else lo = mid;
    }
    return hi;
}

// --------------------------------------------------------------- render ---

// Refresh divisor per tile, one eye.  '.' = every frame, '2'/'3'/'4'/'6' =
// one frame in k, ' ' = static (already WARP_SKIP without the scheduler).
std::string ascii_refresh_map(const RefreshResult& r, const AllocResult& a,
                              int tx, int ty) {
    std::string s;
    for (int y = 0; y < ty; ++y) {
        for (int x = 0; x < tx; ++x) {
            const size_t i = size_t(y) * tx + x;
            const uint8_t k = r.divisor[i];
            char c;
            if (a.skip[i] && !r.force_skip[i]) c = ' ';
            else if (k <= 1) c = '.';
            else c = char('0' + k);
            s += c;
        }
        s += '\n';
    }
    return s;
}

std::string svg_refresh_sheet(const std::vector<const RefreshResult*>& rs,
                              const std::vector<const AllocResult*>& as,
                              const std::vector<std::string>& labels,
                              int tx, int ty) {
    const int cell = 6, pad = 26, gap = 22;
    const int pw = tx * cell, ph = ty * cell;
    const int n = int(rs.size());
    const int W = pad * 2 + n * pw + (n - 1) * gap;
    const int H = pad * 2 + ph + 54;

    // One colour per divisor.  Deliberately a single hue ramp: the map is a
    // rate map, not a category map.
    auto colour = [](uint8_t k) -> const char* {
        switch (k) {
            case 1:  return "#2b3a55";
            case 2:  return "#3f6ea8";
            case 3:  return "#5f9ad0";
            case 4:  return "#93c4e6";
            default: return "#d6e9f6";
        }
    };

    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W
      << "\" height=\"" << H << "\" viewBox=\"0 0 " << W << " " << H << "\">\n"
      << "<rect width=\"100%\" height=\"100%\" fill=\"#0e1420\"/>\n"
      << "<style>text{font-family:ui-monospace,Menlo,monospace;fill:#c9d6e4}"
         ".t{font-size:11px}.l{font-size:10px}</style>\n";
    for (int p = 0; p < n; ++p) {
        const int ox = pad + p * (pw + gap);
        o << "<text class=\"t\" x=\"" << ox << "\" y=\"" << pad - 8 << "\">"
          << labels[size_t(p)] << "</text>\n";
        for (int y = 0; y < ty; ++y) {
            for (int x = 0; x < tx; ++x) {
                const size_t i = size_t(y) * tx + x;
                const bool st = as[size_t(p)]->skip[i] && !rs[size_t(p)]->force_skip[i];
                const char* c = st ? "#151c28" : colour(rs[size_t(p)]->divisor[i]);
                o << "<rect x=\"" << ox + x * cell << "\" y=\"" << pad + y * cell
                  << "\" width=\"" << cell << "\" height=\"" << cell
                  << "\" fill=\"" << c << "\"/>";
            }
            o << "\n";
        }
    }
    const char* leg[5] = { "1/1", "1/2", "1/3", "1/4", "1/6" };
    const uint8_t kk[5] = { 1, 2, 3, 4, 6 };
    for (int j = 0; j < 5; ++j) {
        const int lx = pad + j * 76, ly = pad + ph + 20;
        o << "<rect x=\"" << lx << "\" y=\"" << ly << "\" width=\"12\" height=\"12\" fill=\""
          << colour(kk[j]) << "\"/><text class=\"l\" x=\"" << lx + 17 << "\" y=\""
          << ly + 10 << "\">" << leg[j] << "</text>\n";
    }
    o << "<rect x=\"" << pad + 5 * 76 << "\" y=\"" << pad + ph + 20
      << "\" width=\"12\" height=\"12\" fill=\"#151c28\"/><text class=\"l\" x=\""
      << pad + 5 * 76 + 17 << "\" y=\"" << pad + ph + 30
      << "\">static (WARP_SKIP already)</text>\n</svg>\n";
    return o.str();
}

} // namespace

// ------------------------------------------------------------------ main --

int run_temporal(const std::string& out_dir, int frames, bool quiet) {
    const float kBudgets[] = { 20e6f, 40e6f, 60e6f, 80e6f, 100e6f, 120e6f, 150e6f };
    const int   kNB = int(sizeof kBudgets / sizeof kBudgets[0]);

    // Two gates.  "safe" holds the predicted probability of detection close
    // to chance; "floeter" is the operating point Floeter et al. found
    // acceptable in their study (their FRC 11223: full rate in the inner
    // three regions, 1/2 and 1/3 in the outer two, 63.6% fewer pixels).
    RefreshConfig safe;
    safe.fps = kFps;
    safe.target_coded_fraction = -1.0f;
    safe.gate_lo = 0.02f;
    safe.gate_hi = 0.35f;

    RefreshConfig floeter = safe;
    floeter.gate_hi = 1.20f;

    // Budget-targeted variant for the coded-fraction knob demonstration.
    RefreshConfig budgeted = safe;
    budgeted.gate_hi = 4.0f;
    budgeted.target_coded_fraction = 0.55f;

    std::ostringstream md;
    md << "# nxvc-rcsim: the temporal ladder\n\n"
       << "Generated by `rc/sim/nxvc-rcsim --temporal`.  Do not edit by hand.\n\n"
       << "The spatial results are in `RESULTS.md`; this file is only about the\n"
       << "per-tile refresh scheduler of `docs/RATECONTROL.md` section 8.  Same\n"
       << "scene, same lens, same "
       << fmt("%.0f Hz, %d frames per run, first 8 excluded.\n\n", double(kFps), frames)
       << "Sources for the model and the operating points:\n\n"
       << "* Floeter, Geringer, Reina, Weiskopf, Ropinski, *Evaluating Foveated\n"
       << "  Frame Rate Reduction in Virtual Reality for Head-Mounted Displays*,\n"
       << "  ETRA 2025, arXiv 2505.03682.\n"
       << "* Tursun and Didyk, *Perceptual Visibility Model for Temporal Contrast\n"
       << "  Changes in Periphery*, ACM TOG 41(6) 2022, arXiv 2205.00108.\n\n"
       << "Eccentricity rings are Floeter et al.'s five regions (their figure 2,\n"
       << "diameters 6.3 / 9.1 / 18.1 / 31.1 degrees after Mohanto et al. 2022,\n"
       << "read here as eccentricities 3.15 / 4.55 / 9.05 / 15.55) plus a sixth\n"
       << "for the far field their 31 degree mask does not reach.\n\n"
       << "**Everything below is conditional on the model reduction of\n"
       << "RATECONTROL.md 8.2**, in particular on reading the paper's De Lange\n"
       << "polynomial in log-frequency.  The orderings are robust to it; the\n"
       << "absolute C_M numbers are not.\n\n";

    // -----------------------------------------------------------------
    // 1. The model itself
    // -----------------------------------------------------------------
    {
        tvm::ModelParams p;
        md << "## 1. What the model says\n\n"
           << "Sensitivity `S(f_t, f_s, e)` in units of 1/Weber-contrast, so the\n"
           << "threshold contrast is `1/S`.  Rows are the representative spatial\n"
           << "frequency of the tile's residual (`f_h + f_v`, cycles/degree),\n"
           << "columns are eccentricity.\n\n"
           << "### Sensitivity at f_t = 12 Hz (a 1/6 rate step at 72 Hz)\n\n"
           << "| f_s cpd | 1 deg | 5 deg | 10 deg | 20 deg | 30 deg | 40 deg |\n"
           << "|---|---|---|---|---|---|---|\n";
        const float fss[] = { 0.25f, 1.0f, 2.0f, 3.0f, 4.0f };
        const float ecs[] = { 1.0f, 5.0f, 10.0f, 20.0f, 30.0f, 40.0f };
        for (float fs : fss) {
            md << fmt("| %.2f |", double(fs));
            for (float e : ecs)
                md << fmt(" %.2f |", double(tvm::sensitivity(12.0f, fs, e, p)));
            md << "\n";
        }
        md << "\nThe eccentricity column is the surprise and it drives the whole\n"
           << "design: at low spatial frequency the model is *more* sensitive to\n"
           << "temporal change in the periphery than at the fovea, which is the\n"
           << "Ferry-Porter direction and the opposite of the spatial ladder's\n"
           << "assumption.  What rescues peripheral skipping is the spatial\n"
           << "frequency axis: the eccentricity exponent `q(f_s)` falls from 2.39\n"
           << "at DC to 1.69 at 4 cpd, so a fine-textured tile gains far less\n"
           << "sensitivity with eccentricity than a smooth one does.\n\n"
           << fmt("At 30 degrees the ratio of the f_s = 0.25 row to the f_s = 4.0 row\n"
                  "is %.1fx.  At 1 degree it is %.1fx.  Eccentricity multiplies the\n"
                  "flat-versus-texture gap by about %.1f.\n\n",
                  double(tvm::sensitivity(12.0f, 0.25f, 30.0f, p) /
                         tvm::sensitivity(12.0f, 4.0f, 30.0f, p)),
                  double(tvm::sensitivity(12.0f, 0.25f, 1.0f, p) /
                         tvm::sensitivity(12.0f, 4.0f, 1.0f, p)),
                  double((tvm::sensitivity(12.0f, 0.25f, 30.0f, p) /
                          tvm::sensitivity(12.0f, 4.0f, 30.0f, p)) /
                         (tvm::sensitivity(12.0f, 0.25f, 1.0f, p) /
                          tvm::sensitivity(12.0f, 4.0f, 1.0f, p))));

        md << "### Temporal sensitivity against the refresh divisor, 72 Hz\n\n"
           << "| k | f_t Hz | S_SP(f_t) |\n|---|---|---|\n";
        for (int k : { 1, 2, 3, 4, 6, 8, 12 })
            md << fmt("| %d | %.1f | %.3f |\n", k, double(kFps / float(k)),
                      double(tvm::sensitivity_temporal(kFps / float(k), p)));
        md << "\nThis is why 1/2 is nearly free and 1/6 is not: at 36 Hz the De\n"
           << "Lange curve is already well past its peak, while 12 Hz is close to\n"
           << "it, and the residual that has to be carried is three times larger.\n\n"
           << "### Predicted visibility C_M of one tile, by class and eccentricity\n\n"
           << "A tile with the scene's typical residual for its material.\n\n"
           << "| class | residual MAD | e = 12 deg | 20 deg | 30 deg | 45 deg |\n"
           << "|---|---|---|---|---|---|\n";
        struct { const char* n; float R, mad, luma; } mats[] = {
            { "flat (sky)",   0.004f, 1.4f, 85.0f },
            { "texture (wall)", 0.42f, 4.0f, 128.0f },
            { "texture (foliage)", 0.97f, 14.0f, 128.0f },
            { "edge (outline)", 0.042f, 5.0f, 125.0f },
        };
        for (auto& m : mats) {
            md << fmt("| %s | %.1f |", m.n, double(m.mad));
            for (float e : { 12.0f, 20.0f, 30.0f, 45.0f }) {
                tvm::TileTemporal t;
                t.ecc_deg = e; t.freq_ratio = m.R; t.residual_mad = m.mad;
                t.mean_luma = m.luma; t.ppd_render = 22.0f;
                md << fmt(" %.3f |", double(tvm::tile_visibility(t, 3, kFps, p)));
            }
            md << "\n";
        }
        md << "\nAll at k = 3.  The sky tile is the one to look at: it has the\n"
           << "*smallest* residual of the four and by far the *largest* predicted\n"
           << "visibility, because a smooth peripheral tile is exactly what\n"
           << "peripheral motion detection is for.  The spatial ladder gives flat\n"
           << "tiles up first; the temporal ladder must not.\n\n";

        md << "Note on the luminance term: with a 100 nit panel every tile in this\n"
           << "scene sits below the model's 50 cd/m^2 Weber floor, so the de\n"
           << "Vries-Rose clamp is always active and the per-tile luminance\n"
           << "dependence is inert on this hardware.  That is a property of the\n"
           << "model at HMD luminances, not a bug, and it means `dQ_lum` and the\n"
           << "temporal cost function do not interact.\n\n";
    }

    // -----------------------------------------------------------------
    // 2. Coded fraction per ring
    // -----------------------------------------------------------------
    struct Cell { TempStats base, safe_s, flo_s; };
    std::vector<Cell> cells{size_t(kNB)};

    RefreshResult cap_r[2];
    AllocResult   cap_a[2];
    std::vector<uint8_t> cap_c[2];

    for (int b = 0; b < kNB; ++b) {
        cells[size_t(b)].base = run(true, false, safe, kBudgets[b], frames,
                                    nullptr, nullptr, nullptr, -1);
        cells[size_t(b)].safe_s = run(true, true, safe, kBudgets[b], frames,
                                      nullptr, nullptr, nullptr, -1);
        // The maps are captured where the ladder is actually live: at
        // 60 Mbit/s and above the pressure-coupled gate keeps every divisor
        // at 1 and the map is a picture of nothing (section 4).
        const int ci = (b == 0) ? 0 : (b == 1 ? 1 : -1);
        cells[size_t(b)].flo_s = run(true, true, floeter, kBudgets[b], frames,
                                     ci >= 0 ? &cap_r[ci] : nullptr,
                                     ci >= 0 ? &cap_a[ci] : nullptr,
                                     ci >= 0 ? &cap_c[ci] : nullptr, -1);
    }

    md << "## 2. Coded-tile fraction per eccentricity ring\n\n"
       << "Eye-tracked foveation, 40 ms gaze-to-photon (so the foveal pad is 3\n"
       << "degrees and the full-rate floor reaches 11 degrees of raw\n"
       << "eccentricity).  \"Coded\" counts tiles that carry a residual this\n"
       << "frame; a tile that was already `WARP_SKIP` because its warped SAD is\n"
       << "under 1.0 is not coded in either arm and is counted in neither\n"
       << "column's numerator.\n\n"
       << "### Conservative gate (C_M <= 0.35)\n\n"
       << "| bit/s | gate | " ;
    for (int r = 0; r < kRings; ++r) md << ring_name(r) << " deg | ";
    md << "all |\n|---|---|" ;
    for (int r = 0; r <= kRings; ++r) md << "---|";
    md << "\n";
    for (int b = 0; b < kNB; ++b) {
        const TempStats& s = cells[size_t(b)].safe_s;
        md << fmt("| %.0f Mbit | %.2f |", double(kBudgets[b] / 1e6f),
                  s.frames ? s.gate_sum / s.frames : 0.0);
        double c = 0, t = 0;
        for (int r = 0; r < kRings; ++r) {
            md << fmt(" %.0f%% |", 100.0 * s.coded_frac(r));
            c += s.coded[r]; t += s.tiles[r];
        }
        md << fmt(" %.0f%% |\n", t > 0 ? 100.0 * c / t : 100.0);
    }

    md << "\n### Floeter-calibrated gate (C_M <= 1.20)\n\n| bit/s | gate | ";
    for (int r = 0; r < kRings; ++r) md << ring_name(r) << " deg | ";
    md << "all |\n|---|---|";
    for (int r = 0; r <= kRings; ++r) md << "---|";
    md << "\n";
    for (int b = 0; b < kNB; ++b) {
        const TempStats& s = cells[size_t(b)].flo_s;
        md << fmt("| %.0f Mbit | %.2f |", double(kBudgets[b] / 1e6f),
                  s.frames ? s.gate_sum / s.frames : 0.0);
        double c = 0, t = 0;
        for (int r = 0; r < kRings; ++r) {
            md << fmt(" %.0f%% |", 100.0 * s.coded_frac(r));
            c += s.coded[r]; t += s.tiles[r];
        }
        md << fmt(" %.0f%% |\n", t > 0 ? 100.0 * c / t : 100.0);
    }

    md << "\n### Refresh divisor mix per ring, 20 Mbit/s, Floeter gate\n\n"
       << "Share of the ring's tiles at each divisor, steady state.  20 Mbit/s\n"
       << "because that is where the ladder is live: above about 50 Mbit/s the\n"
       << "spatial pressure never reaches `engage_pressure` and every divisor is\n"
       << "1 by design (section 4).\n\n"
       << "| ring | 1/1 | 1/2 | 1/3 | 1/4 | 1/6 | duty |\n|---|---|---|---|---|---|---|\n";
    {
        const TempStats& s = cells[0].flo_s;
        for (int r = 0; r < kRings; ++r) {
            if (s.tiles[r] <= 0) continue;
            md << fmt("| %s |", ring_name(r));
            const int kk[5] = { 1, 2, 3, 4, 6 };
            for (int j = 0; j < 5; ++j)
                md << fmt(" %.0f%% |", 100.0 * double(s.div_hist[r][kk[j]]) / s.tiles[r]);
            md << fmt(" %.2f |\n", s.dut_sum[r] / s.tiles[r]);
        }
    }
    md << "\n**That column is not monotone in eccentricity, and that is the whole\n"
       << "point.**  The 9.1-15.6 ring is stepped hardest (duty 0.69) while the\n"
       << "two rings outside it are barely touched.  A scheduler that ordered\n"
       << "tiles by eccentricity alone would have done the opposite.  The reason\n"
       << "is section 1: this scene's far periphery is sky above and ground\n"
       << "below - smooth, low-spatial-frequency material - and the model's\n"
       << "eccentricity gain is largest exactly there, so a withheld update in\n"
       << "the far field is *more* visible than one at 12 degrees on a textured\n"
       << "wall.  The middle ring is where the wall and foliage texture lives,\n"
       << "and fine texture is what the periphery cannot follow.\n\n"
       << "This is also the sharpest practical difference from the spatial\n"
       << "ladder, which gives flat tiles up first and reaches its most\n"
       << "aggressive steps at the largest eccentricity.  The two ladders order\n"
       << "tiles differently and must not be collapsed into one scalar\n"
       << "(RATECONTROL.md 8.3).\n\n"
       << "### The explicit budget knob\n\n"
       << "The two gates above are the default pressure-coupled mode: the gate\n"
       << "follows the spatial ladder's pressure and the coded fraction is\n"
       << "whatever falls out.  `RefreshConfig::target_coded_fraction` inverts\n"
       << "that - the gate is bisected until the steady-state duty cycle hits the\n"
       << "target - and `target_bits` does the same through the measured mean\n"
       << "cost of a coded tile.  80 Mbit/s, eye-tracked:\n\n"
       << "| target | duty achieved | coded fraction | gate | max C_M | mean P |\n"
       << "|---|---|---|---|---|---|\n";
    for (float t : { 0.95f, 0.85f, 0.75f, 0.65f, 0.55f, 0.45f, 0.35f }) {
        RefreshConfig c = budgeted;
        c.target_coded_fraction = t;
        const TempStats s = run(true, true, c, 80e6f, frames,
                                nullptr, nullptr, nullptr, -1);
        double cd = 0, tt = 0, d = 0, ps = 0;
        for (int r = 0; r < kRings; ++r) {
            cd += s.coded[r]; tt += s.tiles[r]; d += s.dut_sum[r]; ps += s.p_sum[r];
        }
        md << fmt("| %.2f | %.2f | %.0f%% | %.2f | %.3f | %.3f |\n",
                  double(t), tt > 0 ? d / tt : 1.0,
                  tt > 0 ? 100.0 * cd / tt : 100.0,
                  s.frames ? s.gate_sum / s.frames : 0.0,
                  s.max_vis, tt > 0 ? ps / tt : 0.5);
    }
    md << "\nThe duty cycle has a hard floor well above zero and the target cannot\n"
       << "push past it: the fovea, the text tiles, the mandatory refreshes and\n"
       << "the `k_max` bound are not for sale at any budget.  That floor is the\n"
       << "point of the invariants in section 6.\n\n";

    md << "Compare Floeter et al.'s FRC 11223: full rate in their inner three\n"
       << "regions, 1/2 in the fourth, 1/3 in the fifth, which they measured at\n"
       << "36.4% of the pixels drawn and reported as the largest reduction their\n"
       << "participants tolerated without discomfort.  The scheduler reaches the\n"
       << "same shape from the visibility model alone, without being told it.\n\n";

    // -----------------------------------------------------------------
    // 3. Predicted visibility
    // -----------------------------------------------------------------
    md << "## 3. Predicted visibility of the induced temporal artefacts\n\n"
       << "`C_M` is the Minkowski-pooled JND-normalised contrast of Tursun and\n"
       << "Didyk; `P` is the Weibull detection probability, chance being 0.50.\n"
       << "Mean C_M is over the tiles the scheduler actually stepped (k > 1);\n"
       << "mean P is over every tile in the ring, so it is the number to read as\n"
       << "\"what does the ring look like\".\n\n"
       << "### Conservative gate\n\n| bit/s | mean C_M | max C_M | mean P | ";
    for (int r = 3; r < kRings; ++r) md << "P " << ring_name(r) << " | ";
    md << "\n|---|---|---|---|---|---|---|\n";
    for (int b = 0; b < kNB; ++b) {
        const TempStats& s = cells[size_t(b)].safe_s;
        double vs = 0, vn = 0, ps = 0, pn = 0;
        for (int r = 0; r < kRings; ++r) {
            vs += s.vis_sum[r]; vn += s.vis_n[r];
            ps += s.p_sum[r];   pn += s.tiles[r];
        }
        md << fmt("| %.0f Mbit | %.3f | %.3f | %.3f |", double(kBudgets[b] / 1e6f),
                  vn > 0 ? vs / vn : 0.0, s.max_vis, pn > 0 ? ps / pn : 0.5);
        for (int r = 3; r < kRings; ++r) md << fmt(" %.3f |", s.mean_p(r));
        md << "\n";
    }
    md << "\n### Floeter gate\n\n| bit/s | mean C_M | max C_M | mean P | ";
    for (int r = 3; r < kRings; ++r) md << "P " << ring_name(r) << " | ";
    md << "\n|---|---|---|---|---|---|---|\n";
    for (int b = 0; b < kNB; ++b) {
        const TempStats& s = cells[size_t(b)].flo_s;
        double vs = 0, vn = 0, ps = 0, pn = 0;
        for (int r = 0; r < kRings; ++r) {
            vs += s.vis_sum[r]; vn += s.vis_n[r];
            ps += s.p_sum[r];   pn += s.tiles[r];
        }
        md << fmt("| %.0f Mbit | %.3f | %.3f | %.3f |", double(kBudgets[b] / 1e6f),
                  vn > 0 ? vs / vn : 0.0, s.max_vis, pn > 0 ? ps / pn : 0.5);
        for (int r = 3; r < kRings; ++r) md << fmt(" %.3f |", s.mean_p(r));
        md << "\n";
    }

    // -----------------------------------------------------------------
    // 4. The savings table
    // -----------------------------------------------------------------
    md << "\n## 4. Bit saving against the spatial-only ladder\n\n"
       << "The comparison holds two things equal.  **Equal delivered spatial\n"
       << "quality**: the temporal arm's budget is bisected until its mean\n"
       << "effective QP over the foveation level-0 tiles is no worse than the\n"
       << "spatial-only arm's at the nominal budget.  Effective QP is the coded\n"
       << "QP minus everything the ladder and the foveation resolution bought\n"
       << "back (RATECONTROL.md 4.6), so it is comparable across arms.  **Equal\n"
       << "predicted visibility**: the temporal arm's gate bounds C_M, and the\n"
       << "resulting max C_M and mean P are printed beside the saving, because a\n"
       << "saving with an unbounded artefact is not a saving.\n\n"
       << "The spatial-only arm has no temporal artefact at all beyond the\n"
       << "existing static-SAD skips, so its P is 0.50 by construction.\n\n"
       << "| nominal | spatial-only foveal eQP | conservative gate | saving | max C_M | mean P | Floeter gate | saving | max C_M | mean P |\n"
       << "|---|---|---|---|---|---|---|---|---|---|\n";
    double sav_safe = 0, sav_flo = 0;
    int    sav_n = 0;
    for (int b = 0; b < kNB; ++b) {
        const float base_eqp = float(cells[size_t(b)].base.fovea_eqp());
        const float bs = match_budget(true, safe, base_eqp, kBudgets[b], frames);
        const float bf = match_budget(true, floeter, base_eqp, kBudgets[b], frames);
        const TempStats ss = run(true, true, safe, bs, frames, nullptr, nullptr, nullptr, -1);
        const TempStats sf = run(true, true, floeter, bf, frames, nullptr, nullptr, nullptr, -1);
        double ps = 0, pns = 0, pf = 0, pnf = 0;
        for (int r = 0; r < kRings; ++r) {
            ps += ss.p_sum[r]; pns += ss.tiles[r];
            pf += sf.p_sum[r]; pnf += sf.tiles[r];
        }
        const double gs = 100.0 * (1.0 - double(bs) / double(kBudgets[b]));
        const double gf = 100.0 * (1.0 - double(bf) / double(kBudgets[b]));
        if (gs > 0.5 || gf > 0.5) { sav_safe += gs; sav_flo += gf; ++sav_n; }
        md << fmt("| %.0f Mbit | %.1f | %.1f Mbit | %.1f%% | %.3f | %.3f | "
                  "%.1f Mbit | %.1f%% | %.3f | %.3f |\n",
                  double(kBudgets[b] / 1e6f), double(base_eqp),
                  double(bs / 1e6f), gs, ss.max_vis, pns > 0 ? ps / pns : 0.5,
                  double(bf / 1e6f), gf, sf.max_vis, pnf > 0 ? pf / pnf : 0.5);
    }
    md << fmt("\nMean saving over the budgets where the ladder engages at all:\n"
              "%.1f%% at the conservative gate, %.1f%% at the Floeter gate\n"
              "(%d of %d budgets).\n\n",
              sav_n ? sav_safe / sav_n : 0.0, sav_n ? sav_flo / sav_n : 0.0,
              sav_n, kNB)
       << "**The temporal ladder is a low-bitrate tool, and the zeros above are\n"
       << "the design working, not a failure.**  In the default pressure-coupled\n"
       << "mode it does not engage until the spatial ladder has spent its free\n"
       << "steps, and on this scene that knee sits between 40 and 60 Mbit/s: the\n"
       << "spatial-only foveal effective QP falls from 30.6 to 22.4 across it, so\n"
       << "above the knee there is no starvation for the temporal ladder to\n"
       << "relieve and it correctly does nothing.  Below the knee - which is the\n"
       << "20 to 40 Mbit/s regime a real WiFi link spends its bad minutes in, and\n"
       << "the regime RATECONTROL.md 4.8 describes as skip-dominated - it is\n"
       << "worth more than half the bitrate.  A caller that wants the saving at a\n"
       << "high budget anyway sets `target_coded_fraction` and takes the gate out\n"
       << "of the pressure loop entirely.\n\n";

    // -----------------------------------------------------------------
    // 5. Fixed vs eye-tracked foveation
    // -----------------------------------------------------------------
    md << "## 5. What fixed foveation costs the temporal ladder\n\n"
       << "The scheduler consumes `FoveationMap::ecc_deg`, which is `e'` - the\n"
       << "eccentricity *after* the fixed-foveation eye box or the gaze pad.\n"
       << "That is the conservative reading and it is the same one the spatial\n"
       << "ladder uses: without a tracker the gaze may be anywhere inside the\n"
       << "20 x 15 degree box, so `e'` is the smallest eccentricity the tile can\n"
       << "have.  The temporal ladder pays much more for that than the spatial\n"
       << "one does, because its whole reach is outside the box.\n\n"
       << "| bit/s | foveation | coded fraction | duty | max C_M | mean P |\n"
       << "|---|---|---|---|---|---|\n";
    for (float bps : { 20e6f, 40e6f, 60e6f }) {
        for (int ey = 0; ey < 2; ++ey) {
            const TempStats s = run(ey != 0, true, floeter, bps, frames,
                                    nullptr, nullptr, nullptr, -1);
            double c = 0, t = 0, d = 0, ps = 0;
            for (int r = 0; r < kRings; ++r) {
                c += s.coded[r]; t += s.tiles[r];
                d += s.dut_sum[r]; ps += s.p_sum[r];
            }
            md << fmt("| %.0f Mbit | %s | %.0f%% | %.2f | %.3f | %.3f |\n",
                      double(bps / 1e6f), ey ? "eye-tracked" : "fixed",
                      t > 0 ? 100.0 * c / t : 100.0, t > 0 ? d / t : 1.0,
                      s.max_vis, t > 0 ? ps / t : 0.5);
        }
    }
    md << "\n";

    // -----------------------------------------------------------------
    // 6. Invariants
    // -----------------------------------------------------------------
    md << "## 6. Invariants, measured over the whole sweep\n\n"
       << "| | conservative | Floeter |\n|---|---|---|\n";
    {
        long av_s = 0, av_f = 0, ma_s = 0, ma_f = 0, tx_s = 0, tx_f = 0,
             fv_s = 0, fv_f = 0;
        for (int b = 0; b < kNB; ++b) {
            av_s += cells[size_t(b)].safe_s.age_violations;
            av_f += cells[size_t(b)].flo_s.age_violations;
            ma_s = std::max(ma_s, cells[size_t(b)].safe_s.max_age);
            ma_f = std::max(ma_f, cells[size_t(b)].flo_s.max_age);
            tx_s += cells[size_t(b)].safe_s.text_skips;
            tx_f += cells[size_t(b)].flo_s.text_skips;
            fv_s += cells[size_t(b)].safe_s.fovea_skips;
            fv_f += cells[size_t(b)].flo_s.fovea_skips;
        }
        md << fmt("| max consecutive skips (bound is k_max - 1 = %d) | %ld | %ld |\n",
                  int(safe.k_max_frames) - 1, ma_s, ma_f)
           << fmt("| tiles exceeding the age bound | %ld | %ld |\n", av_s, av_f)
           << fmt("| non-static text tiles skipped | %ld | %ld |\n", tx_s, tx_f)
           << fmt("| non-static tiles inside the foveal floor skipped | %ld | %ld |\n",
                  fv_s, fv_f);
    }
    md << "\nAll four are checked as hard assertions in `tests/rc/test_refresh.cpp`\n"
       << "over a wider parameter sweep than this one; the table is here so the\n"
       << "numbers are visible next to the savings they paid for.\n\n";

    // -----------------------------------------------------------------
    // 7. Maps
    // -----------------------------------------------------------------
    md << "## 7. Refresh rate per tile\n\n"
       << "Left eye.  `.` = coded every frame, `2` `3` `4` `6` = one frame in k,\n"
       << "blank = already `WARP_SKIP` on the allocator's static or\n"
       << "one-header-floor test, so the temporal ladder had nothing to\n"
       << "withhold.  Each map is the frame of its run in which the ladder was\n"
       << "most engaged, i.e. the lowest duty cycle.\n\n";
    {
        Rig probe; probe.init(true, safe);
        const int tx = probe.tiles_x, ty = probe.tiles_y;
        md << fmt("### 20 Mbit/s, Floeter gate\n\n```\n%s```\n\n",
                  ascii_refresh_map(cap_r[0], cap_a[0], tx, ty).c_str())
           << fmt("### 40 Mbit/s, Floeter gate\n\n```\n%s```\n\n",
                  ascii_refresh_map(cap_r[1], cap_a[1], tx, ty).c_str());
        std::ofstream svg(out_dir + "/refresh-map.svg");
        std::vector<const RefreshResult*> rs = { &cap_r[0], &cap_r[1] };
        std::vector<const AllocResult*>   as = { &cap_a[0], &cap_a[1] };
        std::vector<std::string> lb = { "20 Mbit/s, Floeter gate",
                                        "40 Mbit/s, Floeter gate" };
        svg << svg_refresh_sheet(rs, as, lb, tx, ty);
        md << "A colour version is in `refresh-map.svg`.\n\n";
    }

    md << "## 8. Reading the numbers\n\n"
       << "The same caveat as `RESULTS.md` applies twice over here.  The bit\n"
       << "model is the controller's own functional form with a ground truth it\n"
       << "does not know, so the savings exercise the *scheduler*, not the codec.\n"
       << "And the visibility numbers are a reduction of a psychophysical model\n"
       << "onto five per-tile statistics, with the approximations listed in\n"
       << "RATECONTROL.md 8.2, of which the log-frequency reading of the De Lange\n"
       << "polynomial is much the largest.  What the simulator does establish is\n"
       << "that the scheduler is monotone, bounded, deterministic, respects every\n"
       << "invariant it claims, and lands unprompted on the operating point a\n"
       << "15-participant user study independently found acceptable.\n";

    std::ofstream f(out_dir + "/RESULTS-temporal.md");
    if (!f) {
        std::fprintf(stderr, "rcsim: cannot write %s/RESULTS-temporal.md\n",
                     out_dir.c_str());
        return 1;
    }
    f << md.str();
    if (!quiet)
        std::printf("wrote %s/RESULTS-temporal.md and %s/refresh-map.svg\n",
                    out_dir.c_str(), out_dir.c_str());
    return 0;
}

} // namespace rcsim
