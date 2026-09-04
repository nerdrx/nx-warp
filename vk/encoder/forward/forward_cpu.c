/* forward_cpu.c -- see forward_cpu.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "forward_cpu.h"

#include <string.h>

#include "nxe_tables.h"

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static int32_t clamp16(int32_t v) { return clamp_i32(v, -32768, 32767); }

/* ------------------------------------------------------------------ 1D DCT
 * ref/src/transform.cpp, unchanged including the two-word C4 identity.  The
 * split exists because the odd-part rotation operand reaches 8.6e7 on legal
 * int16 input and 8.6e7 * 362 leaves int32; `(s*362 + 256) >> 9` is computed
 * as hi*362 + ((lo*362 + 256) >> 9) with hi = s >> 9, which is the exact value
 * and never overflows.  The shader uses the identical split. */
static int32_t mul_c4_rnd9(int32_t s) {
    const int32_t hi = s >> 9, lo = s & 511;
    return hi * NXE_C4 + ((lo * NXE_C4 + 256) >> 9);
}

static void fdct8_1d(const int32_t *y, int32_t *x) {
    int32_t e0 = y[0] + y[7], O0 = y[0] - y[7];
    int32_t e1 = y[1] + y[6], O1 = y[1] - y[6];
    int32_t e2 = y[2] + y[5], O2 = y[2] - y[5];
    int32_t e3 = y[3] + y[4], O3 = y[3] - y[4];
    int32_t P = mul_c4_rnd9(O1 + O2);
    int32_t Q = mul_c4_rnd9(O1 - O2);
    int32_t A = O0 + P, C = O0 - P;
    int32_t B = O3 + Q, D = Q - O3;
    int32_t t0, t1, t2, t3;
    x[1] = A * NXE_A1 + B * NXE_A7;
    x[7] = A * NXE_A7 - B * NXE_A1;
    x[3] = C * NXE_A3 + D * NXE_A5;
    x[5] = C * NXE_A5 - D * NXE_A3;
    t0 = e0 + e3; t3 = e0 - e3;
    t1 = e1 + e2; t2 = e1 - e2;
    x[0] = (t0 + t1) * NXE_C4;
    x[4] = (t0 - t1) * NXE_C4;
    x[2] = t2 * NXE_S2 + t3 * NXE_C2;
    x[6] = t3 * NXE_S2 - t2 * NXE_C2;
}

static void idct8_1d(const int32_t *x, int32_t *y) {
    int32_t t0 = (x[0] + x[4]) * NXE_C4;
    int32_t t1 = (x[0] - x[4]) * NXE_C4;
    int32_t t2 = x[2] * NXE_S2 - x[6] * NXE_C2;
    int32_t t3 = x[2] * NXE_C2 + x[6] * NXE_S2;
    int32_t e0 = t0 + t3, e3 = t0 - t3;
    int32_t e1 = t1 + t2, e2 = t1 - t2;
    int32_t A = x[1] * NXE_A1 + x[7] * NXE_A7;
    int32_t B = x[1] * NXE_A7 - x[7] * NXE_A1;
    int32_t C = x[3] * NXE_A3 + x[5] * NXE_A5;
    int32_t D = x[3] * NXE_A5 - x[5] * NXE_A3;
    int32_t O0 = A + C;
    int32_t O3 = B - D;
    int32_t P = A - C, Q = B + D;
    int32_t O1 = mul_c4_rnd9(P + Q);
    int32_t O2 = mul_c4_rnd9(P - Q);
    y[0] = e0 + O0; y[7] = e0 - O0;
    y[1] = e1 + O1; y[6] = e1 - O1;
    y[2] = e2 + O2; y[5] = e2 - O2;
    y[3] = e3 + O3; y[4] = e3 - O3;
}

