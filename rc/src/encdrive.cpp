// nxrc::EncDriver - see nxrc/encdrive.hpp.
//
// One note that governs every number in this file, because it is the thing
// most easily got wrong.
//
// The foveation ladder asks "is the detail in this tile above what the eye
// can resolve there?".  That is a question about the *display*: it compares
// the eye's acuity at the tile's eccentricity against the angular pixel
// density of the panel the stream is decoded onto.  It is NOT a question
// about the test clip.  A 1024-px-per-eye clip across the Pico 4's 81.2
// degrees is 10.4 pixels per degree, which is below foveal acuity everywhere,
// so a ladder driven by the clip's own density would answer "never subsample"
// at every eccentricity and the whole mechanism would measure nothing.
//
// So the lens handed to nxfov::foveation_map has the PANEL's pixel count
// (EncDriveConfig::panel_px_per_eye, 2160 for the Pico 4) and a tile grid of
// the same shape as the clip's, i.e. one map texel per codec tile.  The
// eccentricity of a tile is then exactly the eccentricity of the same tile on
// the headset, and so is the resolution decision.  What the clip's own
// density does decide is the conversion of motion vectors to degrees per
// second (`ppd_clip_`), because a motion vector is measured in the clip's
// samples.
//
// ref/RESULTS-percept.md restates this where the numbers are, and the
// foveated metrics are run at the same FOV so the eccentricity the metric
// charges is the eccentricity the ladder assumed.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxrc/encdrive.hpp"

#include <algorithm>
#include <cmath>

