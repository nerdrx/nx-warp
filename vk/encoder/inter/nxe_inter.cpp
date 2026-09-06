/* nxe_inter.cpp -- see nxe_inter.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_inter.h"

#include <cstring>
#include <vector>

#include "nxvc/warp.h"

namespace nxe {

namespace nw = ::nxvw;

void ring_layout(int width, int height, int cw, int ch, int eyes, int nplanes,
                 RingLayout &out) {
    out.nplanes = nplanes;
    nw::nxvw_ring_layout(width, height, cw, ch, eyes, nplanes, out.off,
                         out.stride, out.planeW, &out.slot_u16);
}

/* ref/src/codec_impl.inc, verbatim.  A fixed pseudo-random permutation of the
 * tile index: Knuth's multiplicative hash, shifted so the low bits of the tile
 * index do not survive into the low bits of the result.  It must be the same
 * function on both sides or the two encoders refresh different tiles, so it is
 * copied with its constant rather than reinvented with a better one. */
uint32_t refresh_stagger(uint32_t tile) {
    return (tile * 2654435761u) >> 8;
}

bool refresh_due(uint32_t tile, uint32_t frame, uint32_t period) {
    if (period == 0) return true;
    return ((refresh_stagger(tile) + frame) % period) == 0;
}

/* The conjugated matrix S H S^-1 for a plane subsampled by `sub`, exactly as
 * ref/src/inter.h's plane_homography and the decoder's inter_state.h compute
 * it.  Translation scales by 1/sub, the perspective row by sub; the halving
 * rounds to nearest with ties away from zero, which is symmetric about zero
 * and therefore identical on every implementation. */
static int32_t half_round(int32_t v) {
    return v >= 0 ? (int32_t)((v + 1) >> 1)
                  : (int32_t)(-(int32_t)(((int64_t)(-(int64_t)v) + 1) >> 1));
}

static void plane_matrix(const WarpMatrix &m, int plane_w, int plane_h, int sub,
                         nw::NxvwWarpMat &out) {
    for (int i = 0; i < 9; ++i) out.h[i] = m.h[i];
    if (sub == 2) {
        out.h[2] = half_round(m.h[2]);
        out.h[5] = half_round(m.h[5]);
        out.h[6] = m.h[6] * 2;
        out.h[7] = m.h[7] * 2;
    }
    out.h[8] = nw::kWarpH22;
    out.ox = plane_w / 2;
    out.oy = plane_h / 2;
    out.pad = 0;
}

/* The same conjugation, against a raw nine-int32 matrix, for the ATLAS path's
 * per-tile records.  One function, two callers: a frame matrix and a tile's
 * composed C must be conjugated identically or the chroma planes of the two
 * paths predict from different places. */
void conjugate_plane_matrix(const int32_t h[9], int plane_w, int plane_h,
                            int sub, uint32_t out[NXVW_WARP_MAT_UINTS]) {
    WarpMatrix m;
    for (int i = 0; i < 9; ++i) m.h[i] = h[i];
    nw::NxvwWarpMat mm{};
    plane_matrix(m, plane_w, plane_h, sub, mm);
    for (int i = 0; i < 9; ++i) out[i] = (uint32_t)mm.h[i];
    out[9] = (uint32_t)mm.ox;
    out[10] = (uint32_t)mm.oy;
    out[11] = 0u;
}

nxvw::NxvwWarpPush warp_push(const WarpBuildInfo &bi, const RingLayout &rl) {
    nw::NxvwWarpPush p{};
    p.eyeW = bi.width;
    p.eyeH = bi.height;
    p.chromaW = bi.cw;
    p.chromaH = bi.ch;
    p.eyes = bi.eyes;
    p.colsPerEye = bi.cols_per_eye;
    p.chroma420 = bi.chroma420;
    p.alphaPresent = bi.nplanes > 3 ? 1 : 0;
    /* This pipeline codes YCbCr passthrough and YCoCg-R alike through the
     * ring's u16 samples; the transform flag only decides chroma's maxval and
     * DC offset, and it is the stream's own. */
    p.colorTransform = 0;
    p.chromaQpOff = 0;
    p.alphaQpOff = 0;
    p.wpredStrideI16 = wpred_stride_i16(bi.chroma420, p.alphaPresent);
    p.ringSlotU16 = rl.slot_u16;
    p.tileCount = bi.cols_per_eye * bi.rows * bi.eyes;
    p.eyeFilter = -1;
    p.pad0 = 0;
    return p;
}