void nxe_fdct8x8(const int32_t src[64], int16_t dst[64]) {
    int32_t tmp[64], in[8], out[8];
    int r, c;
    for (r = 0; r < 8; ++r) {
        for (c = 0; c < 8; ++c) in[c] = src[r * 8 + c];
        fdct8_1d(in, out);
        for (c = 0; c < 8; ++c) tmp[c * 8 + r] = clamp16((out[c] + 32) >> 6);
    }
    for (r = 0; r < 8; ++r) {
        for (c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        fdct8_1d(in, out);
        for (c = 0; c < 8; ++c)
            dst[c * 8 + r] = (int16_t)clamp16((out[c] + 8192) >> 14);
    }
}

void nxe_idct8x8(const int32_t src[64], int32_t dst[64]) {
    int32_t tmp[64], in[8], out[8];
    int r, c;
    for (r = 0; r < 8; ++r) {
        for (c = 0; c < 8; ++c) in[c] = src[r * 8 + c];
        idct8_1d(in, out);
        for (c = 0; c < 8; ++c) tmp[c * 8 + r] = clamp16((out[c] + 64) >> 7);
    }
    for (r = 0; r < 8; ++r) {
        for (c = 0; c < 8; ++c) in[c] = tmp[r * 8 + c];
        idct8_1d(in, out);
        for (c = 0; c < 8; ++c) dst[c * 8 + r] = clamp16((out[c] + 4096) >> 13);
    }
}

int32_t nxe_bilinear_q4(const int32_t *src, int w, int h, int stride,
                        int32_t sx, int32_t sy) {
    int32_t x0 = sx >> 4, y0 = sy >> 4;
    int32_t fx = sx & 15, fy = sy & 15;
    int32_t x1 = x0 + 1, y1 = y0 + 1;
    int32_t p00, p01, p10, p11, wx0, wy0;
    x0 = clamp_i32(x0, 0, w - 1);
    x1 = clamp_i32(x1, 0, w - 1);
    y0 = clamp_i32(y0, 0, h - 1);
    y1 = clamp_i32(y1, 0, h - 1);
    p00 = src[y0 * stride + x0]; p01 = src[y0 * stride + x1];
    p10 = src[y1 * stride + x0]; p11 = src[y1 * stride + x1];
    wx0 = 16 - fx; wy0 = 16 - fy;
    return (p00 * wx0 * wy0 + p01 * fx * wy0 + p10 * wx0 * fy + p11 * fx * fy +
            128) >> 8;
}

/* -------------------------------------------------------------- quantizer */
int nxe_dequant_step(int qp, int w) { return (nxe_qstep[qp] * w + 8) >> 4; }
int32_t nxe_dequant(int32_t q, int32_t t) { return clamp16((q * t + 8) >> 4); }
int32_t nxe_quantize(int32_t c, int32_t t, int32_t dz) {
    int32_t a = c < 0 ? -c : c;
    int32_t q = (a * 16 + dz) / t;   /* encoder only; non-normative */
    if (q > 32767) q = 32767;
    return c < 0 ? -q : q;
}

static int dc_qp_of(int qp) { return qp >> 1; }

/* ------------------------------------------------------- sign data hiding
 *
 * The reference compares two candidate moves by
 *
 *     cost = e2*e2 - e1*e1,   e = a - m*step/16
 *
 * in doubles.  Multiply through by 256 -- a positive constant, so the ordering
 * is unchanged -- and every quantity is an integer:
 *
 *     256 * cost = E2*E2 - E1*E1,   E = 16*a - m*step
 *
 * and, because E2 - E1 = -d*step exactly (d = +-1),
 *
 *     256 * cost = (E2 - E1)(E2 + E1) = -d * step * (32*a - (2*m + d)*step)
 *
 * which is a product of two int32s and needs 64 bits only for the product
 * itself.  The model uses int64_t; the shader forms the same product with
 * imulExtended (core SPIR-V, no int64 type) and compares the two halves.
 * a <= 32767 and step <= 46340, so the product is bounded well inside int64.
 */
void nxe_hide_sign_unit(int16_t *coefs, const int32_t *orig, const int32_t *step,
                        int ncoef, const uint8_t *scan) {
    int last = -1;
    int32_t sum = 0;
    int p, want, best_p = -1, best_d = 0, idx;
    int64_t best = 0;
    int32_t q, m;
    for (p = 0; p < ncoef; ++p) {
        q = coefs[scan[p]];
        m = q < 0 ? -q : q;
        sum += m;
        if (m) last = p;
    }
    if (last < NXE_SDH_MIN_LAST) return;
    want = coefs[scan[last]] < 0 ? 1 : 0;
    if ((sum & 1) == want) return;
    for (p = 0; p <= last; ++p) {
        int d;
        int32_t a;
        idx = scan[p];
        a = orig[idx] < 0 ? -orig[idx] : orig[idx];
        q = coefs[idx];
        m = q < 0 ? -q : q;
        for (d = -1; d <= 1; d += 2) {
            int32_t m2 = m + d;
            int64_t cost;
            if (m2 < 0 || m2 > 32767) continue;
            if (p == last && m2 == 0) continue;
            cost = -(int64_t)d * (int64_t)step[idx] *
                   ((int64_t)32 * a - (int64_t)(2 * m + d) * step[idx]);
            if (best_p < 0 || cost < best) { best = cost; best_p = p; best_d = d; }
        }
    }
    if (best_p < 0) return;
    idx = scan[best_p];
    q = coefs[idx];
    m = (q < 0 ? -q : q) + best_d;
    {
        int neg = q != 0 ? (q < 0) : (orig[idx] < 0);
        coefs[idx] = (int16_t)(neg ? -m : m);
    }
}

/* ---------------------------------------------------------- the DC plane
 * analyze_dc_plane followed by reconstruct_dc_plane, which is the order the
 * reference uses: the encoder quantizes the block means, then reconstructs
 * exactly what the decoder will and predicts from that. */
static void nxe_dc_plane(const nxe_plane *pl, const int32_t *src, int16_t *coefs,
                         int32_t *pred) {
    const int nb = pl->nb, size = pl->size, ndc = nb * nb;
    int32_t m[64], orig[64], dc[64], means[64];
    int by, bx, i, y, x;
    const int dcqp = dc_qp_of(pl->qp);
    const int tdc = nxe_dequant_step(dcqp, 16);

    for (by = 0; by < nb; ++by)
        for (bx = 0; bx < nb; ++bx) {
            int32_t sum = 0, mean;
            int j;
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i)
                    sum += src[(size_t)(by * 8 + j) * size + bx * 8 + i];
            mean = sum >= 0 ? (sum + 32) >> 6 : -((-sum + 32) >> 6);
            m[by * nb + bx] = mean;
        }

    if (nb == 8) {
        int32_t in[64];
        int16_t out[64];
        for (i = 0; i < 64; ++i) in[i] = m[i] - pl->dc_off;
        nxe_fdct8x8(in, out);
        for (i = 0; i < 64; ++i) {
            orig[i] = out[i];
            coefs[i] = (int16_t)nxe_quantize(out[i], tdc, tdc / 3);
        }
    } else {
        for (i = 0; i < ndc; ++i) {
            orig[i] = m[i] - pl->dc_off;
            coefs[i] = (int16_t)nxe_quantize(orig[i], tdc, tdc / 3);
        }
    }
    if (pl->sdh) {
        int32_t stepv[64];
        for (i = 0; i < ndc; ++i) stepv[i] = tdc;
        nxe_hide_sign_unit(coefs, orig, stepv, ndc, nxe_scan_table(ndc, 0));
    }

    /* reconstruct_dc_plane */
    for (i = 0; i < ndc; ++i) dc[i] = nxe_dequant(coefs[i], tdc);
    if (nb == 8) {
        int32_t in[64], out[64];
        for (i = 0; i < 64; ++i) in[i] = dc[i];
        nxe_idct8x8(in, out);
        for (i = 0; i < 64; ++i) dc[i] = out[i];
    }
    for (i = 0; i < ndc; ++i)
        means[i] = clamp_i32(pl->dc_off + dc[i], 0, pl->maxval);
    for (y = 0; y < size; ++y)
        for (x = 0; x < size; ++x)
            pred[(size_t)y * size + x] =
                nxe_bilinear_q4(means, nb, nb, nb, 2 * x - 7, 2 * y - 7);
}

