// CPU model of reconstruct.comp.  Read the two side by side: every arithmetic
// line here has a one-to-one counterpart in the shader.
#include "passB_model.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "syntax_constants.h"
// [inter] nxvw_wpred_plane_off(): the WPred buffer's fixed plane offsets.
#include "../inter/inter_layout.h"

namespace nxvw {
namespace {

inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clamp16(int v) { return iclamp(v, kI16Min, kI16Max); }

// ------------------------------------------------------------- 1D IDCT
// Mirrors idct8_1d() in the shader, which mirrors ref/src/transform.cpp.
inline void idct8_1d(const int *x, int *y) {
    int t0 = (x[0] + x[4]) * kC4;
    int t1 = (x[0] - x[4]) * kC4;
    int t2 = x[2] * kS2 - x[6] * kC2;
    int t3 = x[2] * kC2 + x[6] * kS2;
    int e0 = t0 + t3, e3 = t0 - t3;
    int e1 = t1 + t2, e2 = t1 - t2;
    int A = x[1] * kA1 + x[7] * kA7;
    int B = x[1] * kA7 - x[7] * kA1;
    int C = x[3] * kA3 + x[5] * kA5;
    int D = x[3] * kA5 - x[5] * kA3;
    int O0 = A + C;
    int O3 = B - D;
    int P = A - C, Q = B + D;
    // [marked edit] exact product, see nxvw_mul_c4_rnd9.
    int O1 = nxvw_mul_c4_rnd9(P + Q);
    int O2 = nxvw_mul_c4_rnd9(P - Q);
    y[0] = e0 + O0; y[7] = e0 - O0;
    y[1] = e1 + O1; y[6] = e1 - O1;
    y[2] = e2 + O2; y[5] = e2 - O2;
    y[3] = e3 + O3; y[4] = e3 - O3;
}

// ------------------------------- [minor 6] 16- and 32-point 1D IDCT
// Mirrors idct16_1d / idct32_1d in the shader, which mirror
// ref/src/transform.cpp even_odd_inverse().  A length-2M inverse is the
// length-M inverse of the even-indexed coefficients plus a dense M x M
// rotation of the odd-indexed ones, so the whole family is the one 8-point
// Loeffler core with two rotations stacked on it -- and the quantiser sees
// orthonormal coefficients at every size, which is what lets one qstep table
// serve all four.
inline void idct16_1d(const int *x, int *y) {
    int xe[8], e[8];
    for (int k = 0; k < 8; ++k) xe[k] = x[2 * k];
    idct8_1d(xe, e);
    for (int n = 0; n < 8; ++n) {
        int o = 0;
        for (int j = 0; j < 8; ++j) o += x[2 * j + 1] * kOdd16[n * 8 + j];
        y[n] = e[n] + o;
        y[15 - n] = e[n] - o;
    }
}
inline void idct32_1d(const int *x, int *y) {
    int xe[16], e[16];
    for (int k = 0; k < 16; ++k) xe[k] = x[2 * k];
    idct16_1d(xe, e);
    for (int n = 0; n < 16; ++n) {
        int o = 0;
        for (int j = 0; j < 16; ++j) o += x[2 * j + 1] * kOdd32[n * 16 + j];
        y[n] = e[n] + o;
        y[31 - n] = e[n] - o;
    }
}
inline void idctN_1d(const int *x, int *y, int lb) {
    if (lb == 3) idct8_1d(x, y);
    else if (lb == 4) idct16_1d(x, y);
    else idct32_1d(x, y);
}

// The n x n inverse transform: rows then columns, both passes writing
// transposed, with the normative clamp16 after each.  [REF]
// ref/src/transform.cpp idct_block().  The kernel spells this as one thread
// per 1D transform with the intermediate stored un-transposed in the plane
// slot; this is the same arithmetic written to be read.
inline void idct_nxn(const int *src, int *dst, int lb) {
    const int n = 1 << lb;
    const int s1 = nxvw_idct_shift1(lb), r1 = nxvw_idct_round1(lb);
    const int s2 = nxvw_idct_shift2(lb), r2 = nxvw_idct_round2(lb);
    int tmp[32 * 32], in[32], out[32];
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = src[r * n + c];
        idctN_1d(in, out, lb);
        for (int c = 0; c < n; ++c)
            tmp[c * n + r] = clamp16((out[c] + r1) >> s1);
    }
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) in[c] = tmp[r * n + c];
        idctN_1d(in, out, lb);
        for (int c = 0; c < n; ++c)
            dst[c * n + r] = clamp16((out[c] + r2) >> s2);
    }
}

// ------------------------------------------------------------- bilinear
inline int bilinear(const int *src, int w, int h, int stride, int sx, int sy) {
    int x0 = sx >> kBilinFracBits, y0 = sy >> kBilinFracBits;
    int fx = sx & (kBilinOne - 1), fy = sy & (kBilinOne - 1);
    int x1 = iclamp(x0 + 1, 0, w - 1);
    int y1 = iclamp(y0 + 1, 0, h - 1);
    x0 = iclamp(x0, 0, w - 1);
    y0 = iclamp(y0, 0, h - 1);
    int p00 = src[y0 * stride + x0], p01 = src[y0 * stride + x1];
    int p10 = src[y1 * stride + x0], p11 = src[y1 * stride + x1];
    int wx0 = kBilinOne - fx, wy0 = kBilinOne - fy;
    return (p00 * wx0 * wy0 + p01 * fx * wy0 + p10 * wx0 * fy + p11 * fx * fy +
            kBilinRound) >> kBilinShift;
}

