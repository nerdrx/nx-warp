/* nxe_trellis.c -- see nxe_trellis.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Transcribed from ref/src/codec.cpp's rdoq_unit_int and hide_sign_unit_int,
 * line for line, because the reference is the specification of what this
 * encoder produces and a transcription is checkable where a reimplementation
 * is not.
 */

#include "nxe_trellis.h"

#include "nxe_ctx.h"
#include "nxe_rate.h"
#include "nxe_tables.h"
#include "forward_cpu.h"

/* last_shift_of: a 16x16 or 32x32 unit reuses the 64-position LAST classes and
 * the four LEVEL bands by naming a scan GROUP.  ref/src/common.h. */
static int nxe_last_shift_of(int ncoef) {
    int s = 0;
    while ((ncoef >> s) > 64) ++s;
    return s;
}

/* The largest unit the format has: a 32x32 block. */
#define NXE_TRELLIS_MAX_COEF 1024
#define NXE_TRELLIS_INF ((int64_t)0x1FFFFFFFFFFFFFFFll)

void nxe_build_rate_cost(const uint32_t *freq, int nctx, nxe_rate_cost *rc) {
    int c, s;
    for (c = 0; c < NXE_MAX_CTX; ++c)
        for (s = 0; s < NXE_NUM_SYM; ++s) {
            uint32_t f = c < nctx ? freq[c * NXE_NUM_SYM + s] : 0;
            if (f == 0) f = 1;                 /* ref: f <= 0 -> 1/kProbTotal */
            if (f > NXE_PROB_TOTAL) f = NXE_PROB_TOTAL;
            rc->sym[c][s] = (int32_t)nxe_neglog2_q10[f];
        }
    rc->zero_cheapest = 1;
    for (c = 0; c < nctx; ++c)
        if (rc->sym[c][1] < rc->sym[c][0]) rc->zero_cheapest = 0;
}

uint32_t nxe_trellis_lambda_q8(int qp) {
    const uint32_t t = nxe_qstep[qp < 0 ? 0 : (qp > 63 ? 63 : qp)];
    return (uint32_t)(((uint64_t)NXE_TRELLIS_LAM_Q12 * (uint64_t)(t * t)) >> 12);
}

nxe_unit_ctx nxe_block_ctx(int nctx, int chroma, int ncoef) {
    nxe_unit_ctx u;
    u.ucls = chroma ? NXE_UCLS_CHROMA : NXE_UCLS_LUMA;
    u.v3 = nctx >= NXE_NCTX_V3 ? 1 : 0;
    u.cbf = chroma ? NXE_CTX_CBF_CHROMA : NXE_CTX_CBF_LUMA;
    u.last = chroma ? NXE_CTX_LAST_CHROMA : NXE_CTX_LAST_LUMA;
    u.level_fixed = NXE_CTX_NONE;
    u.band_shift = nxe_last_shift_of(ncoef);
    return u;
}

nxe_unit_ctx nxe_dc_ctx(int nctx, int ncoef) {
    nxe_unit_ctx u;
    u.ucls = NXE_UCLS_DC;
    u.v3 = nctx >= NXE_NCTX_V3 ? 1 : 0;
    u.cbf = nctx >= NXE_NCTX_V2 ? NXE_CTX_CBF_DC : NXE_CTX_CBF_LUMA;
    u.last = nctx >= NXE_NCTX_V2 ? NXE_CTX_LAST_DC : NXE_CTX_LAST_LUMA;
    u.level_fixed = nctx >= NXE_NCTX_V2 ? NXE_CTX_LEVEL_DC : NXE_CTX_NONE;
    u.band_shift = nxe_last_shift_of(ncoef);
    return u;
}

nxe_unit_ctx nxe_unit_ctx_nbr(nxe_unit_ctx u, int nbr) {
    if (!u.v3) return u;
    u.cbf = nxe_v3_ctx_cbf(u.ucls, nbr);
    u.last = nxe_v3_ctx_last(u.ucls, nbr);
    return u;
}

int nxe_unit_nbr_class(const int16_t *c, int ncoef, const uint8_t *scan) {
    int last = -1, p;
    for (p = ncoef - 1; p >= 0; --p)
        if (c[scan[p]] != 0) { last = p; break; }
    return nxe_nbr_class_of(last < 0 ? 0 : 1, last < 0 ? 0 : last);
}

/* A DC-plane unit's LEVEL context does not depend on the previous level -- one
 * fixed row under v2, one row plus a separate DC term under v3 -- so its
 * trellis has no Markov chain and its three states collapse to one. */