/* ------------------------------------------------------ residual + blocks */
static void quantize_block(const nxe_plane *pl, const int32_t res[64],
                           int16_t c[64]) {
    int32_t orig[64], stepv[64];
    int i;
    if (pl->tskip) {
        int t = nxe_dequant_step(pl->qp, 16);
        for (i = 0; i < 64; ++i) {
            orig[i] = res[i];
            stepv[i] = t;
            /* intra_dz is 1 on every call the reference makes for an intra
             * tile, so the dead zone is t/3 here as it is on the DC plane. */
            c[i] = (int16_t)nxe_quantize(res[i], t, t / 3);
        }
    } else {
        int16_t co[64];
        nxe_fdct8x8(res, co);
        for (i = 0; i < 64; ++i) {
            int t = nxe_dequant_step(pl->qp, pl->wmat[i]);
            orig[i] = co[i];
            stepv[i] = t;
            c[i] = (int16_t)nxe_quantize(co[i], t, t / 3);
        }
    }
    if (pl->sdh)
        nxe_hide_sign_unit(c, orig, stepv, 64, nxe_scan_table(64, pl->tskip));
}

/* residual_block: dequantize + inverse transform, the decoder's own path. */
static void residual_block(const nxe_plane *pl, const int16_t *c, int32_t res[64]) {
    int i;
    if (pl->tskip) {
        int t = nxe_dequant_step(pl->qp, 16);
        for (i = 0; i < 64; ++i) res[i] = nxe_dequant(c[i], t);
    } else {
        int32_t dq[64];
        for (i = 0; i < 64; ++i)
            dq[i] = nxe_dequant(c[i], nxe_dequant_step(pl->qp, pl->wmat[i]));
        nxe_idct8x8(dq, res);
    }
}