// [minor 6] Mirrors idct4_1d() in the shader, which mirrors
// ref/src/transform.cpp.  Gain 2^10, the same as the 8-point core's, so the
// two-pass shift chain is the 8x8's unchanged.
inline void idct4_1d(const int *x, int *y) {
    int e0 = (x[0] + x[2]) * kD0;
    int e1 = (x[0] - x[2]) * kD0;
    int o0 = x[1] * kD1 + x[3] * kD2;
    int o1 = x[1] * kD2 - x[3] * kD1;
    y[0] = e0 + o0; y[3] = e0 - o0;
    y[1] = e1 + o1; y[2] = e1 - o1;
}

// [minor 6] One 8x8 block coded as four 4x4 sub-blocks: each occupies its own
// quadrant of the 64-value coefficient array and is quantised with the tile's
// matrix subsampled by two in each frequency axis.  [REF] ref/src/codec.cpp
// residual_block(split) and common.h weight4().
//
// The kernel spells this very differently -- a thread computes only the two
// columns it already owns, which is what keeps its live set to eight values on
// an Adreno 650 -- but the arithmetic is this, and this is the readable form.
// `dq16` is the sub-block's sixteen already-dequantized coefficients in its
// own raster order; `res` is the 8x8 block and the sub-block writes its own
// quadrant of it.
inline void split_subblock(const int *dq, int ox, int oy, int *res) {
    int tmp[16], in[4], out[4];
    {
        for (int r = 0; r < 4; ++r) {
            for (int q = 0; q < 4; ++q) in[q] = dq[r * 4 + q];
            idct4_1d(in, out);
            for (int q = 0; q < 4; ++q)
                tmp[q * 4 + r] = clamp16((out[q] + kIdctRound1) >> kIdctShift1);
        }
        for (int r = 0; r < 4; ++r) {
            for (int q = 0; q < 4; ++q) in[q] = tmp[r * 4 + q];
            idct4_1d(in, out);
            for (int q = 0; q < 4; ++q)
                res[(oy + q) * 8 + ox + r] =
                    clamp16((out[q] + kIdctRound2) >> kIdctShift2);
        }
    }
}

// ------------------------------------------- directional intra [v3]
// Mirrors dirDone / dirAt / dirStepOf / predictCols in reconstruct.comp,
// which mirror ref/src/codec.cpp build_refs / predict_block.  The model runs
// the blocks in plain raster order rather than by wavefront step: the two
// produce the same result by construction, because a block's references are
// exactly the blocks the step function already put before it.
// [minor 6] `qsh` is the sub-tile shift in BLOCKS, max(5 - lb, 0): a 32x32
// sub-tile is four 8x8 blocks, two 16x16 ones and one 32x32 one.  At lb == 3
// it is kDirSubTileLog2 and this is the predicate it always was.
inline bool dir_done(int sched, int nbx, int nby, int bx, int by, int qsh) {
    bool done = (nby < by) || (nby == by && nbx < bx);
    if ((sched & kDirSchedNoAboveRight) != 0 && nbx > bx) done = false;
    if ((sched & kDirSchedSubTile) != 0 &&
        ((nbx >> qsh) != (bx >> qsh) || (nby >> qsh) != (by >> qsh)))
        done = false;
    return done;
}

// ---------------------------------------------- [minor 6] chroma from luma
// Mirrors cflLuma / cflFit / cflPredictOne in reconstruct.comp, which mirror
// ref/src/codec.cpp cfl_luma / cfl_fit / predict_block(kIntraCfl).
//
// `luma` is this tile's reconstructed luma plane, `lsize` its edge and `f`
// the luma:chroma ratio, 1 or 2.
inline int cfl_luma(const int *luma, int lsize, int f, int cx, int cy) {
    if (f == 1)
        return luma[(size_t)iclamp(cy, 0, lsize - 1) * lsize +
                    iclamp(cx, 0, lsize - 1)];
    int x0 = iclamp(2 * cx, 0, lsize - 1), x1 = iclamp(2 * cx + 1, 0, lsize - 1);
    int y0 = iclamp(2 * cy, 0, lsize - 1), y1 = iclamp(2 * cy + 1, 0, lsize - 1);
    return (luma[(size_t)y0 * lsize + x0] + luma[(size_t)y0 * lsize + x1] +
            luma[(size_t)y1 * lsize + x0] + luma[(size_t)y1 * lsize + x1] + 2) >>
           2;
}
// kCflRecip[d] = round(2^15 / d), computed: the same number for every d in
// [1, 255] and no ties to round, so the kernel needs no 256-entry table.
inline int cfl_recip(int d) { return (65536 + d) / (2 * d); }

