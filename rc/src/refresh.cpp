// The per-tile refresh scheduler: the temporal axis of the degradation
// ladder.  docs/RATECONTROL.md section 8.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxrc/refresh.hpp"

#include <algorithm>
#include <cmath>

namespace nxrc {

// ---------------------------------------------------------------- result --

void RefreshResult::resize(size_t n) {
    divisor.assign(n, 1u);
    force_skip.assign(n, 0u);
    age.assign(n, 0u);
    mandatory.assign(n, 0u);
    visibility.assign(n, 0.0f);
    p_detect.assign(n, 0.0f);
}

// ----------------------------------------------------------------- phase --

uint32_t refresh_phase(size_t tile_id, uint8_t k) {
    if (k <= 1) return 0;
    // van der Corput / bit reversal of the tile id.  The same permutation
    // family as qp_dither (RATECONTROL.md 4.7) and the rolling intra
    // refresh: adjacent tile ids land far apart, so a k-cycle spreads its
    // refreshes over the map instead of sweeping a band across it.
    uint32_t v = uint32_t(tile_id);
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    v = (v >> 16) | (v << 16);
    // Scale, do NOT take a remainder.  The bit-reversed id of a tile array
    // of at most 2^m entries only ever has its top m bits set, so `v % k`
    // is identically zero for every power-of-two k and badly biased for the
    // rest - the whole array would refresh on the same frame.  Reading the
    // reversal as the van der Corput fraction v / 2^32 in [0, 1) and taking
    // floor(k * v / 2^32) instead is both the textbook use of the sequence
    // and exactly equidistributed over a power-of-two tile count.
    return uint32_t((uint64_t(v) * uint64_t(k)) >> 32);
}

// -------------------------------------------------------------- scheduler --

RefreshScheduler::RefreshScheduler(RefreshConfig cfg) : cfg_(cfg) {}

void RefreshScheduler::reset(size_t n) {
    out_.resize(n);
    age_.assign(n, 0u);
    gate_ = cfg_.gate_lo;
    cost_ = 0.0f;
    have_cost_ = false;
}

void RefreshScheduler::update_cost(float total_bits, int coded_tiles) {
    if (coded_tiles <= 0) return;
    const float c = total_bits / float(coded_tiles);
    cost_ = have_cost_ ? (cost_ + cfg_.cost_ema * (c - cost_)) : c;
    have_cost_ = true;
}

tvm::TileTemporal RefreshScheduler::tile_model(const RefreshInputs& in,
                                               size_t i) const {
    tvm::TileTemporal t;
    const nxfov::FoveationMap& fov = *in.fov;
    t.ecc_deg      = fov.ecc_deg[i];
    t.residual_mad = in.complexity.empty() ? 0.0f : in.complexity[i];

    if (in.stats) {
        t.mean_luma = in.stats->mean_luma[i];
        const float g = tile_gradient_energy(in.stats->jxx[i], in.stats->jyy[i],
                                             4096);
        t.freq_ratio = tile_frequency_ratio(g, in.stats->log_var[i]);
    }

    // ppd_render(theta) = ppd_center / cos^2(theta); the map already carries
    // the off-axis angle of the tile centre (RATECONTROL.md 6.1).
    const float th = fov.theta_deg[i] * 3.14159265358979f / 180.0f;
    const float c  = std::cos(th);
    t.ppd_render = in.ppd_center / std::max(c * c, 1e-3f);
    return t;
}

uint8_t RefreshScheduler::admissible_divisor(const RefreshInputs& in, size_t i,
                                             float gate) const {
    const nxfov::FoveationMap& fov = *in.fov;
    const uint8_t cls = in.cls.empty() ? uint8_t(TileClass::Texture) : in.cls[i];
    const float   mad = in.complexity.empty() ? 0.0f : in.complexity[i];
    const bool    is_static = !in.complexity.empty() && mad < cfg_.static_mad;

    // The k_max bound caps the whole ladder, always.
    uint8_t cap = std::max<uint8_t>(cfg_.k_max_frames, 1u);

    // Foveal floor.  Not a model output: a policy, so that no budget and no
    // model re-fit can ever put the fovea below full rate.
    //
    // It is tested BEFORE the static shortcut below, and the order matters.
    // `is_static` is a statement about the tile's complexity input, which on
    // a real encoder is measured on the previous frame
    // (nxvc_tile_info::warp_mad_q8, docs/RATECONTROL.md 4.1): a tile that was
    // static last frame and starts moving this one would otherwise be handed
    // k = k_max and have a residual it really does have withheld, in the
    // fovea, on the frame the motion starts.  Taking the floor first costs
    // nothing when the tile is genuinely static -- the encoder's own mode
    // search skips it anyway, for free -- and removes the one case where a
    // stale input can reach the middle of the picture.
    if (fov.ecc_deg[i] <= cfg_.fovea_full_deg) return 1u;

    // A static tile is WARP_SKIP in the allocator anyway; there is no
    // residual to withhold, so nothing the model can object to.  It goes
    // straight to the cap, text included.
    if (is_static) return cap;

    cap = std::min<uint8_t>(cap, cfg_.max_k[cls < 4 ? cls : 1]);
    if (cap <= 1) return 1u;

    const tvm::TileTemporal t = tile_model(in, i);

    // Largest ladder entry at or below the cap whose predicted visibility is
    // at or below the gate.  The ladder is ascending and tile_visibility is
    // monotone non-decreasing in k, so this is a scan with an early out and
    // needs no sort.
    uint8_t best = 1u;
    for (int s = 0; s < cfg_.ladder_len; ++s) {
        const uint8_t k = cfg_.ladder[s];
        if (k > cap) break;
        if (k <= 1) { best = k; continue; }
        if (tvm::tile_visibility(t, int(k), cfg_.fps, cfg_.model) <= gate)
            best = k;
        else
            break;
    }
    return best;
}

float RefreshScheduler::duty_cycle_at(const RefreshInputs& in,
                                      float gate) const {
    const size_t n = out_.size();
    if (!n) return 1.0f;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i)
        s += 1.0 / double(admissible_divisor(in, i, gate));
    return float(s / double(n));
}

