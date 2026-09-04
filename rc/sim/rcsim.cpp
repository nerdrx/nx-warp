// nxvc-rcsim - rate control simulator.
//
// Drives nxvc_rc over a synthetic per-tile scene with budgets from 20 Mbit/s
// to 1 Gbit/s and scene cuts, and writes RESULTS.md plus ladder-map.svg.
//
//   nxvc-rcsim [--out DIR] [--frames N] [--dump FILE] [--quiet]
//
// SPDX-License-Identifier: Apache-2.0

#include "nxrc/nxrc.hpp"
#include "nxfov/foveation.hpp"
#include "render.hpp"
#include "scene.hpp"
#include "temporal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace nxrc;

namespace {

constexpr float kFps  = 72.0f;
constexpr int   kEyes = 2;

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap; va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return std::string(buf);
}

// ---------------------------------------------------------------- stats ---

struct RunStats {
    std::vector<float> err;                 // (actual - budget) / budget
    double qp_sum[4]   = {0, 0, 0, 0};
    long   qp_n[4]     = {0, 0, 0, 0};
    long   sev_hist[4][5] = {};          // severity buckets, comparable
    double eqp_sum[4] = {0, 0, 0, 0};    // effective QP per class
    double sev_sum[4] = {0, 0, 0, 0};
    long   res_hist[3] = {0, 0, 0};
    long   dc_tiles = 0, skip_tiles = 0, tile_n = 0;
    double pressure_sum = 0;
    int    frames = 0;
    double model_err_sum = 0;
    long   model_err_n = 0;
    double bits_by_level[3] = {0, 0, 0};

    float mean_abs_err() const {
        if (err.empty()) return 0;
        double s = 0; for (float e : err) s += std::fabs(e);
        return float(s / double(err.size()));
    }
    float p95_abs_err() const {
        if (err.empty()) return 0;
        std::vector<float> v; v.reserve(err.size());
        for (float e : err) v.push_back(std::fabs(e));
        std::sort(v.begin(), v.end());
        return v[size_t(0.95 * double(v.size() - 1))];
    }
    float bias() const {
        if (err.empty()) return 0;
        double s = 0; for (float e : err) s += e;
        return float(s / double(err.size()));
    }
};

// ------------------------------------------------------------ simulator ---

struct Sim {
    rcsim::Scene           scene;
    nxfov::FoveationMap    fov;
    nxfov::LensParams      lens;
    nxfov::FoveationConfig fcfg;
    RateController         rc;
    Governor               gov;
    ClassifyConfig         ccfg;
    std::vector<uint8_t>   cls, prev_cls;
    std::vector<float>     actual;
    int tiles_x = 0, tiles_y = 0;
    uint32_t rs = 0x9e3779b9u;

    void init(uint32_t seed) {
        lens = nxfov::pico4_eye();
        nxfov::FoveationMap one = nxfov::foveation_map(lens, fcfg, nullptr);
        tiles_x = one.tiles_x; tiles_y = one.tiles_y;
        fov = rcsim::stereo_map(one, kEyes);
        scene.build(tiles_x, tiles_y, kEyes, seed);
        rc.reset(fov.size());
        gov.reset();
        cls.assign(fov.size(), 0);
        prev_cls.clear();
        actual.assign(fov.size(), 0.0f);
    }

    void set_region_scale(float s) {
        nxfov::FoveationConfig c = fcfg;
        c.region_scale = s;
        nxfov::FoveationMap one = nxfov::foveation_map(lens, c, nullptr);
        fov = rcsim::stereo_map(one, kEyes);
    }

    // What the tile really costs.  Ground truth is scene.a_true, which the
    // controller's model has to learn from a_init.
    float tile_bits(const AllocResult& a, size_t i, const RateConfig& cfg) {
        if (a.skip[i]) return 1.0f;               // one bit in the row bitmap
        const float s_fov  = nxfov::level_scale(fov.level[i]);
        const float s_code = nxfov::level_scale(a.res_level[i]);
        float g = (s_code * s_code) / (s_fov * s_fov);
        if (a.wm_id[i] == WM_PERIPH) g *= cfg.gain_wm_periph;
        if (a.dc_plane[i])           g *= cfg.gain_dc_plane;

        rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
        const float noise = 0.92f + 0.16f * float(rs & 0xFFFF) / 65535.0f;
        const float payload = scene.a_true[i] * s_fov * s_fov * g *
                              std::exp2(-float(a.qp[i]) / 6.0f) * noise;
        return 64.0f + payload;                   // 8 byte header + payload
    }