void nxe_e3_plane(const nxe_plane *pl, const int32_t *src, int16_t *coef,
                  int32_t *pred) {
    const int nb = pl->nb, size = pl->size, ndc = nb * nb;
    int16_t *bc = coef + ndc;
    int by, bx, i, j;
    nxe_dc_plane(pl, src, coef, pred);
    for (by = 0; by < nb; ++by)
        for (bx = 0; bx < nb; ++bx) {
            int16_t *c = bc + ((size_t)by * nb + bx) * 64;
            int32_t res[64];
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    res[j * 8 + i] = src[(size_t)y * size + x] -
                                     pred[(size_t)y * size + x];
                }
            quantize_block(pl, res, c);
        }
}

/* -------------------------------------------------- directional prediction
 * build_refs and predict_block, ref/src/codec.cpp, unchanged.  `dir_sched` is
 * fixed at 0: the full 8x8 raster dependency of SYNTAX.md 7.4 is the
 * conformant derivation and the shader implements it. */
typedef struct { int32_t a[17], l[17]; } nxe_refs;

static int32_t refs_at(const int32_t *recon, const int32_t *fallback, int size,
                       int bx, int by, int x, int y) {
    int cx = clamp_i32(x, 0, size - 1), cy = clamp_i32(y, 0, size - 1);
    int nbx = cx >> 3, nby = cy >> 3;
    int done = (nby < by) || (nby == by && nbx < bx);
    return (done ? recon : fallback)[(size_t)cy * size + cx];
}

static void nxe_build_refs(const int32_t *recon, const int32_t *fallback,
                           int size, int bx, int by, nxe_refs *r) {
    const int x0 = bx * 8, y0 = by * 8;
    int k;
    r->a[0] = r->l[0] = refs_at(recon, fallback, size, bx, by, x0 - 1, y0 - 1);
    for (k = 0; k < 16; ++k) {
        r->a[1 + k] = refs_at(recon, fallback, size, bx, by, x0 + k, y0 - 1);
        r->l[1 + k] = refs_at(recon, fallback, size, bx, by, x0 - 1, y0 + k);
    }
}

