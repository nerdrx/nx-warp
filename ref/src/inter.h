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
#include <algorithm>
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

// ------------------------------------------------------------------ atlas
// SYNTAX.md 13.12.  The ATLAS reference: per tile position, the pixels of the
// most recent frame that CODED that tile, plus the composed warp back to that
// frame's pose.  A skipped tile writes nothing, which is the whole point --
// the 34 us normative warp of a WARP_SKIP tile stops existing.
//
// The pixels are a RefPicture, unchanged and at full tile extent, so the
// layout never depends on a per-tile choice and a display pass samples it
// directly (13.12.1).
struct AtlasEntry {
    i32 C[9] = {1 << kQNum, 0, 0, 0, 1 << kQNum, 0, 0, 0, kH22};
    u32 src_frame = 0;
    u16 gen = 0;
    u8 flags = 0;       // bit 0 valid, bit 1 static; bits 2-7 reserved zero
    u8 res_level = 0;   // advisory; the pixels are always full extent
    u8 reserved[20] = {};
};
static_assert(sizeof(AtlasEntry) == 64,
              "13.12.1 fixes the atlas entry at 64 bytes");

enum : u8 { kAtlasValid = 1u, kAtlasStatic = 2u };

struct Atlas {
    RefPicture pix;               // 13.12.1 atlas pixels
    std::vector<AtlasEntry> ent;  // 13.12.1 per-tile table, tile order D-3
    // 13.12.3: a tile predicts from the atlas as it stood at the START of the
    // frame.  Prediction reaches outside the tile's own position, so without
    // this a coded tile would read whatever a neighbour coded EARLIER IN THE
    // SAME FRAME had already written, and the result would depend on decode
    // order -- which would forbid applying tiles as they arrive, the property
    // the whole latency argument rests on (ADR-0029 cheat 1).
    //
    // The CPU reference keeps a whole second copy because it is simple and it
    // is not the implementation anyone ships.  A GPU decoder needs far less:
    // only the pre-frame pixels of the tiles CODED this frame can ever be read
    // stale, so a scratch of ~39 tiles an eye suffices, not a second atlas.
    RefPicture prev;
    // The per-tile TABLE as it stood at the start of the frame, for exactly
    // the same reason `prev` exists.  The co-located rule of 13.12.4 never
    // reads a neighbour's entry, so the table needed no snapshot; the
    // neighbour-aware gather of tool bit 33 does, and a neighbour coded
    // earlier in the same frame has already been reset to identity.  Without
    // this the prediction would depend on decode order.
    std::vector<AtlasEntry> ent_prev;
    void snapshot() { prev = pix; ent_prev = ent; }
    void reset() {
        for (auto &e : ent) e = AtlasEntry{};
        ent_prev = ent;
        for (auto &p : pix.plane) std::fill(p.begin(), p.end(), (u16)0);
        prev = pix;
    }
};

// The identity matrix a freshly coded tile takes (13.12.3 step 3).
inline void atlas_identity(i32 C[9]) {
    const i32 I[9] = {1 << kQNum, 0, 0, 0, 1 << kQNum, 0, 0, 0, kH22};
    for (int k = 0; k < 9; ++k) C[k] = I[k];
}

// 13.12.2: round-to-nearest, ties away from zero, in 64-bit integers.
// `b` is nonzero for any matrix that satisfies 3.1.1 condition 3.
inline i64 sdiv_round(i64 a, i64 b) {
    const int sign = ((a < 0) != (b < 0)) ? -1 : 1;
    const u64 ua = a < 0 ? (u64)(-(a + 1)) + 1u : (u64)a;
    const u64 ub = b < 0 ? (u64)(-(b + 1)) + 1u : (u64)b;
    return sign * (i64)((ua * 2u + ub) / (ub * 2u));
}

// 3.1.1 conditions 1-3, on LUMA dimensions.  A composed matrix is required to
// satisfy exactly the same envelope as a transmitted one, which is what lets
// warp_plane_tile() consume it unmodified (ADR-0029).
inline bool warp_legal(const i32 h[9], int luma_w, int luma_h) {
    if (h[8] != kH22) return false;
    for (int k = 0; k < 9; ++k)
        if (h[k] < -nw::kEntryMax || h[k] > nw::kEntryMax) return false;
    const int ox = luma_w >> 1, oy = luma_h >> 1;
    const int cx[2] = {-ox, luma_w - ox}, cy[2] = {-oy, luma_h - oy};
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            const i64 den =
                (i64)h[6] * cx[a] + (i64)h[7] * cy[b] + (i64)h[8];
            if (den < ((i64)1 << 28) || den >= ((i64)1 << 30)) return false;
        }
    return true;
}

// 13.12.2: one composition step, out = renorm(C . H).
//
// The wire scales differ per row (rows 0-1 Q10.21, row 2 Q2.29), so each entry
// is accumulated as TWO independently rounded partial sums.  That is not
// laziness about the rounding: it is what keeps every intermediate inside
// int64 without 128-bit arithmetic, which a GPU implementation of this would
// otherwise need.  Folding both terms into one scale costs an 8-bit shift on
// the linear term and overflows on the translation column.
//
// Returns false when the product leaves the envelope -- which is the staleness
// bound, for free: an entry that cannot compose is invalidated, and an invalid
// entry cannot be skipped or inter-predicted, so the encoder must code it.
inline bool compose_warp(const i32 C[9], const i32 H[9], int luma_w,
                         int luma_h, i32 out[9]) {
    i64 P[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const i64 t_lin = (i64)C[i * 3 + 0] * (i64)H[0 * 3 + j] +
                              (i64)C[i * 3 + 1] * (i64)H[1 * 3 + j];
            const i64 t_per = (i64)C[i * 3 + 2] * (i64)H[2 * 3 + j];
            P[i * 3 + j] =
                ((t_lin + ((i64)1 << 20)) >> 21) +
                ((t_per + ((i64)1 << 28)) >> 29);
        }
    // Guard the renormalising shift.  A legal result is bounded by kEntryMax;
    // anything an order beyond that is already out of the envelope, and this
    // is what keeps `P[k] << 29` inside int64.
    for (int k = 0; k < 9; ++k)
        if (P[k] >= ((i64)1 << 33) || P[k] <= -((i64)1 << 33)) return false;
    if (P[8] == 0) return false;
    i32 r[9];
    for (int k = 0; k < 9; ++k) {
        const i64 v = sdiv_round(P[k] << 29, P[8]);
        if (v > nw::kEntryMax || v < -nw::kEntryMax) return false;
        r[k] = (i32)v;
    }
    r[8] = kH22;   // exact by construction; stated so it cannot drift
    if (!warp_legal(r, luma_w, luma_h)) return false;
    for (int k = 0; k < 9; ++k) out[k] = r[k];
    return true;
}

// 13.12.3 step 1, for one tile position.  `gen` advances for every valid
// entry; `C` advances only for a non-static one, which is what lets a
// STATIC_MV tile (a WayVR panel) be SKIPPED and cost the client nothing.
inline void atlas_advance(AtlasEntry &e, const WarpMatrix &H, int luma_w,
                          int luma_h, u32 gen_max) {
    if (!(e.flags & kAtlasValid)) return;
    if (e.gen != 0xffffu) ++e.gen;
    if (gen_max && e.gen > gen_max) { e.flags = 0; return; }
    if (e.flags & kAtlasStatic) return;
    i32 out[9];
    if (!compose_warp(e.C, H.h, luma_w, luma_h, out)) { e.flags = 0; return; }
    for (int k = 0; k < 9; ++k) e.C[k] = out[k];
}

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
