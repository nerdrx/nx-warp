/* rans_cpu.c -- see rans_cpu.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rans_cpu.h"

#include <string.h>

#include "nxe_tables.h"

/* ------------------------------------------------------------ unit list
 * TileCoder::build_units, ref/src/codec.cpp. */
void nxe_build_units(const nxe_frame_params *fp, const nxe_tile_job *job,
                     nxe_tile_units *tu) {
    int p, b, off = 0, n = 0;
    const int v2 = fp->nctx >= NXE_NCTX_V2;
    memset(tu, 0, sizeof *tu);
    for (p = 0; p < NXE_MAX_PLANES; ++p) {
        const int chroma = (p == 1 || p == 2);
        const int size = (p == 0)
                             ? (int)(NXE_TILE >> job->res_level)
                             : (int)(((job->chroma444 ? NXE_TILE : NXE_TILE / 2) >>
                                      job->res_level));
        const int nb = (size < 8 ? 8 : size) / 8;
        const int ndc = nb * nb;
        const uint8_t ccbf = chroma ? NXE_CTX_CBF_CHROMA : NXE_CTX_CBF_LUMA;
        const uint8_t clast = chroma ? NXE_CTX_LAST_CHROMA : NXE_CTX_LAST_LUMA;
        nxe_unit *u = &tu->u[n++];
        u->coef_off = off;
        u->ncoef = (uint16_t)ndc;
        u->kind = 0;
        u->tskip = 0;                       /* the DC plane never uses raster */
        u->ctx_cbf = v2 ? (uint8_t)NXE_CTX_CBF_DC : ccbf;
        u->ctx_last = v2 ? (uint8_t)NXE_CTX_LAST_DC : clast;
        u->ctx_level = v2 ? (uint8_t)NXE_CTX_LEVEL_DC : (uint8_t)NXE_CTX_NONE;
        u->sdh = (uint8_t)fp->sdh;
        off += ndc;
        if (fp->intra_dir) {
            nxe_unit *m = &tu->u[n++];
            m->coef_off = -1;
            m->kind = 1;
            m->nbx = (uint8_t)nb;
            m->mode_off = (uint8_t)p;
            m->ctx_mode = v2 ? (uint8_t)NXE_CTX_MODE : (uint8_t)NXE_CTX_NONE;
        }
        for (b = 0; b < ndc; ++b) {
            nxe_unit *v = &tu->u[n++];
            v->coef_off = off;
            v->ncoef = 64;
            v->kind = 0;
            v->tskip = (uint8_t)job->tskip;
            v->ctx_cbf = ccbf;
            v->ctx_last = clast;
            v->ctx_level = (uint8_t)NXE_CTX_NONE;
            v->sdh = (uint8_t)fp->sdh;
            off += 64;
        }
    }
    tu->nunits = n;
    tu->nlanes = 1 << job->nsub_log2;
    tu->active = tu->nlanes < n ? tu->nlanes : n;
}

/* ------------------------------------------------------------ intra modes
 * ref/src/entropy.cpp. */
static int mpm_of(const uint8_t *modes, int nbx, int b) {
    int bx = b % nbx, by = b / nbx;
    int left = bx > 0 ? modes[b - 1] : NXE_INTRA_DC_PLANE;
    int above = by > 0 ? modes[b - nbx] : NXE_INTRA_DC_PLANE;
    if (left == above) return left;
    return left < above ? left : above;
}
static int nonmpm_index(int mpm, int mode) {
    int n = 0, m;
    for (m = 0; m < NXE_NUM_INTRA_MODES; ++m) {
        if (m == mpm) continue;
        if (m == mode) return n;
        ++n;
    }
    return 0;
}

/* Exp-Golomb order 3, ref's eg3_encode. */
static void eg3_encode(uint32_t v, int *j, uint32_t *suffix, int *bits) {
    uint32_t n = v + (1u << NXE_ESC_ORDER);
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    *j = b - NXE_ESC_ORDER;
    *bits = b;
    *suffix = n - (1u << b);
}