    // Decode-time model: entropy decode is proportional to bits, the rest to
    // coded samples.  Calibrated so 100 Mbit/s at 72 Hz on an unthrottled
    // Adreno 650 lands near the paper's 5 ms.
    float decode_us(const AllocResult& a, float total_bits, float throttle) const {
        double samples = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a.skip[i]) continue;
            const float s = nxfov::level_scale(a.res_level[i]);
            samples += 4096.0 * double(s) * double(s) * (a.dc_plane[i] ? 0.25 : 1.0);
        }
        const double t = 1.1e-3 * double(total_bits) / 1000.0     // entropy
                       + 6.5e-4 * samples;                        // transform+warp
        return float(t * double(throttle));
    }
};

struct FrameOut {
    float budget = 0, actual = 0, predicted = 0, pressure = 0, decode_us = 0;
    int   knob = 0, skipped = 0;
    bool  cut = false;
};

// Run `frames` frames at a fixed budget.  If `capture_frame` >= 0 the
// allocation of that frame is copied out for the ladder maps.
RunStats run(Sim& sim, float bitrate_bps, int frames,
             bool use_governor, float throttle_at, float throttle_max,
             int capture_frame, AllocResult* capture,
             std::vector<uint8_t>* capture_cls,
             std::vector<FrameOut>* trace) {
    RunStats st;
    sim.init(0xC0FFEEu);
    const float B = bitrate_bps / kFps;

    for (int f = 0; f < frames; ++f) {
        sim.scene.step(f, 0x5EEDu);

        classify_tiles(sim.scene.stats, sim.ccfg,
                       sim.prev_cls.empty() ? std::span<const uint8_t>()
                                            : std::span<const uint8_t>(sim.prev_cls),
                       sim.cls);
        sim.prev_cls = sim.cls;

        FrameInputs in;
        in.fov        = &sim.fov;
        in.cls        = sim.cls;
        in.complexity = sim.scene.complexity;
        in.slip_deg_s = sim.scene.slip;
        in.stats      = &sim.scene.stats;
        in.head_speed_deg_s = sim.scene.head_speed_deg_s;
        in.intra_ratio      = sim.scene.intra_ratio;

        const AllocResult& a = sim.rc.allocate(B, in);

        double total = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            sim.actual[i] = sim.tile_bits(a, i, sim.rc.config());
            total += sim.actual[i];
        }

        // Warm-up frames are excluded from the tracking statistics: the model
        // starts at a_init and needs two or three frames (PAPER.md 4.6).
        if (f >= 8) {
            st.err.push_back(float((total - a.budget_bits) / a.budget_bits));
            st.pressure_sum += a.pressure;
            ++st.frames;
            for (size_t i = 0; i < a.size(); ++i) {
                ++st.tile_n;
                if (a.skip[i]) { ++st.skip_tiles; continue; }
                const uint8_t c = sim.cls[i] < 4 ? sim.cls[i] : 1;
                st.qp_sum[c] += a.qp[i]; ++st.qp_n[c];
                st.eqp_sum[c] += effective_qp(a, sim.fov, i, sim.rc.config());
                const float sev = ladder_severity(a, sim.fov, i, sim.rc.config());
                st.sev_sum[c] += sev;
                const int bk = sev <= 0.01f ? 0 : sev <= 3.0f ? 1
                             : sev <= 9.0f ? 2 : sev <= 15.0f ? 3 : 4;
                ++st.sev_hist[c][bk];
                ++st.res_hist[std::min<int>(a.res_level[i], 2)];
                if (a.dc_plane[i]) ++st.dc_tiles;
                st.bits_by_level[sim.fov.level[i]] += sim.actual[i];
                if (a.predicted_bits[i] > 1.0f) {
                    st.model_err_sum += std::fabs(sim.actual[i] - a.predicted_bits[i]) /
                                        a.predicted_bits[i];
                    ++st.model_err_n;
                }
            }
        }

        float du = 0;
        if (use_governor || throttle_max > 1.0f) {
            const float th = (f >= int(throttle_at))
                ? 1.0f + (throttle_max - 1.0f) *
                          std::min(1.0f, float(f - throttle_at) / 60.0f)
                : 1.0f;
            du = sim.decode_us(a, float(total), th);
            if (use_governor) {
                const int before = sim.gov.state().knob_level;
                const KnobState& k = sim.gov.update(du, 1e6f / kFps);
                sim.rc.set_knobs(k);
                if (k.knob_level != before) sim.set_region_scale(k.fovea_region_scale);
            }
        }

        if (trace) {
            FrameOut fo;
            fo.budget = a.budget_bits; fo.actual = float(total);
            fo.predicted = a.predicted_total; fo.pressure = a.pressure;
            fo.decode_us = du; fo.knob = sim.gov.state().knob_level;
            fo.skipped = a.skipped; fo.cut = a.scene_cut;
            trace->push_back(fo);
        }

        if (capture && f == capture_frame) {
            *capture = a;
            if (capture_cls) *capture_cls = sim.cls;
        }

        sim.rc.update_model(std::span<const float>(sim.actual));
    }
    return st;
}

} // namespace