struct CflModel {
    int alpha, base_l, base_c;
};
// The two smallest co-located luma neighbours set `base`, the two largest the
// far end, both ends averaged; ties take the lowest index.  The pairs are
// walked twice rather than tabulated, exactly as the kernel does.
// [minor 6] The fit reads the block's own 2n reconstructed neighbours, so it
// follows the transform size the way every other predictor does; at n == 8 it
// is character for character the detail package's.
inline CflModel cfl_fit(const int *A, const int *L, const int *luma, int lsize,
                        int f, int x0, int y0, int n) {
    const int npairs = 2 * n;
    int lo0 = 0, hi0 = 0, lo0l = 0, lo0c = 0, hi0l = 0, hi0c = 0;
    for (int k = 0; k < npairs; ++k) {
        int cn = k < n ? A[1 + k] : L[1 + (k - n)];
        int ln = k < n ? cfl_luma(luma, lsize, f, x0 + k, y0 - 1)
                       : cfl_luma(luma, lsize, f, x0 - 1, y0 + (k - n));
        if (k == 0) { lo0l = ln; lo0c = cn; hi0l = ln; hi0c = cn; continue; }
        if (ln < lo0l) { lo0 = k; lo0l = ln; lo0c = cn; }
        if (ln > hi0l) { hi0 = k; hi0l = ln; hi0c = cn; }
    }
    int lo1 = -1, hi1 = -1, lo1l = 0, lo1c = 0, hi1l = 0, hi1c = 0;
    for (int k = 0; k < npairs; ++k) {
        int cn = k < n ? A[1 + k] : L[1 + (k - n)];
        int ln = k < n ? cfl_luma(luma, lsize, f, x0 + k, y0 - 1)
                       : cfl_luma(luma, lsize, f, x0 - 1, y0 + (k - n));
        if (k != lo0 && (lo1 < 0 || ln < lo1l)) { lo1 = k; lo1l = ln; lo1c = cn; }
        if (k != hi0 && (hi1 < 0 || ln > hi1l)) { hi1 = k; hi1l = ln; hi1c = cn; }
    }
    CflModel m{};
    m.base_l = (lo0l + lo1l + 1) >> 1;
    m.base_c = (lo0c + lo1c + 1) >> 1;
    const int top_l = (hi0l + hi1l + 1) >> 1;
    const int top_c = (hi0c + hi1c + 1) >> 1;
    const int dl = top_l - m.base_l;
    if (dl <= 0) { m.alpha = 0; return m; }
    const int q = (top_c - m.base_c) * cfl_recip(dl);
    m.alpha = iclamp((q + kCflRecipRound) >> kCflRecipShift, -kCflAlphaMax,
                     kCflAlphaMax - 1);
    return m;
}

