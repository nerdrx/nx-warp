/* nxe_planar.h -- the piecewise-planar fit of [SYN] 13.13, in exact integers.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is the GPU encoder's copy of the reference's planar fit
 * (ref/src/codec_impl.inc, fit_planar and friends), and it has to agree with
 * it EXACTLY: byte-identity against `nxv-enc --planar` is the acceptance test,
 * the same way E1c_decide.comp is held to `--int-decision on`.
 *
 * It can be held to that because the fit is specified rather than
 * floating-point.  [SYN] 13.13's encoder notes give the four decisions and
 * this file implements them literally:
 *
 *   1. the normal equations are formed with CLEARED DENOMINATORS.  The basis
 *      is R(i)/size with R(i) = 2i - size + 1, so row 0 scales by size and
 *      rows 1 and 2 by size^2 and every entry is an exact integer;
 *   2. they are solved by CRAMER in 128-bit integers, and the only degenerate
 *      test is det == 0 -- singularity, not conditioning;
 *   3. the solution is rounded to Q8 IMMEDIATELY, half away from zero.  That
 *      is what keeps the reassignment step inside 64 bits: exact rationals
 *      over determinants of 2.6e22 would put a squared cell error near 2.6e54;
 *   4. every comparison is exact and a tie goes to the LOWEST region index, so
 *      the result does not depend on the order cells are swept in -- which is
 *      what lets a GPU do the reassignment a lane at a time.
 *
 * Header-only and dependency-free on purpose: the CPU model, the GPU host path
 * and the cross-check test all compile the same lines, so the only way they
 * can disagree is if one of them is not this file.
 *
 * BOUNDS (checkable, and the reason for each width): |R| <= size-1 <= 63,
 * N <= 4096, |e| <= 512; matrix entries <= 4096*63^2 = 1.63e7; det <=
 * 6*(1.63e7)^3 = 2.6e22; Cramer numerators <= 1.4e25, and 3.6e27 once shifted
 * for Q8 -- against __int128's 1.7e38.  After rounding a coefficient fits i32
 * and a cell's squared error over 64 samples reaches 2.6e16, inside i64.
 */
#ifndef NXE_PLANAR_H
#define NXE_PLANAR_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXE_PLANAR_MAX_REGIONS 4
#define NXE_PLANAR_MAX_CELLS 256   /* 16x16 at the fine granularity */
#define NXE_PLANAR_PLANES 3        /* Y, Co, Cg; alpha is never regionised */
#define NXE_PLANAR_FIT_FRAC 8      /* Q8, and the rounding is half away from 0 */

typedef struct {
    int regions;                   /* 2..4 */
    int fine;                      /* 0 = 8x8 cells, 1 = 4x4 */
    uint8_t labels[NXE_PLANAR_MAX_CELLS];
    int8_t coef[NXE_PLANAR_MAX_REGIONS][NXE_PLANAR_PLANES][3];
} nxe_planar_rec;

typedef struct {
    int32_t d0, dh, dv;            /* Q8 sample units */
} nxe_planar_fit;

/* One coded colour plane, as the fit sees it. */
typedef struct {
    const int32_t *samples;        /* size*size, row major */
    int size;
    int dc_off;
    int maxval;
    int qp;                        /* the TILE's qp; the step is derived */
} nxe_planar_plane;

static inline int nxe_planar_log2(int v) {
    int r = 0;
    while ((1 << r) < v) ++r;
    return r;
}

static inline int nxe_planar_ramp(int i, int size) { return 2 * i - size + 1; }

/* Q8, half away from zero.  The rule is part of the definition: rounding half
 * to even, or toward zero, would be a different encoder. */
static inline int64_t nxe_planar_round_q(__int128 num, __int128 den) {
    __int128 scaled, half, r;
    if (den == 0) return 0;
    if (den < 0) { num = -num; den = -den; }
    scaled = num << NXE_PLANAR_FIT_FRAC;
    half = den / 2;
    r = scaled >= 0 ? (scaled + half) / den : -((-scaled + half) / den);
    return (int64_t)r;
}

