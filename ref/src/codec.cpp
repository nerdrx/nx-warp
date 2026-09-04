// nxvc_ref: bitstream syntax, encoder and decoder.  See docs/SYNTAX.md.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>

#include "common.h"
#include "entropy.h"
#include "transform.h"

namespace nxvc {

// ------------------------------------------------------------- byte writer
struct BW {
    std::vector<u8> b;
    void u8v(u32 v) { b.push_back((u8)v); }
    void u16v(u32 v) { b.push_back((u8)v); b.push_back((u8)(v >> 8)); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back((u8)(v >> (8 * i))); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) b.push_back((u8)(v >> (8 * i))); }
    void raw(const u8 *p, size_t n) { b.insert(b.end(), p, p + n); }
    size_t size() const { return b.size(); }
};

struct BR {
    const u8 *p = nullptr;
    size_t n = 0, i = 0;
    bool ok = true;
    bool need(size_t k) { if (i + k > n) { ok = false; return false; } return true; }
    u32 u8v() { if (!need(1)) return 0; return p[i++]; }
    u32 u16v() { if (!need(2)) return 0; u32 v = p[i] | (p[i + 1] << 8); i += 2; return v; }
    u32 u32v() { if (!need(4)) return 0; u32 v = 0; for (int k = 0; k < 4; ++k) v |= (u32)p[i + k] << (8 * k); i += 4; return v; }
    u64 u64v() { if (!need(8)) return 0; u64 v = 0; for (int k = 0; k < 8; ++k) v |= (u64)p[i + k] << (8 * k); i += 8; return v; }
    void skip(size_t k) { if (need(k)) i += k; }
};

// MSB-first bit packing, used only for the custom probability tables.
struct BitW {
    std::vector<u8> b;
    u32 acc = 0; int nbits = 0;
    void put(u32 v, int k) {
        for (int i = k - 1; i >= 0; --i) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++nbits == 8) { b.push_back((u8)acc); acc = 0; nbits = 0; }
        }
    }
    void flush() { while (nbits) put(0, 1); }
};
struct BitR {
    const u8 *p; size_t n; size_t bit = 0;
    u32 get(int k) {
        u32 v = 0;
        for (int i = 0; i < k; ++i) {
            u32 b = 0;
            if (bit >> 3 < n) b = (p[bit >> 3] >> (7 - (bit & 7))) & 1;
            ++bit;
            v = (v << 1) | b;
        }
        return v;
    }
};

// ---------------------------------------------------------------- geometry
struct Geometry {
    u32 width = 0, height = 0;
    u32 chroma = 0;       // nxvc_chroma
    u32 color_transform = 0;
    u32 color_space = 0;
    u32 alpha = 0;
    u32 tiles_x = 0, tiles_y = 0;
    u32 cw = 0, ch = 0;   // chroma plane dimensions
    int nplanes() const { return alpha ? 4 : 3; }
    int maxval(int p) const {
        if (color_transform == NXVC_CT_YCOCGR && (p == 1 || p == 2)) return 511;
        return 255;
    }
    int dc_offset(int p) const {
        if (color_transform == NXVC_CT_YCOCGR && (p == 1 || p == 2)) return 256;
        return 128;
    }
};

static void derive_geometry(Geometry &g) {
    g.tiles_x = (g.width + 63) / 64;
    g.tiles_y = (g.height + 63) / 64;
    if (g.chroma == NXVC_CHROMA_420) {
        g.cw = (g.width + 1) / 2;
        g.ch = (g.height + 1) / 2;
    } else {
        g.cw = g.width;
        g.ch = g.height;
    }
}

// ------------------------------------------------------------ frame params
struct FrameParams {
    u32 frame_number = 0;
    u8 pose[26] = {};
    int base_qp = 24;
    int chroma_qp_off = 0, alpha_qp_off = 0;
    int quant_matrix = 0;
    u32 tables_present = 0;
    u32 ref_slots = 0, flags = 1;
    u8 wm_luma[64] = {}, wm_chroma[64] = {};
    TableSet tabs[8];
};

