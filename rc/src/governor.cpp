// SPDX-License-Identifier: Apache-2.0
//
// The decode-time governor of PAPER.md 4.7.  Five ordered knobs, 3-frame
// step down, 2 s step up.  The governor never touches bits, only
// pixels-of-work; the bits it frees are redistributed by the allocator.

#include "nxrc/nxrc.hpp"

#include <algorithm>
#include <cmath>

namespace nxrc {

void Governor::reset() {
    state_ = KnobState{};
    state_.refresh_hz = cfg_.high_refresh_hz;
    over_ = under_ = 0;
}

void Governor::apply_level() {
    const int L = state_.knob_level;
    state_.drop_enh_class_c   = L >= 1;
    state_.class_c_base_only  = L >= 2;
    state_.fovea_region_scale = (L >= 3) ? cfg_.fovea_shrink : 1.0f;
    state_.drop_enh_class_b   = L >= 4;
    state_.refresh_hz = (L >= 5) ? cfg_.low_refresh_hz : cfg_.high_refresh_hz;

    // Single-layer mapping, RATECONTROL.md 5.3.  Knob 1 and 2 act on the
    // foveation class C (level 2) tiles, knob 4 on class B (level 1).
    state_.res_floor_class_c = (L >= 1) ? 2u : 0u;
    state_.class_c_dc_plane  = L >= 2;
    state_.res_floor_class_b = (L >= 4) ? 2u : 0u;
}

const KnobState& Governor::update(float decode_us, float frame_period_us,
                                  bool deadline_miss) {
    if (state_.refresh_hz == 0.0f) reset();

    const float target = cfg_.target_fraction * frame_period_us;
    const float ratio  = (target > 0.0f) ? decode_us / target : 0.0f;

    bool changed = false;

    if (ratio >= cfg_.panic_ratio) {
        // One frame at 150% of target is enough.
        if (state_.knob_level < cfg_.max_knob) { ++state_.knob_level; changed = true; }
        over_ = 0; under_ = 0;
    } else if (ratio > cfg_.step_down_ratio) {
        ++over_;
        under_ = 0;
        if (over_ >= cfg_.step_down_frames) {
            if (state_.knob_level < cfg_.max_knob) { ++state_.knob_level; changed = true; }
            over_ = 0;
        }
    } else if (ratio < cfg_.step_up_ratio && !deadline_miss) {
        over_ = 0;
        ++under_;
        if (under_ >= cfg_.step_up_frames) {
            if (state_.knob_level > 0) { --state_.knob_level; changed = true; }
            under_ = 0;
        }
    } else {
        // In the dead band, or a deadline miss: neither counter advances,
        // and a miss cancels any progress towards a step up.
        over_ = 0;
        if (deadline_miss) under_ = 0;
    }

    if (changed) { over_ = 0; under_ = 0; }
    apply_level();
    return state_;
}

} // namespace nxrc