int main(int argc, char** argv) {
    std::string out_dir = ".";
    std::string dump;
    int frames = 400;
    bool quiet = false;
    bool temporal = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--temporal")) temporal = true;
        else if (!std::strcmp(argv[i], "--help")) {
            std::printf("nxvc-rcsim [--out DIR] [--frames N] [--dump FILE] "
                        "[--temporal] [--quiet]\n");
            return 0;
        }
    }

    // The temporal-ladder scenario is a separate scene walk with its own
    // report; RATECONTROL.md 8.
    if (temporal) return rcsim::run_temporal(out_dir, frames < 40 ? 200 : frames, quiet);

    Sim sim;
    sim.init(0xC0FFEEu);
    const int tx = sim.tiles_x, ty = sim.tiles_y;
    const size_t n = sim.fov.size();

    if (!dump.empty() && !sim.scene.load_dump(dump))
        std::fprintf(stderr, "rcsim: no usable dump at %s, using the synthetic scene\n",
                     dump.c_str());

    std::ostringstream md;
    md << "# nxvc-rcsim results\n\n"
       << "Generated by `rc/sim/nxvc-rcsim`.  Do not edit by hand.\n\n"
       << fmt("Grid: %d x %d tiles per eye, %d eyes, %zu tiles per stereo frame, "
              "%.0f Hz.\n", tx, ty, kEyes, n, double(kFps))
       << "Scene: sky (flat), walls and ground (band-limited texture), geometry\n"
       << "outlines (edges), a foliage band (high-activity texture, moving) and a\n"
       << "6x4-tile UI panel with the stencil bit set.  Two scene cuts, at frames\n"
       << "120 and 260.  Fixed foveation, no eye tracking.\n\n";

    // ---------------------------------------------------------------------
    // 1. Foveation map
    // ---------------------------------------------------------------------
    {
        long lv[3] = {0, 0, 0};
        for (int i = 0; i < tx * ty; ++i) ++lv[sim.fov.level[i]];
        const double t = double(tx) * double(ty);
        md << "## 1. Foveation map (Pico 4, fixed, 20x15 deg eye box)\n\n"
           << fmt("ppd_center %.1f, corner eccentricity %.1f deg.\n\n",
                  double(nxfov::ppd_center(sim.lens)),
                  double(sim.fov.ecc_raw[size_t(ty - 1) * tx + (tx - 1)]))
           << fmt("| level | s | tiles | share | sample share |\n|---|---|---|---|---|\n")
           << fmt("| 0 | 1 | %ld | %.1f%% | %.3f |\n", lv[0], 100.0 * lv[0] / t, lv[0] / t)
           << fmt("| 1 | 1/2 | %ld | %.1f%% | %.3f |\n", lv[1], 100.0 * lv[1] / t, lv[1] / t / 4.0)
           << fmt("| 2 | 1/4 | %ld | %.1f%% | %.3f |\n", lv[2], 100.0 * lv[2] / t, lv[2] / t / 16.0)
           << fmt("\nTotal coded samples: %.3f of full resolution "
                  "(PAPER.md 5.1.3 predicts 0.50).\n\n",
                  (lv[0] + lv[1] / 4.0 + lv[2] / 16.0) / t)
           << "```\n" << rcsim::ascii_fov_map(sim.fov, tx, ty) << "```\n\n";
    }

    // ---------------------------------------------------------------------
    // 2. Budget sweep
    // ---------------------------------------------------------------------
    const float kBudgets[] = {20e6f, 40e6f, 80e6f, 150e6f, 300e6f, 600e6f, 1000e6f};
    const int   kNB = int(sizeof kBudgets / sizeof kBudgets[0]);

    std::vector<AllocResult>          caps(kNB);
    std::vector<std::vector<uint8_t>> caps_cls(kNB);
    std::vector<RunStats>             runs(kNB);

    md << "## 2. Budget sweep\n\n"
       << "400 frames per budget, first 8 excluded (model warm-up).  Tracking\n"
       << "error is (actual - budget) / budget over the whole frame.\n\n"
       << "| bit/s | bits/frame | mean |err| | p95 |err| | bias | mean P | skip | "
          "model err |\n|---|---|---|---|---|---|---|---|\n";

    for (int b = 0; b < kNB; ++b) {
        runs[b] = run(sim, kBudgets[b], frames, false, 1e9f, 1.0f,
                      frames - 40, &caps[b], &caps_cls[b], nullptr);
        const RunStats& s = runs[b];
        md << fmt("| %.0f Mbit | %.0f k | %.2f%% | %.2f%% | %+.2f%% | %.2f | %.1f%% | %.1f%% |\n",
                  double(kBudgets[b] / 1e6f), double(kBudgets[b] / kFps / 1000.0f),
                  100.0 * s.mean_abs_err(), 100.0 * s.p95_abs_err(),
                  100.0 * s.bias(), s.pressure_sum / std::max(1, s.frames),
                  100.0 * double(s.skip_tiles) / double(std::max(1L, s.tile_n)),
                  100.0 * s.model_err_sum / double(std::max(1L, s.model_err_n)));
    }

    // QP distribution per class
    md << "\n### Coded QP by tile class\n\n| bit/s |";
    for (int c = 0; c < 4; ++c) md << " " << class_name(uint8_t(c)) << " |";
    md << "\n|---|---|---|---|---|\n";
    for (int b = 0; b < kNB; ++b) {
        md << fmt("| %.0f Mbit |", double(kBudgets[b] / 1e6f));
        for (int c = 0; c < 4; ++c) {
            if (runs[b].qp_n[c])
                md << fmt(" %.1f |", runs[b].qp_sum[c] / double(runs[b].qp_n[c]));
            else md << " - |";
        }
        md << "\n";
    }
    md << "\nCoded QP alone is misleading: a texture tile at res 1/4 with the\n"
       << "low-pass matrix has a *lower* coded QP than a text tile at full\n"
       << "resolution, because the ladder took its detail away instead of its\n"
       << "precision.  The comparable number is the effective QP, the coded QP\n"
       << "minus everything the ladder and the foveation resolution bought back\n"
       << "(RATECONTROL.md 4.6).  Lower is better; text must stay lowest.\n\n"
       << "### Effective QP by tile class (comparable across classes)\n\n| bit/s |";
    for (int c = 0; c < 4; ++c) md << " " << class_name(uint8_t(c)) << " |";
    md << "\n|---|---|---|---|---|\n";
    for (int b = 0; b < kNB; ++b) {
        md << fmt("| %.0f Mbit |", double(kBudgets[b] / 1e6f));
        for (int c = 0; c < 4; ++c) {
            if (runs[b].qp_n[c])
                md << fmt(" %.1f |", runs[b].eqp_sum[c] / double(runs[b].qp_n[c]));
            else md << " - |";
        }
        md << "\n";
    }

    // Ladder engagement
    md << "\n### Ladder engagement by severity (share of tiles of that class)\n\n"
       << "Severity is what the ladder removed, in QP-equivalent units.  Buckets:\n"
       << "untouched, <=3 (low-pass matrix), <=9 (res 1/2), <=15 (res 1/4), >15\n"
       << "(DC plane).  This is the table to read for \"blur, never block\": the\n"
       << "text row must stay left of the texture row at every budget.\n\n"
       << "| bit/s | class | mean sev | untouched | <=3 | <=9 | <=15 | >15 |\n"
       << "|---|---|---|---|---|---|---|---|\n";
    for (int b = 0; b < kNB; ++b) {
        for (int c = 3; c >= 0; --c) {
            long tot = 0;
            for (int s2 = 0; s2 < 5; ++s2) tot += runs[b].sev_hist[c][s2];
            if (!tot) continue;
            md << fmt("| %.0f Mbit | %s | %.1f |", double(kBudgets[b] / 1e6f),
                      class_name(uint8_t(c)), runs[b].sev_sum[c] / double(tot));
            for (int s2 = 0; s2 < 5; ++s2)
                md << fmt(" %.0f%% |", 100.0 * double(runs[b].sev_hist[c][s2]) / double(tot));
            md << "\n";
        }
        md << "|  |  |  |  |  |  |  |  |\n";
    }

    md << "\n### Coded resolution and bit share by foveation level\n\n"
       << "| bit/s | res 1/1 | res 1/2 | res 1/4 | DC plane | bits L0 | L1 | L2 |\n"
       << "|---|---|---|---|---|---|---|---|\n";
    for (int b = 0; b < kNB; ++b) {
        const RunStats& s = runs[b];
        const double rt = double(s.res_hist[0] + s.res_hist[1] + s.res_hist[2]);
        const double bt = s.bits_by_level[0] + s.bits_by_level[1] + s.bits_by_level[2];
        md << fmt("| %.0f Mbit | %.0f%% | %.0f%% | %.0f%% | %.1f%% | %.0f%% | %.0f%% | %.1f%% |\n",
                  double(kBudgets[b] / 1e6f),
                  100.0 * s.res_hist[0] / std::max(1.0, rt),
                  100.0 * s.res_hist[1] / std::max(1.0, rt),
                  100.0 * s.res_hist[2] / std::max(1.0, rt),
                  100.0 * double(s.dc_tiles) / std::max(1.0, rt),
                  100.0 * s.bits_by_level[0] / std::max(1.0, bt),
                  100.0 * s.bits_by_level[1] / std::max(1.0, bt),
                  100.0 * s.bits_by_level[2] / std::max(1.0, bt));
    }

    // ---------------------------------------------------------------------
    // 3. Ladder maps
    // ---------------------------------------------------------------------
    md << "\n## 3. Which tile is at which ladder step\n\n"
       << "Left eye only.  Class map: `.` flat, `x` texture, `E` edge, `T` text.\n"
       << "Severity map: `.` untouched, `-` low-pass matrix, `+` res 1/2,\n"
       << "`#` res 1/4, `@` DC plane, blank = SKIP_WARP.  Read the two together:\n"
       << "the `T` block and the `E` tiles stay at `.` or `-` while the `x` field\n"
       << "walks to `#`.\n\n";

    md << "```\n"
       << rcsim::side_by_side(rcsim::ascii_class_map(caps_cls[0], tx, ty),
                              rcsim::ascii_fov_map(sim.fov, tx, ty),
                              "class", "foveation level")
       << "```\n\n";

    for (int b = 0; b < kNB; ++b) {
        md << fmt("### %.0f Mbit/s (mean pressure P = %.2f)\n\n```\n",
                  double(kBudgets[b] / 1e6f),
                  runs[b].pressure_sum / std::max(1, runs[b].frames));
        md << rcsim::side_by_side(
            rcsim::ascii_sev_map(caps[b], sim.fov, sim.rc.config(), tx, ty),
            rcsim::ascii_res_map(caps[b], tx, ty),
            "ladder severity", "coded res (0=1/1 1=1/2 2=1/4 D=DC)");
        md << "```\n\n";
    }

    // SVG sheet
    {
        std::vector<rcsim::SvgPanel> panels;
        const int show[] = {0, 1, 3, 5};
        for (int k : show)
            panels.push_back({fmt("%.0f Mbit/s", double(kBudgets[k] / 1e6f)),
                              &caps[k], &caps_cls[k], &sim.fov});
        std::ofstream svg(out_dir + "/ladder-map.svg");
        svg << rcsim::svg_ladder_sheet(panels, sim.rc.config(), tx, ty);
        md << "A colour version of the same thing is in `ladder-map.svg`.\n\n";
    }

    // ---------------------------------------------------------------------
    // 4. Scene cuts
    // ---------------------------------------------------------------------
    {
        std::vector<FrameOut> trace;
        run(sim, 150e6f, 200, false, 1e9f, 1.0f, -1, nullptr, nullptr, &trace);
        md << "## 4. Scene cuts\n\n"
           << "150 Mbit/s.  Frame 120 is a cut: 85% of tiles above the intra\n"
           << "threshold, so the allocator gets 1.5x the nominal budget and the\n"
           << "excess is repaid over the next 30 frames.\n\n"
           << "| frame | budget (k) | actual (k) | actual/nominal | P | cut |\n"
           << "|---|---|---|---|---|---|\n";
        const float nominal = 150e6f / kFps;
        for (int f = 116; f <= 156; ++f) {
            if (f > 126 && f < 150 && (f % 4)) continue;
            const FrameOut& t = trace[size_t(f)];
            md << fmt("| %d | %.0f | %.0f | %.2fx | %.2f | %s |\n", f,
                      double(t.budget / 1000.0f), double(t.actual / 1000.0f),
                      double(t.actual / nominal), double(t.pressure),
                      t.cut ? "yes" : "");
        }
        float peak = 0;
        for (auto& t : trace) peak = std::max(peak, t.actual / nominal);
        md << fmt("\nPeak frame is %.2fx the nominal budget (the 1.5x cap plus the\n"
                  "model's error on a frame whose content it has never seen).\n\n",
                  double(peak));
    }

    // ---------------------------------------------------------------------
    // 5. Governor under a thermal ramp
    // ---------------------------------------------------------------------
    {
        std::vector<FrameOut> trace;
        run(sim, 300e6f, 900, true, 200.0f, 2.6f, -1, nullptr, nullptr, &trace);
        md << "## 5. Decode-time governor under a thermal ramp\n\n"
           << "300 Mbit/s, 72 Hz (13.9 ms period, 5.6 ms decode target).  From\n"
           << "frame 200 the simulated GPU throttles linearly to 2.6x its decode\n"
           << "cost over 60 frames and stays there.\n\n"
           << "| frame | decode us | % of target | knob level |\n|---|---|---|---|\n";
        const float target = 0.40f * 1e6f / kFps;
        int last = -1;
        for (size_t f = 0; f < trace.size(); ++f) {
            const bool change = trace[f].knob != last;
            if (!change && f % 100) continue;
            last = trace[f].knob;
            md << fmt("| %zu | %.0f | %.0f%% | %d |\n", f, double(trace[f].decode_us),
                      100.0 * double(trace[f].decode_us / target), trace[f].knob);
        }
        int max_knob = 0;
        for (auto& t : trace) max_knob = std::max(max_knob, t.knob);
        md << fmt("\nHighest knob reached: %d.  Final knob: %d.\n\n",
                  max_knob, trace.back().knob);
    }

    // ---------------------------------------------------------------------
    // 6. Model convergence
    // ---------------------------------------------------------------------
    {
        std::vector<FrameOut> trace;
        run(sim, 150e6f, 60, false, 1e9f, 1.0f, -1, nullptr, nullptr, &trace);
        md << "## 6. Bit-model convergence\n\n"
           << "The model starts at a_init = 32768 for every tile while the scene's\n"
           << "true per-tile cost spans 6000 to 90000.  Frame-total error:\n\n"
           << "| frame | predicted (k) | actual (k) | error |\n|---|---|---|---|\n";
        for (int f = 0; f < 12; ++f)
            md << fmt("| %d | %.0f | %.0f | %+.1f%% |\n", f,
                      double(trace[size_t(f)].predicted / 1000.0f),
                      double(trace[size_t(f)].actual / 1000.0f),
                      100.0 * double((trace[size_t(f)].actual - trace[size_t(f)].budget) /
                                     trace[size_t(f)].budget));
        md << "\n";
    }

    md << "## 7. Reading the numbers\n\n"
       << "See docs/RATECONTROL.md for the algorithms and the appendix for the\n"
       << "decisions these numbers were produced under.  Everything here is a\n"
       << "model: the per-tile cost function is the same `a * s^2 * gain *\n"
       << "2^(-QP/6)` the controller assumes, with a per-tile ground truth the\n"
       << "controller does not know and +/-8% noise.  It exercises the control\n"
       << "loop, not the codec; the gain constants in RateConfig are the numbers\n"
       << "a real encoder run has to replace.\n";

    std::ofstream f(out_dir + "/RESULTS.md");
    if (!f) { std::fprintf(stderr, "rcsim: cannot write %s/RESULTS.md\n", out_dir.c_str()); return 1; }
    f << md.str();
    if (!quiet) std::printf("wrote %s/RESULTS.md and %s/ladder-map.svg\n",
                            out_dir.c_str(), out_dir.c_str());
    return 0;
}
