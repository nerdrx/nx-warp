// nxrc::EncDriver - the wire between the rate-control library and the
// reference encoder.
//
// `nxrc` (classification, the spatial degradation ladder, the temporal
// refresh scheduler, the governor) and `nxfov` (the foveation map) are a
// model of a decision the encoder has never actually made: until now nothing
// fed their per-tile output into `nxvc_encoder_encode_frame`.  This class is
// that feed, and nothing more.  It owns no policy of its own; every number it
// produces comes out of `nxrc::RateController` or `nxrc::RefreshScheduler`.
//
// What it does per frame:
//
//   1. tile statistics from the luma plane        (nxrc::compute_tile_stats)
//   2. classification, with hysteresis            (nxrc::classify_tiles)
//   3. the foveation map for the headset          (nxfov::foveation_map)
//   4. the temporal ladder                        (nxrc::RefreshScheduler)
//   5. the spatial ladder and the bit allocation  (nxrc::RateController)
//   6. -> qp_map / res_map / wm_map / skip_map, the four per-tile arrays the
//        C ABI already accepts.  No syntax changes; every one of those four
//        addresses a field the v1 bitstream has carried since v1.2.
//
// and, after the frame is encoded, feeds the real per-tile bit counts back
// into the bit model and the real warped residual back in as the next
// frame's `complexity`.
//
// Tile order is the encoder's, Annex D D-3: `row * eyes * tiles_x + eye *
// tiles_x + col`.  The foveation map is per eye and is replicated across the
// eyes into that order, so one RateController allocates the whole frame's
// budget across both eyes at once rather than splitting it in half up front.
//
// Specification: docs/RATECONTROL.md (all of it), PAPER.md 4.6, 4.6.1, 5.1,
// 5.2.  Results: ref/RESULTS-percept.md.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef NXRC_ENCDRIVE_HPP
#define NXRC_ENCDRIVE_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "nxfov/foveation.hpp"
#include "nxrc/nxrc.hpp"
#include "nxrc/refresh.hpp"
#include "nxvc/nxvc.h"

namespace nxrc {

struct EncDriveConfig {
    // ---- geometry, as the encoder sees it --------------------------------
    int width     = 0;    // luma samples per eye
    int height    = 0;
    int eyes      = 1;
    int tile_size = NXVC_TILE_SIZE;

    // ---- rate ------------------------------------------------------------
    float fps          = 90.0f;
    float bitrate_mbps = 40.0f;
    // Transport overhead the frame budget does not get to spend: FEC, RTP
    // framing and the stream/frame headers.  PAPER.md 4.6's B is what is left
    // after these.
    float overhead_fraction = 0.06f;

    // ---- which halves of the ladder are engaged --------------------------
    bool foveation = true;   // --rc-fov:      the eccentricity map at all
    bool temporal  = true;   // --rc-temporal: the refresh scheduler

    // ---- gaze ------------------------------------------------------------
    // Normalised to [-1, 1] across one eye's render FOV, +x right, +y up.
    // Invalid means fixed foveation on the lens axis, which is the Pico 4
    // case: it has no eye tracker, so the map is the elliptical eye box of
    // FoveationConfig (PAPER.md 5.1.3).
    bool  gaze_valid      = false;
    float gaze_x          = 0.0f;
    float gaze_y          = 0.0f;
    float gaze_latency_ms = 40.0f;

    // ---- the headset -----------------------------------------------------
    // Half-FOV of one eye, degrees, full angle.  The Pico 4's render FOV at
    // WiVRn's default scale is +/-40.6 deg (nxfov::pico4_eye).
    float fov_h_deg = 81.2f;
    float fov_v_deg = 81.2f;
    // Pixels per eye of the PANEL the foveation decision is made for.  The
    // test sequences are lower-resolution proxies for that panel: what a tile
    // subtends is fixed by the FOV, but whether the eye could resolve detail
    // inside it is a property of the display the stream is going to, not of
    // the clip.  See the note at the top of encdrive.cpp.
    int panel_px_per_eye = 2160;
};

// Per-frame summary, for the results harness and the tests.
struct DriveStats {
    float budget_bits     = 0.0f;
    float predicted_bits  = 0.0f;
    float actual_bits     = 0.0f;
    float pressure        = 0.0f;
    float gate            = 0.0f;
    float coded_fraction  = 1.0f;
    float head_speed_deg_s = 0.0f;
    int   forced_skips    = 0;   // tiles the temporal ladder asked to skip
    int   actual_skips    = 0;   // tiles the encoder actually skipped
    int   tiles_class[4]  = {0, 0, 0, 0};
};

class EncDriver {
public:
    explicit EncDriver(const EncDriveConfig& cfg);

