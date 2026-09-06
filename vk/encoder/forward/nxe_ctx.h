/* nxe_ctx.h -- entropy context derivation, the one C copy.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * These are the only places a context is chosen on the encoder's C side,
 * exactly as `nxs_v3_ctx_*` are on the decode side and `ref/src/common.h` is in
 * the reference.  They lived as statics inside rans_cpu.c until the integer
 * trellis needed them too: the trellis prices a candidate level BEFORE the
 * level exists, so it cannot go through `nxe_unit_ops` the way the rate model
 * does and has to derive the same contexts itself.
 *
 * One copy rather than two, therefore.  The GLSL copy in nxe_enc_common.glsl
 * is the third and is policed by `vk.encoder.mirror`.
 */

#ifndef NXE_CTX_H
#define NXE_CTX_H

#include "nxe_enc.h"
#include "nxe_tables.h"

static inline int band_of(int p) {
    if (p == 0) return 0;
    if (p < 4) return 1;
    if (p < 10) return 2;
    return 3;
}
static inline int level_class(int m) { return m == 0 ? 0 : (m == 1 ? 1 : 2); }
static inline int level_ctx(int p, int prev) {
    return NXE_CTX_LEVEL_BASE + nxe_level_ctx_tab[band_of(p)][prev];
}

/* ------------------------------------------------- v3 context derivation
 *
 * These three are the only places a v3 context is chosen, exactly as they are
 * the only three on the decode side (`vk/decoder/passA/syntax_constants.h`,
 * `nxs_v3_ctx_*`) and in the reference (`ref/src/common.h`).  Each is
 * arithmetic over the unit's class and the lane's neighbour class.
 *
 * `nbr` is 0 none, 1 uncoded, 2 coded sparse, 3 coded dense; class 0 keeps the
 * v2 context, which is what makes v3 a refinement of v2 rather than a
 * replacement -- and is why the stream header refuses tool bit 25 without
 * bit 21.
 */
static inline int v3_ctx_cbf(int ucls, int nbr) {
    if (nbr == 0)
        return ucls == NXE_UCLS_DC
                   ? NXE_CTX_CBF_DC
                   : (ucls == NXE_UCLS_CHROMA ? NXE_CTX_CBF_CHROMA
                                              : NXE_CTX_CBF_LUMA);
    return (ucls == NXE_UCLS_CHROMA ? NXE_CTX_CBF_CHROMA_N
                                    : NXE_CTX_CBF_LUMA_N) + (nbr - 1);
}
/* LAST splits coded from not-coded only: the sparse/dense distinction pays on
 * CBF, where it says how likely a coefficient is at all, and not on LAST,
 * where the unit's own magnitudes already say it.  So LAST spends two extra
 * rows against CBF's six. */
static inline int v3_ctx_last(int ucls, int nbr) {
    if (nbr < 2)
        return ucls == NXE_UCLS_DC
                   ? NXE_CTX_LAST_DC
                   : (ucls == NXE_UCLS_CHROMA ? NXE_CTX_LAST_CHROMA
                                              : NXE_CTX_LAST_LUMA);
    return ucls == NXE_UCLS_CHROMA ? NXE_CTX_LAST_CHROMA_N
                                   : NXE_CTX_LAST_LUMA_N;
}
/* LEVEL is NOT conditioned on the neighbour: the previously coded level inside
 * the same unit already carries that, and about this unit rather than the one
 * before it.  It does split the coefficient at scan position LAST, which is
 * nonzero by construction, and gives the DC term of a DC plane its own row.
 *
 * `band_scan_pos` is the scan position after the band mappings of the two
 * transform tools; with neither XFORM_4X4_SPLIT nor XFORM_LARGE implemented
 * here it is always `scan_pos`, and the argument is kept separate so that
 * adding either is a change to the caller and not to this function. */
static inline int v3_ctx_level(int ucls, int scan_pos, int band_scan_pos, int last,
                        int prev_class) {
    if (ucls == NXE_UCLS_DC)
        return scan_pos == 0 ? NXE_CTX_LEVEL_DC0 : NXE_CTX_LEVEL_DC;
    if (scan_pos == last)
        return band_of(band_scan_pos) < 2 ? NXE_CTX_LEVEL_LAST_LO
                                          : NXE_CTX_LEVEL_LAST_HI;
    return level_ctx(band_scan_pos, prev_class);
}
/* The class a finished coefficient unit publishes to its lane. */
static inline int nbr_class_of(int cbf, int last) {
    if (cbf == 0) return 1;
    return last < NXE_NBR_DENSE_LAST ? 2 : 3;
}

#endif /* NXE_CTX_H */