void build_warp_params(const WarpBuildInfo &bi, const RingLayout &rl,
                       WarpParams &out) {
    const uint32_t ntiles =
        (uint32_t)(bi.cols_per_eye * bi.rows * bi.eyes);
    out.w.assign(warp_params_uints(ntiles, bi.atlas), 0u);

    /* ---- the four matrix records, indexed (eye * 2 + (sub - 1)) * 12.
     * Both subsamplings of both eyes are always present, even for a mono
     * 4:4:4 stream that will only ever read one of them: the index is
     * computed in the shader from the plane, and a hole there is a wrong
     * matrix rather than a missing one. */
    for (int eye = 0; eye < 2; ++eye) {
        const WarpMatrix &m =
            bi.warp ? bi.warp[eye < bi.eyes ? eye : 0] : WarpMatrix{};
        for (int sub = 1; sub <= 2; ++sub) {
            nw::NxvwWarpMat mm{};
            const int pw = sub == 2 ? bi.cw : bi.width;
            const int ph = sub == 2 ? bi.ch : bi.height;
            plane_matrix(m, pw, ph, sub, mm);
            const size_t base =
                (size_t)((eye * 2 + (sub - 1)) * NXVW_WARP_MAT_UINTS);
            for (int i = 0; i < 9; ++i)
                out.w[base + (size_t)i] = (uint32_t)mm.h[i];
            out.w[base + 9] = (uint32_t)mm.ox;
            out.w[base + 10] = (uint32_t)mm.oy;
            out.w[base + 11] = 0u;
        }
    }

    /* ---- the ring geometry, from NXVW_WARP_HDR_RING. */
    const size_t r = (size_t)NXVW_WARP_HDR_RING;
    out.w[r + 0] = (uint32_t)rl.slot_u16;
    out.w[r + 1] = (uint32_t)bi.eyes;
    out.w[r + 2] = (uint32_t)bi.cols_per_eye;
    out.w[r + 3] = (uint32_t)(bi.frame_number & 3u);
    for (int p = 0; p < 4; ++p) {
        out.w[r + 4 + (size_t)p] = (uint32_t)rl.off[p];
        out.w[r + 8 + (size_t)p] = (uint32_t)rl.stride[p];
        out.w[r + 12 + (size_t)p] = (uint32_t)rl.planeW[p];
    }

    /* ---- the per-tile records.  Geometry now; the mode later.
     *
     * `refBase` is a u16 ELEMENT offset here, not a slot index: the decoder's
     * host multiplies the parsed slot index by ringSlotU16 once at upload and
     * the shader adds a plane offset to it, so the encoder does the same
     * multiplication rather than shifting the meaning of the field.
     * 0xffffffff is "no usable reference", which makes Pass W fill the tile
     * with mid-grey -- a value the decision then rejects, rather than a
     * confident prediction from a slot that does not exist. */
    const uint32_t refbase =
        bi.ref_slot < 0 ? 0xffffffffu
                        : (uint32_t)bi.ref_slot * (uint32_t)rl.slot_u16;
    for (uint32_t t = 0; t < ntiles; ++t) {
        const uint32_t row = t / (uint32_t)(bi.cols_per_eye * bi.eyes);
        const uint32_t rem = t % (uint32_t)(bi.cols_per_eye * bi.eyes);
        const uint32_t eye = rem / (uint32_t)bi.cols_per_eye;
        const uint32_t col = rem % (uint32_t)bi.cols_per_eye;
        const size_t b = out.tile_word(t);
        /* mode INTRA, inter bit clear: the state a tile is in until the
         * decision says otherwise, and the state every tile of a
         * reference-less frame stays in. */
        out.w[b + 0] = (uint32_t)nw::kModeIntra | (eye << 4);
        out.w[b + 1] = col;
        out.w[b + 2] = row;
        out.w[b + 3] = 0u;   /* mv_x, quarter luma samples */
        out.w[b + 4] = 0u;   /* mv_y */
        out.w[b + 5] = 0u;   /* quadrant deltas */
        out.w[b + 6] = refbase;
        out.w[b + 7] = 0u;   /* tile qp, filled by the caller if it differs */
        out.w[b + 8] = 0u;   /* near-skip records, unused here */
        out.w[b + 9] = 0u;
        out.w[b + 10] = 0u;
        /* Word 11 is `mat_idx`, and its zero value is NOT "none": zero is a
         * legal matrix offset -- it is the frame's eye-0 sub-1 record -- so a
         * value-initialised word would silently point every tile at it.  A
         * non-atlas frame must therefore say NONE explicitly, which is what
         * keeps every stream this encoder has ever produced byte-identical
         * across the per-tile-matrix change. */
        out.w[b + NXVW_WARP_TILE_MATIDX] =
            bi.atlas ? warp_atlas_mat_idx(ntiles, t) : NXVW_WARP_MAT_NONE;
    }
}

