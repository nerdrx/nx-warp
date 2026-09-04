// SPDX-License-Identifier: Apache-2.0
//
// Per-tile bit allocation, the degradation ladder and the QP model.
// PAPER.md 4.6, 4.6.1, 5.2.  See docs/RATECONTROL.md for the write-up.

#include "nxrc/nxrc.hpp"

#include <algorithm>
#include <cmath>

namespace nxrc {

// ------------------------------------------------------------ the ladder --
//
// Step 0 is "untouched".  `res_level_abs` is an absolute floor: the tile's
// coded resolution is max(foveation level, res_level_abs), so a step never
// makes a periphery tile sharper.
//
// This is the table of PAPER.md 4.6.1 as implemented.  Row order is
// Flat, Texture, Edge, Text to match TileClass.

// The qp_add on the texture and flat rows at steps 3 and 4 is not in the
// paper's table.  It is needed for the ordering invariant to hold in the far
// periphery: a tile the foveation map already put at 1/4 cannot descend any
// further by resolution, so without a QP term its severity would stop rising
// while the edge row's "+4 QP" kept going, and an edge tile would end up
// more degraded than a texture tile beside it.  See RATECONTROL.md A.3.
static const LadderStep kLadder[4][kLadderSteps] = {
    // Flat: follows Texture exactly (PAPER.md 4.6.1 "Same" column).
    { { 0, WM_LUMA,   0,  0 },
      { 0, WM_PERIPH, 0,  0 },
      { 1, WM_PERIPH, 0,  0 },
      { 2, WM_PERIPH, 0,  4 },
      { 2, WM_PERIPH, 1,  4 } },
    // Texture
    { { 0, WM_LUMA,   0,  0 },
      { 0, WM_PERIPH, 0,  0 },
      { 1, WM_PERIPH, 0,  0 },
      { 2, WM_PERIPH, 0,  4 },
      { 2, WM_PERIPH, 1,  4 } },
    // Edge: QP only until step 4, then one resolution step.
    { { 0, WM_LUMA,   0,  0 },
      { 0, WM_LUMA,   0,  2 },
      { 0, WM_LUMA,   0,  2 },
      { 0, WM_PERIPH, 0,  4 },
      { 1, WM_PERIPH, 0,  4 } },
    // Text: untouched until step 4, then +4 QP inside the class ceiling.
    { { 0, WM_FLAT,   0,  0 },
      { 0, WM_FLAT,   0,  0 },
      { 0, WM_FLAT,   0,  0 },
      { 0, WM_FLAT,   0,  0 },
      { 0, WM_FLAT,   0,  4 } },
};

const LadderStep& ladder_step(uint8_t cls, int step) {
    const uint8_t c = cls < 4 ? cls : uint8_t(TileClass::Texture);
    return kLadder[c][std::clamp(step, 0, kLadderSteps - 1)];
}

int ladder_max_step(uint8_t) { return kLadderSteps - 1; }

// (defined after step_gain, below)

// ------------------------------------------------------------- helpers ----

namespace {

inline float exp2f_(float x) { return std::exp2(x); }

// Van der Corput (bit-reversed) dither in [0, 1), indexed by tile id.
//
// The QP has to be an integer, and tiles that share a class and a foveation
// level share a bit target, so plain rounding sends a whole group the same
// way and the frame misses its budget by up to half a QP step - 6% of the
// bits, in one direction, every frame.  Dithering the rounding threshold
// with a low-discrepancy sequence makes the group's *average* cost hit the
// target exactly, at the price of neighbouring tiles differing by one QP,
// which is below the visible threshold and is what adaptive quantisation
// does anyway.  It is deterministic, so the encoder and the model agree.
inline float qp_dither(size_t i) {
    uint32_t b = uint32_t(i);
    b = (b << 16) | (b >> 16);
    b = ((b & 0x00ff00ffu) << 8) | ((b & 0xff00ff00u) >> 8);
    b = ((b & 0x0f0f0f0fu) << 4) | ((b & 0xf0f0f0f0u) >> 4);
    b = ((b & 0x33333333u) << 2) | ((b & 0xccccccccu) >> 2);
    b = ((b & 0x55555555u) << 1) | ((b & 0xaaaaaaaau) >> 1);
    return float(b) * 2.3283064365386963e-10f;   // / 2^32
}

// Bit-domain gain of one ladder step relative to the tile's foveation
// resolution.  This is what makes the ladder cheap: coding fewer samples,
// rolling off the high frequencies, or keeping only the block DCs all buy
// bits back at the same coded QP.
float step_gain(const LadderStep& st, uint8_t fov_level, const RateConfig& cfg,
                uint8_t& out_res_level, uint8_t extra_res_floor = 0,
                bool force_dc = false) {
    const uint8_t res = std::max({fov_level, st.res_level_abs, extra_res_floor});
    out_res_level = res;
    const float s_step = nxfov::level_scale(res) / nxfov::level_scale(fov_level);
    float g = s_step * s_step;                     // sample count
    if (st.wm_id == WM_PERIPH) g *= cfg.gain_wm_periph;
    if (st.dc_plane || force_dc) g *= cfg.gain_dc_plane;
    return g;
}

// dQ_motion from retinal slip, PAPER.md 5.2: 0 below 10 deg/s, +2 at 30,
// +4 at 60, +6 above 100, linear in between.
float dq_motion(float slip, const float knots[4]) {
    if (slip <= knots[0]) return 0.0f;
    if (slip <= knots[1]) return 2.0f * (slip - knots[0]) / (knots[1] - knots[0]);
    if (slip <= knots[2]) return 2.0f + 2.0f * (slip - knots[1]) / (knots[2] - knots[1]);
    if (slip <= knots[3]) return 4.0f + 2.0f * (slip - knots[2]) / (knots[3] - knots[2]);
    return 6.0f;
}

uint8_t chroma_for(uint8_t res_level, uint8_t cls) {
    if (cls == uint8_t(TileClass::Text)) return CHROMA_444;  // glyph fringes
    switch (res_level) {
        case 0:  return CHROMA_444;
        case 1:  return CHROMA_420;
        default: return CHROMA_410;
    }
}

} // namespace

float ladder_severity(uint8_t cls, int step, uint8_t fov_level,
                      const RateConfig& cfg) {
    uint8_t res = 0;
    const LadderStep& st = ladder_step(cls, step);
    const float g = step_gain(st, fov_level, cfg, res);
    return -6.0f * std::log2(g) + float(st.qp_add);
}

float ladder_severity(const AllocResult& a, const nxfov::FoveationMap& fov,
                      size_t i, const RateConfig& cfg) {
    const float s_fov  = nxfov::level_scale(fov.level[i]);
    const float s_code = nxfov::level_scale(a.res_level[i]);
    float g = (s_code * s_code) / (s_fov * s_fov);
    if (a.wm_id[i] == WM_PERIPH) g *= cfg.gain_wm_periph;
    if (a.dc_plane[i])           g *= cfg.gain_dc_plane;
    return -6.0f * std::log2(g);
}

void AllocResult::resize(size_t n) {
    qp.assign(n, 0);            res_level.assign(n, 0);
    chroma_mode.assign(n, 0);   wm_id.assign(n, WM_LUMA);
    ladder_step.assign(n, 0);   dc_plane.assign(n, 0);
    skip.assign(n, 0);
    predicted_bits.assign(n, 0.0f);
    weight.assign(n, 0.0f);
}

float effective_qp(const AllocResult& a, const nxfov::FoveationMap& fov,
                   size_t i, const RateConfig& cfg) {
    const float s_fov  = nxfov::level_scale(fov.level[i]);
    const float s_code = nxfov::level_scale(a.res_level[i]);
    float g = (s_code * s_code) / (s_fov * s_fov);
    if (a.wm_id[i] == WM_PERIPH) g *= cfg.gain_wm_periph;
    if (a.dc_plane[i])           g *= cfg.gain_dc_plane;
    // The QP the tile would need, at its foveation resolution and with no
    // ladder help, to cost the bits it was given.
    return float(a.qp[i]) - 6.0f * std::log2(g);
}

// ------------------------------------------------------------ controller --

RateController::RateController(RateConfig cfg) : cfg_(cfg) {}

void RateController::reset(size_t n) {
    a_.assign(n, cfg_.a_init);
    w_.assign(n, 0.0f);
    dq_.assign(n, 0.0f);
    bits_.assign(n, 0.0f);
    pinned_.assign(n, 0u);
    res_floor_.assign(n, 0u);
    force_dc_.assign(n, 0u);
    out_.resize(n);
    pressure_ = 0.0f;
    debt_ = 0.0f; debt_frames_ = 0;
    last_actual_ = 0.0f; have_last_ = false;
}

float RateController::effective_budget(float b, bool scene_cut) {
    float eff = b;
    if (scene_cut) {
        // Capped burst instead of an IDR spike; the excess becomes debt that
        // is repaid over the next scene_cut_recovery frames.
        eff = b * cfg_.scene_cut_cap;
        debt_ = (eff - b);
        debt_frames_ = cfg_.scene_cut_recovery;
    } else if (debt_frames_ > 0) {
        const float per = debt_ / float(debt_frames_);
        eff = b - per;
        debt_ -= per;
        --debt_frames_;
        if (debt_frames_ == 0) debt_ = 0.0f;
    }
    return std::max(eff, b * 0.25f);
}

// Per-tile perceptual QP offset and allocation weight.  One pass, no
// reductions except the two means, which the GPU does with a workgroup
// reduction over the tile array.
void RateController::compute_weights(const FrameInputs& in) {
    const size_t n = out_.size();
    const nxfov::FoveationMap& fov = *in.fov;

    // Two frame-level means.
    double sum_lv = 0.0, sum_cplx = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum_lv   += in.stats ? in.stats->log_var[i] : 0.0;
        sum_cplx += in.complexity.empty() ? 1.0 : double(in.complexity[i]);
    }
    const float mean_lv   = n ? float(sum_lv / double(n)) : 0.0f;
    const float mean_cplx = n ? std::max(float(sum_cplx / double(n)), 1e-6f) : 1.0f;