static void resolve_matrices(FrameParams &fp, const u8 *custom) {
    if (fp.quant_matrix == 255 && custom) {
        for (int i = 0; i < 64; ++i) {
            fp.wm_luma[i] = (u8)clamp_i32(custom[i], 1, 32);
            fp.wm_chroma[i] = (u8)clamp_i32(custom[64 + i], 1, 32);
        }
    } else {
        int m = clamp_i32(fp.quant_matrix, 0, 3);
        for (int i = 0; i < 64; ++i) {
            fp.wm_luma[i] = kWeight[m][i];
            fp.wm_chroma[i] = kWeight[m == 0 ? 0 : 3][i];
        }
    }
}

// --------------------------------------------------------------- tile info
struct TileParams {
    int layer = 0, eye = 0, tile_index = 0, payload_len = 0;
    int mode = NXVC_MODE_INTRA, res_level = 0, chroma444 = 0, alpha_mode = 0;
    int qp_delta = 0, table_set = 0, nsub_log2 = 3, mv_present = 0;
    int ref_sel = 0, tskip = 0, wgt = 0, wm_id = 0;
    int mv_x = 0, mv_y = 0, alpha_value = 255;
};

static void pack_tile_header(BW &bw, const TileParams &t) {
    u32 w0 = ((u32)t.layer & 3) | (((u32)t.eye & 1) << 2) |
             (((u32)t.tile_index & 0xfff) << 4) |
             (((u32)t.payload_len & 0xffff) << 16);
    u32 w1 = ((u32)t.mode & 7);
    w1 |= ((u32)t.res_level & 3) << 3;
    w1 |= ((u32)t.chroma444 & 1) << 5;
    w1 |= ((u32)t.alpha_mode & 3) << 6;
    w1 |= ((u32)(t.qp_delta & 0x3f)) << 8;
    w1 |= ((u32)t.table_set & 7) << 14;
    w1 |= ((u32)t.nsub_log2 & 7) << 17;
    w1 |= ((u32)t.mv_present & 1) << 20;
    w1 |= ((u32)t.ref_sel & 3) << 21;
    w1 |= ((u32)t.tskip & 1) << 23;
    w1 |= ((u32)t.wgt & 3) << 24;
    w1 |= ((u32)t.wm_id & 3) << 26;
    bw.u32v(w0);
    bw.u32v(w1);
}

static void unpack_tile_header(u32 w0, u32 w1, TileParams &t) {
    t.layer = w0 & 3;
    t.eye = (w0 >> 2) & 1;
    t.tile_index = (w0 >> 4) & 0xfff;
    t.payload_len = (w0 >> 16) & 0xffff;
    t.mode = w1 & 7;
    t.res_level = (w1 >> 3) & 3;
    t.chroma444 = (w1 >> 5) & 1;
    t.alpha_mode = (w1 >> 6) & 3;
    int qd = (w1 >> 8) & 0x3f;
    t.qp_delta = qd >= 32 ? qd - 64 : qd;
    t.table_set = (w1 >> 14) & 7;
    t.nsub_log2 = (w1 >> 17) & 7;
    t.mv_present = (w1 >> 20) & 1;
    t.ref_sel = (w1 >> 21) & 3;
    t.tskip = (w1 >> 23) & 1;
    t.wgt = (w1 >> 24) & 3;
    t.wm_id = (w1 >> 26) & 3;
}

// ------------------------------------------------------------- tile coding
// Per-plane coding state inside one tile.
// The DC plane is 1/64 of the samples but carries the whole intra predictor,
// so it is quantized at half the tile's QP index (one step per two QP steps).
// Coarse block means make the planar prediction blocky, which the AC residual
// then has to pay for twice over; measured at +3 dB and -10% bits at QP 38.
static inline int dc_qp_of(int qp) { return qp >> 1; }

