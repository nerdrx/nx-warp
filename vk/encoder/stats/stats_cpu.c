/* stats_cpu.c -- bit-exact CPU models of E0, E1 and E2.  See stats_cpu.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "stats_cpu.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ E0 */

void nxe_e0_convert_cpu(const uint32_t *src_rgba, uint32_t src_w, uint32_t src_h,
                        const nxe_frame_params *fp, int chroma_420,
                        uint32_t *dst, uint32_t dst_words)
{
    uint32_t cwords = chroma_420 ? (uint32_t)NXE_CHROMA_WORDS_420
                                 : (uint32_t)NXE_CHROMA_WORDS_444;
    uint32_t cside  = chroma_420 ? (uint32_t)NXE_CTILE_SIZE_420
                                 : (uint32_t)NXE_CTILE_SIZE_444;
    uint32_t ty, tx, j, i;

    memset(dst, 0, (size_t)dst_words * sizeof(uint32_t));

    for (ty = 0; ty < fp->tiles_y; ++ty) {
        for (tx = 0; tx < fp->tiles_x; ++tx) {
            uint32_t tile = ty * fp->tiles_x + tx;
            uint32_t ybase  = fp->plane_y_off  + tile * (uint32_t)NXE_LUMA_WORDS_PER_TILE;
            uint32_t cobase = fp->plane_co_off + tile * cwords;
            uint32_t cgbase = fp->plane_cg_off + tile * cwords;
            /* Whole 64x64 tile of YCoCg-R, kept so the 4:2:0 decimation reads
             * exactly the samples the shader's 4x4 quad had in registers. */
            int32_t Y[NXE_TILE_PIXELS], CO[NXE_TILE_PIXELS], CG[NXE_TILE_PIXELS];

            for (j = 0; j < (uint32_t)NXE_TILE_SIZE; ++j) {
                for (i = 0; i < (uint32_t)NXE_TILE_SIZE; ++i) {
                    /* Edge replication for frames that are not a multiple of 64. */
                    int32_t x = (int32_t)(tx * NXE_TILE_SIZE + i);
                    int32_t y = (int32_t)(ty * NXE_TILE_SIZE + j);
                    const uint32_t *px;
                    int32_t yy, co, cg;
                    if (x > (int32_t)src_w - 1) x = (int32_t)src_w - 1;
                    if (y > (int32_t)src_h - 1) y = (int32_t)src_h - 1;
                    if (x < 0) x = 0;
                    if (y < 0) y = 0;
                    px = src_rgba + ((size_t)y * src_w + (size_t)x) * 4u;
                    nxe_rgb_to_ycocgr((int32_t)px[0], (int32_t)px[1], (int32_t)px[2],
                                      &yy, &co, &cg);
                    Y[j * NXE_TILE_SIZE + i]  = yy;
                    CO[j * NXE_TILE_SIZE + i] = co;
                    CG[j * NXE_TILE_SIZE + i] = cg;
                }
            }

            for (j = 0; j < (uint32_t)NXE_TILE_PIXELS; ++j)
                nxe_plane_set(dst + ybase, (uint32_t)NXE_LUMA_WORDS_PER_TILE, j, Y[j]);

            if (chroma_420) {
                /* Fixed rounded 2x2 box (paper 1.3):
                 *   c = (c00 + c01 + c10 + c11 + 2) >> 2
                 * with an arithmetic shift, so a negative sum rounds half
                 * towards +infinity.  The asymmetry is normative: the
                 * decoder's half-phase bilinear upsample is specified against
                 * exactly this rule. */
                for (j = 0; j < cside; ++j) {
                    for (i = 0; i < cside; ++i) {
                        uint32_t k = (j * 2u) * NXE_TILE_SIZE + i * 2u;
                        int32_t so = CO[k] + CO[k + 1] + CO[k + NXE_TILE_SIZE]
                                   + CO[k + NXE_TILE_SIZE + 1];
                        int32_t sg = CG[k] + CG[k + 1] + CG[k + NXE_TILE_SIZE]
                                   + CG[k + NXE_TILE_SIZE + 1];
                        uint32_t ci = j * cside + i;
                        nxe_plane_set(dst + cobase, cwords, ci, nxe_sar(so + 2, 2));
                        nxe_plane_set(dst + cgbase, cwords, ci, nxe_sar(sg + 2, 2));
                    }
                }
            } else {
                for (j = 0; j < (uint32_t)NXE_TILE_PIXELS; ++j) {
                    nxe_plane_set(dst + cobase, cwords, j, CO[j]);
                    nxe_plane_set(dst + cgbase, cwords, j, CG[j]);
                }
            }
        }
    }
}