float RefreshScheduler::search_gate(const RefreshInputs& in, float target) {
    // duty_cycle_at is monotone non-increasing in the gate, so this is the
    // same shape of search as the spatial ladder's pressure: a fixed grid,
    // smallest value that meets the target, no data-dependent loop.
    const int steps = std::max(cfg_.gate_steps, 2);
    const float lo = std::max(cfg_.gate_lo, 1e-4f);
    const float hi = std::max(cfg_.gate_hi, lo * 2.0f);
    if (duty_cycle_at(in, lo) <= target) return lo;
    for (int s = 1; s <= steps; ++s) {
        const float f = float(s) / float(steps);
        const float g = lo * std::pow(hi / lo, f);
        if (duty_cycle_at(in, g) <= target) return g;
    }
    return hi;
}

const RefreshResult& RefreshScheduler::schedule(const RefreshInputs& in) {
    const nxfov::FoveationMap& fov = *in.fov;
    const size_t n = fov.size();
    if (out_.size() != n) reset(n);

    // ---- 1. the frame's gate --------------------------------------------
    float want;
    if (cfg_.target_coded_fraction > 0.0f) {
        want = search_gate(in, std::clamp(cfg_.target_coded_fraction, 0.0f, 1.0f));
    } else if (cfg_.target_bits > 0.0f && have_cost_ && cost_ > 0.0f && n) {
        const float frac = cfg_.target_bits / (cost_ * float(n));
        want = search_gate(in, std::clamp(frac, 0.0f, 1.0f));
    } else {
        // Pressure-coupled default: the temporal ladder engages only once
        // the spatial ladder has spent its free steps (RATECONTROL.md 8.4).
        const float span = std::max(cfg_.max_pressure - cfg_.engage_pressure, 1e-3f);
        const float u = std::clamp((in.spatial_pressure - cfg_.engage_pressure) / span,
                                   0.0f, 1.0f);
        want = cfg_.gate_lo * std::pow(cfg_.gate_hi / cfg_.gate_lo, u);
    }

    // A scene cut codes everything, so the gate is irrelevant this frame but
    // must not be allowed to ratchet down through it.
    if (in.scene_cut) want = std::min(want, gate_);

    // Tighten immediately, relax slowly.
    gate_ = (want >= gate_) ? want : std::max(want, gate_ / cfg_.gate_slew_up);
    gate_ = std::clamp(gate_, cfg_.gate_lo, cfg_.gate_hi);

    // ---- 2. per tile ----------------------------------------------------
    out_.coded = out_.skipped = out_.forced = out_.age_forced = 0;
    double dsum = 0.0, vsum = 0.0;
    long   vn = 0;
    out_.max_visibility = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        const uint8_t k = admissible_divisor(in, i, gate_);
        out_.divisor[i] = k;
        dsum += 1.0 / double(k);

        // Predicted visibility of the decision actually taken.
        const float cm = (k > 1)
            ? tvm::tile_visibility(tile_model(in, i), int(k), cfg_.fps, cfg_.model)
            : 0.0f;
        out_.visibility[i] = cm;
        out_.p_detect[i]   = tvm::detect_prob(cm, cfg_.model);
        if (k > 1) { vsum += cm; ++vn; out_.max_visibility = std::max(out_.max_visibility, cm); }

        // ---- the four things that override the model ---------------------
        const bool intra_due = !in.intra_due.empty() && in.intra_due[i];
        const bool ref_bad   = !in.ref_ineligible.empty() && in.ref_ineligible[i];
        // The drift bound: at most k_max_frames - 1 consecutive skips, so
        // the gap between two coded frames never exceeds k_max_frames.
        const bool age_hit   = age_[i] + 1u >= std::max<uint8_t>(cfg_.k_max_frames, 1u);
        const bool must      = in.scene_cut || intra_due || ref_bad;

        bool coded;
        if (must) {
            coded = true;
        } else if (age_hit) {
            coded = true;
            if (k > 1) ++out_.age_forced;
        } else if (k <= 1) {
            coded = true;
        } else {
            // Phase-dispersed cadence: tile i of divisor k refreshes on the
            // frames congruent to its own phase, not on a common one.
            const uint32_t ph = refresh_phase(i, k);
            coded = ((uint32_t(in.frame_index) + ph) % uint32_t(k)) == 0u;
        }

        out_.mandatory[i]  = must ? 1u : 0u;
        out_.force_skip[i] = coded ? 0u : 1u;
        if (must) ++out_.forced;
        if (coded) { ++out_.coded; age_[i] = 0u; }
        else       { ++out_.skipped; age_[i] = uint8_t(age_[i] + 1u); }
        out_.age[i] = age_[i];
    }

    out_.gate            = gate_;
    out_.duty_cycle      = n ? float(dsum / double(n)) : 1.0f;
    out_.coded_fraction  = n ? float(out_.coded) / float(n) : 1.0f;
    out_.mean_visibility = vn ? float(vsum / double(vn)) : 0.0f;
    return out_;
}

} // namespace nxrc