void set_tile_mode(WarpParams &wp, uint32_t tile, int mode, int mv_x,
                   int mv_y) {
    const size_t b = wp.tile_word(tile);
    const uint32_t eye_bit = wp.w[b + 0] & (1u << 4);
    const uint32_t inter = mode == nw::kModeIntra ? 0u : (1u << 3);
    wp.w[b + 0] = (uint32_t)(mode & 7) | inter | eye_bit;
    wp.w[b + 3] = (uint32_t)mv_x;
    wp.w[b + 4] = (uint32_t)mv_y;
}



bool select_reference(const RingState &ring, const HeldState &held,
                      uint32_t frame_number, int base_ref_sel,
                      int *out_ref_sel, int *out_slot) {
    if (base_ref_sel < 0) base_ref_sel = 0;
    /* [SYN] 4.1: ref_sel is two bits and the value 3 is reserved. */
    if (base_ref_sel > 2) base_ref_sel = 2;
    for (int d = base_ref_sel; d <= 2; ++d) {
        const int slot = ring.resolve(frame_number, d);
        if (slot < 0) continue;
        /* The encoder has the picture; the question is whether the headset
         * does.  Both must be true, and the second is the one a dropped frame
         * makes false. */
        const uint32_t want = frame_number - 1u - (uint32_t)d;
        if (held.confirmation_required() ? !held.confirms(want)
                                          : !held.holds(want))
            continue;
        if (out_ref_sel) *out_ref_sel = d;
        if (out_slot) *out_slot = slot;
        return true;
    }
    return false;
}

// ------------------------------------------------------ snap to identity
// docs/PASSB-ADRENO-PLAN.md 3b: a skipped tile whose warp is the IDENTITY on
// the integer grid is a plain copy, and the decoder's fast path takes it --
// 8.25 of 13.7 ms of Pass B per pair on the Pico is the integer warp on
// WARP_SKIP tiles, and at rest almost all of it buys sub-sample motion nobody
// can see.
//
// This measures how far from the identity the frame's warp actually is, in Q.6
// (1/64 sample), as the LARGEST displacement of any tile corner in the
// picture.  Every tile corner is evaluated rather than only the picture's four:
// the map is projective, so the extreme need not be at a picture corner, and
// 289 corners of host arithmetic once a frame is not worth an approximation
// that could be wrong.
//
// The caller compares it against its threshold and, if it is under, uses the
// identity matrix instead.  That is encoder-side and needs no syntax: an
// identity warp_ext is a legal matrix -- it is what a frame with no reference
// carries -- and every WARP_SKIP tile then derives identity corners, which
// with this encoder's permanently-zero stored vectors (update_pred_state only
// records one for WARP_MV, which this encoder never emits) is exactly the
// decoder's copy predicate.
int32_t warp_max_corner_offset(const WarpMatrix &m, int width, int height,
                               int eyes) {
    ::nxvc::warp::Homography H{};
    for (int i = 0; i < 9; ++i) H.h[i] = m.h[i];
    const int cols = (width + 63) / 64, rows = (height + 63) / 64;
    int32_t worst = 0;
    for (int e = 0; e < eyes; ++e)
        for (int ty = 0; ty < rows; ++ty)
            for (int tx = 0; tx < cols; ++tx) {
                const int32_t px = (int32_t)(e * width) + tx * 64;
                const int32_t py = ty * 64;
                const ::nxvc::warp::TileCorners c = ::nxvc::warp::warp_tile_corners(
                    H, px, py, ::nxvc::warp::kModeWarp);
                // The identity corners of this tile, in Q.6, in the order
                // warp_tile_corners returns them.
                const int32_t ix[4] = {px << 6, (px + 64) << 6, px << 6,
                                       (px + 64) << 6};
                const int32_t iy[4] = {py << 6, py << 6, (py + 64) << 6,
                                       (py + 64) << 6};
                for (int k = 0; k < 4; ++k) {
                    const int32_t dx = c.x[k] - ix[k];
                    const int32_t dy = c.y[k] - iy[k];
                    const int32_t ax = dx < 0 ? -dx : dx;
                    const int32_t ay = dy < 0 ? -dy : dy;
                    if (ax > worst) worst = ax;
                    if (ay > worst) worst = ay;
                }
            }
    return worst;
}