// [SYN] 7.4: every mode but 0 and kIntraCfl is a weighted average of in-range
// references, so no clamp is applied; kIntraCfl clamps because a fitted slope
// can leave the sample domain.  A[0] / L[0] are the corner; the spec's A[k] is
// A[1 + k].
//
// `luma` is non-null only for a chroma plane of a tile with INTRA_CFL, which
// is the only way mode kIntraCfl can appear at all: Pass A refuses the symbol
// otherwise, because the alphabet is nine everywhere else.
inline void predict_block(int mode, const int *A, const int *L, const int *base,
                          int size, int bx, int by, int lb, int *P,
                          const int *luma = nullptr, int lsize = 0, int f = 0,
                          int maxval = 255) {
    const int n = 1 << lb;
    const int sh = lb + 1;   // the DC / planar averaging shift
    const int rnd = n;       // ... and its rounding term
    if (mode == kIntraCfl) {
        const int x0 = bx * n, y0 = by * n;
        const CflModel m = cfl_fit(A, L, luma, lsize, f, x0, y0, n);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                P[j * n + i] = iclamp(
                    m.base_c + ((m.alpha * (cfl_luma(luma, lsize, f, x0 + i,
                                                     y0 + j) -
                                            m.base_l) +
                                 kCflPredRound) >>
                                kCflAlphaBits),
                    0, maxval);
        return;
    }
    const int tl = A[0];
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            int v = 0;
            switch (mode) {
                case kIntraDcPlane:
                    v = base[(size_t)(by * n + j) * size + bx * n + i];
                    break;
                case kIntraDc: {
                    int sum = 0;
                    for (int k = 0; k < n; ++k) sum += A[1 + k] + L[1 + k];
                    v = (sum + rnd) >> sh;
                    break;
                }
                case kIntraPlanar:
                    v = ((n - 1 - i) * L[1 + j] + (i + 1) * A[1 + n] +
                         (n - 1 - j) * A[1 + i] + (j + 1) * L[1 + n] +
                         rnd) >> sh;
                    break;
                case kIntraH: v = L[1 + j]; break;
                case kIntraV: v = A[1 + i]; break;
                case kIntraDdl: {
                    int k = i + j;
                    v = (k == 2 * n - 2)
                            ? (A[1 + 2 * n - 2] + 3 * A[1 + 2 * n - 1] +
                               kIntraTap3Round) >> kIntraTap3Shift
                            : (A[1 + k] + 2 * A[1 + k + 1] + A[1 + k + 2] +
                               kIntraTap3Round) >> kIntraTap3Shift;
                    break;
                }
                case kIntraDdr:
                    if (i > j) {
                        int k = i - j;
                        v = (A[k - 1] + 2 * A[k] + A[1 + k] + kIntraTap3Round) >>
                            kIntraTap3Shift;
                    } else if (i < j) {
                        int k = j - i;
                        v = (L[k - 1] + 2 * L[k] + L[1 + k] + kIntraTap3Round) >>
                            kIntraTap3Shift;
                    } else {
                        v = (A[1] + 2 * tl + L[1] + kIntraTap3Round) >> kIntraTap3Shift;
                    }
                    break;
                case kIntraVr: {
                    int z = 2 * i - j, k = i - (j >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        v = (A[k] + A[1 + k] + kIntraTap2Round) >> kIntraTap2Shift;
                    else if (z >= 0)
                        v = (A[k - 1] + 2 * A[k] + A[1 + k] + kIntraTap3Round) >>
                            kIntraTap3Shift;
                    else if (z == -1)
                        v = (L[1] + 2 * tl + A[1] + kIntraTap3Round) >> kIntraTap3Shift;
                    else {
                        int q = j - 2 * i;
                        v = (L[q] + 2 * L[q - 1] + L[q - 2] + kIntraTap3Round) >>
                            kIntraTap3Shift;
                    }
                    break;
                }
                default: {  // kIntraHd
                    int z = 2 * j - i, k = j - (i >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        v = (L[k] + L[1 + k] + kIntraTap2Round) >> kIntraTap2Shift;
                    else if (z >= 0)
                        v = (L[k - 1] + 2 * L[k] + L[1 + k] + kIntraTap3Round) >>
                            kIntraTap3Shift;
                    else if (z == -1)
                        v = (A[1] + 2 * tl + L[1] + kIntraTap3Round) >> kIntraTap3Shift;
                    else {
                        int q = i - 2 * j;
                        v = (A[q] + 2 * A[q - 1] + A[q - 2] + kIntraTap3Round) >>
                            kIntraTap3Shift;
                    }
                    break;
                }
            }
            P[j * n + i] = v;
        }
}

// One tile's reconstructed planes at their coded resolution.
struct TilePlanes {
    int size[4] = {0, 0, 0, 0};
    int full[4] = {64, 64, 64, 64};
    std::array<std::vector<int>, 4> s;
};

inline int planeAtFull(const TilePlanes &tp, int p, int px, int py) {
    int size = tp.size[p], full = tp.full[p];
    if (size == full) return tp.s[p][(size_t)py * size + px];
    int mul = (16 * size) / full;
    int off = mul / 2 - 8;
    return bilinear(tp.s[p].data(), size, size, size, mul * px + off,
                    mul * py + off);
}

inline int planeAtDisplay(const TilePlanes &tp, int p, int x, int y) {
    int full = tp.full[p];
    if (full == 64) return planeAtFull(tp, p, x, y);
    int sx = 8 * x - 4, sy = 8 * y - 4;
    int x0 = sx >> kBilinFracBits, y0 = sy >> kBilinFracBits;
    int fx = sx & (kBilinOne - 1), fy = sy & (kBilinOne - 1);
    int x1 = iclamp(x0 + 1, 0, full - 1);
    int y1 = iclamp(y0 + 1, 0, full - 1);
    x0 = iclamp(x0, 0, full - 1);
    y0 = iclamp(y0, 0, full - 1);
    int p00 = planeAtFull(tp, p, x0, y0);
    int p01 = planeAtFull(tp, p, x1, y0);
    int p10 = planeAtFull(tp, p, x0, y1);
    int p11 = planeAtFull(tp, p, x1, y1);
    int wx0 = kBilinOne - fx, wy0 = kBilinOne - fy;
    return (p00 * wx0 * wy0 + p01 * fx * wy0 + p10 * wx0 * fy + p11 * fx * fy +
            kBilinRound) >> kBilinShift;
}

// Reconstruct one tile's planes.  Same order of operations as the kernel.
void reconstruct_tile(const PassBInput &in, int tile, TilePlanes &tp,
                      int &alpha_mode, int &alpha_value) {
    const NxvwPassBPush &pcp = in.push;
    const NxvwTileRec &rec = in.recs[tile];
    int res_level = nxvw_rec_res_level(rec.w1);
    int chroma444 = pcp.chroma420 != 0 ? 0 : nxvw_rec_chroma444(rec.w1);
    alpha_mode = nxvw_rec_alpha_mode(rec.w1);
    alpha_value = nxvw_rec_alpha_value(rec.w2);
    int tskip = nxvw_rec_tskip(rec.w1);
    int mode = nxvw_rec_mode(rec.w1);
    int qp = iclamp(pcp.baseQp + nxvw_rec_qp_delta(rec.w1), 0, 63);
    // INTER HOOK, see reconstruct.comp.  `interTile` is what decides whether
    // the DC plane's block means are clamped to the sample domain, and
    // whether the planar interpolation is the prediction or the correction on
    // top of the warp predictor ([SYN] 13.3).
    bool intra = (mode == kModeIntra);
    const bool interTile = (in.interPred != 0) && !intra;
    // [marked edit] per-tile weighting-matrix override, docs/SYNTAX.md 4.1.
    const int wmSet = nxvw_rec_wm_id(rec.w1) * 128;
    const int sched = in.dirSched;

    int nplanes = pcp.alphaPresent != 0 ? 4 : 3;
    int ncoded = nplanes;
    if (alpha_mode != kAlphaCoded) ncoded = std::min(ncoded, 3);

    for (int p = 0; p < 4; ++p) {
        tp.size[p] = nxvw_plane_size(p, res_level, chroma444);
        tp.full[p] = ((p == 1 || p == 2) && pcp.chroma420 != 0) ? 32 : 64;
        tp.s[p].assign((size_t)tp.size[p] * tp.size[p], 0);
    }

    const int16_t *coefBase = in.coef + (size_t)tile * pcp.coefStrideI16;

    // [sparse] Coefficient `pos` of a unit, by its raster index inside the
    // unit.  Dense: the slot is the raster index.  Sparse: the unit holds
    // `len` coefficients in scan order and everything past LAST is zero.
    auto coef_in = [&](const int16_t *base, int scanId, int len, int pos) {
        if (!pcp.sparse) return int(base[pos]);
        int k = nxvw_scan_pos(scanId, pos);
        return k < len ? int(base[k]) : 0;
    };
    const uint32_t *lenWords =
        in.unit_lens ? in.unit_lens + (size_t)tile * NXVW_UNIT_LEN_WORDS_PER_TILE
                     : nullptr;
    // [minor 6] The unit-length field width follows the tile's transform
    // size: a 32x32 unit's LAST + 1 reaches 1024 and does not fit a byte.
    const int xformSize = nxvw_rec_xform_size(rec.w1);
    const uint32_t lenPerWord = xformSize != 0 ? NXVW_UNIT_LENS_PER_WORD_LARGE
                                               : NXVW_UNIT_LENS_PER_WORD;
    const uint32_t lenBits =
        xformSize != 0 ? NXVW_UNIT_LEN_BITS_LARGE : NXVW_UNIT_LEN_BITS;
    const uint32_t lenMask =
        xformSize != 0 ? NXVW_UNIT_LEN_MASK_LARGE : NXVW_UNIT_LEN_MASK;
    auto unit_len = [&](int ui) {
        if (!pcp.sparse) return kBlock * kBlock;
        uint32_t w = lenWords[ui / int(lenPerWord)];
        return int((w >> ((uint32_t(ui) % lenPerWord) * lenBits)) & lenMask);
    };
    // Unit index of the plane being decoded, [SYN] 9.1.  The mode unit exists
    // whenever the stream sets INTRA_DIR, which is what Pass A numbers units
    // by -- not the narrower per-tile `dir` below.
    int unitBase = 0;
    // [inter] ... and only on an INTRA tile ([SYN] 13.3); mirrors
    // reconstruct.comp.
    const int unitsPerPlaneExtra = (pcp.intraDir != 0 && intra) ? 2 : 1;

    for (int p = 0; p < ncoded; ++p) {
        bool chroma = (p == 1 || p == 2);
        int size = tp.size[p];
        // [minor 6] XFORM_LARGE: the transform edge is the tile's `8 <<
        // xform_size` capped by this plane's own coded extent, and the block
        // grid, DC plane, planar mapping, weighting matrix, scan and
        // predictors all follow it ([SYN] 6.7).  At xform_size 0 this is
        // lb = 3, bs = 8, nb = size >> 3, unchanged.
        const int lb = nxvw_block_log2(xformSize, size);
        const int bs = 1 << lb;
        int nb = size >> lb;
        int ndc = nb * nb;
        const int ncoef = bs * bs;
        const int qsh = lb < 5 ? 5 - lb : 0;
        int planeQp = qp;
        if (chroma) planeQp = iclamp(qp + pcp.chromaQpOff, 0, 63);
        else if (p == 3) planeQp = iclamp(qp + pcp.alphaQpOff, 0, 63);
        const int *wmat = in.weights + wmSet + (chroma ? 64 : 0);
        // [v3] this plane's per-block intra modes, unpacked from Pass A's
        // 4-bit-per-block array.
        int modes[64] = {};  // nb*nb <= 64 at every transform size
        if (in.modes)
            for (int b = 0; b < ndc && b < 64; ++b) {
                uint32_t w = in.modes[(size_t)tile * NXVW_MODE_REGION_UINTS +
                                      p * NXVW_MODE_WORDS_PER_PLANE +
                                      b / int(NXVW_MODES_PER_UINT)];
                modes[b] = int((w >> (uint32_t(b % int(NXVW_MODES_PER_UINT)) *
                                      NXVW_MODE_BITS)) & NXVW_MODE_MASK);
            }
        bool ctChroma = (pcp.colorTransform == kCtYCoCgR) && chroma;
        int dcOff = ctChroma ? kDcOffsetChromaCT : kDcOffset8;
        int maxval = ctChroma ? kMaxvalChromaCT : kMaxval8;

        // --- DC plane (PAPER 3.2.4)
        int dcqp = nxvw_dc_qp(planeQp);  // [marked edit] qp >> 1, was qp - 6
        int tdc = model_dequant_step(dcqp, kFlatWeight);
        const int dcLen = unit_len(unitBase);
        const int dcScan = nxvw_scan_id(ndc, 0);
        std::vector<int> dc(ndc, 0);
        if (dcLen != 0)
            for (int i = 0; i < ndc; ++i)
                dc[i] = model_dequant(coef_in(coefBase, dcScan, dcLen, i), tdc);
        if (nb == 8 && dcLen != 0) {
            int out[64];
            model_idct8x8(dc.data(), out);
            for (int i = 0; i < 64; ++i) dc[i] = out[i];
        }
        std::vector<int> means(ndc);
        // [inter] An INTRA tile's block mean is a sample value and is clamped
        // to the sample domain.  An INTER tile's is `dc_offset + a residual
        // mean`, whose legal range is wider on both sides; clamping it there
        // would cap the DC correction the warp needs.  Mirrors
        // inter_hook.glsl's nxvwMeanClamp().
        for (int i = 0; i < ndc; ++i)
            means[i] = interTile ? dcOff + dc[i]
                                 : iclamp(dcOff + dc[i], 0, maxval);

        // --- the DC-plane prediction, which is `pred` in SYNTAX.md 7.2 and
        // the base of both forms of 7.5.  For an inter tile it is the
        // per-block DC correction instead, and the prediction is
        //     clamp(W + planar(M) - dc_offset, 0, maxval)
        // with W the predictor Pass W wrote ([SYN] 13.3).
        std::vector<int> pred((size_t)size * size, 0);
        if (intra || interTile)
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    pred[(size_t)y * size + x] =
                        bilinear(means.data(), nb, nb, nb,
                                 nxvw_planar_q4(x, bs, lb),
                                 nxvw_planar_q4(y, bs, lb));
        if (interTile) {
            const int16_t *wp =
                in.wpred + (size_t)tile * in.wpredStrideI16 +
                nxvw_wpred_plane_off(p, pcp.chroma420);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    const size_t i = (size_t)y * size + x;
                    pred[i] = iclamp((int)wp[i] + pred[i] - dcOff, 0, maxval);
                }
        }

        const bool dir = intra && pcp.intraDir != 0;
        const bool layer = pcp.dirLayer != 0;
        // In the layered form the modes predict the DC-plane residual, whose
        // out-of-block fallback is zero rather than `pred`.
        std::vector<int> zero;
        const int *fallback = pred.data();
        if (dir && layer) {
            zero.assign((size_t)size * size, 0);
            fallback = zero.data();
        }
        std::vector<int> recon;
        if (dir) recon.assign((size_t)size * size, 0);

        // --- residual blocks
        const int16_t *bc = coefBase + ndc;
        // [minor 6] The per-block 4x4 split flags Pass A published, one bit
        // per block after the mode words of the same per-tile region.
        // `in.modes` is optional in the model's callers: a harness that sets
        // no modes and no split flags passes null, and every flag is then 0.
        std::vector<int> splitFlag(ndc, 0);
        for (int b = 0; in.modes && b < ndc; ++b) {
            const uint32_t w =
                in.modes[(size_t)tile * NXVW_MODE_REGION_UINTS +
                         NXVW_MODE_WORDS_PER_TILE +
                         p * NXVW_SPLIT_WORDS_PER_PLANE + (b >> 5)];
            splitFlag[b] = int((w >> (uint32_t(b) & 31u)) & 1u);
        }
        for (int b = 0; b < ndc; ++b) {
            int bx = b % nb, by = b / nb;
            const int blockScan =
                splitFlag[b] ? kScan4Split : nxvw_scan_id(ncoef, tskip);
            const int16_t *c = bc + (size_t)b * ncoef;
            const int blockLen = unit_len(unitBase + unitsPerPlaneExtra + b);
            int res[32 * 32] = {};
            if (blockLen == 0) {
                // [sparse] An uncoded block transforms to all zeros; the
                // kernel skips both passes of the IDCT for it.
            } else if (tskip) {
                // Mutually exclusive with a transform size other than 8x8
                // ([SYN] 6.6 / 6.7), so ncoef is 64 on this arm.
                int t = model_dequant_step(planeQp, kFlatWeight);
                for (int i = 0; i < ncoef; ++i)
                    res[i] = model_dequant(coef_in(c, blockScan, blockLen, i), t);
            } else if (splitFlag[b]) {
                // [minor 6] Four 4x4 sub-blocks, each in its own quadrant of
                // the coefficient array and quantised with the tile's matrix
                // subsampled by two in each frequency axis.  The scan is the
                // concatenated one, which `blockScan` already selected.
                for (int sb = 0; sb < 4; ++sb) {
                    const int ox = (sb & 1) * 4, oy = (sb >> 1) * 4;
                    int dq[16];
                    for (int k = 0; k < 16; ++k) {
                        const int pos = (oy + (k >> 2)) * 8 + ox + (k & 3);
                        const int w = wmat[(k >> 2) * 16 + (k & 3) * 2];
                        dq[k] =
                            model_dequant(coef_in(c, blockScan, blockLen, pos),
                                          model_dequant_step(planeQp, w));
                    }
                    split_subblock(dq, ox, oy, res);
                }
            } else {
                // [minor 6] ONE transmitted 8x8 matrix serves every size: an
                // n x n block replicates it, entry (u, v) being entry
                // (u >> k, v >> k) with k = lb - 3 ([SYN] 6.5).  At lb == 3
                // the index is `i` and this is the 8x8 form unchanged.
                int dq[32 * 32];
                for (int i = 0; i < ncoef; ++i)
                    dq[i] = model_dequant(
                        coef_in(c, blockScan, blockLen, i),
                        model_dequant_step(
                            planeQp, wmat[nxvw_block_weight_index(i, lb)]));
                idct_nxn(dq, res, lb);
            }
            if (!dir) {
                for (int j = 0; j < bs; ++j)
                    for (int i = 0; i < bs; ++i) {
                        int x = bx * bs + i, y = by * bs + j;
                        int pv = (intra || interTile)
                                     ? pred[(size_t)y * size + x]
                                     : 0;
                        tp.s[p][(size_t)y * size + x] =
                            iclamp(pv + res[j * bs + i], 0, maxval);
                    }
                continue;
            }
            // Reference samples, clamped into the tile: a tile never reads a
            // neighbour, so its borders read this tile's own DC plane.
            // The reference arrays are 2n long at every size, so DDL
            // reaches A[2n-1] and the above-right dependency is one block
            // wide however large the block is ([SYN] 7.4).  The model
            // materialises them; the kernel reads each one through dirAt()
            // instead, because 2n+1 is sixty-five ints at 32x32 and that is
            // not a live set any GPU has -- one of the two places the two
            // spellings deliberately differ.
            int A[2 * 32 + 1], L[2 * 32 + 1];
            const int x0 = bx * bs, y0 = by * bs;
            auto at = [&](int x, int y) {
                int cx = iclamp(x, 0, size - 1), cy = iclamp(y, 0, size - 1);
                const int *src =
                    dir_done(sched, cx >> lb, cy >> lb, bx, by, qsh)
                        ? recon.data()
                        : fallback;
                return src[(size_t)cy * size + cx];
            };
            A[0] = L[0] = at(x0 - 1, y0 - 1);
            for (int k = 0; k < 2 * bs; ++k) {
                A[1 + k] = at(x0 + k, y0 - 1);
                L[1 + k] = at(x0 - 1, y0 + k);
            }
            int P[32 * 32];
            // [minor 6] INTRA_CFL reads this tile's reconstructed luma plane,
            // which is complete because plane 0 runs first and the planes are
            // reconstructed in order.  `f` is the luma:chroma ratio, 1 or 2;
            // no other ratio is defined and none can arise.
            const bool cflPlane = (p == 1 || p == 2) && tp.size[0] > 0;
            const int cflF = cflPlane ? (tp.size[0] == size
                                             ? 1
                                             : (tp.size[0] == 2 * size ? 2 : 0))
                                      : 0;
            predict_block(modes[b], A, L, fallback, size, bx, by, lb, P,
                          cflPlane ? tp.s[0].data() : nullptr, tp.size[0],
                          cflF, maxval);
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    int x = x0 + i, y = y0 + j;
                    int v = P[j * bs + i] + res[j * bs + i];
                    if (layer) {
                        int base = pred[(size_t)y * size + x];
                        int full = iclamp(base + v, 0, maxval);
                        recon[(size_t)y * size + x] = full - base;
                        tp.s[p][(size_t)y * size + x] = full;
                    } else {
                        int full = iclamp(v, 0, maxval);
                        recon[(size_t)y * size + x] = full;
                        tp.s[p][(size_t)y * size + x] = full;
                    }
                }
        }
        coefBase += ndc + ndc * ncoef;
        unitBase += unitsPerPlaneExtra + ndc;
    }
}

