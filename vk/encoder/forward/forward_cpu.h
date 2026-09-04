/* forward_cpu.h -- bit-exact CPU model of E3 (`forward.comp`).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The model is the specification and the shader is validated against it, which
 * is the relationship paper 3.7 sets up for the decoder and `stats_cpu.h`
 * already uses for E0/E1/E2.  Every function below is a line-for-line port of
 * the corresponding one in `ref/src/codec.cpp` / `ref/src/transform.cpp`:
 *
 *   nxe_fdct8x8          <- fdct8x8            (transform.cpp)
 *   nxe_idct8x8          <- idct8x8            (transform.cpp)
 *   nxe_bilinear_q4      <- bilinear_q4_i32    (transform.cpp)
 *   nxe_dc_plane         <- analyze_dc_plane + reconstruct_dc_plane
 *   nxe_hide_sign_unit   <- hide_sign_unit
 *   nxe_e3_plane         <- analyze_plane
 *   nxe_e3_plane_dir     <- analyze_plane_dir, with the mode decision removed
 *   nxe_predict_block    <- predict_block
 *   nxe_build_refs       <- build_refs
 *
 * Two deviations from the reference, both deliberate and both exact:
 *
 *  1. `hide_sign_unit` compares candidate moves by a `double` squared error.
 *     The comparison is reproduced here in integers -- see the derivation at
 *     nxe_hide_sign_unit -- because the shader cannot use doubles and a
 *     mismatch would change the bitstream.  The ordering is identical for
 *     every input the syntax admits.
 *  2. `analyze_plane_dir` chooses the per-block intra mode by SATD and a
 *     floating-point RD comparison.  That decision is a host-side (or E1) job
 *     in this pipeline: the mode array is an *input* here and to the shader,
 *     so E3 applies modes rather than choosing them.  Everything downstream of
 *     the choice -- the reference derivation, the predictor, the residual, the
 *     running reconstruction -- is the reference's, unchanged.
 *
 * Not modelled: the RD trellis (`rdoq_unit`).  It changes which levels are
 * coded, never how they are decoded, and it is a `double` trellis; a GPU
 * encoder that wants it runs `--no-rdo`'s plain dead-zone quantiser, which is
 * what this kernel is.
 */

#ifndef NXE_FORWARD_CPU_H
#define NXE_FORWARD_CPU_H

#include <stdint.h>

#include "nxe_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One plane of one tile, as `ref`'s PlaneState reduced to what E3 reads. */
typedef struct nxe_plane {
    int size;               /* coded edge */
    int nb;                 /* blocks per edge */
    int qp;
    int maxval;
    int dc_off;
    uint8_t wmat[64];       /* Q4, raster order inside the block */
    int tskip;
    int sdh;
    int ctx_level_dc;       /* NXE_CTX_LEVEL_DC under the v2 model, else 0 */
} nxe_plane;

/* Fill `pl` from the frame parameters and the tile job for plane `p`. */
void nxe_plane_setup(const nxe_frame_params *fp, const nxe_tile_job *job, int p,
                     nxe_plane *pl);

/* Byte offset, in levels, of plane `p`'s coefficients inside the tile slot.
 * Mirrors ref's plane_coef_offset. */
int nxe_plane_coef_offset(const nxe_frame_params *fp, const nxe_tile_job *job,
                          int p);
int nxe_plane_size(const nxe_frame_params *fp, const nxe_tile_job *job, int p);

/* ------------------------------------------------------------- primitives */
void nxe_fdct8x8(const int32_t src[64], int16_t dst[64]);
void nxe_idct8x8(const int32_t src[64], int32_t dst[64]);
int32_t nxe_bilinear_q4(const int32_t *src, int w, int h, int stride,
                        int32_t sx, int32_t sy);
int nxe_dequant_step(int qp, int w);
int32_t nxe_dequant(int32_t q, int32_t t);
int32_t nxe_quantize(int32_t c, int32_t t, int32_t dz);

/* Sign data hiding over one coding unit.  `orig` is the unquantized value at
 * each block-local index and `step` its reconstruction step (Q4). */
void nxe_hide_sign_unit(int16_t *coefs, const int32_t *orig, const int32_t *step,
                        int ncoef, const uint8_t *scan);

/* ------------------------------------------------------------------- E3
 *
 * `src` is the plane's source samples, `size*size`, tile-local raster order.
 * `coef` receives `nb*nb + nb*nb*64` levels in coding-unit order.  `pred`, if
 * non-null, receives the DC-plane prediction (`size*size`) -- the shader keeps
 * it only in registers, but the harness wants it for diffing.
 */
void nxe_e3_plane(const nxe_plane *pl, const int32_t *src, int16_t *coef,
                  int32_t *pred);

/* Directional intra (tool bit 17).  `modes` is `nb*nb` per-block modes, an
 * input.  `recon` (size*size) receives the running reconstruction. */
void nxe_e3_plane_dir(const nxe_plane *pl, const int32_t *src,
                      const uint8_t *modes, int layer, int16_t *coef,
                      int32_t *pred, int32_t *recon);

/* One whole tile: every coded plane, in plane order. */
void nxe_e3_tile(const nxe_frame_params *fp, const nxe_tile_job *job,
                 const int32_t *const src[NXE_MAX_PLANES], const uint8_t *modes,
                 int16_t *coef);

#ifdef __cplusplus
}
#endif

#endif /* NXE_FORWARD_CPU_H */