    const float head_norm = in.head_speed_deg_s / cfg_.head_speed_ref;
    const float head_fac  = 1.0f / (1.0f + cfg_.head_speed_k *
                                    std::clamp(head_norm, 0.0f, 4.0f));
    const float head_extra = (in.head_speed_deg_s > cfg_.head_fast_deg_s)
                             ? float(cfg_.dq_head_fast) : 0.0f;

    out_.skipped = 0;
    out_.skipped_temporal = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t cls = in.cls.empty() ? uint8_t(TileClass::Texture) : in.cls[i];
        const uint8_t lvl = fov.level[i];

        // Governor knobs, PAPER.md 4.7.  They act on the foveation classes B
        // (level 1) and C (level 2) only; class A is never touched.  Both the
        // bit share and the cost model see the reduction, so the knob buys
        // decode time at constant quality-per-sample instead of just moving
        // the QP around.
        res_floor_[i] = (lvl == 1) ? knobs_.res_floor_class_b
                      : (lvl == 2) ? knobs_.res_floor_class_c : uint8_t(0);
        force_dc_[i] = (knobs_.class_c_dc_plane && lvl == 2 &&
                        cls != uint8_t(TileClass::Text)) ? 1u : 0u;

        // --- dQ_ecc: one eccentricity control, in the bit domain.
        float dq_ecc;
        if (lvl == 0)
            dq_ecc = (fov.ecc_deg[i] <= cfg_.mid_ring_deg)
                         ? float(cfg_.dq_ecc_fovea) : float(cfg_.dq_ecc_mid);
        else if (lvl == 1) dq_ecc = float(cfg_.dq_ecc_half);
        else               dq_ecc = float(cfg_.dq_ecc_quarter);