struct Rgba {
    int r, g, b, a;
};

inline Rgba tile_pixel(const PassBInput &in, const TilePlanes &tp, int alpha_mode,
                       int alpha_value, int x, int y) {
    int P0 = planeAtDisplay(tp, 0, x, y);
    int P1 = planeAtDisplay(tp, 1, x, y);
    int P2 = planeAtDisplay(tp, 2, x, y);
    int R, G, B;
    if (in.push.colorTransform == kCtYCoCgR) {
        int Y = P0;
        int Co = P1 - kDcOffsetChromaCT;
        int Cg = P2 - kDcOffsetChromaCT;
        int t = Y - (Cg >> 1);
        G = Cg + t;
        B = t - (Co >> 1);
        R = B + Co;
    } else {
        R = P0; G = P1; B = P2;
    }
    Rgba o;
    o.r = iclamp(R, 0, 255);
    o.g = iclamp(G, 0, 255);
    o.b = iclamp(B, 0, 255);
    o.a = 255;
    if (alpha_mode == kAlphaConstant) o.a = alpha_value;
    else if (alpha_mode == kAlphaCoded)
        o.a = iclamp(planeAtDisplay(tp, 3, x, y), 0, 255);
    return o;
}

template <typename Store>
void run(const PassBInput &in, Store store) {
    TilePlanes tp;
    for (int ty = 0; ty < in.tilesY; ++ty)
        for (int tx = 0; tx < in.tilesX; ++tx) {
            int tile = ty * in.push.tilesX + tx;
            int alpha_mode = 0, alpha_value = 255;
            reconstruct_tile(in, tile, tp, alpha_mode, alpha_value);
            int ox = tx * 64, oy = ty * 64;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    int gx = ox + x, gy = oy + y;
                    if (gx >= in.push.imageW || gy >= in.push.imageH) continue;
                    store(gx, gy, tile_pixel(in, tp, alpha_mode, alpha_value, x, y));
                }
        }
}

}  // namespace

