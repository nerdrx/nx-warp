/* nxe_atlas.cpp -- see nxe_atlas.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_atlas.h"

#include "nxe_inter.h"

#include <cmath>
#include <cstring>

namespace nxe {

namespace nw = ::nxvw;

void atlas_identity(int32_t C[9]) {
    C[0] = 1 << nw::kWarpQNum;  C[1] = 0;                   C[2] = 0;
    C[3] = 0;                   C[4] = 1 << nw::kWarpQNum;  C[5] = 0;
    C[6] = 0;                   C[7] = 0;                   C[8] = nw::kWarpH22;
}

/* [SYN] 13.12.2.  The two partial sums are what keep every intermediate inside
 * int64 without 128-bit arithmetic: the linear term is exact at Q42 (rows 0-1)
 * or Q50 (row 2) and the perspective term at Q50 or Q58, and each is rounded
 * to the row's own scale BEFORE they are added.  The added constants are the
 * round-to-nearest terms of each scale and the shifts are arithmetic, so the
 * rounding is toward positive infinity on a tie -- which is what the reference
 * does and what makes the two bit-identical.  It is deliberately NOT the
 * half-away-from-zero rule sdiv_round uses; the two rules live in the same
 * clause because they are different operations. */
void atlas_compose_partial(const int32_t C[9], const int32_t H[9],
                           int64_t P[9]) {
    const int64_t kRn = (int64_t)1 << (nw::kWarpQNum - 1);   /* 1 << 20 */
    const int64_t kRd = (int64_t)1 << (nw::kWarpQDen - 1);   /* 1 << 28 */
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const int64_t t_lin = (int64_t)C[i * 3 + 0] * (int64_t)H[0 * 3 + j] +
                                  (int64_t)C[i * 3 + 1] * (int64_t)H[1 * 3 + j];
            const int64_t t_per = (int64_t)C[i * 3 + 2] * (int64_t)H[2 * 3 + j];
            P[i * 3 + j] = ((t_lin + kRn) >> nw::kWarpQNum) +
                           ((t_per + kRd) >> nw::kWarpQDen);
        }
}

int64_t atlas_sdiv_round(int64_t a, int64_t b) {
    /* sign(a/b) * ((|a| * 2 + |b|) / (|b| * 2)), the division truncating
     * toward zero, so the result is round-to-nearest with ties away from zero.
     * Formed in 128 bits because |a| can be 2^60 inside the envelope and
     * |a| * 2 would then be within one bit of int64's range; the value the
     * expression denotes is unchanged. */
    const bool neg = (a < 0) != (b < 0);
    const unsigned __int128 ua =
        a < 0 ? (unsigned __int128)(-(unsigned __int128)a) : (unsigned __int128)a;
    const unsigned __int128 ub =
        b < 0 ? (unsigned __int128)(-(unsigned __int128)b) : (unsigned __int128)b;
    const unsigned __int128 q = (ua * 2u + ub) / (ub * 2u);
    return neg ? -(int64_t)q : (int64_t)q;
}