        // --- dQ_motion
        const float slip = in.slip_deg_s.empty() ? 0.0f : in.slip_deg_s[i];
        const float dq_mot = dq_motion(slip, cfg_.slip_knots) + head_extra;

        // --- dQ_lum
        const float mean_y = in.stats ? in.stats->mean_luma[i] : 128.0f;
        const float dq_lum = (mean_y < cfg_.lum_dark)   ? float(cfg_.dq_lum_dark)
                           : (mean_y > cfg_.lum_bright) ? float(cfg_.dq_lum_bright)
                                                        : 0.0f;

        // --- dQ_act (x264-style adaptive quantisation).  Text is exempt:
        // contrast masking is what makes a busy tile tolerate a coarse step,
        // and glyphs are the one high-variance content where it does not
        // hold.  Without the exemption a text tile's own sharpness would
        // take bits away from it.  RATECONTROL.md appendix A.2.
        const bool  is_text = (cls == uint8_t(TileClass::Text));
        const float lv = in.stats ? in.stats->log_var[i] : mean_lv;
        const float dq_act = is_text ? 0.0f
            : std::clamp(cfg_.act_strength * (lv - mean_lv),
                         -cfg_.act_clamp, cfg_.act_clamp);

        // --- dQ_class
        const float dq_cls = float(cfg_.dq_class[cls < 4 ? cls : 1]);