static void nxe_predict_block(int mode, const nxe_refs *r, const int32_t *base,
                              int size, int bx, int by, int32_t P[64]) {
    const int32_t *A = r->a + 1;
    const int32_t *L = r->l + 1;
    const int32_t tl = r->a[0];
    int i, j;
    switch (mode) {
        case 0:  /* kIntraDcPlane */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i)
                    P[j * 8 + i] = base[(size_t)(by * 8 + j) * size + bx * 8 + i];
            return;
        case 1: { /* kIntraDc */
            int32_t sum = 0, dc;
            for (i = 0; i < 8; ++i) sum += A[i] + L[i];
            dc = (sum + 8) >> 4;
            for (i = 0; i < 64; ++i) P[i] = dc;
            return;
        }
        case 2:  /* kIntraPlanar */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i)
                    P[j * 8 + i] = ((7 - i) * L[j] + (i + 1) * A[8] +
                                    (7 - j) * A[i] + (j + 1) * L[8] + 8) >> 4;
            return;
        case 3:  /* kIntraH */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) P[j * 8 + i] = L[j];
            return;
        case 4:  /* kIntraV */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) P[j * 8 + i] = A[i];
            return;
        case 5:  /* kIntraDdl */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) {
                    int k = i + j;
                    P[j * 8 + i] = (k == 14)
                        ? (A[14] + 3 * A[15] + 2) >> 2
                        : (A[k] + 2 * A[k + 1] + A[k + 2] + 2) >> 2;
                }
            return;
        case 6:  /* kIntraDdr */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) {
                    if (i > j) {
                        int k = i - j;
                        P[j * 8 + i] = (A[k - 2] + 2 * A[k - 1] + A[k] + 2) >> 2;
                    } else if (i < j) {
                        int k = j - i;
                        P[j * 8 + i] = (L[k - 2] + 2 * L[k - 1] + L[k] + 2) >> 2;
                    } else {
                        P[j * 8 + i] = (A[0] + 2 * tl + L[0] + 2) >> 2;
                    }
                }
            return;
        case 7:  /* kIntraVr */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) {
                    int z = 2 * i - j;
                    int k = i - (j >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        P[j * 8 + i] = (A[k - 1] + A[k] + 1) >> 1;
                    else if (z >= 0)
                        P[j * 8 + i] = (A[k - 2] + 2 * A[k - 1] + A[k] + 2) >> 2;
                    else if (z == -1)
                        P[j * 8 + i] = (L[0] + 2 * tl + A[0] + 2) >> 2;
                    else {
                        int q = j - 2 * i;
                        P[j * 8 + i] = (L[q - 1] + 2 * L[q - 2] + L[q - 3] + 2) >> 2;
                    }
                }
            return;
        default: /* kIntraHd */
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) {
                    int z = 2 * j - i;
                    int k = j - (i >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        P[j * 8 + i] = (L[k - 1] + L[k] + 1) >> 1;
                    else if (z >= 0)
                        P[j * 8 + i] = (L[k - 2] + 2 * L[k - 1] + L[k] + 2) >> 2;
                    else if (z == -1)
                        P[j * 8 + i] = (A[0] + 2 * tl + L[0] + 2) >> 2;
                    else {
                        int q = i - 2 * j;
                        P[j * 8 + i] = (A[q - 1] + 2 * A[q - 2] + A[q - 3] + 2) >> 2;
                    }
                }
            return;
    }
}

