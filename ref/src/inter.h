// nxvc_ref: the Phase 2 inter path -- reference ring, per-tile prediction
// state, and the glue to the normative warp library.
//
// Everything normative here is either an integer expression written out in
// full or a call into `nxvc_warp_ref` (warp/), which owns the bit-exact
// predictor.  This file never reimplements warp arithmetic; where the library
// does not offer something the gap is named in a comment rather than papered
// over (see plane_homography and warp_plane_tile below).
//
// See docs/SYNTAX.md 8 for the normative prose and
// spec/annex-d-inter-decisions.md for the decisions behind it.
#pragma once
#include <cstdint>
#include <vector>

#include "common.h"
#include "nxvc/warp.h"

namespace nxvc {

namespace nw = ::nxvc::warp;

// ---------------------------------------------------------------- warp_ext
// The nine quantised coefficients of one eye, exactly as they travel.
struct WarpMatrix {
    i32 h[9] = {1 << 21, 0, 0, 0, 1 << 21, 0, 0, 0, 1 << 29};
};

constexpr i32 kQNum = nw::kQNum;   // 21, rows 0 and 1
constexpr i32 kQDen = nw::kQDen;   // 29, row 2
constexpr i32 kH22 = 1 << kQDen;   // 0x20000000

// The reference ring: four slots, addressed by `frame_number mod 4`
// (Annex D D-10).  A slot holds the whole reconstructed picture of every eye
// in the CODED sample domain -- Y/Co/Cg, not the RGB the output image carries
// when the colour transform is on -- because that is the domain the predictor
// predicts in.  Samples are u16 so a 9-bit YCoCg-R chroma plane fits and so
// the buffer is directly a `nw::RefImage`.
struct RefPicture {
    bool valid = false;
    u32 frame_number = 0;
    // plane[p] is `eyes * eye_width` wide and `height` tall.
    std::vector<u16> plane[4];
    int w[4] = {}, h[4] = {}, maxval[4] = {};
};

struct RefRing {
    RefPicture slot[4];
    void reset() {
        for (auto &s : slot) {
            s.valid = false;
            for (auto &p : s.plane) p.clear();
        }
    }
};

// ------------------------------------------------- per-tile prediction state
// Annex D D-9: six bytes per tile position per eye.  It is a running history,
// not a property of a stored frame, which is what makes it a different object
// from the transport's four-byte receiver record.
struct PredState {
    i16 last_mv_x = 0;
    i16 last_mv_y = 0;
    u16 last_disp = 0;
};
static_assert(sizeof(PredState) == 6, "D-9 fixes the prediction state at 6 bytes");

// Update rules of Annex D D-9, applied after a tile is reconstructed.
inline void update_pred_state(PredState &st, int mode, int mv_x, int mv_y,
                              int disparity) {
    switch (mode) {
        case NXVC_MODE_WARP_MV:
            st.last_mv_x = (i16)mv_x;
            st.last_mv_y = (i16)mv_y;
            break;
        case NXVC_MODE_INTRA:
            st.last_mv_x = 0;
            st.last_mv_y = 0;
            st.last_disp = 0;
            break;
        case NXVC_MODE_STEREO:
            st.last_disp = (u16)disparity;
            break;
        // WARP_SKIP consumed last_mv and leaves it; STATIC_MV displaces an
        // unwarped reference and must not be stored, because WARP_SKIP and
        // concealment apply the stored vector *after* the warp.
        default:
            break;
    }
}

// ------------------------------------------------------------ plane matrices
// The homography of `warp_ext()` is stated on luma sample indices centred at
// (width/2, height/2).  A plane subsampled by `sub` in both axes uses the
// conjugated matrix S H S^-1 with S = diag(1/sub, 1/sub, 1):
//
//     h02, h12   scale by 1/sub        (translation is in samples)
//     h20, h21   scale by sub          (the perspective row is per sample)
//
// Only sub == 1 and sub == 2 exist in version 1.  The halving rounds to
// nearest, ties away from zero, so it is symmetric about zero and identical on
// every implementation; the doubling is exact and stays inside kEntryMax
// because a legal h20/h21 is of order 2^15.
inline i32 half_round(i32 v) {
    return v >= 0 ? (v + 1) >> 1 : -((-(i64)v + 1) >> 1);
}

inline nw::Homography plane_homography(const WarpMatrix &m, int eye_w, int eye_h,
                                       int sub) {
    nw::Homography H{};
    for (int i = 0; i < 9; ++i) H.h[i] = m.h[i];
    if (sub == 2) {
        H.h[2] = half_round(m.h[2]);
        H.h[5] = half_round(m.h[5]);
        H.h[6] = m.h[6] * 2;
        H.h[7] = m.h[7] * 2;
    }
    H.h[8] = kH22;
    H.ox = eye_w / 2;
    H.oy = eye_h / 2;
    return H;
}

// A motion vector is coded in quarter LUMA samples.  In a plane subsampled by
// `sub` the same displacement is `mv / sub` quarter plane-samples; the shift
// is arithmetic (floor), which is one definition rather than two.
inline i32 plane_mv(i32 mv_qpel, int sub) {
    return sub == 2 ? (mv_qpel >> 1) : mv_qpel;
}

// ------------------------------------------------------------- the predictor
// `out` is `extent * extent` samples, row stride `extent`.
//
// LIMITATION, stated rather than hidden: nxvc_warp_ref::warp_tile produces a
// fixed 64x64 block (warp.h kTile), so a plane whose per-tile extent is 32 --
// 4:2:0 chroma -- is predicted by asking for the 64x64 block at the same
// origin and keeping its top-left 32x32.  The samples are the library's, bit
// for bit; what differs from a hypothetical 32x32 kernel is only the span the
// in-tile corner interpolation is fitted over.  Both sides of the codec do the
// same thing, so it is exact, and warp/ was not modified to add an extent
// parameter.  A GPU Pass B doing chroma at 32x32 natively must be given the
// same corner basis (see docs/SYNTAX.md 8.4).
void warp_plane_tile(const nw::RefImage &ref, int tile_x, int tile_y,
                     const nw::Homography &H, const i32 mv[2], nw::Mode mode,
                     int extent, i32 *out);

// The same predictor with one vector per tile quadrant (tool bit 29 QUAD_MV,
// SYNTAX.md 13.10).  warp_plane_tile() is this with four equal vectors.
void warp_plane_tile_quad(const nw::RefImage &ref, int tile_x, int tile_y,
                          const nw::Homography &H, const i32 mv[4][2],
                          nw::Mode mode, int extent, i32 *out);

}  // namespace nxvc