        const float dq = dq_ecc + dq_mot + dq_lum + dq_act + dq_cls;
        dq_[i] = dq;

        // --- complexity
        const float cplx_raw = in.complexity.empty() ? mean_cplx : in.complexity[i];
        float cplx = std::clamp(cplx_raw / mean_cplx,
                                cfg_.cplx_clamp_lo, cfg_.cplx_clamp_hi);
        // A text tile never gets less than an average share just because its
        // pose-warped predictor happens to be good this frame.
        if (is_text) cplx = std::max(cplx, cfg_.cplx_text_floor);

        // --- skip decision: a static tile costs one bit in the row bitmap.
        // Two routes in.  The first is the static test that has always been
        // here: the pose warp is already good enough.  The second is the
        // temporal ladder (RATECONTROL.md 8) telling us to withhold this
        // tile's residual this frame even though it has one; the tile is
        // still WARP_SKIP on the wire and the decoder cannot tell the two
        // apart.  Either way the bits go back into the pot through the
        // skip_rounds redistribution below.
        const bool skip_static = (!in.complexity.empty() && cplx_raw < cfg_.skip_sad);
        const bool skip_temporal = (!in.force_warp_skip.empty() &&
                                    in.force_warp_skip[i] != 0);
        const bool skip = skip_static || skip_temporal;
        out_.skip[i] = skip ? 1u : 0u;
        if (skip) {
            ++out_.skipped;
            if (skip_temporal && !skip_static) ++out_.skipped_temporal;
            w_[i] = 0.0f;
            continue;
        }

        // w_t = fov_t * percep_t * cplx_t, with every perceptual term
        // expressed in the bit domain as 2^(-dQ/6), and the *foveation*
        // sample count s_fov^2 (never the ladder's extra reduction, see
        // RATECONTROL.md 4.4).
        const float s = nxfov::level_scale(lvl);
        w_[i] = s * s * exp2f_(-dq / 6.0f) * head_fac * cplx;
        if (res_floor_[i] > lvl) {
            const float s1 = nxfov::level_scale(res_floor_[i]);
            w_[i] *= (s1 * s1) / (s * s);
        }
        if (force_dc_[i]) w_[i] *= cfg_.gain_dc_plane;
    }
}