struct PlaneState {
    int size = 0;      // coded edge
    int nb = 0;        // blocks per edge
    int qp = 0;
    const u8 *wmat = nullptr;
    int maxval = 255, dc_off = 128;
    std::vector<i32> samples;  // size*size, source (encoder) / recon (decoder)
    std::vector<i32> means;    // nb*nb reconstructed block means
    std::vector<i32> pred;     // size*size
};

struct TileCoder {
    const Geometry *g = nullptr;
    const FrameParams *fp = nullptr;
    TileParams tp;
    int nplanes = 3;
    PlaneState pl[4];
    std::vector<i16> coef;
    std::vector<Unit> units;

    void setup();
    void build_units();
};

static inline int dequant_step(int qp, int w) {
    // t is the quantizer step in Q4, bounded by 23170 * 32 / 16 = 46340.
    return (kQStep[qp] * w + 8) >> 4;
}
static inline i32 dequant(i32 q, i32 t) { return clamp16((q * t + 8) >> 4); }
static inline i32 quantize(i32 c, i32 t, i32 dz) {
    i32 a = c < 0 ? -c : c;
    i32 q = (a * 16 + dz) / t;   // encoder only: division is non-normative
    if (q > 32767) q = 32767;
    return c < 0 ? -q : q;
}

void TileCoder::setup() {
    TileGeom tg = tile_geom(tp.res_level, tp.chroma444);
    nplanes = g->nplanes();
    int qp = clamp_i32(fp->base_qp + tp.qp_delta, 0, 63);
    for (int p = 0; p < nplanes; ++p) {
        PlaneState &s = pl[p];
        bool chroma = (p == 1 || p == 2);
        s.size = chroma ? tg.chroma_size : tg.coded_size;
        s.nb = s.size / 8;
        s.qp = chroma ? clamp_i32(qp + fp->chroma_qp_off, 0, 63)
                      : (p == 3 ? clamp_i32(qp + fp->alpha_qp_off, 0, 63) : qp);
        // wm_id 0 means "the frame's matrix"; 1..3 override it with a
        // built-in pair for this tile alone (the degradation ladder's step 1,
        // PAPER 4.6.1).  A frame carrying custom matrices forbids wm_id != 0.
        if (tp.wm_id == 0) {
            s.wmat = chroma ? fp->wm_chroma : fp->wm_luma;
        } else {
            s.wmat = kWeight[chroma ? 3 : tp.wm_id];
        }
        s.maxval = g->maxval(p);
        s.dc_off = g->dc_offset(p);
        s.samples.assign((size_t)s.size * s.size, 0);
        s.means.assign((size_t)s.nb * s.nb, 0);
        s.pred.assign((size_t)s.size * s.size, 0);
    }
}

void TileCoder::build_units() {
    size_t total = 0;
    int np = nplanes;
    if (tp.alpha_mode != 2) np = std::min(np, 3);
    for (int p = 0; p < np; ++p) {
        total += (size_t)pl[p].nb * pl[p].nb;              // DC plane
        total += (size_t)pl[p].nb * pl[p].nb * 64;         // blocks
    }
    coef.assign(total, 0);
    units.clear();
    size_t off = 0;
    for (int p = 0; p < np; ++p) {
        PlaneState &s = pl[p];
        bool chroma = (p == 1 || p == 2);
        u8 ccbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
        u8 clast = chroma ? kCtxLastChroma : kCtxLastLuma;
        int ndc = s.nb * s.nb;
        Unit u{};
        u.coef = &coef[off];
        u.ncoef = (u16)ndc;
        u.scan = scan_table(ndc, false);
        u.ctx_cbf = ccbf;
        u.ctx_last = clast;
        units.push_back(u);
        off += ndc;
        for (int b = 0; b < ndc; ++b) {
            Unit v{};
            v.coef = &coef[off];
            v.ncoef = 64;
            v.scan = scan_table(64, tp.tskip != 0);
            v.ctx_cbf = ccbf;
            v.ctx_last = clast;
            units.push_back(v);
            off += 64;
        }
    }
}

