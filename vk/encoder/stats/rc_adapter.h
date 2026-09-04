/* rc_adapter.h -- turning a GPU nxe_tile_stats record into the numbers the
 * CPU rate controller in `rc/` consumes.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The two sides deliberately have different shapes and this header is the
 * single place that reconciles them.
 *
 *   nxrc::TileStats is a struct of arrays of floats -- mean_luma, log_var,
 *   jxx/jxy/jyy -- because rc/ is a CPU library doing float reductions and
 *   log2 over a tile array, and that is the natural form for it.
 *
 *   nxe_tile_stats is an array of integer structs, one 52-byte record per
 *   tile, because it is written by a GPU workgroup and has to be bit-exact
 *   across vendors (paper 3.7).  Emitting floats from the shader would throw
 *   that away for nothing: log2 and the variance division are the consumer's
 *   business, and a float sum is not order-independent, so the diff harness
 *   could no longer prove the kernel right.
 *
 * So the GPU emits exact integer moments and this header does the two
 * conversions rc/ needs -- a division and a logarithm -- on the CPU, where
 * they are free and harmless.  Nothing downstream of rc/ is normative.
 *
 * ---------------------------------------------------------------------------
 * Agreement with nxrc::compute_one_tile_stats
 * ---------------------------------------------------------------------------
 * E1 uses the same operator and the same border policy as rc/'s own CPU model:
 * the central difference with the neighbour clamped inside the tile.  The one
 * difference is scale -- E1 accumulates the undivided difference, see the
 * gradient-operator commentary in tile_stats.h -- and nxe_rc_j_scale() undoes
 * it exactly.
 *
 * There is one case where the two legitimately disagree, and it is worth
 * naming rather than papering over: a tile that hangs off the right or bottom
 * edge of the frame.  rc/'s model averages over the pixels that are present;
 * E0 pads the tile by edge replication because that is what the *coder* has to
 * do -- the tile is coded as a full 64x64 block whatever the frame size.  The
 * records for such tiles carry NXE_TS_F_PADDED, and a consumer that cares can
 * either accept the replicated statistics (they are the statistics of the data
 * actually being coded, which is arguably the more useful answer) or skip
 * those tiles.
 */

#ifndef NXE_RC_ADAPTER_H
#define NXE_RC_ADAPTER_H

#include "tile_stats.h"

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Multiply a record's j_xx / j_xy / j_yy by this to get rc/'s convention
 * (sum of products of the halved central difference).
 *
 * E1 accumulates d*d where rc accumulates (d/2)*(d/2), so the factor is 1/4
 * exactly, at every depth.
 *
 * Depth is a separate matter the caller must not forget: 10-bit codes are on a
 * 4x larger scale than the 8-bit material rc/'s absolute gradient thresholds
 * were calibrated on, so a 10-bit caller has to scale flat_gradient and
 * text_gradient by 16 -- or classify on coherence, which is scale-free. */
static inline float nxe_rc_j_scale(uint32_t flags)
{
    (void)flags;
    return 0.25f;
}

/* The signed cross term, reassembled from the two unsigned sums.  Both are
 * exactly representable in a double, so the subtraction is exact. */
static inline double nxe_j_xy(const nxe_tile_stats *s)
{
    return (double)s->j_xy_pos - (double)s->j_xy_neg;
}

/* The five values nxrc::TileStats holds for one tile.  Fill rc/'s
 * struct-of-arrays from an array of records by calling this in a loop. */
typedef struct nxe_rc_tile_stats {
    float mean_luma;   /* mean of the luma plane over the tile              */
    float log_var;     /* log2(variance + 1), rc/'s activity term           */
    float jxx, jxy, jyy;
} nxe_rc_tile_stats;

static inline void nxe_to_rc_tile_stats(const nxe_tile_stats *in,
                                        nxe_rc_tile_stats *out)
{
    const float js = nxe_rc_j_scale(in->flags);
    const double n = (double)NXE_TILE_PIXELS;

    /* mean_luma_q8 is the rounded mean in Q8.8; recovering it as a float here
     * rather than dividing sum_luma keeps the two consumers of the record
     * (this and any GPU-side ladder) on the same rounded value. */
    out->mean_luma = (float)((double)in->mean_luma_q8 / 256.0);

    /* Variance from the exact integer sum of squared deviations.  E1 took the
     * deviations about the *rounded* integer mean, so this is the population
     * variance about that mean, which differs from the variance about the
     * exact mean by at most 1/4 -- far below the resolution of a log2 that
     * feeds a threshold at 3.0 and 11.0. */
    const double var = (double)in->sum_dev_sq / n;
    out->log_var = (float)log2(var + 1.0);

    out->jxx = (float)((double)in->j_xx * (double)js);
    out->jxy = (float)(nxe_j_xy(in) * (double)js);
    out->jyy = (float)((double)in->j_yy * (double)js);
}

/* Paper 4.6.1's gradient coherence: the normalised spread of the structure
 * tensor's eigenvalues, 0 for isotropic detail (texture), 1 for a single
 * dominant orientation (an edge or a glyph stroke).  Scale-free, so it can be
 * taken straight off the record without the j_scale above.
 *
 * lambda± = (tr ± sqrt(tr^2 - 4 det)) / 2, and coherence is
 * (lambda+ - lambda-) / (lambda+ + lambda-) = sqrt(tr^2 - 4 det) / tr. */
static inline float nxe_coherence(const nxe_tile_stats *s)
{
    const double xx = (double)s->j_xx, xy = nxe_j_xy(s), yy = (double)s->j_yy;
    const double tr = xx + yy;
    if (tr <= 0.0) return 0.0f;
    double disc = (xx - yy) * (xx - yy) + 4.0 * xy * xy;
    if (disc < 0.0) disc = 0.0;
    double c = sqrt(disc) / tr;
    if (c > 1.0) c = 1.0;
    return (float)c;
}

/* Paper 4.6's cplx_t numerator, the warped SAD, as a per-pixel mean so it is
 * comparable across tile sizes.  Returns 0 when no reference was bound. */
static inline float nxe_mean_sad(const nxe_tile_stats *s)
{
    if (!(s->flags & NXE_TS_F_SAD_VALID)) return 0.0f;
    return (float)((double)s->sad / (double)NXE_TILE_PIXELS);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NXE_RC_ADAPTER_H */