// The DECODER's predicate, exactly (vk/decoder/inter/warp_pred.glsl, the
// NXVW_ABL_IDENTITY block): the copy path claims a tile only when the tile is
// WARP_SKIP and the luma matrix is bit-exactly the identity.  Not "the corners
// come out on the grid" -- the decoder does not evaluate corners to decide, it
// tests the matrix, and a corner test would count tiles the decoder then warps
// anyway.  A predicate measured differently from the one that runs is a
// measurement of the difference.
bool warp_is_identity(const WarpMatrix &m) {
    static const WarpMatrix kIdentity{};
    for (int i = 0; i < 9; ++i)
        if (m.h[i] != kIdentity.h[i]) return false;
    return true;
}

// How many of the picture's tiles map onto their own grid position exactly --
// the decoder's identity predicate, counted rather than assumed.  With a
// snapped (or derived-identity) matrix this is every tile; with a real warp it
// is however many happen to round to the grid, which near the centre of a slow
// rotation is not always zero.
int warp_identity_tiles(const WarpMatrix &m, int width, int height, int eyes,
                        int *total_out) {
    const int cols = (width + 63) / 64, rows = (height + 63) / 64;
    const bool id = warp_is_identity(m);
    int identity = 0, total = 0;
    for (int e = 0; e < eyes; ++e)
        for (int ty = 0; ty < rows; ++ty)
            for (int tx = 0; tx < cols; ++tx) {
                (void)tx;
                identity += id ? 1 : 0;
                ++total;
            }
    if (total_out) *total_out = total;
    return identity;
}

// The same predicate, per tile, in raster order over the picture (eye-minor,
// exactly the tile order the frame uses).  One byte per tile: 1 identity,
// 0 warped.
void warp_identity_tile_map(const WarpMatrix &m, int width, int height,
                            int eyes, std::vector<uint8_t> &out) {
    const int cols = (width + 63) / 64, rows = (height + 63) / 64;
    const uint8_t id = warp_is_identity(m) ? 1u : 0u;
    out.assign((size_t)cols * rows * eyes, id);
}

WarpMatrix derive_warp(const ViewState &vs, int ref_slot, int eye, int width,
                       int height) {
    WarpMatrix m;   /* identity */
    if (!vs.have || ref_slot < 0) return m;
    const View &a = vs.slot[ref_slot & 3][eye];
    const View &b = vs.cur[eye];
    ::nxvc::warp::Homography H{};
    const ::nxvc::warp::Quat qa{a.qx, a.qy, a.qz, a.qw};
    const ::nxvc::warp::Quat qb{b.qx, b.qy, b.qz, b.qw};
    const ::nxvc::warp::Fov fa{a.fov_left, a.fov_right, a.fov_up, a.fov_down};
    const ::nxvc::warp::Fov fb{b.fov_left, b.fov_right, b.fov_up, b.fov_down};
    if (!::nxvc::warp::derive_homography(qa, fa, qb, fb, width, height, &H))
        return m;
    for (int i = 0; i < 9; ++i) m.h[i] = H.h[i];
    m.h[8] = ::nxvw::kWarpH22;
    return m;
}

}  // namespace nxe