// Reconstruct one plane from its coefficients (normative decode path).
static void reconstruct_plane(PlaneState &s, const i16 *coefs, int tskip) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    // --- DC plane
    int dcqp = dc_qp_of(s.qp);
    int tdc = dequant_step(dcqp, 16);
    std::vector<i32> dc(ndc);
    for (int i = 0; i < ndc; ++i) dc[i] = dequant(coefs[i], tdc);
    if (nb == 8) {
        i32 in[64], out[64];
        for (int i = 0; i < 64; ++i) in[i] = dc[i];
        idct8x8(in, out);
        for (int i = 0; i < 64; ++i) dc[i] = out[i];
    }
    for (int i = 0; i < ndc; ++i)
        s.means[i] = clamp_i32(s.dc_off + dc[i], 0, s.maxval);
    // --- planar prediction: bilinear over block centres (8x8 blocks)
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            s.pred[(size_t)y * size + x] = bilinear_q4_i32(
                s.means.data(), nb, nb, nb, 2 * x - 7, 2 * y - 7);
    // --- residual blocks
    const i16 *bc = coefs + ndc;
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            const i16 *c = bc + ((size_t)by * nb + bx) * 64;
            i32 res[64];
            if (tskip) {
                int t = dequant_step(s.qp, 16);
                for (int i = 0; i < 64; ++i) res[i] = dequant(c[i], t);
            } else {
                i32 dq[64];
                for (int i = 0; i < 64; ++i)
                    dq[i] = dequant(c[i], dequant_step(s.qp, s.wmat[i]));
                idct8x8(dq, res);
            }
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    s.samples[(size_t)y * size + x] = clamp_i32(
                        s.pred[(size_t)y * size + x] + res[j * 8 + i], 0,
                        s.maxval);
                }
        }
}

// ------------------------------------------------------------------- RDOQ
// Rate-distortion optimized quantization.  Encoder only: it changes which
// levels are coded, never how they are decoded, so it is invisible to
// docs/SYNTAX.md and to the GPU decoder.
//
// The syntax of one coding unit is CBF, then LAST, then the levels from
// `last` down to 0 in reverse scan order, each level's context depending on
// the magnitude class of the *previously decoded* level.  That is a Markov
// chain with three states, so the rate-optimal level assignment is a trellis,
// not a per-coefficient threshold.  The recursion is
//
//   f[p][s] = min over m of ( rate(m | band(p), s) + D(p, m) + f[p-1][cls(m)] )
//
// over scan positions p ascending, where `s` is the class of the level at
// p+1 (the one decoded just before p).  One ascending pass gives every
// prefix cost, after which every candidate `last` is evaluated in O(1):
//
//   cost(L) = rate(CBF=1) + rate(LAST=L) + f_nz[L][0] + (distortion of
//             zeroing every scan position above L)
//
// against the all-zero alternative rate(CBF=0) + the unit's whole energy.
// Cost is D + lambda*R with D in squared coefficient units, which for this
// orthonormal transform is squared sample units.
struct RateCost {
    i32 sym[kNumCtx][kNumSym];  // Q10 bits
};

static void build_rate_cost(const TableSet &ts, RateCost &rc) {
    for (int c = 0; c < kNumCtx; ++c)
        for (int s = 0; s < kNumSym; ++s) {
            double f = (double)ts.ctx[c].freq[s] / 1024.0;
            if (f <= 0) f = 1.0 / 1024.0;
            rc.sym[c][s] = (i32)(-std::log2(f) * 1024.0 + 0.5);
        }
}

// Bypass bits an escape suffix costs for magnitude m >= 15 (Exp-Golomb 3 of
// m - 15), matching eg3_encode in entropy.cpp exactly.
static inline int escape_bits(i32 m) {
    u32 n = (u32)(m - 15) + 8u;
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    return (b - kEscOrder) + 1 + b;  // j ones, one zero, b suffix bits
}

static inline i32 level_rate(const RateCost &rc, int scan_pos, int prev_class,
                             i32 m) {
    int ctx = level_ctx(scan_pos, prev_class);
    i32 sym = m > 14 ? kEscSym : m;
    i32 r = rc.sym[ctx][sym];
    if (m > 14) r += escape_bits(m) << 10;
    if (m != 0) r += 1 << 10;  // sign, one bypass bit
    return r;
}