bool atlas_renorm(const int64_t P[9], int32_t out[9]) {
    if (P[8] == 0) return false;
    /* [SYN] 13.12.2, "Width of P[k] << 29".  The NORMATIVE arithmetic is
     * int64, made safe by this guard rather than by a wider type: if any
     * |P[k]| is at least 2^33 the composition FAILS and the entry is
     * invalidated, before the shift is evaluated.
     *
     * This implementation shifts in 128 bits, and the clause permits that --
     * but only WITH the same guard.  Without it a 128-bit implementation would
     * accept a composition an int64 one rejects, which is a conformance
     * difference and not an optimisation.  So the guard is applied first and
     * the wider intermediate is then merely a way of writing the same values.
     *
     * It is not a threshold to be tuned: a legal composed matrix is bounded by
     * kEntryMax (2^30), so anything at 2^33 is eight times outside the
     * envelope and the check that follows would reject it anyway. */
    const int64_t kGuard = (int64_t)1 << 33;
    for (int k = 0; k < 9; ++k) {
        const int64_t v = P[k];
        const int64_t a = v < 0 ? -v : v;
        if (a >= kGuard) return false;
    }
    for (int k = 0; k < 9; ++k) {
        /* `P[k] << 29` in 128 bits.  Inside the envelope the product is at
         * most about 2^60 and int64 holds it; the wider intermediate is here
         * so that a composition the envelope check is about to reject is
         * defined rather than undefined.  Every value int64 can represent is
         * represented identically. */
        const __int128 num = (__int128)P[k] << nw::kWarpQDen;
        int64_t v;
        {
            const bool neg = (num < 0) != (P[8] < 0);
            const unsigned __int128 ua = num < 0
                                             ? (unsigned __int128)(-num)
                                             : (unsigned __int128)num;
            const unsigned __int128 ub =
                P[8] < 0 ? (unsigned __int128)(-(__int128)P[8])
                         : (unsigned __int128)P[8];
            const unsigned __int128 q = (ua * 2u + ub) / (ub * 2u);
            /* A quotient that does not fit i32 is a matrix outside the
             * envelope -- reachable inside the 2^33 guard when P[2][2] is
             * small -- so it is saturated to a defined value and
             * atlas_envelope_ok() then rejects it. */
            const unsigned __int128 cap = (unsigned __int128)1 << 62;
            const int64_t s = q >= cap ? (int64_t)((uint64_t)1 << 62)
                                       : (int64_t)(uint64_t)q;
            v = neg ? -s : s;
        }
        out[k] = (int32_t)(v > (int64_t)INT32_MAX
                               ? INT32_MAX
                               : (v < (int64_t)INT32_MIN ? INT32_MIN : v));
    }
    /* The renormalisation's whole purpose: h22 is back on the normalised value
     * the wire format fixes, so warp_plane_tile() consumes the composed matrix
     * exactly as it consumes a transmitted one. */
    out[8] = nw::kWarpH22;
    return true;
}

bool atlas_compose(const int32_t C[9], const int32_t H[9], int32_t out[9]) {
    int64_t P[9];
    atlas_compose_partial(C, H, P);
    return atlas_renorm(P, out);
}

bool atlas_envelope_ok(const int32_t C[9], int width, int height) {
    /* Condition 1 is a post-condition of the renormalisation, not a test. */
    if (C[8] != nw::kWarpH22) return false;
    /* Condition 2: every entry in [-2^30, 2^30]. */
    for (int k = 0; k < 9; ++k)
        if (C[k] < -nw::kWarpEntryMax || C[k] > nw::kWarpEntryMax) return false;
    /* Condition 3: at each of the four picture corners the denominator,
     * accumulated in 64 bits, fits int32 and lies in [2^28, 2^30).  `den` is
     * affine in (cx, cy), so the four corners bound the whole picture. */
    const int ox = width >> 1, oy = height >> 1;
    const int xs[2] = {-ox, width - ox};
    const int ys[2] = {-oy, height - oy};
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            const int64_t den = (int64_t)C[6] * (int64_t)xs[a] +
                                (int64_t)C[7] * (int64_t)ys[b] + (int64_t)C[8];
            if (den < (int64_t)INT32_MIN || den > (int64_t)INT32_MAX)
                return false;
            if (den < (int64_t)nw::kWarpDenMin || den >= (int64_t)nw::kWarpDenMax)
                return false;
        }
    return true;
}

/* ---------------------------------------------------------------- the table */

void AtlasTable::reset(const AtlasGeom &geom) {
    g = geom;
    /* [SYN] 13.12.1: on tile_map_reset the whole table is zeroed, which makes
     * every entry invalid, and the atlas pixels are undefined until written.
     * Zero is therefore the correct reset value and not merely a convenient
     * one: `flags` zero is `valid == 0`. */
    e.assign(g.ntiles(), AtlasEntry{});
}

void AtlasTable::advance(const int32_t H[2][9]) {
    for (uint32_t t = 0; t < e.size(); ++t) {
        AtlasEntry &a = e[t];
        if (!(a.flags & kAtlasValid)) continue;   /* not advanced */
        /* `gen` increments for every valid entry, static or not: it is the
         * cadence clock and it counts frames since the source, not
         * compositions. */
        if (a.gen != 0xffffu) ++a.gen;
        if (!(a.flags & kAtlasStatic)) {
            const int eye = g.eye_of(t);
            int32_t out[9];
            if (!atlas_compose(a.C, H[eye & 1], out)) {
                a.flags &= (uint8_t)~kAtlasValid;
                continue;
            }
            std::memcpy(a.C, out, sizeof out);
            if (!atlas_envelope_ok(a.C, g.width, g.height)) {
                a.flags &= (uint8_t)~kAtlasValid;
                continue;
            }
        }
        if (gen_max != 0 && (uint32_t)a.gen > gen_max)
            a.flags &= (uint8_t)~kAtlasValid;
    }
}