static int band_of(int p) {
    if (p == 0) return 0;
    if (p < 4) return 1;
    if (p < 10) return 2;
    return 3;
}
static int level_class(int m) { return m == 0 ? 0 : (m == 1 ? 1 : 2); }
static int level_ctx(int p, int prev) {
    return NXE_CTX_LEVEL_BASE + nxe_level_ctx_tab[band_of(p)][prev];
}

/* ----------------------------------------------------------- unit -> ops
 *
 * The LaneMachine of ref/src/entropy.cpp, unrolled.  Unrolling is legitimate
 * because on the *encoding* side the machine is a pure function of the
 * coefficients: `feed()` writes back only the values `next()` had just read
 * (and, for the hidden position, a magnitude whose sign the end-of-unit parity
 * restores), so the coefficient array is unchanged across a unit and no state
 * survives one.  That is what lets a lane materialise a unit's operations in
 * one forward pass and then consume them backwards.
 */
int nxe_unit_ops(const nxe_tile_units *tu, int ui, const int16_t *coef,
                 const uint8_t *modes, uint32_t *ops) {
    const nxe_unit *u = &tu->u[ui];
    int n = 0;

    if (u->kind == 1) {
        const uint8_t *md = modes + (size_t)u->mode_off * 64;
        const int nbx = u->nbx;
        int b;
        if (nbx == 0) return 0;
        for (b = 0; b < nbx * nbx; ++b) {
            const int mpm = mpm_of(md, nbx, b);
            const int m = md[b];
            if (u->ctx_mode != NXE_CTX_NONE) {
                int v = (m == mpm) ? 0 : 1 + nonmpm_index(mpm, m);
                ops[n++] = NXE_OP_PACK(NXE_OP_SYM, u->ctx_mode, v);
            } else {
                ops[n++] = NXE_OP_PACK(NXE_OP_BYPASS, 1, (m == mpm) ? 1 : 0);
                if (m != mpm)
                    ops[n++] = NXE_OP_PACK(NXE_OP_BYPASS, 3,
                                           nonmpm_index(mpm, m));
            }
        }
        return n;
    }

    {
        const int16_t *c = coef + u->coef_off;
        const int ncoef = u->ncoef;
        const uint8_t *scan = nxe_scan_table(ncoef, u->tskip);
        int last = -1, p, prev = 0, hide;
        for (p = ncoef - 1; p >= 0; --p)
            if (c[scan[p]] != 0) { last = p; break; }

        ops[n++] = NXE_OP_PACK(NXE_OP_SYM, u->ctx_cbf, last >= 0 ? 1 : 0);
        if (last < 0) return n;

        if (ncoef > 1) {
            int cls = nxe_last_class_of(last);
            ops[n++] = NXE_OP_PACK(NXE_OP_SYM, u->ctx_last, cls);
            if (nxe_last_raw_bits[cls] > 0)
                ops[n++] = NXE_OP_PACK(NXE_OP_BYPASS, nxe_last_raw_bits[cls],
                                       last - nxe_last_base[cls]);
        } else {
            last = 0;
        }

        hide = u->sdh != 0 && last >= NXE_SDH_MIN_LAST;
        for (p = last; p >= 0; --p) {
            int32_t q = c[scan[p]];
            int32_t m = q < 0 ? -q : q;
            int ctx = u->ctx_level != NXE_CTX_NONE ? u->ctx_level
                                                   : level_ctx(p, prev);
            ops[n++] = NXE_OP_PACK(NXE_OP_SYM, ctx, m > 14 ? NXE_ESC_SYM : m);
            if (m > 14) {
                int j, bits, i, nchunks, done = 0;
                uint32_t suf;
                eg3_encode((uint32_t)(m - 15), &j, &suf, &bits);
                for (i = 0; i < j; ++i)
                    ops[n++] = NXE_OP_PACK(NXE_OP_BYPASS, 1, 1);
                ops[n++] = NXE_OP_PACK(NXE_OP_BYPASS, 1, 0);
                nchunks = (bits + 7) / 8;
                while (done < bits) {
                    int chunk = done == 0 ? bits - 8 * (nchunks - 1) : 8;
                    int shift = bits - done - chunk;
                    ops[n++] = NXE_OP_PACK(NXE_OP_BYPASS, chunk,
                                           (suf >> shift) & ((1u << chunk) - 1u));
                    done += chunk;
                }
            }
            if (m != 0 && !(hide && p == last))
                ops[n++] = NXE_OP_PACK(NXE_OP_BYPASS, 1, q < 0 ? 1 : 0);
            prev = level_class((int)m);
        }
        return n;
    }
}