// One evaluation of the allocation at ladder pressure `p`.  Returns the
// ceiling deficit: the bits that tiles pinned at their class QP ceiling
// overspend relative to what they were allocated.  Zero means no tile has
// to be coded above the QP at which its class starts to block, which is the
// whole point of 4.6.1 - the ladder engages on the "would this tile block?"
// question, not on the frame total, because a frame can be on budget while
// individual periphery tiles are mosaics.  With commit=true the result is
// written into out_.
float RateController::run_pressure(float p, const FrameInputs& in,
                                   float budget, bool commit) {
    const size_t n = out_.size();
    const nxfov::FoveationMap& fov = *in.fov;

    const int   base_step = int(std::floor(p));
    const float frac      = p - float(base_step);

    // Per-tile ladder step: the periphery steps first inside a class, so a
    // fractional pressure engages the outer tiles only.  `ecc_rank` is the
    // tile's eccentricity normalised to the map's maximum; it is a per-tile
    // quantity, no sorting needed.
    float max_ecc = 1e-6f;
    for (size_t i = 0; i < n; ++i) max_ecc = std::max(max_ecc, fov.ecc_deg[i]);

    double sum_w = 0.0;
    for (size_t i = 0; i < n; ++i) sum_w += w_[i];
    if (sum_w <= 0.0) sum_w = 1.0;

    // Pass 1: ladder state and the first bit split.
    for (size_t i = 0; i < n; ++i) {
        if (out_.skip[i]) { bits_[i] = 0.0f; pinned_[i] = 1u; continue; }
        pinned_[i] = 0u;
        bits_[i] = float(budget * (double(w_[i]) / sum_w));
    }

    // Iterative clamp / redistribute.  Each pass moves the bits of the newly
    // pinned tiles onto the free ones AND closes the gap left by rounding
    // the QP to an integer.  The rounding gap is not small: tiles that share
    // a class and a foveation level share a target, so they all round the
    // same way and the bias does not average out over the frame the way it
    // would with per-tile noise.  The correction is one scalar per pass, so
    // it is one more workgroup reduction on the GPU.
    int clamped_hi = 0, clamped_lo = 0;
    float total = 0.0f;
    double ceiling_deficit = 0.0;
    const int kPasses = 6;
    for (int pass = 0; pass < kPasses; ++pass) {
        clamped_hi = clamped_lo = 0;
        ceiling_deficit = 0.0;
        double pinned_bits = 0.0, free_w = 0.0;
        total = 0.0f;

        for (size_t i = 0; i < n; ++i) {
            if (out_.skip[i]) continue;
            const uint8_t cls = in.cls.empty() ? uint8_t(TileClass::Texture) : in.cls[i];
            const int step = std::min(
                base_step + ((frac > 0.0f &&
                              fov.ecc_deg[i] >= (1.0f - frac) * max_ecc) ? 1 : 0),
                kLadderSteps - 1);
            const LadderStep& st = ladder_step(cls, step);
            uint8_t res = 0;
            const float g = step_gain(st, fov.level[i], cfg_, res,
                                      res_floor_[i], force_dc_[i] != 0);

            const float s = nxfov::level_scale(fov.level[i]);
            const float a_eff = a_[i] * s * s * g;

            const int ceil_qp = std::min<int>(cfg_.qp_ceiling[cls < 4 ? cls : 1] +
                                              st.qp_add, cfg_.qp_max);
            const int floor_qp = std::max<int>(cfg_.qp_floor[cls < 4 ? cls : 1],
                                               cfg_.qp_min);

            const float b = std::max(bits_[i], 1.0f);
            const float qpf = 6.0f * std::log2(a_eff / b);
            int qp = int(std::floor(qpf + qp_dither(i)));

            bool at_ceiling = false;
            if (qp > ceil_qp) {
                qp = ceil_qp; ++clamped_hi;
                pinned_[i] = 1u; at_ceiling = true;
            } else if (qp < floor_qp) {
                qp = floor_qp; ++clamped_lo;
                pinned_[i] = 1u;
            } else {
                pinned_[i] = 0u;
            }

            const float pred = a_eff * exp2f_(-float(qp) / 6.0f);
            total += pred;
            if (at_ceiling) ceiling_deficit += double(pred) - double(bits_[i]);

            if (pinned_[i]) pinned_bits += pred;
            else            free_w += pred;      // note: bits, not weight

            if (commit) {
                out_.qp[i]          = uint8_t(std::clamp(qp, 0, 63));
                out_.res_level[i]   = res;
                out_.wm_id[i]       = st.wm_id;
                out_.dc_plane[i]    = uint8_t(st.dc_plane | force_dc_[i]);
                out_.ladder_step[i] = uint8_t(step);
                out_.chroma_mode[i] = chroma_for(res, cls);
                out_.predicted_bits[i] = pred;
                out_.weight[i]      = w_[i];
            }
        }

        if (pass + 1 == kPasses) break;
        const double rest = double(budget) - pinned_bits;
        if (free_w <= 0.0) break;
        // free_w now holds the bits the unpinned tiles actually predict, so
        // this ratio corrects the integer-QP rounding as well as handing on
        // the bits of the tiles that just got pinned.
        const double scale = std::clamp(std::max(rest, 0.0) / free_w, 0.05, 20.0);
        for (size_t i = 0; i < n; ++i)
            if (!out_.skip[i] && !pinned_[i])
                bits_[i] = float(double(bits_[i]) * scale);
    }

    const float deficit = float(std::max(0.0, ceiling_deficit));

    if (commit) {
        out_.predicted_total = total;
        out_.clamped_ceiling = clamped_hi;
        out_.clamped_floor   = clamped_lo;
        out_.pressure        = p;
    }
    return deficit;
}