void AtlasTable::code_tile(uint32_t t, uint32_t frame_number, int mode,
                           int res_level) {
    AtlasEntry &a = e[t];
    atlas_identity(a.C);
    a.src_frame = frame_number;
    a.gen = 0;
    a.flags = (uint8_t)(kAtlasValid |
                        (mode == nw::kModeStaticMv ? kAtlasStatic : 0u));
    a.res_level = (uint8_t)res_level;
    std::memset(a.reserved, 0, sizeof a.reserved);
}

/* ----------------------------------------------------------- the undo log */

void AtlasUndo::reset(const AtlasGeom &geom) {
    g = geom;
    s.assign(geom.ntiles(), Snap{});
    for (int i = 0; i < kDepth; ++i) step[i] = Step{};
}

void AtlasUndo::note_frame(uint32_t frame_number, const int32_t H[2][9],
                           bool advanced) {
    Step &st = step[frame_number % (uint32_t)kDepth];
    st = Step{};
    st.frame = frame_number;
    st.used = 1;
    st.advanced = advanced ? 1u : 0u;
    if (H) std::memcpy(st.H, H, sizeof st.H);
}

void AtlasUndo::note_coded(uint32_t t, uint32_t frame_number,
                           const AtlasEntry &before) {
    if (t >= s.size()) return;
    s[t].before = before;
    s[t].frame = frame_number;
    s[t].used = 1;
}

bool AtlasUndo::rollback(uint32_t t, uint32_t lost_frame, uint32_t now,
                         AtlasEntry &out) const {
    if (!holds(t, lost_frame)) return false;
    /* The snapshot is the state at frame `lost_frame`, after that frame's own
     * advance and before its write-back.  Replay the advances of the frames
     * strictly after it, which is bit-identical to having advanced the
     * snapshot alongside the live entry every frame -- the advance is a fixed
     * function of the entry and the frame's matrix -- and costs one
     * composition per frame per NAMED tile rather than per frame per tile. */
    AtlasEntry a = s[t].before;
    const int eye = g.eye_of(t);
    for (uint32_t f = lost_frame + 1u; f <= now; ++f) {
        if (!(a.flags & kAtlasValid)) break;
        const Step &st = step[f % (uint32_t)kDepth];
        /* A frame outside the window is a frame whose matrix is gone, and
         * replaying without it would produce a matrix neither side holds.
         * Refusing is the safe direction: the caller invalidates instead and
         * the tile is coded INTRA. */
        if (!st.used || st.frame != f) return false;
        if (!st.advanced) continue;
        if (a.gen != 0xffffu) ++a.gen;
        if (!(a.flags & kAtlasStatic)) {
            int32_t o[9];
            if (!atlas_compose(a.C, st.H[eye & 1], o)) {
                a.flags &= (uint8_t)~kAtlasValid;
                break;
            }
            std::memcpy(a.C, o, sizeof o);
            if (!atlas_envelope_ok(a.C, g.width, g.height)) {
                a.flags &= (uint8_t)~kAtlasValid;
                break;
            }
        }
        if (gen_max != 0 && (uint32_t)a.gen > gen_max)
            a.flags &= (uint8_t)~kAtlasValid;
    }
    out = a;
    return true;
}

/* [SYN] 13.12.5, and every word of the header comment applies: this is NOT
 * normative and nothing it computes is compared by conformance. */