constexpr double kRdInf = 1e30;

// `orig[i]` is the unquantized value at block-local index i, `step[i]` its
// reconstruction step (the dequantizer's t, Q4).  Writes the chosen levels
// back into `coefs`.
static void rdoq_unit(i16 *coefs, const i32 *orig, const i32 *step, int ncoef,
                      const u8 *scan, int ctx_cbf, int ctx_last,
                      const RateCost &rc, double lambda) {
    double f[64][3], fnz[64];
    i32 best_m[64][3], best_m_nz[64];
    double tail = 0;               // energy of scan positions above p
    double energy = 0;
    for (int i = 0; i < ncoef; ++i) {
        double c = orig[i];
        energy += c * c;
    }
    double prev[3] = {0, 0, 0};
    for (int p = 0; p < ncoef; ++p) {
        int idx = scan[p];
        double c = orig[idx];
        double a = c < 0 ? -c : c;
        double st = (double)step[idx] / 16.0;
        i32 m0 = (i32)(a / st);
        if (m0 > 32767) m0 = 32767;
        i32 cand[3];
        int nc = 0;
        cand[nc++] = 0;
        if (m0 > 0) cand[nc++] = m0;
        if (m0 + 1 <= 32767 && m0 + 1 != 0) cand[nc++] = m0 + 1;
        for (int s = 0; s < 3; ++s) {
            double best = kRdInf;
            i32 bm = 0;
            double bestnz = kRdInf;
            i32 bmnz = -1;
            for (int k = 0; k < nc; ++k) {
                i32 m = cand[k];
                double d = a - (double)m * st;
                double cost = d * d + lambda * (level_rate(rc, p, s, m) / 1024.0) +
                              prev[level_class(m)];
                if (cost < best) { best = cost; bm = m; }
                if (m != 0 && cost < bestnz) { bestnz = cost; bmnz = m; }
            }
            f[p][s] = best;
            best_m[p][s] = bm;
            if (s == 0) { fnz[p] = bestnz; best_m_nz[p] = bmnz; }
        }
        for (int s = 0; s < 3; ++s) prev[s] = f[p][s];
    }

    // Choose `last`.
    double best_total = rc.sym[ctx_cbf][0] * lambda / 1024.0 + energy;
    int best_last = -1;
    tail = 0;
    for (int p = ncoef - 1; p >= 0; --p) {
        if (fnz[p] < kRdInf) {
            double r = rc.sym[ctx_cbf][1];
            if (ncoef > 1) {
                int cls = last_class_of(p);
                r += rc.sym[ctx_last][cls] + (kLastRawBits[cls] << 10);
            }
            double total = fnz[p] + tail + lambda * (r / 1024.0);
            if (total < best_total) { best_total = total; best_last = p; }
        }
        double c = orig[scan[p]];
        tail += c * c;
    }

    for (int i = 0; i < ncoef; ++i) coefs[i] = 0;
    if (best_last < 0) return;
    int s = 0;
    for (int p = best_last; p >= 0; --p) {
        i32 m = (p == best_last) ? best_m_nz[p] : best_m[p][s];
        int idx = scan[p];
        coefs[idx] = (i16)(orig[idx] < 0 ? -m : m);
        s = level_class(m);
    }
}

