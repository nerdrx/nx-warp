/* nxe_trellis.h -- the rate-distortion trellis, in exact integers.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `ref/src/codec.cpp`'s `rdoq_unit_int` and `hide_sign_unit_int`, transcribed,
 * and the encoder's effort level 2.  The reference is the specification: a
 * stream this produces must be the stream `nxv-enc --int-trellis 1
 * --rdoq-effort 3` produces at the matching flags, byte for byte, and
 * `vk.encoder.acid.trellis` requires it.
 *
 * Everything here is integer and nothing here divides.  A candidate's cost is
 *
 *     (d * d) << 18   +   lam_q8 * rate_q10
 *
 * where `d` is `orig - dequant(m, step)` -- the decoder's own reconstruction,
 * not a float approximation of it -- and `rate_q10` comes from a table built
 * once per table set with `nxe_neglog2_q10`.  There is no epsilon and no
 * tolerance: two implementations that perform these adds in this order reach
 * the same levels or one of them has a bug.  That is what lets the shader and
 * this file be checked against each other at all.
 */

#ifndef NXE_TRELLIS_H
#define NXE_TRELLIS_H

#include <stdint.h>

#include "nxe_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Q10 bits per (context, symbol), and whether a zero is never dearer than a
 * one in any context of the table.
 *
 * `zero_cheapest` carries the `hi` bound: above the highest position whose
 * magnitude reaches half a step, level 0 beats level 1 in distortion AND in
 * rate, so those positions are provably zero and are never searched.  The
 * distortion half is an identity; the rate half is a property of the TABLE,
 * and a transmitted table set is whatever the frame chose to send -- so it is
 * checked once per table rather than assumed, and the bound is dropped if it
 * does not hold.  A slower trellis, not a wrong one. */
typedef struct nxe_rate_cost {
    int32_t sym[NXE_MAX_CTX][NXE_NUM_SYM];
    int zero_cheapest;
} nxe_rate_cost;

/* `freq` is `&tabs->freq[set][0][0]`, the plane E4 reads, row stride
 * NXE_NUM_SYM.  Mirrors ref's build_rate_cost with the libm removed: the
 * reference computes `(i32)(-log2(f) * 1024 + 0.5)` and `nxe_neglog2_q10` is
 * that function tabulated, which agrees with it for all 1024 frequencies. */
void nxe_build_rate_cost(const uint32_t *freq, int nctx, nxe_rate_cost *rc);

/* One coding unit's contexts, ref's UnitCtx reduced to what the trellis reads.
 * `nbr` is applied by the caller through nxe_unit_ctx_nbr. */
typedef struct nxe_unit_ctx {
    int cbf;
    int last;
    int level_fixed;   /* NXE_CTX_NONE: the banded LEVEL contexts */
    int ucls;
    int v3;
    int band_shift;    /* last_shift_of(ncoef) */
} nxe_unit_ctx;

nxe_unit_ctx nxe_block_ctx(int nctx, int chroma, int ncoef);
nxe_unit_ctx nxe_dc_ctx(int nctx, int ncoef);
/* v3 shifts CBF and LAST by the lane's neighbour class; nothing else moves. */
nxe_unit_ctx nxe_unit_ctx_nbr(nxe_unit_ctx u, int nbr);
/* The class a finished unit publishes to its lane. */
int nxe_unit_nbr_class(const int16_t *c, int ncoef, const uint8_t *scan);

/* The trellis's lambda: `(901 * t * t) >> 12` over the Q4 quantiser step.  The
 * requantiser's integer family with ref's own rate-distortion constant
 * (kLambdaScale 0.22, and 0.22 * 4096 = 901) rather than the requantiser's
 * 0.342, because this weighs a whole unit against its real rate and that one
 * weighs one coefficient against a constant three bits. */
/* Trellis effort, ref's RdoqEffort.  Only FULL is reachable from the library:
 * the fast form is measurably POSITIVE on one of the two measured clips. */
#define NXE_RDOQ_FAST   0
#define NXE_RDOQ_MEDIUM 1
#define NXE_RDOQ_FULL   2

#define NXE_TRELLIS_LAM_Q12 901
uint32_t nxe_trellis_lambda_q8(int qp);

/* Writes the chosen levels into `coefs`.  `step[i]` is the reconstruction step
 * (Q4) at block-local index i, `orig[i]` the unquantised value.  `sdh` says the
 * unit will hide its LAST sign, which is worth exactly one bit and is part of
 * the LAST decision. */
void nxe_rdoq_unit_int(int16_t *coefs, const int32_t *orig, const int32_t *step,
                       int ncoef, const uint8_t *scan, const nxe_unit_ctx *uc,
                       const nxe_rate_cost *rc, uint32_t lam_q8, int effort,
                       int sdh);

/* Sign data hiding on the trellis's own footing.  `rc`/`lam_q8` may be
 * null/zero for a rate-blind choice. */
void nxe_hide_sign_unit_int(int16_t *coefs, const int32_t *orig,
                            const int32_t *step, int ncoef, const uint8_t *scan,
                            const nxe_rate_cost *rc, uint32_t lam_q8,
                            const nxe_unit_ctx *uc);

#ifdef __cplusplus
}
#endif

#endif /* NXE_TRELLIS_H */