void nxe_e0_passthrough_cpu(const uint32_t *luma, const uint32_t *chroma,
                            uint32_t src_w, uint32_t src_h, int32_t mid,
                            const nxe_frame_params *fp,
                            uint32_t *dst, uint32_t dst_words)
{
    /* The compositor source is 4:2:0 by construction. */
    const uint32_t cwords = (uint32_t)NXE_CHROMA_WORDS_420;
    const uint32_t cside  = (uint32_t)NXE_CTILE_SIZE_420;
    const uint32_t cw = (src_w + 1u) >> 1, ch = (src_h + 1u) >> 1;
    uint32_t ty, tx, j, i;

    memset(dst, 0, (size_t)dst_words * sizeof(uint32_t));

    for (ty = 0; ty < fp->tiles_y; ++ty) {
        for (tx = 0; tx < fp->tiles_x; ++tx) {
            uint32_t tile = ty * fp->tiles_x + tx;
            uint32_t ybase  = fp->plane_y_off  + tile * (uint32_t)NXE_LUMA_WORDS_PER_TILE;
            uint32_t cobase = fp->plane_co_off + tile * cwords;
            uint32_t cgbase = fp->plane_cg_off + tile * cwords;

            for (j = 0; j < (uint32_t)NXE_TILE_SIZE; ++j) {
                for (i = 0; i < (uint32_t)NXE_TILE_SIZE; ++i) {
                    int32_t x = (int32_t)(tx * NXE_TILE_SIZE + i);
                    int32_t y = (int32_t)(ty * NXE_TILE_SIZE + j);
                    if (x > (int32_t)src_w - 1) x = (int32_t)src_w - 1;
                    if (y > (int32_t)src_h - 1) y = (int32_t)src_h - 1;
                    nxe_plane_set(dst + ybase, (uint32_t)NXE_LUMA_WORDS_PER_TILE,
                                  j * NXE_TILE_SIZE + i,
                                  (int32_t)luma[(size_t)y * src_w + (size_t)x]);
                }
            }

            for (j = 0; j < cside; ++j) {
                for (i = 0; i < cside; ++i) {
                    int32_t x = (int32_t)(tx * cside + i);
                    int32_t y = (int32_t)(ty * cside + j);
                    const uint32_t *p;
                    if (x > (int32_t)cw - 1) x = (int32_t)cw - 1;
                    if (y > (int32_t)ch - 1) y = (int32_t)ch - 1;
                    p = chroma + ((size_t)y * cw + (size_t)x) * 2u;
                    nxe_plane_set(dst + cobase, cwords, j * cside + i,
                                  (int32_t)p[0] - mid);
                    nxe_plane_set(dst + cgbase, cwords, j * cside + i,
                                  (int32_t)p[1] - mid);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ E1 */

static int32_t fetch_ref_cpu(const uint32_t *plane, uint32_t plane_words,
                             const nxe_frame_params *fp, int32_t x, int32_t y)
{
    uint32_t t, i, idx, half, w;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > (int32_t)fp->width - 1)  x = (int32_t)fp->width - 1;
    if (y > (int32_t)fp->height - 1) y = (int32_t)fp->height - 1;
    t = ((uint32_t)y >> 6) * fp->tiles_x + ((uint32_t)x >> 6);
    i = (uint32_t)((y & 63) * NXE_TILE_SIZE + (x & 63));
    idx = fp->plane_y_off + t * (uint32_t)NXE_LUMA_WORDS_PER_TILE + (i >> 1);
    half = i & 1u;
    w = (idx < plane_words) ? plane[idx] : 0u;
    return (int32_t)((int16_t)((w >> (half * 16u)) & 0xffffu));
}

void nxe_e1_stats_cpu(const uint32_t *plane, uint32_t plane_words,
                      const nxe_frame_params *fp,
                      const uint32_t *ref_plane,
                      const int32_t *mv_q, int has_ref,
                      nxe_tile_stats *out, uint32_t num_tiles)
{
    uint32_t tile;
    int32_t *lum = (int32_t *)malloc(sizeof(int32_t) * NXE_TILE_PIXELS);

    for (tile = 0; tile < num_tiles; ++tile) {
        uint32_t ybase = fp->plane_y_off + tile * (uint32_t)NXE_LUMA_WORDS_PER_TILE;
        uint32_t tx = tile % fp->tiles_x;
        uint32_t ty = tile / fp->tiles_x;
        int32_t org_x = (int32_t)tx * NXE_TILE_SIZE;
        int32_t org_y = (int32_t)ty * NXE_TILE_SIZE;
        int32_t mvqx = mv_q ? mv_q[tile * 2 + 0] : 0;
        int32_t mvqy = mv_q ? mv_q[tile * 2 + 1] : 0;
        /* Truncation towards zero, matching GLSL's OpSDiv. */
        int32_t mvix = mvqx / 4, mviy = mvqy / 4;
        /* Signed sums are accumulated through uint32_t so that overflow wraps
         * two's-complement here exactly as SPIR-V's OpIAdd does, instead of
         * being undefined behaviour in C. */
        uint32_t sum = 0, sumsq = 0, sad = 0;
        uint32_t jxx = 0, jxy_p = 0, jxy_n = 0, jyy = 0, dev = 0;
        int32_t mean_int;
        uint32_t p, flags;
        nxe_tile_stats *o = &out[tile];

        for (p = 0; p < (uint32_t)NXE_TILE_PIXELS; ++p)
            lum[p] = nxe_plane_get(plane + ybase,
                                   (plane_words > ybase) ? plane_words - ybase : 0u, p);

        for (p = 0; p < (uint32_t)NXE_TILE_PIXELS; ++p) {
            int32_t px = (int32_t)(p & 63u);
            int32_t py = (int32_t)(p >> 6);
            int32_t v = lum[p];
            int32_t xm = px > 0 ? px - 1 : 0, xp = px < 63 ? px + 1 : 63;
            int32_t ym = py > 0 ? py - 1 : 0, yp = py < 63 ? py + 1 : 63;
            /* Central difference, clamped inside the tile: the operator
             * nxrc::compute_one_tile_stats() uses. */
            int32_t gx = lum[py * NXE_TILE_SIZE + xp] - lum[py * NXE_TILE_SIZE + xm];
            int32_t gy = lum[yp * NXE_TILE_SIZE + px] - lum[ym * NXE_TILE_SIZE + px];

            sum   += (uint32_t)v;
            sumsq += (uint32_t)(v * v);

            {
                int32_t xy = gx * gy;
                jxx += (uint32_t)(gx * gx);
                if (xy > 0) jxy_p += (uint32_t)xy;
                else if (xy < 0) jxy_n += (uint32_t)(-xy);
                jyy += (uint32_t)(gy * gy);
            }

            if (has_ref) {
                int32_t r = fetch_ref_cpu(ref_plane, plane_words, fp,
                                          org_x + px + mvix, org_y + py + mviy);
                int32_t d = v - r;
                sad += (uint32_t)(d < 0 ? -d : d);
            }
        }

        mean_int = (int32_t)((sum + 2048u) >> 12);
        for (p = 0; p < (uint32_t)NXE_TILE_PIXELS; ++p) {
            int32_t d = lum[p] - mean_int;
            dev += (uint32_t)(d * d);
        }

        flags = fp->flags;
        if (has_ref) flags |= NXE_TS_F_SAD_VALID;
        if ((uint32_t)(org_x + NXE_TILE_SIZE) > fp->width ||
            (uint32_t)(org_y + NXE_TILE_SIZE) > fp->height)
            flags |= NXE_TS_F_PADDED;

        o->sum_luma     = sum;
        o->sum_sq_luma  = sumsq;
        o->mean_luma_q8 = (sum + 8u) >> 4;
        o->sum_dev_sq   = dev;
        o->j_xx         = jxx;
        o->j_xy_pos     = jxy_p;
        o->j_xy_neg     = jxy_n;
        o->j_yy         = jyy;
        o->sad          = sad;
        o->mv_qx        = mvqx;
        o->mv_qy        = mvqy;
        o->mv_mag_q4    = nxe_isqrt((uint32_t)(mvqx * mvqx + mvqy * mvqy));
        o->flags        = flags;
    }

    free(lum);
}

/* ------------------------------------------------------------------------ E2 */

uint32_t nxe_e2_prefix_cpu(const uint32_t *in, uint32_t *out, uint32_t n)
{
    uint32_t run = 0, i;
    for (i = 0; i < n; ++i) {
        out[i] = run;
        run += in[i];   /* wraps mod 2^32, as OpIAdd does */
    }
    return run;
}

/* ------------------------------------------------------------------ selftest */

int nxe_stats_selftest(void)
{
    int32_t neg = -5;
    if (nxe_sar(neg, 1) != -3) return -1;             /* arithmetic shift    */
    if (nxe_sar(-1, 3) != -1) return -1;
    if ((-7) / 4 != -1) return -2;                    /* division truncates  */
    if (nxe_isqrt(0u) != 0u) return -3;
    if (nxe_isqrt(1u) != 1u) return -3;
    if (nxe_isqrt(15u) != 3u) return -3;
    if (nxe_isqrt(16u) != 4u) return -3;
    if (nxe_isqrt(0xffffffffu) != 65535u) return -3;
    {   /* YCoCg-R round trips exactly over the whole 8-bit cube corners */
        int32_t r, g, b, y, co, cg, t, r2, g2, b2;
        for (r = 0; r <= 255; r += 17)
            for (g = 0; g <= 255; g += 17)
                for (b = 0; b <= 255; b += 17) {
                    nxe_rgb_to_ycocgr(r, g, b, &y, &co, &cg);
                    t = y - nxe_sar(cg, 1);
                    g2 = cg + t;
                    b2 = t - nxe_sar(co, 1);
                    r2 = co + b2;
                    if (r2 != r || g2 != g || b2 != b) return -4;
                }
    }
    if (sizeof(nxe_tile_stats) != NXE_TILE_STATS_SIZE) return -5;
    return 0;
}
