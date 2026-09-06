/* nxe_planar_host.h -- fit a tile planar and serialise its body.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The part of [SYN] 13.13 that runs once per tile on the host: the k-means
 * seed, the k-planes refinement, the per-plane quantisation, and the body
 * bytes E5 copies out.  The arithmetic itself is nxe_planar.h, which the
 * REFERENCE includes too -- so this file is sequencing, not a second
 * transcription, and the only way it can disagree with `nxv-enc --planar` is
 * by sequencing differently.
 *
 * Header-only for the same reason nxe_planar.h is: the CPU model and the
 * Vulkan host path compile the same lines.
 */
#ifndef NXE_PLANAR_HOST_H
#define NXE_PLANAR_HOST_H

#include "nxe_planar.h"

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int nxe_planar_map_dim(int fine) { return fine ? 16 : 8; }
static inline int nxe_planar_label_bits(int regions) { return regions > 2 ? 2 : 1; }
static inline int nxe_planar_map_bytes(int regions, int fine) {
    const int M = nxe_planar_map_dim(fine);
    return (M * M * nxe_planar_label_bits(regions) + 7) / 8;
}
static inline int nxe_planar_body_bytes(int regions, int fine, int planes) {
    return 1 + nxe_planar_map_bytes(regions, fine) + 3 * regions * planes;
}


/* Fit one tile.  `planes[]` are the coded colour planes in Y, Co, Cg order.
 * Returns the reconstruction's squared error over them, which the caller
 * prices against the transform. */