static inline __int128 nxe_planar_det3(__int128 a, __int128 b, __int128 c,
                                       __int128 d, __int128 e, __int128 f,
                                       __int128 g, __int128 h, __int128 i) {
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

/* Solve the cleared-denominator system.  false only when singular. */
static inline int nxe_planar_solve(const __int128 a[3][4], nxe_planar_fit *out) {
    __int128 det = nxe_planar_det3(a[0][0], a[0][1], a[0][2], a[1][0], a[1][1],
                                   a[1][2], a[2][0], a[2][1], a[2][2]);
    __int128 d0, d1, d2;
    if (det == 0) return 0;
    d0 = nxe_planar_det3(a[0][3], a[0][1], a[0][2], a[1][3], a[1][1], a[1][2],
                         a[2][3], a[2][1], a[2][2]);
    d1 = nxe_planar_det3(a[0][0], a[0][3], a[0][2], a[1][0], a[1][3], a[1][2],
                         a[2][0], a[2][3], a[2][2]);
    d2 = nxe_planar_det3(a[0][0], a[0][1], a[0][3], a[1][0], a[1][1], a[1][3],
                         a[2][0], a[2][1], a[2][3]);
    out->d0 = (int32_t)nxe_planar_round_q(d0, det);
    out->dh = (int32_t)nxe_planar_round_q(d1, det);
    out->dv = (int32_t)nxe_planar_round_q(d2, det);
    return 1;
}

/* Accumulate the normal equations over the samples whose map cell carries
 * `want`.  `labels` is the M*M map and `shift` maps a sample to its cell. */
static inline void nxe_planar_normal_eq(const nxe_planar_plane *p,
                                        const uint8_t *labels, int M, int shift,
                                        int want, __int128 a[3][4]) {
    int64_t N = 0, Sx = 0, Sy = 0, Sxx = 0, Sxy = 0, Syy = 0;
    int64_t Se = 0, Sxe = 0, Sye = 0;
    const int size = p->size;
    int x, y;
    for (y = 0; y < size; ++y) {
        const int ry = nxe_planar_ramp(y, size);
        for (x = 0; x < size; ++x) {
            int rx;
            int64_t e;
            if (labels[(size_t)(y >> shift) * M + (x >> shift)] != want) continue;
            rx = nxe_planar_ramp(x, size);
            e = (int64_t)p->samples[(size_t)y * size + x] - p->dc_off;
            N += 1;
            Sx += rx; Sy += ry;
            Sxx += (int64_t)rx * rx;
            Sxy += (int64_t)rx * ry;
            Syy += (int64_t)ry * ry;
            Se += e;
            Sxe += (int64_t)rx * e;
            Sye += (int64_t)ry * e;
        }
    }
    {
        const __int128 sz = size;
        a[0][0] = (__int128)N * sz;  a[0][1] = Sx;  a[0][2] = Sy;
        a[0][3] = (__int128)Se * sz;
        a[1][0] = (__int128)Sx * sz; a[1][1] = Sxx; a[1][2] = Sxy;
        a[1][3] = (__int128)Sxe * sz;
        a[2][0] = (__int128)Sy * sz; a[2][1] = Sxy; a[2][2] = Syy;
        a[2][3] = (__int128)Sye * sz;
    }
}

/* The squared error one region's plane leaves over one map cell.  Exact i64:
 * the residual is formed at scale size<<8 and every region carries the same
 * denominator, which is why the argmin needs no cross multiplication. */
static inline int64_t nxe_planar_cell_err(const nxe_planar_plane *p,
                                          const nxe_planar_fit *f, int cx,
                                          int cy, int cell) {
    const int size = p->size;
    int64_t e = 0;
    int x, y;
    for (y = cy * cell; y < (cy + 1) * cell; ++y) {
        const int64_t ry = nxe_planar_ramp(y, size);
        for (x = cx * cell; x < (cx + 1) * cell; ++x) {
            const int64_t rx = nxe_planar_ramp(x, size);
            const int64_t E = (int64_t)p->samples[(size_t)y * size + x] - p->dc_off;
            const int64_t num = E * size * (1 << NXE_PLANAR_FIT_FRAC) -
                                ((int64_t)f->d0 * size + (int64_t)f->dh * rx +
                                 (int64_t)f->dv * ry);
            e += num * num;
        }
    }
    return e;
}

/* The near-skip quantiser, in Q8: lvl = floor((32|q| + 256t) / (512t)). */
static inline int8_t nxe_planar_quant(int32_t v, int t) {
    const int64_t av = v < 0 ? -(int64_t)v : (int64_t)v;
    int64_t lvl = (32 * av + 256 * (int64_t)t) / (512 * (int64_t)t);
    if (lvl < 0) lvl = 0;
    if (lvl > 127) lvl = 127;
    return (int8_t)(v < 0 ? -lvl : lvl);
}

/* Fit one plane to a settled map, writing the three quantised bytes a region. */
static inline void nxe_planar_fit_plane(const nxe_planar_plane *p,
                                        const uint8_t *labels, int M,
                                        int regions, int dc_step,
                                        int8_t out[NXE_PLANAR_MAX_REGIONS][3]) {
    const int shift = nxe_planar_log2(p->size) - nxe_planar_log2(M);
    int r;
    for (r = 0; r < regions; ++r) {
        __int128 a[3][4];
        nxe_planar_fit f;
        memset(a, 0, sizeof a);
        memset(&f, 0, sizeof f);
        nxe_planar_normal_eq(p, labels, M, shift, r, a);
        if (!nxe_planar_solve(a, &f)) {
            /* Singular: an empty region, or one collinear in the basis.  Keep
             * the mean if there was one and no ramps -- the same fallback the
             * double form had, reached by an exact test. */
            f.d0 = f.dh = f.dv = 0;
            if (a[0][0] != 0)
                f.d0 = (int32_t)nxe_planar_round_q(a[0][3], a[0][0]);
        }
        out[r][0] = nxe_planar_quant(f.d0, dc_step);
        out[r][1] = nxe_planar_quant(f.dh, dc_step);
        out[r][2] = nxe_planar_quant(f.dv, dc_step);
    }
}

#ifdef __cplusplus
}
#endif
#endif /* NXE_PLANAR_H */