namespace nxrc {

namespace {

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;

int tiles_across(int px, int tile) { return (px + tile - 1) / tile; }

// Bytes of tile header a coded tile pays on top of its payload
// (SYNTAX.md 5: 8 bytes, plus 2 for a transmitted vector).
constexpr uint32_t kTileHeaderBytes = 8;

// Angular half-extent of one eye as a tangent.
float half_tan(float fov_deg) {
    return std::tan(std::clamp(fov_deg, 1.0f, 179.0f) * 0.5f * kDeg2Rad);
}

} // namespace

EncDriver::EncDriver(const EncDriveConfig& cfg) : cfg_(cfg) {
    tiles_x_ = tiles_across(cfg_.width, cfg_.tile_size);
    tiles_y_ = tiles_across(cfg_.height, cfg_.tile_size);
    const size_t n = size_t(tiles_x_) * size_t(tiles_y_) * size_t(cfg_.eyes);

    build_foveation();

    rc_.config().act_strength = cfg_.act_strength;
    rc_.reset(n);
    refresh_.reset(n);
    refresh_.config().fps = cfg_.fps;

    qp_map_.assign(n, 0);
    res_map_.assign(n, 0);
    wm_map_.assign(n, 0);
    skip_map_.assign(n, 0);
    cls_.assign(n, uint8_t(TileClass::Texture));
    cplx_.assign(n, 0.0f);
    slip_.assign(n, 0.0f);
    actual_bits_.assign(n, 0);
    stats_all_.resize(n);

    // Pixels per degree of the CLIP's own samples on axis; the unit motion
    // vectors are measured in.
    ppd_clip_ = (float(cfg_.width) * 0.5f / half_tan(cfg_.fov_h_deg)) / kDeg2Rad;
}

void EncDriver::build_foveation() {
    // One map texel per codec tile, at the panel's angular density.
    const int t = std::max(1, (cfg_.panel_px_per_eye + tiles_x_ / 2) / std::max(tiles_x_, 1));
    lens_ = nxfov::pico4_eye();
    lens_.tile_size = t;
    lens_.width_px  = tiles_x_ * t;
    lens_.height_px = tiles_y_ * t;
    const float tx = half_tan(cfg_.fov_h_deg), ty = half_tan(cfg_.fov_v_deg);
    lens_.tan_left = -tx; lens_.tan_right = tx;
    lens_.tan_down = -ty; lens_.tan_up    = ty;

    fov_cfg_.mid_ring_deg = rc_.config().mid_ring_deg;

    nxfov::Gaze gaze{};
    gaze.valid = cfg_.gaze_valid;
    gaze.tan_x = cfg_.gaze_x * tx;
    gaze.tan_y = cfg_.gaze_y * ty;
    gaze.latency_ms = cfg_.gaze_latency_ms;
    const nxfov::FoveationMap eye =
        nxfov::foveation_map(lens_, fov_cfg_, cfg_.gaze_valid ? &gaze : nullptr);

    // Replicate into the encoder's tile order, Annex D D-3.
    const size_t n = size_t(tiles_x_) * size_t(tiles_y_) * size_t(cfg_.eyes);
    fov_.tiles_x = tiles_x_ * cfg_.eyes;
    fov_.tiles_y = tiles_y_;
    fov_.r8.resize(n); fov_.level.resize(n);
    fov_.ecc_deg.resize(n); fov_.ecc_raw.resize(n);
    fov_.theta_deg.resize(n); fov_.ratio.resize(n); fov_.weight.resize(n);
    for (int row = 0; row < tiles_y_; ++row)
        for (int e = 0; e < cfg_.eyes; ++e)
            for (int col = 0; col < tiles_x_; ++col) {
                const size_t dst = size_t(row) * cfg_.eyes * tiles_x_ +
                                   size_t(e) * tiles_x_ + col;
                const size_t src = size_t(row) * tiles_x_ + col;
                // --rc-fov off: every tile is foveal.  Level 0 and zero
                // eccentricity switch off the resolution ladder, dQ_ecc and
                // the temporal fovea floor together, which is what "no
                // foveation" has to mean; the rest of the allocator (class,
                // activity, luminance, motion) is untouched.
                const bool on = cfg_.foveation;
                fov_.level[dst]     = on ? eye.level[src] : uint8_t(0);
                fov_.r8[dst]        = nxfov::r8_from_level(fov_.level[dst]);
                fov_.ecc_deg[dst]   = on ? eye.ecc_deg[src] : 0.0f;
                fov_.ecc_raw[dst]   = eye.ecc_raw[src];
                fov_.theta_deg[dst] = eye.theta_deg[src];
                fov_.ratio[dst]     = on ? eye.ratio[src] : 1.0f;
                fov_.weight[dst]    = on ? eye.weight[src] : 1.0f;
            }
}

void EncDriver::tile_stats(const uint8_t* luma, int stride) {
    for (int e = 0; e < cfg_.eyes; ++e) {
        compute_tile_stats(luma + size_t(e) * cfg_.width, stride, cfg_.width,
                           cfg_.height, cfg_.tile_size, stats_eye_);
        for (int row = 0; row < tiles_y_; ++row)
            for (int col = 0; col < tiles_x_; ++col) {
                const size_t src = size_t(row) * tiles_x_ + col;
                const size_t dst = size_t(row) * cfg_.eyes * tiles_x_ +
                                   size_t(e) * tiles_x_ + col;
                stats_all_.mean_luma[dst] = stats_eye_.mean_luma[src];
                stats_all_.log_var[dst]   = stats_eye_.log_var[src];
                stats_all_.jxx[dst]       = stats_eye_.jxx[src];
                stats_all_.jxy[dst]       = stats_eye_.jxy[src];
                stats_all_.jyy[dst]       = stats_eye_.jyy[src];
            }
    }
}

void EncDriver::analyse(const uint8_t* luma, int stride, int frame_index) {
    const size_t n = qp_map_.size();

    tile_stats(luma, stride);
    classify_tiles(stats_all_, ClassifyConfig{}, std::span<const uint8_t>(prev_cls_),
                   cls_);
    prev_cls_ = cls_;

    // ---- complexity.  The encoder's own measurement of the warped residual
    // (nxvc_tile_info::warp_mad_q8) is the right input and is what feedback()
    // installs; it is one frame old, which is exactly the lag a real encoder's
    // analysis pass has.  Before there is one -- the first frame, and any tile
    // that had no reference -- the source-domain frame difference stands in.
    const int tile = cfg_.tile_size;
    if (have_prev_luma_) {
        for (int row = 0; row < tiles_y_; ++row)
            for (int e = 0; e < cfg_.eyes; ++e)
                for (int col = 0; col < tiles_x_; ++col) {
                    const size_t i = size_t(row) * cfg_.eyes * tiles_x_ +
                                     size_t(e) * tiles_x_ + col;
                    if (have_warp_mad_ && cplx_[i] >= 0.0f) continue;
                    const int x0 = e * cfg_.width + col * tile;
                    const int y0 = row * tile;
                    const int w = std::min(tile, cfg_.width - col * tile);
                    const int h = std::min(tile, cfg_.height - row * tile);
                    const int pw = cfg_.width * cfg_.eyes;
                    uint64_t acc = 0;
                    for (int y = 0; y < h; ++y) {
                        const uint8_t* a = luma + size_t(y0 + y) * stride + x0;
                        const uint8_t* b = prev_luma_.data() +
                                           size_t(y0 + y) * pw + x0;
                        for (int x = 0; x < w; ++x)
                            acc += uint64_t(std::abs(int(a[x]) - int(b[x])));
                    }
                    cplx_[i] = float(acc) / float(std::max(1, w * h));
                }
    }

    for (float& c : cplx_) c = std::max(0.0f, c);

    // Keep this frame's luma for the next difference.
    {
        const int pw = cfg_.width * cfg_.eyes;
        prev_luma_.resize(size_t(pw) * cfg_.height);
        for (int y = 0; y < cfg_.height; ++y)
            std::copy_n(luma + size_t(y) * stride, pw,
                        prev_luma_.data() + size_t(y) * pw);
        have_prev_luma_ = true;
    }

    const bool have_cplx = frame_index > 0;

    // ---- the temporal ladder.
    std::fill(skip_map_.begin(), skip_map_.end(), uint8_t(0));
    if (cfg_.temporal && frame_index > 0) {
        RefreshInputs ri;
        ri.fov         = &fov_;
        ri.cls         = cls_;
        ri.complexity  = have_cplx ? std::span<const float>(cplx_)
                                   : std::span<const float>();
        ri.stats       = &stats_all_;
        ri.frame_index = frame_index;
        ri.spatial_pressure = rc_.last().pressure;
        ri.ppd_center  = nxfov::ppd_center(lens_);
        const RefreshResult& r = refresh_.schedule(ri);
        for (size_t i = 0; i < n; ++i) skip_map_[i] = r.force_skip[i];
        stats_.gate = r.gate;
        stats_.coded_fraction = r.coded_fraction;
    } else {
        stats_.gate = 0.0f;
        stats_.coded_fraction = 1.0f;
    }

    // ---- the spatial ladder and the allocation.
    FrameInputs fi;
    fi.fov        = &fov_;
    fi.cls        = cls_;
    fi.complexity = have_cplx ? std::span<const float>(cplx_)
                              : std::span<const float>();
    fi.slip_deg_s = have_cplx ? std::span<const float>(slip_)
                              : std::span<const float>();
    fi.stats      = &stats_all_;
    fi.force_warp_skip = cfg_.temporal ? std::span<const uint8_t>(skip_map_)
                                       : std::span<const uint8_t>();
    fi.head_speed_deg_s = head_speed_;
    fi.force_scene_cut  = (frame_index == 0);

    const float budget =
        cfg_.bitrate_mbps * 1.0e6f / std::max(1.0f, cfg_.fps) *
        (1.0f - std::clamp(cfg_.overhead_fraction, 0.0f, 0.5f));
    const AllocResult& a = rc_.allocate(budget, fi);

    for (size_t i = 0; i < n; ++i) {
        qp_map_[i]  = uint8_t(std::min<int>(63, a.qp[i]));
        res_map_[i] = a.res_level[i];
        wm_map_[i]  = uint8_t(std::min<int>(3, a.wm_id[i]));
    }

    stats_.budget_bits    = a.budget_bits;
    stats_.predicted_bits = a.predicted_total;
    stats_.pressure       = a.pressure;
    stats_.head_speed_deg_s = head_speed_;
    stats_.forced_skips = 0;
    for (size_t i = 0; i < n; ++i) stats_.forced_skips += skip_map_[i] ? 1 : 0;
    for (int c = 0; c < 4; ++c) stats_.tiles_class[c] = 0;
    for (size_t i = 0; i < n; ++i)
        if (cls_[i] < 4) ++stats_.tiles_class[cls_[i]];

    // Mark the complexity array stale: feedback() refills it from the
    // encoder's measurement, and analyse() refills whatever is left over from
    // the source difference.
    have_warp_mad_ = false;
}

void EncDriver::feedback(const nxvc_tile_info* tiles, uint32_t count) {
    const size_t n = std::min<size_t>(count, qp_map_.size());
    double total = 0.0;
    int skipped = 0;
    for (size_t i = 0; i < n; ++i) {
        const nxvc_tile_info& t = tiles[i];
        uint32_t bytes = t.payload_len;
        if (!t.skipped) bytes += kTileHeaderBytes + (t.mv_present ? 2u : 0u);
        else ++skipped;
        actual_bits_[i] = bytes * 8u;
        total += actual_bits_[i];

        // The encoder measured the warped residual for every tile that had a
        // reference; that is the next frame's complexity.  A tile it could
        // not measure is marked -1 and analyse() fills it from the source
        // difference instead.
        cplx_[i] = t.warp_mad_q8 == NXVC_WARP_MAD_UNMEASURED
                       ? -1.0f
                       : float(t.warp_mad_q8) / 256.0f;

        // Residual motion after the pose warp, in degrees per second: the
        // tile's transmitted vector, which is quarter samples of the clip.
        const float mv = std::hypot(float(t.mv_x), float(t.mv_y)) * 0.25f;
        slip_[i] = t.mv_present ? mv / std::max(1.0f, ppd_clip_) * cfg_.fps : 0.0f;
    }
    have_warp_mad_ = true;
    rc_.update_model(std::span<const uint32_t>(actual_bits_.data(), n));
    if (cfg_.temporal)
        refresh_.update_cost(float(total), int(n) - skipped);
    stats_.actual_bits = float(total);
    stats_.actual_skips = skipped;
}

} // namespace nxrc