void nxe_e3_plane_dir(const nxe_plane *pl, const int32_t *src,
                      const uint8_t *modes, int layer, int16_t *coef,
                      int32_t *pred, int32_t *recon) {
    const int nb = pl->nb, size = pl->size, ndc = nb * nb;
    int16_t *bc = coef + ndc;
    int by, bx, i, j;
    static int32_t zero[NXE_TILE * NXE_TILE];
    const int32_t *fallback;

    nxe_dc_plane(pl, src, coef, pred);
    memset(recon, 0, (size_t)size * size * sizeof(int32_t));
    if (layer) {
        memset(zero, 0, (size_t)size * size * sizeof(int32_t));
        fallback = zero;
    } else {
        fallback = pred;
    }

    for (by = 0; by < nb; ++by)
        for (bx = 0; bx < nb; ++bx) {
            int16_t *c = bc + ((size_t)by * nb + bx) * 64;
            int32_t tgt[64], P[64], res[64], rr[64];
            nxe_refs r;
            nxe_build_refs(recon, fallback, size, bx, by, &r);
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    int32_t v = src[(size_t)y * size + x];
                    if (layer) v -= pred[(size_t)y * size + x];
                    tgt[j * 8 + i] = v;
                }
            nxe_predict_block(modes[(size_t)by * nb + bx], &r, fallback, size,
                              bx, by, P);
            for (i = 0; i < 64; ++i) res[i] = tgt[i] - P[i];
            quantize_block(pl, res, c);
            residual_block(pl, c, rr);
            for (j = 0; j < 8; ++j)
                for (i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    int32_t v = P[j * 8 + i] + rr[j * 8 + i];
                    if (layer) {
                        int32_t full = clamp_i32(pred[(size_t)y * size + x] + v,
                                                 0, pl->maxval);
                        recon[(size_t)y * size + x] =
                            full - pred[(size_t)y * size + x];
                    } else {
                        recon[(size_t)y * size + x] =
                            clamp_i32(v, 0, pl->maxval);
                    }
                }
        }
}

/* --------------------------------------------------------------- plumbing */
int nxe_plane_size(const nxe_frame_params *fp, const nxe_tile_job *job, int p) {
    int chroma = (p == 1 || p == 2);
    int sz;
    if (!chroma) return NXE_TILE >> job->res_level;
    sz = ((job->chroma444 ? NXE_TILE : NXE_TILE / 2) >> job->res_level);
    (void)fp;
    return sz < 8 ? 8 : sz;
}

int nxe_plane_coef_offset(const nxe_frame_params *fp, const nxe_tile_job *job,
                          int p) {
    int off = 0, i;
    for (i = 0; i < p; ++i) {
        int nb = nxe_plane_size(fp, job, i) / 8;
        off += nb * nb + nb * nb * 64;
    }
    return off;
}

void nxe_plane_setup(const nxe_frame_params *fp, const nxe_tile_job *job, int p,
                     nxe_plane *pl) {
    const int chroma = (p == 1 || p == 2);
    const int qp = clamp_i32((int)fp->base_qp + job->qp_delta, 0, 63);
    memset(pl, 0, sizeof *pl);
    pl->size = nxe_plane_size(fp, job, p);
    pl->nb = pl->size / 8;
    pl->qp = chroma ? clamp_i32(qp + fp->chroma_qp_off, 0, 63) : qp;
    {
        int i;
        for (i = 0; i < 64; ++i)
            pl->wmat[i] = job->wm_id == 0
                              ? (uint8_t)(chroma ? fp->wm_chroma[i]
                                                 : fp->wm_luma[i])
                              : nxe_weight[chroma ? 3 : (int)job->wm_id][i];
    }
    pl->maxval = (fp->ycocgr && chroma) ? 511 : 255;
    pl->dc_off = (fp->ycocgr && chroma) ? 256 : 128;
    pl->tskip = (int)job->tskip;
    pl->sdh = (int)fp->sdh;
    pl->ctx_level_dc = fp->nctx >= NXE_NCTX_V2 ? NXE_CTX_LEVEL_DC : 0;
}

void nxe_e3_tile(const nxe_frame_params *fp, const nxe_tile_job *job,
                 const int32_t *const src[NXE_MAX_PLANES], const uint8_t *modes,
                 int16_t *coef) {
    static int32_t pred[NXE_TILE * NXE_TILE];
    static int32_t recon[NXE_TILE * NXE_TILE];
    int p;
    for (p = 0; p < NXE_MAX_PLANES; ++p) {
        nxe_plane pl;
        int off = nxe_plane_coef_offset(fp, job, p);
        nxe_plane_setup(fp, job, p, &pl);
        if (fp->intra_dir)
            nxe_e3_plane_dir(&pl, src[p], modes + (size_t)p * 64,
                             (int)fp->dir_layer, coef + off, pred, recon);
        else
            nxe_e3_plane(&pl, src[p], coef + off, pred);
    }
}