// ------------------------------------------------------------- exported
int model_dequant_step(int qp, int w) {
    return (kQStep[qp] * w + kQStepRound) >> kQStepShift;
}
int model_dequant(int q, int t) {
    return clamp16((q * t + kDequantRound) >> kDequantShift);
}
int model_bilinear_q4(const int *src, int w, int h, int stride, int sx, int sy) {
    return bilinear(src, w, h, stride, sx, sy);
}

void model_split_subblock(const int *dq, int ox, int oy, int *res) {
    split_subblock(dq, ox, oy, res);
}

void model_idct_nxn(const int *src, int *dst, int lb) {
    idct_nxn(src, dst, lb);
}

void model_idct8x8(const int src[64], int dst[64]) {
    int tmp[64];
    int in[8], out[8];
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = src[r * 8 + c];
        idct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            tmp[c * 8 + r] = clamp16((out[c] + kIdctRound1) >> kIdctShift1);
    }
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        idct8_1d(in, out);
        for (int c = 0; c < 8; ++c)
            dst[c * 8 + r] = clamp16((out[c] + kIdctRound2) >> kIdctShift2);
    }
}

void model_resolve_matrices(int quant_matrix, const uint8_t *custom128,
                            int *out128) {
    if (quant_matrix == 255 && custom128) {
        for (int i = 0; i < 64; ++i) {
            out128[i] = iclamp(custom128[i], 1, 32);
            out128[64 + i] = iclamp(custom128[64 + i], 1, 32);
        }
    } else {
        int m = iclamp(quant_matrix, 0, 3);
        int cm = (m == 0) ? 0 : 3;
        for (int i = 0; i < 64; ++i) {
            out128[i] = kWeightFlat[m * 64 + i];
            out128[64 + i] = kWeightFlat[cm * 64 + i];
        }
    }
}

