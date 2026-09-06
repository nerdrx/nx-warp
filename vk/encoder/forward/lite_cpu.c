/* lite_cpu.c -- the CPU model of E4-lite.  See lite_cpu.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Transcribed from `ref/src/entropy_lite.cpp`'s `lite_encode_units()`, section
 * for section, so that the two are byte-identical by construction rather than
 * by coincidence.  The one structural difference is that this file computes
 * the per-unit bit WIDTHS as it goes and the shader turns them into offsets
 * with a prefix sum; the reference appends to a bit writer.  They produce the
 * same bits in the same order, which is the only thing that has to be true.
 */

#include "lite_cpu.h"

#include "nxe_rate.h"

#include <stddef.h>

#include "nxe_tables.h"

const uint8_t nxe_lite_mag_bits[8] = {0, 1, 2, 3, 4, 6, 8, 16};

int nxe_lite_last_bits(int ncoef) {
    int b = 0;
    while ((1 << b) < ncoef) ++b;
    return b;
}

/* ------------------------------------------------------------ bit plumbing
 * ref's BitW: MSB-first inside a byte, every section padded by align(). */
typedef struct {
    uint8_t *b;      /* NULL when only the length is wanted */
    size_t cap;
    size_t n;        /* bytes written */
    uint32_t acc;
    int nbits;
} LiteBitW;

static void bw_byte(LiteBitW *w, uint8_t v) {
    if (w->b && w->n < w->cap) w->b[w->n] = v;
    ++w->n;
}

static void bw_put(LiteBitW *w, uint32_t v, int k) {
    while (k > 0) {
        int take = k > 8 ? 8 : k;
        uint32_t chunk = (v >> (k - take)) & ((1u << take) - 1u);
        w->acc = (w->acc << take) | chunk;
        w->nbits += take;
        k -= take;
        while (w->nbits >= 8) {
            bw_byte(w, (uint8_t)((w->acc >> (w->nbits - 8)) & 0xffu));
            w->nbits -= 8;
        }
    }
    w->acc &= (1u << w->nbits) - 1u;
}

static void bw_align(LiteBitW *w) {
    if (w->nbits) {
        bw_byte(w, (uint8_t)((w->acc << (8 - w->nbits)) & 0xffu));
        w->nbits = 0;
        w->acc = 0;
    }
}

/* ------------------------------------------------------------ intra modes
 * ref/src/entropy.cpp, the same pair rans_cpu.c has.  They are static there,
 * and duplicating six lines is preferable to widening that file's ABI for
 * a second consumer. */
static int lite_mpm_of(const uint8_t *modes, int nbx, int b) {
    int bx = b % nbx, by = b / nbx;
    int left = bx > 0 ? modes[b - 1] : NXE_INTRA_DC_PLANE;
    int above = by > 0 ? modes[b - nbx] : NXE_INTRA_DC_PLANE;
    if (left == above) return left;
    return left < above ? left : above;
}

static int lite_nonmpm_index(int mpm, int mode) {
    int n = 0, m;
    for (m = 0; m < NXE_NUM_INTRA_MODES; ++m) {
        if (m == mpm) continue;
        if (m == mode) return n;
        ++n;
    }
    return 0;
}

/* ref's UnitFacts. */
typedef struct {
    int coded;
    int last;
    int nnz;
    int param;      /* FIXED: the magnitude class */
} LiteFacts;

/* Lite's rate, exactly, without coding it.  See nxe_rate.h.
 *
 * Lite has no arithmetic coder, so there is nothing to estimate: every field is
 * a fixed width and the only non-additive term is the align-to-byte at the end
 * of each of the five sections.  And that is additive too, one level up -- a
 * section's total is a sum over units, and the pad is a function of that total
 * -- so this is five independent sums and five roundings, which is a shape a
 * workgroup can produce in five reductions.  It agrees with `nxe_lite_tile`
 * exactly, and `--rate-check` requires it to.
 *
 * The unit classification below is `nxe_lite_tile`'s first loop verbatim; it is
 * repeated rather than shared because the coder's copy fills a `unitf` array it
 * then codes from, and a rate query has nowhere to put one. */