/* ------------------------------------------------------------------- E4 */
int nxe_e4_tile(const nxe_frame_params *fp, const nxe_tile_job *job,
                const nxe_tile_units *tu, const int16_t *coef,
                const uint8_t *modes, const nxe_tables *tabs, uint8_t *out) {
    const int active = tu->active;
    const int set = (int)job->table_set;
    static uint32_t opbuf[NXE_MAX_LANES][NXE_UNIT_MAX_OPS];
    static uint16_t slot_ops[NXE_MAX_LANES][NXE_TILE_UNIT_SLOTS];
    int nops[NXE_MAX_LANES], nslots[NXE_MAX_LANES];
    int cur_slot[NXE_MAX_LANES], ops_base[NXE_MAX_LANES];
    uint32_t state[NXE_MAX_LANES];
    uint32_t word[NXE_MAX_LANES];
    int emit[NXE_MAX_LANES];
    int l, r, R = 0, pass, E = 0, payload = 0;

    /* Phase A: operation counts per lane and per unit slot.  This also fixes
     * the round structure, which is all the byte ordering depends on. */
    for (l = 0; l < active; ++l) {
        int ui, s = 0, total = 0;
        for (ui = l; ui < tu->nunits; ui += tu->nlanes) {
            int k = nxe_unit_ops(tu, ui, coef, modes, opbuf[0]);
            slot_ops[l][s++] = (uint16_t)k;
            total += k;
        }
        nops[l] = total;
        nslots[l] = s;
        if (total > R) R = total;
    }

    /* Two sweeps: the first counts emissions (so the tile's byte size is known
     * before a byte is placed), the second writes them.  Identical code path,
     * so they cannot disagree. */
    for (pass = 0; pass < (out ? 2 : 1); ++pass) {
        int e = 0;
        for (l = 0; l < active; ++l) {
            state[l] = NXE_RANS_L;
            cur_slot[l] = nslots[l] - 1;
            ops_base[l] = nops[l];
            if (cur_slot[l] >= 0) {
                ops_base[l] -= slot_ops[l][cur_slot[l]];
                nxe_unit_ops(tu, l + cur_slot[l] * tu->nlanes, coef, modes,
                             opbuf[l]);
            }
        }
        for (r = R - 1; r >= 0; --r) {
            for (l = 0; l < active; ++l) {
                uint32_t op, f, c, x;
                emit[l] = 0;
                if (r >= nops[l]) continue;
                while (r < ops_base[l]) {
                    cur_slot[l]--;
                    ops_base[l] -= slot_ops[l][cur_slot[l]];
                    nxe_unit_ops(tu, l + cur_slot[l] * tu->nlanes, coef, modes,
                                 opbuf[l]);
                }
                op = opbuf[l][r - ops_base[l]];
                if (NXE_OP_KIND(op) == NXE_OP_SYM) {
                    int ctx = (int)NXE_OP_ARG(op), sym = (int)NXE_OP_VALUE(op);
                    f = tabs->freq[set][ctx][sym];
                    c = tabs->cum[set][ctx][sym];
                } else {
                    int k = (int)NXE_OP_ARG(op);
                    f = 1u << (NXE_PROB_BITS - k);
                    c = (uint32_t)NXE_OP_VALUE(op) << (NXE_PROB_BITS - k);
                }
                x = state[l];
                if (x >= (f << 22)) {
                    emit[l] = 1;
                    word[l] = x & 0xffffu;
                    x >>= 16;
                }
                state[l] = ((x / f) << NXE_PROB_BITS) + (x % f) + c;
            }
            /* Byte order inside a round is lanes descending: the global order
             * is (round asc, lane asc) and this sweep runs it backwards. */
            for (l = active - 1; l >= 0; --l) {
                if (r >= nops[l] || !emit[l]) continue;
                if (pass == 1) {
                    int off = 8 + 4 * active + 2 * (E - 1 - e);
                    out[off] = (uint8_t)(word[l] >> 8);
                    out[off + 1] = (uint8_t)(word[l] & 0xffu);
                }
                ++e;
            }
        }
        if (pass == 0) {
            E = e;
            payload = 4 * active + 2 * E;
            if (payload > 65535) return -1;
        }
    }

    if (out) {
        nxe_tile_job j2 = *job;
        j2.payload_len = (uint32_t)payload;
        nxe_pack_tile_header(fp, &j2, out);
        for (l = 0; l < active; ++l) {
            int off = 8 + 4 * l;
            out[off] = (uint8_t)(state[l] & 0xffu);
            out[off + 1] = (uint8_t)((state[l] >> 8) & 0xffu);
            out[off + 2] = (uint8_t)((state[l] >> 16) & 0xffu);
            out[off + 3] = (uint8_t)((state[l] >> 24) & 0xffu);
        }
    }
    return payload;
}