static int uc_level_markov(const nxe_unit_ctx *uc) {
    if (uc->ucls == NXE_UCLS_DC) return 0;
    return uc->level_fixed == NXE_CTX_NONE;
}

static int uc_level(const nxe_unit_ctx *uc, int scan_pos, int last,
                    int prev_class) {
    /* band_pos: with neither XFORM_4X4_SPLIT nor XFORM_LARGE implemented here
     * the band position is the scan position shifted by band_shift. */
    const int bp = scan_pos >> uc->band_shift;
    if (uc->v3) return nxe_v3_ctx_level(uc->ucls, scan_pos, bp, last, prev_class);
    return uc->level_fixed != NXE_CTX_NONE ? uc->level_fixed
                                           : nxe_level_ctx(bp, prev_class);
}

/* Bypass bits an escape suffix costs for magnitude m >= 15, matching
 * eg3_encode exactly. */
static int escape_bits(int32_t m) {
    uint32_t n = (uint32_t)(m - 15) + 8u;
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    return (b - NXE_ESC_ORDER) + 1 + b;
}

static int32_t level_rate(const nxe_rate_cost *rc, const nxe_unit_ctx *uc,
                          int scan_pos, int last, int prev_class, int32_t m) {
    const int ctx = uc_level(uc, scan_pos, last, prev_class);
    const int32_t sym = m > 14 ? NXE_ESC_SYM : m;
    int32_t r = rc->sym[ctx][sym];
    if (m > 14) r += escape_bits(m) << 10;
    if (m != 0) r += 1 << 10;   /* sign, one bypass bit */
    return r;
}

void nxe_rdoq_unit_int(int16_t *coefs, const int32_t *orig, const int32_t *step,
                       int ncoef, const uint8_t *scan, const nxe_unit_ctx *uc,
                       const nxe_rate_cost *rc, uint32_t lam_q8, int effort,
                       int sdh) {
    static int64_t f[NXE_TRELLIS_MAX_COEF][3], fnz[NXE_TRELLIS_MAX_COEF];
    static int32_t best_m[NXE_TRELLIS_MAX_COEF][3];
    static int32_t best_m_nz[NXE_TRELLIS_MAX_COEF];
    int64_t prev[3] = {0, 0, 0};
    int64_t energy = 0, tail = 0, best_total;
    int hi = -1, p, sc, best_last = -1;

    for (p = 0; p < ncoef; ++p) coefs[p] = 0;

    for (p = 0; p < ncoef; ++p) {
        const int idx = scan[p];
        const int64_t c = orig[idx];
        const int64_t a = c < 0 ? -c : c;
        energy += c * c;
        if (32 * a >= (int64_t)step[idx]) hi = p;
    }
    if (hi < 0 || !rc->zero_cheapest) hi = hi < 0 ? -1 : ncoef - 1;
    if (hi < 0) return;

    for (p = 0; p <= hi; ++p) {
        const int idx = scan[p];
        const int32_t c = orig[idx];
        const int32_t a = c < 0 ? -c : c;
        const int32_t st = step[idx];
        int32_t cand[4];
        int nc = 0, k;
        int32_t m0 = st > 0 ? (int32_t)(((int64_t)a * 16) / st) : 0;
        if (m0 > 32767) m0 = 32767;
        cand[nc++] = 0;
        if (effort == NXE_RDOQ_FAST) {
            int32_t mn =
                st > 0 ? (int32_t)(((int64_t)a * 32 + st) / (2 * (int64_t)st))
                       : 0;
            if (mn > 32767) mn = 32767;
            if (mn > 0) cand[nc++] = mn;
        } else {
            if (effort >= NXE_RDOQ_FULL && m0 >= 2) cand[nc++] = m0 - 1;
            if (m0 > 0) cand[nc++] = m0;
            if (m0 + 1 <= 32767) cand[nc++] = m0 + 1;
        }
        for (sc = 0; sc < 3; ++sc) {
            int64_t best = NXE_TRELLIS_INF, bestnz = NXE_TRELLIS_INF;
            int32_t bm = 0, bmnz = -1;
            for (k = 0; k < nc; ++k) {
                const int32_t m = cand[k];
                const int64_t d = (int64_t)a - nxe_dequant(m, st);
                const int64_t dd = (d * d) << 18;
                const int64_t chain =
                    prev[uc_level_markov(uc) ? nxe_level_class(m) : 0];
                const int64_t cost =
                    dd + (int64_t)lam_q8 * level_rate(rc, uc, p, -1, sc, m) +
                    chain;
                if (cost < best) { best = cost; bm = m; }
                if (m != 0) {
                    const int64_t cnz =
                        dd + (int64_t)lam_q8 * level_rate(rc, uc, p, p, sc, m) +
                        chain;
                    if (cnz < bestnz) { bestnz = cnz; bmnz = m; }
                }
            }
            f[p][sc] = best;
            best_m[p][sc] = bm;
            if (sc == 0) { fnz[p] = bestnz; best_m_nz[p] = bmnz; }
        }
        for (sc = 0; sc < 3; ++sc) prev[sc] = f[p][sc];
    }

    best_total = (int64_t)lam_q8 * rc->sym[uc->cbf][0] + (energy << 18);
    for (p = ncoef - 1; p > hi; --p) {
        const int64_t c = orig[scan[p]];
        tail += c * c;
    }
    for (p = hi; p >= 0; --p) {
        if (fnz[p] < NXE_TRELLIS_INF) {
            int32_t r = rc->sym[uc->cbf][1];
            int64_t total;
            if (ncoef > 1) {
                const int cls = nxe_last_class_of(p >> uc->band_shift);
                r += rc->sym[uc->last][cls] +
                     ((nxe_last_raw_bits[cls] + uc->band_shift) << 10);
            }
            if (sdh && p >= NXE_SDH_MIN_LAST) r -= 1 << 10;
            total = fnz[p] + (tail << 18) + (int64_t)lam_q8 * r;
            if (total < best_total) { best_total = total; best_last = p; }
        }
        {
            const int64_t c = orig[scan[p]];
            tail += c * c;
        }
    }

    if (best_last < 0) return;
    sc = 0;
    for (p = best_last; p >= 0; --p) {
        const int32_t m = (p == best_last) ? best_m_nz[p] : best_m[p][sc];
        const int idx = scan[p];
        coefs[idx] = (int16_t)(orig[idx] < 0 ? -m : m);
        sc = uc_level_markov(uc) ? nxe_level_class(m) : 0;
    }
}