uint32_t nxe_lite_tile_bits_q10(const nxe_frame_params *fp,
                                const nxe_tile_job *job,
                                const nxe_tile_units *tu, const int16_t *coef,
                                const uint8_t *modes, int variant) {
    const int nunits = tu->nunits;
    int h0 = 0, h1 = 0, pp = 0, ss = 0, bb = 0;
    int ngroups, g, i;
    (void)fp;
    (void)job;
    (void)variant;

    ngroups = (nunits + NXE_LITE_CBF_GROUP - 1) / NXE_LITE_CBF_GROUP;
    h0 = ngroups;

    for (g = 0; g < ngroups; ++g) {
        const int lo = g * NXE_LITE_CBF_GROUP;
        const int hi = lo + NXE_LITE_CBF_GROUP < nunits ? lo + NXE_LITE_CBF_GROUP
                                                       : nunits;
        int any = 0;
        for (i = lo; i < hi && !any; ++i) {
            const nxe_unit *u = &tu->u[i];
            if (u->kind == 1) {
                any = u->nbx != 0;
            } else {
                const int16_t *c = coef + u->coef_off;
                const uint8_t *scan = nxe_scan_table((int)u->ncoef, u->tskip);
                int p;
                for (p = (int)u->ncoef - 1; p >= 0; --p)
                    if (c[scan[p]] != 0) { any = 1; break; }
            }
        }
        if (any) h1 += hi - lo;
    }

    for (i = 0; i < nunits; ++i) {
        const nxe_unit *u = &tu->u[i];
        if (u->kind == 1) {
            const uint8_t *md = modes + (size_t)u->mode_off * 64;
            const int n = u->nbx * u->nbx;
            int b;
            if (!u->nbx) continue;
            ss += n;
            for (b = 0; b < n; ++b)
                if (md[b] != lite_mpm_of(md, u->nbx, b))
                    bb += NXE_LITE_MODE_BITS;
        } else {
            const int16_t *c = coef + u->coef_off;
            const uint8_t *scan = nxe_scan_table((int)u->ncoef, u->tskip);
            int last = -1, p, nnz = 0, cls = 0, mb;
            int32_t maxa = 0;
            for (p = (int)u->ncoef - 1; p >= 0; --p)
                if (c[scan[p]] != 0) { last = p; break; }
            if (last < 0) continue;
            for (p = 0; p <= last; ++p) {
                int32_t q = c[scan[p]];
                int32_t a = q < 0 ? -q : q;
                if (a) { ++nnz; if (a > maxa) maxa = a; }
            }
            for (cls = 0; cls < 8; ++cls)
                if ((int64_t)maxa <= ((int64_t)1 << nxe_lite_mag_bits[cls]))
                    break;
            if (cls > 7) cls = 7;
            mb = nxe_lite_mag_bits[cls];
            pp += nxe_lite_last_bits((int)u->ncoef) + NXE_LITE_PARAM_BITS;
            ss += last;
            bb += nnz * (mb + 1);
        }
    }

    return nxe_lite_bits_q10(h0, h1, pp, ss, bb);
}