// [inter] The reference-ring store: the tile's reconstruction in the coded
// sample domain at full tile extent.  It is the same `planeAtFull` resample
// the display store runs, clamped to the PLANE's own maxval rather than to 255
// -- a YCoCg-R chroma plane is 9-bit and the ring holds it as it is coded.
// [REF] codec_impl.inc store_ref_tile().
void passB_reconstruct_ref_tile(const PassBInput &in, int tile,
                                std::vector<int> out[4]) {
    TilePlanes tp;
    int alpha_mode = 0, alpha_value = 255;
    reconstruct_tile(in, tile, tp, alpha_mode, alpha_value);
    const int nplanes = in.push.alphaPresent != 0 ? 4 : 3;
    for (int p = 0; p < 4; ++p) {
        if (p >= nplanes) {
            out[p].clear();
            continue;
        }
        const bool chroma = (p == 1 || p == 2);
        const int full = tp.full[p];
        const bool ctChroma = (in.push.colorTransform == kCtYCoCgR) && chroma;
        const int maxval = ctChroma ? kMaxvalChromaCT : kMaxval8;
        const bool constAlpha = (p == 3) && (alpha_mode != kAlphaCoded);
        const int cval = (alpha_mode == kAlphaConstant) ? alpha_value : 255;
        out[p].assign((size_t)full * full, 0);
        for (int y = 0; y < full; ++y)
            for (int x = 0; x < full; ++x)
                out[p][(size_t)y * full + x] =
                    constAlpha ? cval
                               : iclamp(planeAtFull(tp, p, x, y), 0, maxval);
    }
}