// Encoder side: quantize a plane into `coefs` and leave the same
// reconstruction in s.samples that the decoder will produce.
static void analyze_plane(PlaneState &s, i16 *coefs, int tskip, int intra_dz) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    // block means
    std::vector<i32> m(ndc);
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            i32 sum = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    sum += s.samples[(size_t)(by * 8 + j) * size + bx * 8 + i];
            m[by * nb + bx] = (sum + 32) >> 6;
        }
    int dcqp = dc_qp_of(s.qp);
    int tdc = dequant_step(dcqp, 16);
    if (nb == 8) {
        i32 in[64];
        i16 out[64];
        for (int i = 0; i < 64; ++i) in[i] = m[i] - s.dc_off;
        fdct8x8(in, out);
        for (int i = 0; i < 64; ++i) coefs[i] = (i16)quantize(out[i], tdc, tdc / 3);
    } else {
        for (int i = 0; i < ndc; ++i)
            coefs[i] = (i16)quantize(m[i] - s.dc_off, tdc, tdc / 3);
    }
    // mirror the decoder's DC-plane reconstruction
    std::vector<i32> dc(ndc);
    for (int i = 0; i < ndc; ++i) dc[i] = dequant(coefs[i], tdc);
    if (nb == 8) {
        i32 in[64], out[64];
        for (int i = 0; i < 64; ++i) in[i] = dc[i];
        idct8x8(in, out);
        for (int i = 0; i < 64; ++i) dc[i] = out[i];
    }
    for (int i = 0; i < ndc; ++i)
        s.means[i] = clamp_i32(s.dc_off + dc[i], 0, s.maxval);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            s.pred[(size_t)y * size + x] = bilinear_q4_i32(
                s.means.data(), nb, nb, nb, 2 * x - 7, 2 * y - 7);
    // residual blocks
    i16 *bc = coefs + ndc;
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            i16 *c = bc + ((size_t)by * nb + bx) * 64;
            i32 res[64];
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    res[j * 8 + i] = s.samples[(size_t)y * size + x] -
                                     s.pred[(size_t)y * size + x];
                }
            if (tskip) {
                int t = dequant_step(s.qp, 16);
                for (int i = 0; i < 64; ++i)
                    c[i] = (i16)quantize(res[i], t, intra_dz ? t / 3 : t / 2);
            } else {
                i16 co[64];
                fdct8x8(res, co);
                for (int i = 0; i < 64; ++i) {
                    int t = dequant_step(s.qp, s.wmat[i]);
                    c[i] = (i16)quantize(co[i], t, t / 3);
                }
            }
        }
}

// Re-quantize the residual blocks of a plane with the RD trellis above.  The
// DC plane is deliberately left on the plain dead-zone quantizer: it is the
// intra predictor, so a level chosen there changes `pred` for all 64 blocks
// and the trellis's single-unit distortion model would be wrong about it.
static void rdoq_plane(PlaneState &s, i16 *coefs, int tskip, bool chroma,
                       const RateCost &rc, double lambda_scale) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    const double base = (double)kQStep[s.qp] / 16.0;
    const double lambda = lambda_scale * base * base;
    const u8 *scan = scan_table(64, tskip != 0);
    const int ctx_cbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
    const int ctx_last = chroma ? kCtxLastChroma : kCtxLastLuma;
    i32 stepv[64];
    if (tskip) {
        int t = dequant_step(s.qp, 16);
        for (int i = 0; i < 64; ++i) stepv[i] = t;
    } else {
        for (int i = 0; i < 64; ++i) stepv[i] = dequant_step(s.qp, s.wmat[i]);
    }
    i16 *bc = coefs + ndc;
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            i16 *c = bc + ((size_t)by * nb + bx) * 64;
            i32 res[64];
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    res[j * 8 + i] = s.samples[(size_t)y * size + x] -
                                     s.pred[(size_t)y * size + x];
                }
            i32 orig[64];
            if (tskip) {
                for (int i = 0; i < 64; ++i) orig[i] = res[i];
            } else {
                i16 co[64];
                fdct8x8(res, co);
                for (int i = 0; i < 64; ++i) orig[i] = co[i];
            }
            rdoq_unit(c, orig, stepv, 64, scan, ctx_cbf, ctx_last, rc, lambda);
        }
}

}  // namespace nxvc

// ============================================================== public API
using namespace nxvc;

struct nxvc_encoder {
    nxvc_config cfg{};
    Geometry g;
    FrameParams fp;
    std::vector<u8> custom_matrix;
    std::vector<nxvc_tile_info> tiles;
    std::vector<u8> tlv;
    u8 pose[26] = {};
    u32 frame_number = 0;
    nxvc_encode_stats stats{};
};

