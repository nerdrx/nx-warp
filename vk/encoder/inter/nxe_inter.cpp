/* nxe_inter.cpp -- see nxe_inter.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_inter.h"

#include <cstring>

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
    out.w.assign((size_t)NXVW_WARP_HDR_UINTS +
                     (size_t)ntiles * NXVW_WARP_TILE_UINTS,
                 0u);

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
        out.w[b + 11] = 0u;
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