void passB_reconstruct_rgba8(const PassBInput &in, uint8_t *out) {
    const int W = in.push.imageW;
    run(in, [&](int x, int y, Rgba p) {
        uint8_t *d = out + ((size_t)y * W + x) * 4;
        d[0] = (uint8_t)p.r;
        d[1] = (uint8_t)p.g;
        d[2] = (uint8_t)p.b;
        d[3] = (uint8_t)p.a;
    });
}

void passB_reconstruct_ycbcr420(const PassBInput &in, uint8_t *luma,
                                uint8_t *cbcr) {
    const int W = in.push.imageW, H = in.push.imageH;
    const int cw = (W + 1) >> 1, ch = (H + 1) >> 1;
    TilePlanes tp;
    for (int ty = 0; ty < in.tilesY; ++ty)
        for (int tx = 0; tx < in.tilesX; ++tx) {
            int tile = ty * in.push.tilesX + tx;
            int alpha_mode = 0, alpha_value = 255;
            reconstruct_tile(in, tile, tp, alpha_mode, alpha_value);
            int ox = tx * 64, oy = ty * 64;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    int gx = ox + x, gy = oy + y;
                    if (gx >= W || gy >= H) continue;
                    luma[(size_t)gy * W + gx] =
                        (uint8_t)iclamp(planeAtFull(tp, 0, x, y), 0, 255);
                }
            int cox = tx * 32, coy = ty * 32;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) {
                    int gx = cox + x, gy = coy + y;
                    if (gx >= cw || gy >= ch) continue;
                    cbcr[((size_t)gy * cw + gx) * 2 + 0] =
                        (uint8_t)iclamp(planeAtFull(tp, 1, x, y), 0, 255);
                    cbcr[((size_t)gy * cw + gx) * 2 + 1] =
                        (uint8_t)iclamp(planeAtFull(tp, 2, x, y), 0, 255);
                }
        }
}

void passB_reconstruct_rgb10a2(const PassBInput &in, uint32_t *out) {
    const int W = in.push.imageW;
    run(in, [&](int x, int y, Rgba p) {
        uint32_t r = (uint32_t)((p.r << 2) | (p.r >> 6));
        uint32_t g = (uint32_t)((p.g << 2) | (p.g >> 6));
        uint32_t b = (uint32_t)((p.b << 2) | (p.b >> 6));
        uint32_t a = (uint32_t)(p.a >> 6);
        out[(size_t)y * W + x] = r | (g << 10) | (b << 20) | (a << 30);
    });
}

}  // namespace nxvw