static inline int64_t nxe_planar_fit_tile(const nxe_planar_plane *planes,
                                          int nplanes, int regions, int fine,
                                          nxe_planar_rec *pr) {
    const nxe_planar_plane *luma = &planes[0];
    const int M = nxe_planar_map_dim(fine);
    const int cell = luma->size / M;
    const int ncell = M * M;
    const int shift = nxe_planar_log2(luma->size) - nxe_planar_log2(M);
    int64_t cs[NXE_PLANAR_MAX_CELLS];
    int64_t sorted[NXE_PLANAR_MAX_CELLS];
    int64_t cnum[NXE_PLANAR_MAX_REGIONS], cden[NXE_PLANAR_MAX_REGIONS];
    nxe_planar_fit fits[NXE_PLANAR_MAX_REGIONS];
    int64_t A;
    int c, r, it, cx, cy, i, j, p;

    memset(pr, 0, sizeof *pr);
    pr->regions = regions;
    pr->fine = fine;

    /* Cell SUMS, not means: the divisor is the constant cell*cell, so ordering
     * the sums is ordering the means exactly and equal sums are equal means --
     * which is why the seeding needs no tie rule of its own. */
    for (cy = 0; cy < M; ++cy)
        for (cx = 0; cx < M; ++cx) {
            int64_t sum = 0;
            int x, y;
            for (y = cy * cell; y < (cy + 1) * cell; ++y)
                for (x = cx * cell; x < (cx + 1) * cell; ++x)
                    sum += luma->samples[(size_t)y * luma->size + x];
            cs[cy * M + cx] = sum;
        }
    A = (int64_t)cell * cell;
    memcpy(sorted, cs, (size_t)ncell * sizeof(int64_t));
    /* Insertion sort: ncell is at most 256 and this has to be the SAME order
     * on every implementation, which a library sort does not promise. */
    for (i = 1; i < ncell; ++i) {
        const int64_t v = sorted[i];
        for (j = i - 1; j >= 0 && sorted[j] > v; --j) sorted[j + 1] = sorted[j];
        sorted[j + 1] = v;
    }
    for (r = 0; r < regions; ++r) {
        cnum[r] = sorted[(2 * r + 1) * (ncell - 1) / (2 * regions)];
        cden[r] = A;
    }
    for (it = 0; it < 8; ++it) {
        int64_t sum[NXE_PLANAR_MAX_REGIONS] = {0, 0, 0, 0};
        int64_t cnt[NXE_PLANAR_MAX_REGIONS] = {0, 0, 0, 0};
        for (c = 0; c < ncell; ++c) {
            int best = 0;
            int64_t bn = 0, bd = 1;
            for (r = 0; r < regions; ++r) {
                int64_t n = cs[c] * cden[r] - cnum[r] * A;
                if (n < 0) n = -n;
                if (r == 0) { bn = n; bd = cden[r]; best = 0; continue; }
                /* Strictly less: a tie keeps the LOWER region index. */
                if (n * bd < bn * cden[r]) { bn = n; bd = cden[r]; best = r; }
            }
            pr->labels[c] = (uint8_t)best;
            sum[best] += cs[c];
            cnt[best] += 1;
        }
        for (r = 0; r < regions; ++r)
            if (cnt[r] > 0) { cnum[r] = sum[r]; cden[r] = cnt[r] * A; }
    }

    memset(fits, 0, sizeof fits);
    for (it = 0; it < 8; ++it) {
        int moved = 0;
        for (r = 0; r < regions; ++r) {
            __int128 a[3][4];
            nxe_planar_fit f;
            memset(a, 0, sizeof a);
            memset(&f, 0, sizeof f);
            nxe_planar_normal_eq(luma, pr->labels, M, shift, r, a);
            /* An empty or singular region keeps the plane it had: it costs its
             * three bytes and buys nothing, which the rate-distortion decision
             * then reads as a reason to prefer fewer regions. */
            if (nxe_planar_solve(a, &f)) fits[r] = f;
        }
        for (cy = 0; cy < M; ++cy)
            for (cx = 0; cx < M; ++cx) {
                int best = pr->labels[cy * M + cx];
                int64_t bd = 0;
                int first = 1;
                for (r = 0; r < regions; ++r) {
                    const int64_t e =
                        nxe_planar_cell_err(luma, &fits[r], cx, cy, cell);
                    if (first || e < bd) { bd = e; best = r; first = 0; }
                }
                if (best != pr->labels[cy * M + cx]) moved = 1;
                pr->labels[cy * M + cx] = (uint8_t)best;
            }
        if (!moved) break;
    }

    for (p = 0; p < nplanes && p < NXE_PLANAR_PLANES; ++p) {
        int8_t co[NXE_PLANAR_MAX_REGIONS][3];
        memset(co, 0, sizeof co);
        nxe_planar_fit_plane(&planes[p], pr->labels, M, regions,
                             planes[p].dc_step, co);
        for (r = 0; r < regions; ++r)
            for (i = 0; i < 3; ++i) pr->coef[r][p][i] = co[r][i];
    }

    /* The reconstruction's own error, by 13.13's decoding process. */
    {
        int64_t sse = 0;
        for (p = 0; p < nplanes && p < NXE_PLANAR_PLANES; ++p) {
            const nxe_planar_plane *pp = &planes[p];
            const int size = pp->size;
            const int lg = nxe_planar_log2(size);
            const int sh = lg - nxe_planar_log2(M);
            const int t = pp->dc_step;
            int x, y;
            for (y = 0; y < size; ++y)
                for (x = 0; x < size; ++x) {
                    const int rr = pr->labels[(size_t)(y >> sh) * M + (x >> sh)];
                    const int32_t d0 = (int32_t)(((int64_t)pr->coef[rr][p][0] * t + 8) >> 4);
                    const int32_t dh = (int32_t)(((int64_t)pr->coef[rr][p][1] * t + 8) >> 4);
                    const int32_t dv = (int32_t)(((int64_t)pr->coef[rr][p][2] * t + 8) >> 4);
                    int32_t v = pp->dc_off + d0 +
                                ((dh * (2 * x - size + 1)) >> lg) +
                                ((dv * (2 * y - size + 1)) >> lg);
                    int64_t e;
                    if (v < 0) v = 0;
                    if (v > pp->maxval) v = pp->maxval;
                    e = (int64_t)pp->samples[(size_t)y * size + x] - v;
                    sse += e * e;
                }
        }
        return sse;
    }
}

/* The body of 13.13, into `out` (at least NXE_PLANAR_BODY_UINTS*4 bytes).
 * Returns its length. */
static inline int nxe_planar_serialize(const nxe_planar_rec *pr, int planes,
                                       uint8_t *out) {
    const int M = nxe_planar_map_dim(pr->fine);
    const int lb = nxe_planar_label_bits(pr->regions);
    const int mb = nxe_planar_map_bytes(pr->regions, pr->fine);
    int c, r, k, p, n = 0;
    memset(out, 0, (size_t)nxe_planar_body_bytes(pr->regions, pr->fine, planes));
    out[n++] = (uint8_t)(((unsigned)(pr->regions - 2) & 3u) |
                         (((unsigned)pr->fine & 1u) << 3));
    for (c = 0; c < M * M; ++c) {
        const int bit = c * lb;
        out[1 + (bit >> 3)] |=
            (uint8_t)((pr->labels[c] & ((1 << lb) - 1)) << (bit & 7));
    }
    n = 1 + mb;
    for (r = 0; r < pr->regions; ++r)
        for (p = 0; p < planes && p < NXE_PLANAR_PLANES; ++p)
            for (k = 0; k < 3; ++k) out[n++] = (uint8_t)pr->coef[r][p][k];
    return n;
}

#ifdef __cplusplus
}
#endif
#endif /* NXE_PLANAR_HOST_H */
