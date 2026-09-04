/* nxe_tables.h -- the normative constant tables the encoder kernels need.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * These are copies of the tables in `ref/src/tables.cpp`, kept here so that
 * `vk/encoder` builds without the reference codec (paper 3.10: the Vulkan
 * components are separable).  `nxvc-vkenc` asserts every one of them against
 * the reference library's own copy at start-up when it is linked, so a drift
 * is a test failure rather than a silent bitstream difference.
 *
 * The same values appear again as GLSL constants in `nxe_enc_common.glsl`.
 * That is three copies of a 64-entry table, which is two too many for comfort
 * and exactly what the assertion above exists to police.
 */

#ifndef NXE_TABLES_H
#define NXE_TABLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* qstep[qp] = round(16 * 2^(qp/6)), Q4. */
extern const uint16_t nxe_qstep[64];
/* Built-in weighting matrices, Q4, raster order: 0 flat, 1 luma roll-off,
 * 2 periphery roll-off, 3 chroma. */
extern const uint8_t nxe_weight[4][64];

extern const uint8_t nxe_zigzag8[64];
extern const uint8_t nxe_zigzag4[16];
extern const uint8_t nxe_zigzag2[4];
extern const uint8_t nxe_raster8[64];

extern const uint8_t nxe_last_base[16];
extern const uint8_t nxe_last_raw_bits[16];
extern const uint8_t nxe_level_ctx_tab[4][3];

/* 9-bit transform constants (ref/src/transform.h). */
#define NXE_C4 362
#define NXE_C2 473
#define NXE_S2 196
#define NXE_A1 502
#define NXE_A3 426
#define NXE_A5 284
#define NXE_A7 100

const uint8_t *nxe_scan_table(int n, int tskip);
int nxe_last_class_of(int pos);

#ifdef __cplusplus
}
#endif

#endif /* NXE_TABLES_H */