/* --------------------------------------------------------------- headers */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void nxe_pack_tile_header(const nxe_frame_params *fp, const nxe_tile_job *job,
                          uint8_t out[8]) {
    uint32_t w0, w1;
    (void)fp;
    w0 = ((uint32_t)job->col & 0xfffu) << 4;
    w0 |= ((uint32_t)job->eye & 1u) << 2;
    w0 |= ((uint32_t)job->payload_len & 0xffffu) << 16;
    w1 = (uint32_t)job->mode & 7u;
    w1 |= ((uint32_t)job->res_level & 3u) << 3;
    w1 |= ((uint32_t)job->chroma444 & 1u) << 5;
    w1 |= ((uint32_t)(job->qp_delta & 0x3f)) << 8;
    w1 |= ((uint32_t)job->table_set & 7u) << 14;
    w1 |= ((uint32_t)job->nsub_log2 & 7u) << 17;
    w1 |= ((uint32_t)job->tskip & 1u) << 23;
    w1 |= ((uint32_t)job->wm_id & 3u) << 26;
    put32(out, w0);
    put32(out + 4, w1);
}

uint32_t nxe_e5_tile_offset(const nxe_frame_params *fp, uint32_t t,
                            const uint32_t *tile_prefix) {
    uint32_t rowgroup = t / fp->tiles_x;
    return NXE_FRAME_HEADER_BYTES + NXE_ROW_HEADER_BYTES * (rowgroup + 1u) +
           tile_prefix[t];
}

uint32_t nxe_e5_frame_bytes(const nxe_frame_params *fp,
                            uint32_t total_tile_bytes) {
    uint32_t rowgroups = fp->tiles_y * fp->eyes;
    return NXE_FRAME_HEADER_BYTES + NXE_ROW_HEADER_BYTES * rowgroups +
           total_tile_bytes;
}

void nxe_e5_row_header(const nxe_frame_params *fp, uint32_t rowgroup,
                       uint8_t out[NXE_ROW_HEADER_BYTES]) {
    int i;
    uint32_t row = rowgroup / fp->eyes;
    out[0] = (uint8_t)fp->frame_number;
    out[1] = (uint8_t)(fp->frame_number >> 8);
    out[2] = (uint8_t)row;
    out[3] = (uint8_t)fp->tiles_x;   /* coded tiles; nothing is skipped here */
    for (i = 0; i < 8; ++i) out[4 + i] = 0;   /* skip bitmap */
}

void nxe_e5_frame_header(const nxe_frame_params *fp, const uint8_t pose[26],
                         uint32_t total, uint8_t out[NXE_FRAME_HEADER_BYTES]) {
    out[0] = (uint8_t)fp->frame_number;
    out[1] = (uint8_t)(fp->frame_number >> 8);
    memcpy(out + 2, pose, 26);
    out[28] = (uint8_t)fp->base_qp;
    out[29] = (uint8_t)(int8_t)fp->chroma_qp_off;
    out[30] = 0;                       /* alpha_qp_off */
    out[31] = (uint8_t)fp->quant_matrix;
    out[32] = (uint8_t)fp->tables_present;
    out[33] = 0;                       /* ref_slots: no reference ring */
    out[34] = (uint8_t)fp->frame_flags;
    out[35] = 0;
    put32(out + 36, total);
}