    // Head angular rate for the frame about to be analysed, degrees per
    // second.  Feeds dQ_motion and the perceptual budget scale (PAPER.md 5.2).
    void set_head_speed(float deg_s) { head_speed_ = deg_s; }

    // One frame.  `luma` is the frame's luma plane, `eyes * width` samples
    // wide, `stride` bytes per row.  After this the four maps are valid.
    void analyse(const uint8_t* luma, int stride, int frame_index);

    const std::vector<uint8_t>& qp_map()   const { return qp_map_; }
    const std::vector<uint8_t>& res_map()  const { return res_map_; }
    const std::vector<uint8_t>& wm_map()   const { return wm_map_; }
    const std::vector<uint8_t>& skip_map() const { return skip_map_; }

    // Call after nxvc_encoder_encode_frame with nxvc_encoder_tiles().  Feeds
    // the measured bits into the bit model, the measured warped residual into
    // the next frame's complexity, and the coded-tile count into the
    // scheduler's cost estimate.
    void feedback(const nxvc_tile_info* tiles, uint32_t count);

    // ---- read-back --------------------------------------------------------
    const nxfov::FoveationMap& fov()      const { return fov_; }
    const AllocResult&         alloc()    const { return rc_.last(); }
    const RefreshResult&       refresh()  const { return refresh_.last(); }
    std::span<const uint8_t>   classes()  const { return cls_; }
    std::span<const float>     complexity() const { return cplx_; }
    const DriveStats&          stats()    const { return stats_; }
    size_t                     tile_count() const { return qp_map_.size(); }
    int tiles_x() const { return tiles_x_; }   // per eye
    int tiles_y() const { return tiles_y_; }

    // The base QP the encoder must be created with for `qp_map` to be
    // representable: a tile header carries qp_delta in [-32, 31].
    static constexpr uint32_t kBaseQp = 32;
    // The frame weighting matrix rc mode requires.  A tile's `wm_id == 0`
    // means "the frame's matrix" (SYNTAX.md 6.5), so the frame matrix has to
    // be the flat one for nxrc::WM_FLAT..WM_CHROMA to address the four
    // built-in matrices 0..3 directly.
    static constexpr uint32_t kFrameMatrix = 0;

    RateConfig&    rate_config()    { return rc_.config(); }
    RefreshConfig& refresh_config() { return refresh_.config(); }
    const RefreshConfig& refresh_config() const { return refresh_.config(); }

private:
    void build_foveation();
    void tile_stats(const uint8_t* luma, int stride);

    EncDriveConfig cfg_;
    int tiles_x_ = 0, tiles_y_ = 0;

    nxfov::FoveationMap fov_;      // frame order, eyes replicated
    nxfov::LensParams   lens_{};
    nxfov::FoveationConfig fov_cfg_{};

    RateController   rc_;
    RefreshScheduler refresh_;

    TileStats stats_all_, stats_eye_;
    std::vector<uint8_t> cls_, prev_cls_;
    std::vector<float>   cplx_, slip_;
    std::vector<uint8_t> qp_map_, res_map_, wm_map_, skip_map_;
    std::vector<uint32_t> actual_bits_;

    std::vector<uint8_t> prev_luma_;   // previous frame, tightly packed
    bool have_prev_luma_ = false;
    bool have_warp_mad_  = false;

    float head_speed_ = 0.0f;
    float ppd_clip_   = 0.0f;   // ppd of the CLIP's own pixels, for slip
    DriveStats stats_{};
};

} // namespace nxrc

#endif // NXRC_ENCDRIVE_HPP