void atlas_display_luma(const AtlasTable &at, const uint16_t *atlas, int stride,
                        int eye_w, int height, std::vector<uint16_t> &out) {
    const uint32_t ntiles = (uint32_t)at.e.size();
    out.assign((size_t)ntiles * 64u * 64u, 0u);
    const double ox = (double)(eye_w >> 1);
    const double oy = (double)(height >> 1);
    const int cols = at.g.cols_per_eye * at.g.eyes;
    for (uint32_t t = 0; t < ntiles; ++t) {
        uint16_t *dst = &out[(size_t)t * 64u * 64u];
        if (!(at.e[t].flags & kAtlasValid)) {
            /* No pixels a client could show.  Mid-grey rather than zero, so a
             * picture with an invalid tile in it reads as a hole and not as a
             * black rectangle that might be content. */
            for (int i = 0; i < 64 * 64; ++i) dst[i] = 128;
            continue;
        }
        const int row = (int)(t / (uint32_t)cols);
        const int rem = (int)(t % (uint32_t)cols);
        const int eye = rem / at.g.cols_per_eye;
        const int col = rem % at.g.cols_per_eye;
        /* The real matrix, in the row scales of 3.1.1.  m22 is 1 exactly,
         * because the renormalisation put it there. */
        double m[9];
        for (int k = 0; k < 9; ++k)
            m[k] = (double)at.e[t].C[k] / (k < 6 ? 2097152.0 : 536870912.0);
        const int xbase = eye * eye_w;
        for (int v = 0; v < 64; ++v)
            for (int u = 0; u < 64; ++u) {
                /* Centred indices of THIS frame, in this eye's own picture. */
                const double cx = (double)(col * 64 + u) - ox;
                const double cy = (double)(row * 64 + v) - oy;
                const double den = m[6] * cx + m[7] * cy + m[8];
                double sx, sy;
                if (den == 0.0) {
                    sx = cx; sy = cy;
                } else {
                    sx = (m[0] * cx + m[1] * cy + m[2]) / den;
                    sy = (m[3] * cx + m[4] * cy + m[5]) / den;
                }
                /* Back to this eye's sample grid, then to the pair-wide
                 * atlas.  Clamped inside the EYE: an eye never samples across
                 * the seam into its neighbour's picture. */
                double px = sx + ox, py = sy + oy;
                if (px < 0) px = 0;
                if (py < 0) py = 0;
                if (px > (double)(eye_w - 1)) px = (double)(eye_w - 1);
                if (py > (double)(height - 1)) py = (double)(height - 1);
                const int ix = (int)px, iy = (int)py;
                const int ix1 = ix + 1 < eye_w ? ix + 1 : ix;
                const int iy1 = iy + 1 < height ? iy + 1 : iy;
                const double fx = px - (double)ix, fy = py - (double)iy;
                const double a = (double)atlas[(size_t)iy * stride + xbase + ix];
                const double b = (double)atlas[(size_t)iy * stride + xbase + ix1];
                const double c = (double)atlas[(size_t)iy1 * stride + xbase + ix];
                const double d = (double)atlas[(size_t)iy1 * stride + xbase + ix1];
                const double val = a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) +
                                   c * (1 - fx) * fy + d * fx * fy;
                double r = val + 0.5;
                if (r < 0) r = 0;
                if (r > 65535.0) r = 65535.0;
                dst[(size_t)v * 64 + (size_t)u] = (uint16_t)r;
            }
    }
}

double luma_psnr_tilemajor(const uint16_t *a, const int32_t *b, uint32_t ntiles,
                           int maxval) {
    double sse = 0;
    const size_t n = (size_t)ntiles * 64u * 64u;
    for (size_t i = 0; i < n; ++i) {
        const double d = (double)a[i] - (double)b[i];
        sse += d * d;
    }
    if (sse <= 0) return 99.0;
    const double mse = sse / (double)n;
    return 10.0 * std::log10((double)maxval * (double)maxval / mse);
}

void atlas_build_matrices(const AtlasTable &at, int width, int height, int cw,
                          int ch, WarpParams &wp) {
    const uint32_t ntiles = (uint32_t)at.e.size();
    int32_t I[9];
    atlas_identity(I);
    for (uint32_t t = 0; t < ntiles; ++t) {
        const int32_t *C =
            (at.e[t].flags & kAtlasValid) ? at.e[t].C : I;
        const uint32_t base = warp_atlas_mat_idx(ntiles, t);
        uint32_t rec[NXVW_WARP_MAT_UINTS];
        conjugate_plane_matrix(C, width, height, 1, rec);
        for (int i = 0; i < NXVW_WARP_MAT_UINTS; ++i) wp.w[base + (uint32_t)i] = rec[i];
        conjugate_plane_matrix(C, cw, ch, 2, rec);
        for (int i = 0; i < NXVW_WARP_MAT_UINTS; ++i)
            wp.w[base + (uint32_t)NXVW_WARP_MAT_UINTS + (uint32_t)i] = rec[i];
    }
}

}  // namespace nxe
