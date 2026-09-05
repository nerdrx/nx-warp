// NX Warp decoder, inter path: the host state the predictor needs and the
// parameter block it is driven from.
//
// Three objects live here, and they are three because the reference says they
// are three (docs/SYNTAX.md 13.5, "a running history, which is what makes it
// a different object from the transport's four-byte per-slot receiver
// record"):
//
//   * `PredState`  -- six bytes per tile position per eye, the running
//                     `last_mv` / `last_disp` history;
//   * `RingState`  -- which reference-ring slot holds which frame number, so
//                     `ref_sel` can be resolved and a stream that names a
//                     slot it never wrote is refused;
//   * `MissingMap` -- the tiles the client did not receive, which is
//                     `nxvc_vk_decoder_mark_missing()`'s half of
//                     docs/TRANSPORT.md 8.
//
// Vulkan-free, exactly like nxvc_vkdec_parse.{h,cpp}: this is host arithmetic
// and it is unit-testable without a device.
//
// NORMATIVE SOURCE: docs/SYNTAX.md 13.2, 13.5, 13.6 and Annex D D-9/D-10, and
// ref/src/inter.h (`RefRing`, `PredState`, `update_pred_state`,
// `plane_homography`, `half_round`) plus ref/src/codec_impl.inc
// (`nxvc_decoder_decode_frame`'s `ref_for` and `tile_lost`,
// `nxvc_decoder_set_lost_tiles`).  Where this file and ref/ disagree, ref/
// wins.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVC_VKDEC_INTER_STATE_H
#define NXVC_VKDEC_INTER_STATE_H

#include <cstdint>
#include <vector>

#include "inter_layout.h"

namespace nxvcvk {

// docs/SYNTAX.md 4.1 mode values, spelled here so this header needs neither
// <nxvc/nxvc.h> nor passB/syntax_constants.h.
enum : uint8_t {
    kModeWarpSkip = 0,
    kModeStaticMv = 1,
    kModeWarpMv = 2,
    kModeIntra = 3,
    kModeStereo = 4
};

inline bool mode_needs_warp(int mode) {
    return mode == kModeWarpSkip || mode == kModeWarpMv;
}

// --------------------------------------------------- per-tile prediction state
// [SYN] 13.5 / Annex D D-9: six bytes per tile position per eye.
struct PredState {
    int16_t last_mv_x = 0;
    int16_t last_mv_y = 0;
    uint16_t last_disp = 0;
};
static_assert(sizeof(PredState) == 6, "D-9 fixes the prediction state at 6 bytes");

// [REF] ref/src/inter.h update_pred_state(), applied AFTER a tile is
// reconstructed.  STATIC_MV does not update `last_mv` because its vector
// displaces an *unwarped* reference while WARP_SKIP and concealment apply the
// stored vector *after* the warp; storing it would conceal from the wrong
// place.  INTRA clears the state, because after a refresh there is no motion
// history and a stale vector is worse than zero.
inline void update_pred_state(PredState &st, int mode, int mv_x, int mv_y,
                              int disparity) {
    switch (mode) {
        case kModeWarpMv:
            st.last_mv_x = (int16_t)mv_x;
            st.last_mv_y = (int16_t)mv_y;
            break;
        case kModeIntra:
            st.last_mv_x = 0;
            st.last_mv_y = 0;
            st.last_disp = 0;
            break;
        case kModeStereo:
            st.last_disp = (uint16_t)disparity;
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------- warp_ext()
// The nine quantised coefficients of one eye, exactly as they travel.
// [REF] ref/src/inter.h WarpMatrix.
struct WarpMatrix {
    int32_t h[9] = {1 << 21, 0, 0, 0, 1 << 21, 0, 0, 0, 1 << 29};
};

// [REF] ref/src/inter.h half_round(): to nearest, ties away from zero, so it
// is symmetric about zero and identical on every implementation.
inline int32_t half_round(int32_t v) {
    return v >= 0 ? (int32_t)((v + 1) >> 1)
                  : (int32_t)(-(int32_t)(((int64_t)(-(int64_t)v) + 1) >> 1));
}

// [SYN] 13.3 step 1 / [REF] ref/src/inter.h plane_homography(): the frame's
// matrix conjugated by the plane's subsampling factor, S H S^-1 with
// S = diag(1/sub, 1/sub, 1).  Only sub 1 and 2 exist in version 1.
struct PlaneMatrix {
    int32_t h[9] = {};
    int32_t ox = 0, oy = 0;
};

inline PlaneMatrix plane_homography(const WarpMatrix &m, int plane_w,
                                    int plane_h, int sub) {
    PlaneMatrix H{};
    for (int i = 0; i < 9; ++i) H.h[i] = m.h[i];
    if (sub == 2) {
        H.h[2] = half_round(m.h[2]);
        H.h[5] = half_round(m.h[5]);
        H.h[6] = m.h[6] * 2;
        H.h[7] = m.h[7] * 2;
    }
    H.h[8] = nxvw::kWarpH22;
    H.ox = plane_w / 2;
    H.oy = plane_h / 2;
    return H;
}

// ------------------------------------------------------------ the ring
// [SYN] 13.2.  Four slots, addressed by `frame_number mod 4`.  Only the
// bookkeeping lives here; the samples are a device buffer the two kernels
// share, laid out by nxvw::nxvw_ring_layout().
struct RingState {
    uint8_t valid[4] = {};
    uint16_t frame_number[4] = {};

    void reset() {
        for (int i = 0; i < 4; ++i) {
            valid[i] = 0;
            frame_number[i] = 0;
        }
    }
    // The slot `ref_sel` names, or -1 when this decoder does not hold it.
    // [REF] codec_impl.inc `ref_for`: the slot is
    // (frame_number - 1 - ref_sel) mod 4 and its STORED frame number must be
    // frame_number - 1 - ref_sel; if it is not, the stream is malformed.
    int resolve(uint32_t frame_number_now, int ref_sel) const {
        const int s = (int)((frame_number_now - 1u - (uint32_t)ref_sel) & 3u);
        if (!valid[s]) return -1;
        const uint16_t want = (uint16_t)(frame_number_now - 1u - (uint32_t)ref_sel);
        if (frame_number[s] != want) return -1;
        return s;
    }
};

// ------------------------------------------------------- the decoder's state
struct InterCtx {
    std::vector<PredState> state;   // one per tile, cleared on tile_map_reset
    std::vector<uint8_t> missing;   // one per tile, consumed by the next frame
    RingState ring;
    bool have_missing = false;

    void resize(uint32_t ntiles) {
        state.assign(ntiles, PredState{});
        missing.assign(ntiles, 0);
        have_missing = false;
        ring.reset();
    }
    bool is_missing(uint32_t t) const {
        return have_missing && t < missing.size() && missing[t] != 0;
    }
    // A frame consumes the map, exactly as ref/'s decoder does: the caller
    // marks the tiles of ONE frame and the next call starts clean.
    void consume_missing() {
        if (!have_missing) return;
        for (auto &m : missing) m = 0;
        have_missing = false;
    }
};

}  // namespace nxvcvk

#endif  // NXVC_VKDEC_INTER_STATE_H