void nxe_hide_sign_unit_int(int16_t *coefs, const int32_t *orig,
                            const int32_t *step, int ncoef, const uint8_t *scan,
                            const nxe_rate_cost *rc, uint32_t lam_q8,
                            const nxe_unit_ctx *uc) {
    int last = -1, p, want, best_p = -1, best_d = 0;
    int32_t sum = 0;
    int64_t best = 0;
    for (p = 0; p < ncoef; ++p) {
        const int32_t q = coefs[scan[p]];
        const int32_t m = q < 0 ? -q : q;
        sum += m;
        if (m) last = p;
    }
    if (last < NXE_SDH_MIN_LAST) return;
    want = coefs[scan[last]] < 0 ? 1 : 0;
    if ((sum & 1) == want) return;
    for (p = 0; p <= last; ++p) {
        const int idx = scan[p];
        const int32_t a = orig[idx] < 0 ? -orig[idx] : orig[idx];
        const int32_t st = step[idx];
        const int32_t q = coefs[idx];
        const int32_t m = q < 0 ? -q : q;
        int d;
        for (d = -1; d <= 1; d += 2) {
            const int32_t m2 = m + d;
            int64_t e1, e2, cost;
            if (m2 < 0 || m2 > 32767) continue;
            if (p == last && m2 == 0) continue;
            e1 = (int64_t)a - nxe_dequant(m, st);
            e2 = (int64_t)a - nxe_dequant(m2, st);
            cost = (e2 * e2 - e1 * e1) << 18;
            if (rc && lam_q8) {
                const int cx = uc ? uc_level(uc, p, last, 0)
                                  : nxe_level_ctx(p, 0);
                const int32_t r1 = rc->sym[cx][m > 14 ? NXE_ESC_SYM : m] +
                                   (m > 14 ? (escape_bits(m) << 10) : 0) +
                                   (m != 0 ? (1 << 10) : 0);
                const int32_t r2 = rc->sym[cx][m2 > 14 ? NXE_ESC_SYM : m2] +
                                   (m2 > 14 ? (escape_bits(m2) << 10) : 0) +
                                   (m2 != 0 ? (1 << 10) : 0);
                cost += (int64_t)lam_q8 * (r2 - r1);
            }
            if (best_p < 0 || cost < best) {
                best = cost;
                best_p = p;
                best_d = d;
            }
        }
    }
    if (best_p < 0) return;
    {
        const int idx = scan[best_p];
        const int32_t q = coefs[idx];
        const int32_t m = (q < 0 ? -q : q) + best_d;
        const int neg = q != 0 ? (q < 0) : (orig[idx] < 0);
        coefs[idx] = (int16_t)(neg ? -m : m);
    }
}