const AllocResult& RateController::allocate(float frame_budget_bits,
                                            const FrameInputs& in) {
    const size_t n = in.fov ? in.fov->size() : 0;
    if (a_.size() != n) reset(n);

    const bool cut = in.force_scene_cut ||
                     (in.intra_ratio > cfg_.scene_cut_intra_ratio);
    const float budget = effective_budget(frame_budget_bits, cut);

    out_.requested_bits = frame_budget_bits;
    out_.budget_bits    = budget;
    out_.scene_cut      = cut;
    out_.cg_qp_offset   = cfg_.cg_qp_offset;

    compute_weights(in);

    // Pressure search: a fixed grid of quarter steps, smallest P that fits.
    // The predicate is monotone in P and in the budget, which is what makes
    // the allocation monotone (RATECONTROL.md 4.5).  Then the tiles that
    // cannot afford a header become SKIP_WARP and their bits go back into
    // the pot, which is why the whole thing runs `skip_rounds` times.
    const int steps = std::max(cfg_.pressure_steps, 1);
    for (int round = 0; round < std::max(cfg_.skip_rounds, 1); ++round) {
        // Some ceiling clamps cannot be relieved by the ladder at all - a
        // text tile's ladder barely moves, by design - so the target is not
        // zero deficit.  It is "most of the reduction the ladder can buy":
        // the deficit at maximum pressure plus a fraction of the span
        // between no pressure and maximum.  Chasing the last few bits
        // instead would slam the frame to step 4 over one unfixable tile,
        // and step 4 is cheap enough that whole tiles then fall under the
        // one-header floor and drop out.
        const float pmax = float(kLadderSteps - 1);
        const float d_max = run_pressure(pmax, in, budget, false);
        const float d_min = run_pressure(0.0f, in, budget, false);
        const float target = d_max + cfg_.pressure_slack *
                             std::max(0.0f, d_min - d_max) + 1.0f;
        float chosen = pmax;
        for (int k = 0; k <= steps; ++k) {
            const float p = pmax * float(k) / float(steps);
            if (run_pressure(p, in, budget, false) <= target) { chosen = p; break; }
        }
        // Rise immediately, fall slowly.
        if (chosen < pressure_) chosen = std::max(chosen, pressure_ - cfg_.pressure_slew_down);
        pressure_ = chosen;
        run_pressure(chosen, in, budget, true);

        if (round + 1 >= std::max(cfg_.skip_rounds, 1)) break;
        int newly = 0;
        for (size_t i = 0; i < n; ++i) {
            if (out_.skip[i]) continue;
            const uint8_t c = in.cls.empty() ? uint8_t(TileClass::Texture) : in.cls[i];
            if (out_.predicted_bits[i] < cfg_.min_tile_bits &&
                c != uint8_t(TileClass::Text)) {
                out_.skip[i] = 1u; w_[i] = 0.0f;
                out_.predicted_bits[i] = 0.0f;
                ++out_.skipped; ++newly;
            }
        }
        if (!newly) break;
    }

    return out_;
}

// ------------------------------------------------------------ the model ---

void RateController::update_model(std::span<const float> actual) {
    const size_t n = std::min(actual.size(), a_.size());
    double tot = 0.0;
    for (size_t i = 0; i < n; ++i) {
        tot += actual[i];
        if (out_.skip[i]) continue;
        const float pred = out_.predicted_bits[i];
        if (pred < 1.0f || actual[i] < 1.0f) continue;
        const float ratio = std::clamp(actual[i] / pred,
                                       1.0f / cfg_.a_ratio_clamp, cfg_.a_ratio_clamp);
        a_[i] = std::clamp(a_[i] * std::pow(ratio, cfg_.a_exponent),
                           cfg_.a_min, cfg_.a_max);
    }
    last_actual_ = float(tot);
    have_last_ = true;

    // Frame-level overrun: PAPER.md 4.6.  The excess is taken off the next
    // frame's budget through the same debt mechanism as a scene cut.
    if (out_.budget_bits > 0.0f &&
        last_actual_ > cfg_.overrun_trigger * out_.budget_bits) {
        const float excess = last_actual_ - out_.budget_bits;
        debt_ += excess;
        debt_frames_ = std::max(debt_frames_, 1);
    }
}

void RateController::update_model(std::span<const uint32_t> actual) {
    static thread_local std::vector<float> tmp;
    tmp.assign(actual.begin(), actual.end());
    update_model(std::span<const float>(tmp));
}

} // namespace nxrc