struct nxvc_decoder {
    bool have_stream = false;
    Geometry g;
    nxvc_stream_info si{};
    FrameParams fp;
    nxvc_frame_info fi{};
    std::vector<nxvc_tile_info> tiles;
};

#include "codec_impl.inc"

extern "C" {

void nxvc_config_default(nxvc_config *cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->width = 256;
    cfg->height = 256;
    cfg->chroma = NXVC_CHROMA_420;
    cfg->bit_depth = 8;
    cfg->color_transform = NXVC_CT_NONE;
    cfg->base_qp = 24;
    cfg->quant_matrix = 1;
    // Defaults chosen for rate: the rANS flush is 4 bytes per lane, which
    // dominates cheap tiles, and per-frame tables are worth 10-45%.
    cfg->nsub_log2 = 255;   // auto, capped at 8 lanes
    cfg->custom_tables = 1;
    cfg->profile = 1;
    cfg->level = 1;
    // Rate-distortion quantization is on by default: it is encoder-only work
    // that costs no syntax and, measured on the quality harness, is the single
    // largest remaining win over the plain dead-zone quantizer.
    cfg->rdo = 1;
    cfg->rdo_lambda_q8 = 0;   // built-in default
    cfg->qp_search = 0;
    cfg->wm_id = 0;           // frame matrix everywhere (see --wm)
}

void nxvc_tile_layout_get(uint32_t w, uint32_t h, nxvc_tile_layout *out) {
    if (!out) return;
    out->tiles_x = (w + 63) / 64;
    out->tiles_y = (h + 63) / 64;
    out->tile_count = out->tiles_x * out->tiles_y;
    out->tile_size = 64;
}

#define NXVC_STR2(x) #x
#define NXVC_STR(x) NXVC_STR2(x)
const char *nxvc_version_string(void) {
    return "nxvc_ref " NXVC_STR(NXVC_VERSION) "." NXVC_STR(NXVC_BITSTREAM_MINOR)
           " (syntax v" NXVC_STR(NXVC_VERSION) "." NXVC_STR(NXVC_BITSTREAM_MINOR) ")";
}

const char *nxvc_status_string(nxvc_status st) {
    switch (st) {
        case NXVC_OK: return "ok";
        case NXVC_ERR_ARG: return "bad argument";
        case NXVC_ERR_UNSUPPORTED: return "unsupported syntax";
        case NXVC_ERR_BITSTREAM: return "malformed bitstream";
        case NXVC_ERR_TRUNCATED: return "truncated bitstream";
        case NXVC_ERR_NOMEM: return "buffer too small";
        case NXVC_ERR_VERSION: return "unsupported version or tools";
    }
    return "unknown";
}

void nxvc_ycocgr_forward(const uint8_t *r, const uint8_t *g, const uint8_t *b,
                         uint8_t *y, uint16_t *co, uint16_t *cg, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        i32 R = r[i], G = g[i], B = b[i];
        i32 Co = R - B;
        i32 t = B + (Co >> 1);
        i32 Cg = G - t;
        i32 Y = t + (Cg >> 1);
        y[i] = (u8)Y;
        co[i] = (u16)(Co + 256);
        cg[i] = (u16)(Cg + 256);
    }
}

void nxvc_ycocgr_inverse(const uint8_t *y, const uint16_t *co,
                         const uint16_t *cg, uint8_t *r, uint8_t *g,
                         uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        i32 Y = y[i], Co = (i32)co[i] - 256, Cg = (i32)cg[i] - 256;
        i32 t = Y - (Cg >> 1);
        i32 G = Cg + t;
        i32 B = t - (Co >> 1);
        i32 R = B + Co;
        r[i] = (u8)clamp_i32(R, 0, 255);
        g[i] = (u8)clamp_i32(G, 0, 255);
        b[i] = (u8)clamp_i32(B, 0, 255);
    }
}

}  // extern "C"