int nxe_lite_tile(const nxe_frame_params *fp, const nxe_tile_job *job,
                  const nxe_tile_units *tu, const int16_t *coef,
                  const uint8_t *modes, int variant, uint8_t *out) {
    LiteFacts f[NXE_TILE_UNITS_MAX];
    uint8_t gflag[(NXE_TILE_UNITS_MAX + NXE_LITE_CBF_GROUP - 1) /
                  NXE_LITE_CBF_GROUP];
    uint8_t mode_idx[3 * 64];
    LiteBitW w;
    const int nunits = tu->nunits;
    int ngroups, i, g, midx = 0, nmodeidx = 0;
    uint8_t hdr[8];
    nxe_tile_job j2;

    /* Only FIXED exists on this path, as on the decoder's: RICE would need a
     * per-unit Exp-Golomb search and a 12-bit body length, and Pass A does not
     * implement it, so emitting it would produce a stream this project's own
     * decoder refuses. */
    if (variant != NXE_LITE_FIXED) return -1;
    if (nunits <= 0 || nunits > NXE_TILE_UNITS_MAX) return -1;

    /* ---- the per-unit facts. */
    for (i = 0; i < nunits; ++i) {
        const nxe_unit *u = &tu->u[i];
        LiteFacts *uf = &f[i];
        uf->coded = 0;
        uf->last = 0;
        uf->nnz = 0;
        uf->param = 0;
        if (u->kind == 1) {
            uf->coded = (u->nbx * u->nbx) != 0;
            continue;
        }
        {
            const int16_t *c = coef + u->coef_off;
            const int ncoef = u->ncoef;
            const uint8_t *scan = nxe_scan_table(ncoef, u->tskip);
            int last = -1, p, cls, nnz = 0;
            int32_t maxa = 0;
            for (p = ncoef - 1; p >= 0; --p)
                if (c[scan[p]] != 0) { last = p; break; }
            if (last < 0) continue;
            uf->coded = 1;
            uf->last = last;
            for (p = 0; p <= last; ++p) {
                int32_t q = c[scan[p]];
                int32_t a = q < 0 ? -q : q;
                if (a) { ++nnz; if (a > maxa) maxa = a; }
            }
            uf->nnz = nnz;
            cls = 7;
            for (p = 0; p < 8; ++p)
                if ((int64_t)maxa <= ((int64_t)1 << nxe_lite_mag_bits[p])) {
                    cls = p;
                    break;
                }
            uf->param = cls;
        }
    }

    /* ---- H0: one bit per group. */
    ngroups = (nunits + NXE_LITE_CBF_GROUP - 1) / NXE_LITE_CBF_GROUP;
    for (g = 0; g < ngroups; ++g) {
        int lo = g * NXE_LITE_CBF_GROUP;
        int hi = lo + NXE_LITE_CBF_GROUP < nunits ? lo + NXE_LITE_CBF_GROUP
                                                 : nunits;
        gflag[g] = 0;
        for (i = lo; i < hi; ++i)
            if (f[i].coded) { gflag[g] = 1; break; }
    }

    w.b = out ? out + NXE_TILE_HEADER_BYTES : NULL;
    w.cap = out ? (size_t)NXE_LITE_PAYLOAD_MAX : 0;
    w.n = 0;
    w.acc = 0;
    w.nbits = 0;

    for (g = 0; g < ngroups; ++g) bw_put(&w, gflag[g], 1);
    bw_align(&w);

    /* ---- H1: one bit per unit of every flagged group. */
    for (g = 0; g < ngroups; ++g) {
        int lo, hi;
        if (!gflag[g]) continue;
        lo = g * NXE_LITE_CBF_GROUP;
        hi = lo + NXE_LITE_CBF_GROUP < nunits ? lo + NXE_LITE_CBF_GROUP
                                              : nunits;
        for (i = lo; i < hi; ++i) bw_put(&w, (uint32_t)f[i].coded, 1);
    }
    bw_align(&w);

    /* ---- P: LAST and the magnitude class of every coded coefficient unit. */
    for (i = 0; i < nunits; ++i) {
        const nxe_unit *u = &tu->u[i];
        if (!f[i].coded || u->kind == 1) continue;
        bw_put(&w, (uint32_t)f[i].last, nxe_lite_last_bits((int)u->ncoef));
        bw_put(&w, (uint32_t)f[i].param, NXE_LITE_PARAM_BITS);
    }
    bw_align(&w);

    /* ---- S, and the mode indices section B will carry for it. */
    for (i = 0; i < nunits; ++i) {
        const nxe_unit *u = &tu->u[i];
        if (!f[i].coded) continue;
        if (u->kind == 1) {
            const uint8_t *md = modes + (size_t)u->mode_off * 64;
            const int n = u->nbx * u->nbx;
            int b;
            if (!modes) return -1;
            for (b = 0; b < n; ++b) {
                int mpm, hit;
                if (md[b] >= NXE_NUM_INTRA_MODES) return -1;
                mpm = lite_mpm_of(md, u->nbx, b);
                hit = md[b] == mpm;
                bw_put(&w, (uint32_t)hit, 1);
                if (!hit)
                    mode_idx[nmodeidx++] =
                        (uint8_t)lite_nonmpm_index(mpm, md[b]);
            }
        } else {
            const int16_t *c = coef + u->coef_off;
            const uint8_t *scan = nxe_scan_table((int)u->ncoef, u->tskip);
            int p;
            /* Position `last` is nonzero by construction and is not coded. */
            for (p = 0; p < f[i].last; ++p)
                bw_put(&w, c[scan[p]] != 0 ? 1u : 0u, 1);
        }
    }
    bw_align(&w);

    /* ---- B: the bodies. */
    for (i = 0; i < nunits; ++i) {
        const nxe_unit *u = &tu->u[i];
        if (!f[i].coded) continue;
        if (u->kind == 1) {
            const uint8_t *md = modes + (size_t)u->mode_off * 64;
            const int n = u->nbx * u->nbx;
            int b;
            for (b = 0; b < n; ++b) {
                int mpm = lite_mpm_of(md, u->nbx, b);
                if (md[b] == mpm) continue;
                bw_put(&w, mode_idx[midx++], NXE_LITE_MODE_BITS);
            }
            continue;
        }
        {
            const int16_t *c = coef + u->coef_off;
            const uint8_t *scan = nxe_scan_table((int)u->ncoef, u->tskip);
            const int mb = nxe_lite_mag_bits[f[i].param];
            int p;
            for (p = 0; p <= f[i].last; ++p) {
                int32_t q = c[scan[p]];
                int32_t a;
                if (!q) continue;
                a = q < 0 ? -q : q;
                if (mb) bw_put(&w, (uint32_t)(a - 1), mb);
                bw_put(&w, q < 0 ? 1u : 0u, 1);
            }
        }
    }
    bw_align(&w);
    /* ref appends a zero byte rather than emit a zero-length payload.  It
     * cannot be reached here -- H0 is at least one bit and therefore at least
     * one byte for any nonempty unit list -- but the encoders have to agree on
     * unreachable cases too, or the agreement is a coincidence. */
    if (w.n == 0) bw_byte(&w, 0);

    if (w.n > 65535u) return -1;
    if (!out) return (int)w.n;

    /* ---- the 8-byte tile header.  `table_set` names the VARIANT under
     * ENTROPY_LITE and `nsub_log2` is fixed at 3, which is what the decoder
     * checks (codec_impl.inc: `tp.table_set >= kLiteNumVariants ||
     * tp.nsub_log2 != 3` is a bitstream error).  The host sets both on the job
     * so that E5 and the shader read one value, but they are asserted here
     * rather than assumed. */
    j2 = *job;
    j2.payload_len = (uint32_t)w.n;
    j2.table_set = (uint32_t)variant;
    j2.nsub_log2 = 3;
    nxe_pack_tile_header(fp, &j2, hdr);
    for (i = 0; i < 8; ++i) out[i] = hdr[i];
    return (int)w.n;
}
