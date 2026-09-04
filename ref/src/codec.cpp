// nxvc_ref: bitstream syntax, encoder and decoder.  See docs/SYNTAX.md.
#include <algorithm>
#include <cmath>
#ifdef NXVC_XFORM_CTX_EXPERIMENT
#include <cstdio>
#endif
#include <cstdlib>
#include <new>

#include "common.h"
#include "entropy.h"
#include "inter.h"
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
    u32 width = 0, height = 0;   // PER EYE (Annex D D-3: a picture is one eye)
    u32 eyes = 1;
    u32 chroma = 0;       // nxvc_chroma
    u32 color_transform = 0;
    u32 color_space = 0;
    u32 alpha = 0;
    u32 tiles_x = 0, tiles_y = 0;   // per eye: cols_per_eye and rows
    u32 cw = 0, ch = 0;   // chroma plane dimensions, per eye
    u32 ntiles() const { return eyes * tiles_x * tiles_y; }
    // Per-eye extent of plane p, in samples.
    u32 pw(int p) const { return (p == 1 || p == 2) ? cw : width; }
    u32 ph(int p) const { return (p == 1 || p == 2) ? ch : height; }
    // Chroma subsampling factor of plane p (1 or 2), the factor the warp
    // matrix and the motion vector are conjugated by.
    int psub(int p) const {
        return (p == 1 || p == 2) && chroma == NXVC_CHROMA_420 ? 2 : 1;
    }
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
    int warp_present = 0;       // frame flags bit 3
    WarpMatrix warp[2];         // warp_ext(), one record per eye
    int inter = 0;              // stream tool bit 10
    int stereo = 0;             // stream tool bit 12
    int nctx = kNumCtxV1;   // 12 or 16, from the stream's CTX_V2 tool bit
    int intra_dir = 0;      // stream tool bit 17
    int dir_layer = 0;      // frame flags bit 2
    int sdh = 0;            // stream tool bit 22
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
    int ref_sel = 0, tskip = 0, wgt = 0, wm_id = 0, xform_size = 0;
    int mv_x = 0, mv_y = 0, alpha_value = 255;
    int disparity = 0;   // STEREO only, quarter samples, 12 bits
    int skipped = 0;     // signalled by skip_bitmap, no tile structure at all
};

// Does this mode read the frame's warp matrix?  STATIC_MV and STEREO use the
// identity predictor and do not (Annex D D-1).
static inline bool mode_needs_warp(int mode) {
    return mode == NXVC_MODE_WARP_SKIP || mode == NXVC_MODE_WARP_MV;
}
static inline bool mode_is_inter(int mode) { return mode != NXVC_MODE_INTRA; }

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
    w1 |= ((u32)t.xform_size & 3) << 28;
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
    t.xform_size = (w1 >> 28) & 3;
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
    int bsize = 8;     // transform edge, 8/16/32 (SYNTAX.md 6.7)
    int log2b = 3;     // log2(bsize)
    int nb = 0;        // blocks per edge, size / bsize
    int qp = 0;
    const u8 *wmat = nullptr;
    int maxval = 255, dc_off = 128;
    std::vector<i32> samples;  // size*size, source (encoder) / recon (decoder)
    std::vector<i32> means;    // nb*nb reconstructed block means
    std::vector<i32> pred;     // size*size, the final prediction
    std::vector<i32> wpred;    // size*size, the inter predictor (empty = intra)
    std::vector<i32> recon;    // size*size, INTRA_DIR: running reconstruction
    std::vector<u8> modes;     // nb*nb, INTRA_DIR: per-block intra mode
};

struct TileCoder {
    const Geometry *g = nullptr;
    const FrameParams *fp = nullptr;
    TileParams tp;
    int nplanes = 3;
    int intra_dir = 0;   // stream tool bit 17
    int dir_layer = 0;   // frame flag bit 2: predict the DC-plane residual
    int sdh = 0;         // stream tool bit 22
    int nctx = kNumCtxV1;
    int inter = 0;       // tp.mode != INTRA
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
    inter = mode_is_inter(tp.mode) ? 1 : 0;
    // The directional predictor and its mode unit belong to INTRA tiles: an
    // inter tile's prediction is the warp, and a mode unit there would code
    // nine ways of saying nothing.  SYNTAX.md 9.6.
    intra_dir = fp->intra_dir && !inter;
    dir_layer = fp->dir_layer;
    sdh = fp->sdh;
    nctx = fp->nctx;
    int qp = clamp_i32(fp->base_qp + tp.qp_delta, 0, 63);
    for (int p = 0; p < nplanes; ++p) {
        PlaneState &s = pl[p];
        bool chroma = (p == 1 || p == 2);
        s.size = chroma ? tg.chroma_size : tg.coded_size;
        // The tile's transform size, capped by the plane's own coded extent:
        // a plane never carries a block larger than itself, so no combination
        // of res_level, chroma format and xform_size is illegal (SYNTAX 6.7).
        s.bsize = block_edge_for(tp.xform_size, s.size);
        s.log2b = log2_of(s.bsize);
        s.nb = s.size / s.bsize;
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
        if (inter) s.wpred.assign((size_t)s.size * s.size, 0);
        if (intra_dir) {
            s.recon.assign((size_t)s.size * s.size, 0);
            s.modes.assign((size_t)s.nb * s.nb, 0);
        }
    }
}

void TileCoder::build_units() {
    size_t total = 0;
    int np = nplanes;
    if (tp.alpha_mode != 2) np = std::min(np, 3);
    for (int p = 0; p < np; ++p) {
        const size_t nblk = (size_t)pl[p].nb * pl[p].nb;
        const size_t ncoef = (size_t)pl[p].bsize * pl[p].bsize;
        total += nblk;                                     // DC plane
        total += nblk * ncoef;                             // blocks
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
        // The v2 model gives the DC plane its own CBF, LAST and LEVEL
        // contexts: it is a dense, low-frequency image, nothing like the
        // sparse AC blocks it used to share statistics with.
        u.ctx_cbf = nctx >= kNumCtxV2 ? (u8)kCtxCbfDc : ccbf;
        u.ctx_last = nctx >= kNumCtxV2 ? (u8)kCtxLastDc : clast;
        u.ctx_level = nctx >= kNumCtxV2 ? (u8)kCtxLevelDc : 0;
        u.sdh = (u8)sdh;
        units.push_back(u);
        off += ndc;
        if (intra_dir) {
            Unit mu{};
            mu.kind = UNIT_MODE;
            mu.modes = s.modes.data();
            mu.nbx = (u8)s.nb;
            mu.ctx_mode = nctx >= kNumCtxV2 ? (u8)kCtxMode : 0;
            mu.scan = scan_table(1, false);
            units.push_back(mu);
        }
        const int ncoef = s.bsize * s.bsize;
        for (int b = 0; b < ndc; ++b) {
            Unit v{};
            v.coef = &coef[off];
            v.ncoef = (u16)ncoef;
            v.scan = scan_table(ncoef, tp.tskip != 0);
            v.ctx_cbf = ccbf;
            v.ctx_last = clast;
            v.sdh = (u8)sdh;
            units.push_back(v);
            off += ncoef;
        }
    }
}

// -------------------------------------------------- directional prediction
// Reference samples for the 8x8 block at (bx, by), normative.
//
// The neighbour source is the tile's own running reconstruction for every
// block already reconstructed in raster order, and the DC-plane prediction
// everywhere else -- which is what makes a tile independent: the tile's top
// and left borders never read a neighbouring tile, they read `pred`, which is
// derived from this tile's own DC plane.  Coordinates are clamped into the
// tile, so the reference array is always fully populated.
//
// `base` is the plane the residual is measured against: `pred` in the default
// (replace) form, so the references are reconstructed samples; the all-zero
// plane in the layered form (frame flag bit 2), so the references are
// reconstructed *DC-plane residuals*.
struct IntraRefs {
    // a[0] = corner, a[1 + k] = top[k],  k = 0 .. 2*bsize-1
    i32 a[2 * kMaxBlock + 1];
    i32 l[2 * kMaxBlock + 1];
};

// Development hook for the Pass B wavefront cost study (ref/RESULTS-intra.md
// section 5).  Build with -DNXVC_DIR_SCHED_EXPERIMENT and set NXVC_DIR_SCHED
// to 1 (drop the top-right reference), 2 (confine the dependency to 32x32
// sub-tiles) or 3 (both) to measure what each restriction costs in rate.
//
// A stream produced with a nonzero value is NOT conformant -- the reference
// derivation in SYNTAX.md 7.4 is the one with the full raster dependency --
// which is why the hook is compiled out of a normal build rather than merely
// defaulted off.
#ifdef NXVC_DIR_SCHED_EXPERIMENT
static int dir_sched() {
    static const int v = [] {
        const char *e = std::getenv("NXVC_DIR_SCHED");
        return e ? clamp_i32(std::atoi(e), 0, 3) : 0;
    }();
    return v;
}
#else
static inline int dir_sched() { return 0; }
#endif

static void build_refs(const i32 *recon, const i32 *fallback, int size, int bx,
                       int by, int log2b, IntraRefs &r) {
    const int n = 1 << log2b;
    const int x0 = bx << log2b, y0 = by << log2b;
    const int sched = dir_sched();
    auto at = [&](int x, int y) -> i32 {
        int cx = clamp_i32(x, 0, size - 1), cy = clamp_i32(y, 0, size - 1);
        int nbx = cx >> log2b, nby = cy >> log2b;
        bool done = (nby < by) || (nby == by && nbx < bx);
        if ((sched & 1) && nbx > bx) done = false;          // no top-right
        // 32x32 sub-tiles: the sub-tile index is the block index divided by
        // the number of blocks a 32x32 quadrant holds.
        const int qsh = 5 - log2b > 0 ? 5 - log2b : 0;
        if ((sched & 2) && ((nbx >> qsh) != (bx >> qsh) ||
                            (nby >> qsh) != (by >> qsh)))
            done = false;
        const i32 *src = done ? recon : fallback;
        return src[(size_t)cy * size + cx];
    };
    r.a[0] = r.l[0] = at(x0 - 1, y0 - 1);
    for (int k = 0; k < 2 * n; ++k) {
        r.a[1 + k] = at(x0 + k, y0 - 1);
        r.l[1 + k] = at(x0 - 1, y0 + k);
    }
}

// P[j * n + i] for one n x n block, n = 1 << log2b.  Every mode but
// kIntraDcPlane is a weighted average of in-range references, so no clamp is
// needed and none is applied.  The formulas are those of SYNTAX.md 7.4 with
// the block edge left as `n`: at n == 8 they are the v1.3 predictors
// character for character.
static void predict_block(int mode, const IntraRefs &r, const i32 *base,
                          int size, int bx, int by, int log2b, i32 *P) {
    const i32 *A = r.a + 1;  // A[-1] == corner
    const i32 *L = r.l + 1;  // L[-1] == corner
    const i32 tl = r.a[0];
    const int n = 1 << log2b;
    const int sh = log2b + 1;         // the planar / DC averaging shift
    const int rnd = 1 << log2b;       // ... and its rounding term, n
    switch (mode) {
        case kIntraDcPlane:
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                    P[j * n + i] =
                        base[(size_t)((by << log2b) + j) * size +
                             (bx << log2b) + i];
            return;
        case kIntraDc: {
            i32 sum = 0;
            for (int k = 0; k < n; ++k) sum += A[k] + L[k];
            i32 dc = (sum + rnd) >> sh;
            for (int i = 0; i < n * n; ++i) P[i] = dc;
            return;
        }
        case kIntraPlanar:
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                    P[j * n + i] = ((n - 1 - i) * L[j] + (i + 1) * A[n] +
                                    (n - 1 - j) * A[i] + (j + 1) * L[n] + rnd) >> sh;
            return;
        case kIntraH:
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) P[j * n + i] = L[j];
            return;
        case kIntraV:
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) P[j * n + i] = A[i];
            return;
        case kIntraDdl:
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    int k = i + j;
                    P[j * n + i] = (k == 2 * n - 2)
                        ? (A[2 * n - 2] + 3 * A[2 * n - 1] + 2) >> 2
                        : (A[k] + 2 * A[k + 1] + A[k + 2] + 2) >> 2;
                }
            return;
        case kIntraDdr:
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    if (i > j) {
                        int k = i - j;
                        P[j * n + i] =
                            (A[k - 2] + 2 * A[k - 1] + A[k] + 2) >> 2;
                    } else if (i < j) {
                        int k = j - i;
                        P[j * n + i] =
                            (L[k - 2] + 2 * L[k - 1] + L[k] + 2) >> 2;
                    } else {
                        P[j * n + i] = (A[0] + 2 * tl + L[0] + 2) >> 2;
                    }
                }
            return;
        case kIntraVr:
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    int z = 2 * i - j;
                    int k = i - (j >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        P[j * n + i] = (A[k - 1] + A[k] + 1) >> 1;
                    else if (z >= 0)
                        P[j * n + i] =
                            (A[k - 2] + 2 * A[k - 1] + A[k] + 2) >> 2;
                    else if (z == -1)
                        P[j * n + i] = (L[0] + 2 * tl + A[0] + 2) >> 2;
                    else {
                        int q = j - 2 * i;
                        P[j * n + i] =
                            (L[q - 1] + 2 * L[q - 2] + L[q - 3] + 2) >> 2;
                    }
                }
            return;
        default:  // kIntraHd
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    int z = 2 * j - i;
                    int k = j - (i >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        P[j * n + i] = (L[k - 1] + L[k] + 1) >> 1;
                    else if (z >= 0)
                        P[j * n + i] =
                            (L[k - 2] + 2 * L[k - 1] + L[k] + 2) >> 2;
                    else if (z == -1)
                        P[j * n + i] = (A[0] + 2 * tl + L[0] + 2) >> 2;
                    else {
                        int q = i - 2 * j;
                        P[j * n + i] =
                            (A[q - 1] + 2 * A[q - 2] + A[q - 3] + 2) >> 2;
                    }
                }
            return;
    }
}

// The weighting matrix entry of an n x n block at raster index i.  The
// transmitted matrix is always the 8x8 one; a larger block replicates it, so
// entry (u, v) of an n x n block is entry (u >> k, v >> k) of the 8x8 matrix
// with k = log2(n) - 3.  The roll-off therefore covers the same fraction of
// the frequency plane at every size, and no extra matrix is transmitted.
// SYNTAX.md 6.5.
static inline int block_weight(const PlaneState &s, int i) {
    const int k = s.log2b - 3;
    const int u = (i >> s.log2b) >> k, v = (i & (s.bsize - 1)) >> k;
    return s.wmat[u * 8 + v];
}

// Dequantize + inverse transform one residual block.
static void residual_block(const i16 *c, const PlaneState &s, int tskip,
                           i32 *res) {
    const int ncoef = s.bsize * s.bsize;
    if (tskip) {
        int t = dequant_step(s.qp, 16);
        for (int i = 0; i < ncoef; ++i) res[i] = dequant(c[i], t);
    } else {
        i32 dq[kMaxBlock * kMaxBlock];
        for (int i = 0; i < ncoef; ++i)
            dq[i] = dequant(c[i], dequant_step(s.qp, block_weight(s, i)));
        idct_block(dq, res, s.bsize);
    }
}

// The DC plane and the bilinear prediction it drives: s.means and s.pred.
// Shared by the encoder's analysis pass and the decoder.
//
// For an inter tile (s.wpred non-empty) the DC plane codes the block means of
// the residual against the warp predictor, and the final prediction is
//
//     pred = clamp(wpred + planar(means) - dc_offset, 0, maxval)
//
// so the unit list, the scan, the contexts and the block syntax are exactly
// the intra ones and the DC plane doubles as the per-block DC correction the
// warp needs.  On a well-predicted tile every DC-plane coefficient is zero and
// the whole structure costs one CBF symbol.
static void reconstruct_dc_plane(PlaneState &s, const i16 *coefs) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
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
    // An intra tile's block mean is a sample value and is clamped to the
    // sample domain.  An inter tile's is `dc_offset + a residual mean`, whose
    // legal range is wider than the sample domain on both sides; clamping it
    // there would silently cap the DC correction the warp needs.  dequant()
    // has already bounded |dc| by 32767.
    for (int i = 0; i < ndc; ++i)
        s.means[i] = s.wpred.empty() ? clamp_i32(s.dc_off + dc[i], 0, s.maxval)
                                     : s.dc_off + dc[i];
    // Planar prediction: bilinear over block centres.  Block (bx, by)'s mean
    // sits at sample (bx*bs + (bs-1)/2, ...), so the Q4 coordinate of sample x
    // in the means grid is (16*x - 8*(bs-1)) / bs, evaluated as a rounding
    // shift.  At bs == 8 that is exactly 2*x - 7, the v1 formula.
    const int bs = s.bsize, lb = s.log2b;
    for (int y = 0; y < size; ++y) {
        const i32 uy = (16 * y - 8 * (bs - 1) + (bs >> 1)) >> lb;
        for (int x = 0; x < size; ++x) {
            const i32 ux = (16 * x - 8 * (bs - 1) + (bs >> 1)) >> lb;
            s.pred[(size_t)y * size + x] =
                bilinear_q4_i32(s.means.data(), nb, nb, nb, ux, uy);
        }
    }
    if (!s.wpred.empty()) {
        for (size_t i = 0; i < s.pred.size(); ++i)
            s.pred[i] = clamp_i32(s.wpred[i] + s.pred[i] - s.dc_off, 0, s.maxval);
    }
}

// Reconstruct one plane from its coefficients (normative decode path).
// With `dir` the per-block intra modes in s.modes select the predictor and
// the blocks are reconstructed in raster order, each seeing the ones before
// it; `layer` makes the modes predict the DC-plane residual instead of the
// samples.  Without `dir` this is the v1 predictor exactly.
static void reconstruct_plane(PlaneState &s, const i16 *coefs, int tskip,
                              int dir = 0, int layer = 0) {
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    reconstruct_dc_plane(s, coefs);
    const i16 *bc = coefs + ndc;
    if (!dir) {
        for (int by = 0; by < nb; ++by)
            for (int bx = 0; bx < nb; ++bx) {
                const i16 *c = bc + ((size_t)by * nb + bx) * ncoef;
                i32 res[kMaxBlock * kMaxBlock];
                residual_block(c, s, tskip, res);
                for (int j = 0; j < bs; ++j)
                    for (int i = 0; i < bs; ++i) {
                        int y = (by << lb) + j, x = (bx << lb) + i;
                        s.samples[(size_t)y * size + x] = clamp_i32(
                            s.pred[(size_t)y * size + x] + res[j * bs + i], 0,
                            s.maxval);
                    }
            }
        return;
    }
    // --- directional intra
    if ((int)s.recon.size() != size * size) s.recon.assign((size_t)size * size, 0);
    // In the layered form the reference plane is the reconstructed DC-plane
    // *residual*, whose out-of-block fallback is zero, not `pred`.
    std::vector<i32> zero;
    const i32 *fallback = s.pred.data();
    if (layer) {
        zero.assign((size_t)size * size, 0);
        fallback = zero.data();
    }
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            const i16 *c = bc + ((size_t)by * nb + bx) * ncoef;
            i32 res[kMaxBlock * kMaxBlock], P[kMaxBlock * kMaxBlock];
            residual_block(c, s, tskip, res);
            IntraRefs r;
            build_refs(s.recon.data(), fallback, size, bx, by, lb, r);
            predict_block(s.modes[(size_t)by * nb + bx], r, fallback, size, bx,
                          by, lb, P);
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    int y = (by << lb) + j, x = (bx << lb) + i;
                    i32 v = P[j * bs + i] + res[j * bs + i];
                    if (layer) {
                        // the mode predicts the residual; the DC plane is the
                        // base, and the running reference plane is that
                        // residual, so it is stored unclamped-by-the-base.
                        i32 full = clamp_i32(s.pred[(size_t)y * size + x] + v,
                                             0, s.maxval);
                        s.recon[(size_t)y * size + x] =
                            full - s.pred[(size_t)y * size + x];
                        s.samples[(size_t)y * size + x] = full;
                    } else {
                        i32 full = clamp_i32(v, 0, s.maxval);
                        s.recon[(size_t)y * size + x] = full;
                        s.samples[(size_t)y * size + x] = full;
                    }
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
                             int band_shift, i32 m) {
    int ctx = level_ctx(scan_pos, prev_class, band_shift);
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
                      const u16 *scan, int ctx_cbf, int ctx_last,
                      const RateCost &rc, double lambda) {
    // Buffers for the largest unit the format has, a 32x32 block.  They are
    // thread_local rather than automatic because a 1024-position trellis is
    // 40 kB and this runs once per candidate per block.
    constexpr int kMaxCoef = kMaxBlock * kMaxBlock;
    static thread_local double f[kMaxCoef][3], fnz[kMaxCoef];
    static thread_local i32 best_m[kMaxCoef][3], best_m_nz[kMaxCoef];
    const int band_shift = last_shift_of(ncoef);
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
                double cost = d * d +
                              lambda * (level_rate(rc, p, s, band_shift, m) /
                                        1024.0) +
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
                int cls = last_class_of(p >> band_shift);
                r += rc.sym[ctx_last][cls] +
                     ((kLastRawBits[cls] + band_shift) << 10);
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

// ------------------------------------------------------- sign data hiding
// The sign at scan position `last` is not coded; it is the parity of the sum
// of the unit's absolute levels.  The encoder therefore has to make that
// parity agree with the sign it wants, by moving exactly one level by one
// step.  It picks the move that costs the least squared error, over every
// scan position below `last`: raising a level, lowering a nonzero one, or
// creating a new nonzero (whose sign then follows the unquantized value).
// `orig[i]` is the unquantized value and `step[i]` its reconstruction step
// (Q4), the same convention rdoq_unit uses.  Encoder only.
static void hide_sign_unit(i16 *coefs, const i32 *orig, const i32 *step,
                           int ncoef, const u16 *scan) {
    int last = -1;
    i32 sum = 0;
    for (int p = 0; p < ncoef; ++p) {
        i32 q = coefs[scan[p]];
        i32 m = q < 0 ? -q : q;
        sum += m;
        if (m) last = p;
    }
    if (last < kSdhMinLast) return;
    const int want = coefs[scan[last]] < 0 ? 1 : 0;
    if ((sum & 1) == want) return;
    double best = 0;
    int best_p = -1, best_d = 0;
    for (int p = 0; p <= last; ++p) {
        int idx = scan[p];
        double a = orig[idx] < 0 ? -(double)orig[idx] : (double)orig[idx];
        double st = (double)step[idx] / 16.0;
        i32 q = coefs[idx];
        i32 m = q < 0 ? -q : q;
        for (int d = -1; d <= 1; d += 2) {
            i32 m2 = m + d;
            if (m2 < 0 || m2 > 32767) continue;
            // never move `last` itself: zeroing it would change LAST, and
            // changing its magnitude is allowed only away from zero.
            if (p == last && m2 == 0) continue;
            double e1 = a - (double)m * st, e2 = a - (double)m2 * st;
            double cost = e2 * e2 - e1 * e1;
            if (best_p < 0 || cost < best) { best = cost; best_p = p; best_d = d; }
        }
    }
    if (best_p < 0) return;
    int idx = scan[best_p];
    i32 q = coefs[idx];
    i32 m = (q < 0 ? -q : q) + best_d;
    bool neg = q != 0 ? (q < 0) : (orig[idx] < 0);
    coefs[idx] = (i16)(neg ? -m : m);
}

// Encoder side: quantize the DC plane and mirror the decoder's
// reconstruction of it into s.means / s.pred.
static void analyze_dc_plane(PlaneState &s, i16 *coefs, int sdh) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    std::vector<i32> m(ndc);
    const bool inter = !s.wpred.empty();
    // The block mean is (sum of the bsize^2 samples + half) >> 2*log2(bsize),
    // rounded away from zero; at bsize 8 that is the v1 `(sum + 32) >> 6`.
    const int bs = s.bsize, lb = s.log2b;
    const int msh = 2 * lb, mrnd = 1 << (msh - 1);
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            i32 sum = 0;
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    size_t k = (size_t)((by << lb) + j) * size + (bx << lb) + i;
                    // For an inter tile the DC plane codes the mean of the
                    // residual, offset back so the quantized quantity is the
                    // same `m - dc_off` in both paths.
                    sum += s.samples[k] - (inter ? s.wpred[k] - s.dc_off : 0);
                }
            i32 mean = sum >= 0 ? (sum + mrnd) >> msh
                                : -((-sum + mrnd) >> msh);
            m[by * nb + bx] = mean;
        }
    int dcqp = dc_qp_of(s.qp);
    int tdc = dequant_step(dcqp, 16);
    i32 orig[64];
    if (nb == 8) {
        i32 in[64];
        i16 out[64];
        for (int i = 0; i < 64; ++i) in[i] = m[i] - s.dc_off;
        fdct8x8(in, out);
        for (int i = 0; i < 64; ++i) {
            orig[i] = out[i];
            coefs[i] = (i16)quantize(out[i], tdc, tdc / 3);
        }
    } else {
        for (int i = 0; i < ndc; ++i) {
            orig[i] = m[i] - s.dc_off;
            coefs[i] = (i16)quantize(orig[i], tdc, tdc / 3);
        }
    }
    if (sdh) {
        i32 stepv[64];
        for (int i = 0; i < ndc; ++i) stepv[i] = tdc;
        hide_sign_unit(coefs, orig, stepv, ndc, scan_table(ndc, false));
    }
    reconstruct_dc_plane(s, coefs);
    (void)size;
}

// Encoder side: quantize a plane into `coefs` and leave the same
// reconstruction in s.samples that the decoder will produce.
static void analyze_plane(PlaneState &s, i16 *coefs, int tskip, int intra_dz,
                          int sdh) {
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    analyze_dc_plane(s, coefs, sdh);
    // residual blocks
    i16 *bc = coefs + ndc;
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            i16 *c = bc + ((size_t)by * nb + bx) * ncoef;
            i32 res[kMaxBlock * kMaxBlock];
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    int y = (by << lb) + j, x = (bx << lb) + i;
                    res[j * bs + i] = s.samples[(size_t)y * size + x] -
                                      s.pred[(size_t)y * size + x];
                }
            i32 orig[kMaxBlock * kMaxBlock], stepv[kMaxBlock * kMaxBlock];
            if (tskip) {
                int t = dequant_step(s.qp, 16);
                for (int i = 0; i < ncoef; ++i) {
                    orig[i] = res[i];
                    stepv[i] = t;
                    c[i] = (i16)quantize(res[i], t, intra_dz ? t / 3 : t / 2);
                }
            } else {
                i16 co[kMaxBlock * kMaxBlock];
                fdct_block(res, co, bs);
                for (int i = 0; i < ncoef; ++i) {
                    int t = dequant_step(s.qp, block_weight(s, i));
                    orig[i] = co[i];
                    stepv[i] = t;
                    c[i] = (i16)quantize(co[i], t, t / 3);
                }
            }
            if (sdh)
                hide_sign_unit(c, orig, stepv, ncoef,
                               scan_table(ncoef, tskip != 0));
        }
}

// Re-quantize the residual blocks of a plane with the RD trellis above.  The
// DC plane is deliberately left on the plain dead-zone quantizer: it is the
// intra predictor, so a level chosen there changes `pred` for all the blocks
// of the plane and the trellis's single-unit distortion model would be wrong
// about it.
static void rdoq_plane(PlaneState &s, i16 *coefs, int tskip, bool chroma,
                       const RateCost &rc, double lambda_scale, int sdh) {
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    const double base = (double)kQStep[s.qp] / 16.0;
    const double lambda = lambda_scale * base * base;
    const u16 *scan = scan_table(ncoef, tskip != 0);
    const int ctx_cbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
    const int ctx_last = chroma ? kCtxLastChroma : kCtxLastLuma;
    i32 stepv[kMaxBlock * kMaxBlock];
    if (tskip) {
        int t = dequant_step(s.qp, 16);
        for (int i = 0; i < ncoef; ++i) stepv[i] = t;
    } else {
        for (int i = 0; i < ncoef; ++i)
            stepv[i] = dequant_step(s.qp, block_weight(s, i));
    }
    i16 *bc = coefs + ndc;
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            i16 *c = bc + ((size_t)by * nb + bx) * ncoef;
            i32 res[kMaxBlock * kMaxBlock];
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    int y = (by << lb) + j, x = (bx << lb) + i;
                    res[j * bs + i] = s.samples[(size_t)y * size + x] -
                                      s.pred[(size_t)y * size + x];
                }
            i32 orig[kMaxBlock * kMaxBlock];
            if (tskip) {
                for (int i = 0; i < ncoef; ++i) orig[i] = res[i];
            } else {
                i16 co[kMaxBlock * kMaxBlock];
                fdct_block(res, co, bs);
                for (int i = 0; i < ncoef; ++i) orig[i] = co[i];
            }
            rdoq_unit(c, orig, stepv, ncoef, scan, ctx_cbf, ctx_last, rc,
                      lambda);
            if (sdh) hide_sign_unit(c, orig, stepv, ncoef, scan);
        }
}

#ifdef NXVC_XFORM_CTX_EXPERIMENT
// How often each intra mode is chosen at each transform size (SYNTAX.md 7.4:
// whether the larger sizes need all nine modes).  Encoder side, experiment
// build only.
u64 g_mode_hist[kXformSizes][kNumIntraModes];
#endif

// --------------------------------------------------- directional intra (enc)
// Sum of absolute Hadamard-transformed differences of an 8x8 residual: the
// mode-decision metric.  Encoder only.
static i32 satd8x8(const i32 d[64]) {
    i32 t[64];
    for (int j = 0; j < 8; ++j) {
        const i32 *r = d + j * 8;
        i32 a0 = r[0] + r[4], a1 = r[1] + r[5], a2 = r[2] + r[6], a3 = r[3] + r[7];
        i32 a4 = r[0] - r[4], a5 = r[1] - r[5], a6 = r[2] - r[6], a7 = r[3] - r[7];
        i32 b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
        i32 b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
        i32 *o = t + j * 8;
        o[0] = b0 + b1; o[1] = b0 - b1; o[2] = b2 + b3; o[3] = b2 - b3;
        o[4] = b4 + b5; o[5] = b4 - b5; o[6] = b6 + b7; o[7] = b6 - b7;
    }
    i32 sum = 0;
    for (int i = 0; i < 8; ++i) {
        i32 a0 = t[i] + t[32 + i], a1 = t[8 + i] + t[40 + i];
        i32 a2 = t[16 + i] + t[48 + i], a3 = t[24 + i] + t[56 + i];
        i32 a4 = t[i] - t[32 + i], a5 = t[8 + i] - t[40 + i];
        i32 a6 = t[16 + i] - t[48 + i], a7 = t[24 + i] - t[56 + i];
        i32 b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
        i32 b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
        i32 v[8] = {b0 + b1, b0 - b1, b2 + b3, b2 - b3,
                    b4 + b5, b4 - b5, b6 + b7, b6 - b7};
        for (int k = 0; k < 8; ++k) sum += v[k] < 0 ? -v[k] : v[k];
    }
    return sum;
}


// The mode-decision metric of an n x n residual: the sum of the SATDs of its
// 8x8 sub-blocks.  Encoder only.
static i32 satd_block(const i32 *d, int n) {
    if (n == 8) return satd8x8(d);
    i32 sum = 0;
    for (int by = 0; by < n; by += 8)
        for (int bx = 0; bx < n; bx += 8) {
            i32 sub[64];
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    sub[j * 8 + i] = d[(by + j) * n + bx + i];
            sum += satd8x8(sub);
        }
    return sum;
}

// Q10 bits one coding unit costs under `rc`, mirroring the LaneMachine.
static i32 unit_bits(const i16 *c, int ncoef, const u16 *scan, int ctx_cbf,
                     int ctx_last, int ctx_level, const RateCost &rc,
                     int sdh) {
    const int band_shift = last_shift_of(ncoef);
    int last = -1;
    for (int p = ncoef - 1; p >= 0; --p)
        if (c[scan[p]] != 0) { last = p; break; }
    if (last < 0) return rc.sym[ctx_cbf][0];
    i32 r = rc.sym[ctx_cbf][1];
    if (ncoef > 1) {
        int cls = last_class_of(last >> band_shift);
        r += rc.sym[ctx_last][cls] + ((kLastRawBits[cls] + band_shift) << 10);
    }
    if (last >= kSdhMinLast && sdh) r -= 1 << 10;
    int prev = 0;
    for (int p = last; p >= 0; --p) {
        i32 q = c[scan[p]];
        i32 m = q < 0 ? -q : q;
        int ctx = ctx_level ? ctx_level : level_ctx(p, prev, band_shift);
        r += rc.sym[ctx][m > 14 ? kEscSym : m];
        if (m > 14) r += escape_bits(m) << 10;
        if (m != 0) r += 1 << 10;
        prev = level_class(m);
    }
    return r;
}

// One plane, directional intra.  Blocks are visited in raster order and each
// is fully quantized and reconstructed before the next one sees it, so the
// encoder's references are exactly the decoder's.  Mode decision is SATD over
// all nine modes, then a real D + lambda*R comparison over the best `ncand`
// of them (plus the DC-plane mode, which is always considered so that the
// tool can never be worse than v1 on a block).
static void analyze_plane_dir(PlaneState &s, i16 *coefs, int tskip, int layer,
                              bool chroma, const RateCost &rc,
                              double lambda_scale, bool use_rdo, int ncand,
                              int mode_ctx, int sdh) {
    constexpr int kMaxCoef = kMaxBlock * kMaxBlock;
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    analyze_dc_plane(s, coefs, sdh);
    if ((int)s.recon.size() != size * size) s.recon.assign((size_t)size * size, 0);
    if ((int)s.modes.size() != ndc) s.modes.assign((size_t)ndc, 0);

    std::vector<i32> zero;
    const i32 *fallback = s.pred.data();
    if (layer) {
        zero.assign((size_t)size * size, 0);
        fallback = zero.data();
    }
    const double base = (double)kQStep[s.qp] / 16.0;
    const double lambda = lambda_scale * base * base;
    const u16 *scan = scan_table(ncoef, tskip != 0);
    const int ctx_cbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
    const int ctx_last = chroma ? kCtxLastChroma : kCtxLastLuma;
    i32 stepv[kMaxCoef];
    if (tskip) {
        int t = dequant_step(s.qp, 16);
        for (int i = 0; i < ncoef; ++i) stepv[i] = t;
    } else {
        for (int i = 0; i < ncoef; ++i)
            stepv[i] = dequant_step(s.qp, block_weight(s, i));
    }
    i16 *bc = coefs + ndc;

    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            const int bi = by * nb + bx;
            i16 *c = bc + (size_t)bi * ncoef;
            IntraRefs r;
            build_refs(s.recon.data(), fallback, size, bx, by, lb, r);
            // target: the samples this block must reproduce, in the domain
            // the modes predict (samples, or the DC-plane residual).
            i32 tgt[kMaxCoef];
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i) {
                    int y = (by << lb) + j, x = (bx << lb) + i;
                    i32 v = s.samples[(size_t)y * size + x];
                    if (layer) v -= s.pred[(size_t)y * size + x];
                    tgt[j * bs + i] = v;
                }
            // SATD over all nine modes.  The predictions are not kept -- at
            // 32x32 nine of them are 36 kB -- so the candidates are predicted
            // again below; predict_block is a few adds per sample.
            i32 P[kMaxCoef], d[kMaxCoef];
            i32 cost[kNumIntraModes];
            for (int m = 0; m < kNumIntraModes; ++m) {
                predict_block(m, r, fallback, size, bx, by, lb, P);
                for (int i = 0; i < ncoef; ++i) d[i] = tgt[i] - P[i];
                cost[m] = satd_block(d, bs);
            }
            // candidate list: the DC-plane mode plus the best `ncand` by SATD
            int cand[kNumIntraModes];
            int nc = 0;
            cand[nc++] = kIntraDcPlane;
            bool taken[kNumIntraModes] = {};
            taken[kIntraDcPlane] = true;
            for (int k = 0; k < ncand; ++k) {
                int bm = -1;
                for (int m = 0; m < kNumIntraModes; ++m)
                    if (!taken[m] && (bm < 0 || cost[m] < cost[bm])) bm = m;
                if (bm < 0) break;
                taken[bm] = true;
                cand[nc++] = bm;
            }
            const int mpm = mpm_of(s.modes.data(), nb, bi);
            double best = 0;
            int best_mode = kIntraDcPlane;
            static thread_local i16 best_c[kMaxCoef];
            static thread_local i32 best_rec[kMaxCoef];
            bool have = false;
            for (int k = 0; k < nc; ++k) {
                const int m = cand[k];
                predict_block(m, r, fallback, size, bx, by, lb, P);
                i32 res[kMaxCoef], orig[kMaxCoef];
                for (int i = 0; i < ncoef; ++i) res[i] = tgt[i] - P[i];
                if (tskip) {
                    for (int i = 0; i < ncoef; ++i) orig[i] = res[i];
                } else {
                    i16 co[kMaxCoef];
                    fdct_block(res, co, bs);
                    for (int i = 0; i < ncoef; ++i) orig[i] = co[i];
                }
                i16 q[kMaxCoef];
                if (use_rdo) {
                    rdoq_unit(q, orig, stepv, ncoef, scan, ctx_cbf, ctx_last,
                              rc, lambda);
                } else {
                    for (int i = 0; i < ncoef; ++i)
                        q[i] = (i16)quantize(orig[i], stepv[i], stepv[i] / 3);
                }
                if (sdh) hide_sign_unit(q, orig, stepv, ncoef, scan);
                i32 rr[kMaxCoef];
                residual_block(q, s, tskip, rr);
                // exact sample-domain distortion of this candidate
                double d2 = 0;
                i32 rec[kMaxCoef];
                for (int j = 0; j < bs; ++j)
                    for (int i = 0; i < bs; ++i) {
                        int y = (by << lb) + j, x = (bx << lb) + i;
                        i32 v = P[j * bs + i] + rr[j * bs + i];
                        i32 full = layer
                                       ? clamp_i32(s.pred[(size_t)y * size + x] + v,
                                                   0, s.maxval)
                                       : clamp_i32(v, 0, s.maxval);
                        rec[j * bs + i] =
                            layer ? full - s.pred[(size_t)y * size + x] : full;
                        double e = (double)s.samples[(size_t)y * size + x] -
                                   (double)full;
                        d2 += e * e;
                    }
                double bits =
                    unit_bits(q, ncoef, scan, ctx_cbf, ctx_last, 0, rc, sdh) /
                    1024.0;
                // mode signalling, exactly as the LaneMachine will code it
                if (m == mpm) {
                    bits += mode_ctx ? rc.sym[mode_ctx][0] / 1024.0 : 1.0;
                } else {
                    bits += mode_ctx
                                ? rc.sym[mode_ctx][1 + nonmpm_index(mpm, m)] / 1024.0
                                : 4.0;
                }
                double tc = d2 + lambda * bits;
                if (!have || tc < best) {
                    have = true;
                    best = tc;
                    best_mode = m;
                    std::memcpy(best_c, q, sizeof(i16) * ncoef);
                    std::memcpy(best_rec, rec, sizeof(i32) * ncoef);
                }
            }
#ifdef NXVC_XFORM_CTX_EXPERIMENT
            g_mode_hist[s.log2b - 3][best_mode]++;
#endif
            s.modes[bi] = (u8)best_mode;
            std::memcpy(c, best_c, sizeof(i16) * ncoef);
            for (int j = 0; j < bs; ++j)
                for (int i = 0; i < bs; ++i)
                    s.recon[(size_t)((by << lb) + j) * size + (bx << lb) + i] =
                        best_rec[j * bs + i];
        }
}

}  // namespace nxvc

// ============================================================== public API
using namespace nxvc;

// One tile's mode decision, taken once per frame and reused by the
// table-training pass and the encoding pass so the two cannot disagree.
struct TileDecision {
    int mode = NXVC_MODE_INTRA;
    int mv_x = 0, mv_y = 0;
    int ref_sel = 0;
    int disparity = 0;
    int skipped = 0;
};

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

    // --- Phase 2
    nxvc::RefRing ring;                      // the encoder's shadow of the client
    std::vector<nxvc::PredState> state;      // per tile position per eye
    std::vector<nxvc::PredState> state_pre;  // the same, before this frame
    std::vector<TileDecision> dec;
    std::vector<u8> skip_map;                // rc/'s force_warp_skip request
    std::vector<u16> age_since_coded;        // per tile position per eye
    std::vector<nxvc_view> views_cur;
    // The view each ring slot was rendered with, so the matrix a frame emits
    // is the one between its actual reference (N-1-ref_sel) and itself.
    std::vector<nxvc_view> views_slot[4];
    bool have_views = false;
    int last_slot = -1;          // ring slot the most recent frame wrote
    u32 last_frame_number = 0;
    // The encoder's shadow of what the client displays, in the OUTPUT domain,
    // written through exactly the same store_tile() the decoder uses.
    std::vector<u8> shadow_plane[4];
    nxvc_image shadow_img{};
    int last_warp_present = 0;
    nxvc::WarpMatrix last_warp[2];
};

struct nxvc_decoder {
    bool have_stream = false;
    Geometry g;
    nxvc_stream_info si{};
    FrameParams fp;
    nxvc_frame_info fi{};
    std::vector<nxvc_tile_info> tiles;

    // --- Phase 2
    nxvc::RefRing ring;
    std::vector<nxvc::PredState> state;
    std::vector<u8> lost;        // consumed by one decode_frame call
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
    // The v2 intra tools are on by default because they win on the quality
    // harness: together they are worth about -13 % BD-rate at the Phase 1
    // operating point (ref/RESULTS-intra.md).  Both set a tool bit, so a
    // decoder without them refuses the stream at the handshake rather than
    // misparsing it; `--intra-dir off --ctx v1` gets a v1.2 stream back.
    cfg->intra_dir = 1;
    cfg->intra_dir_layer = 0;  // the replace form, measured better than layer
    cfg->ctx_v2 = 1;
    cfg->intra_dir_cand = 0;   // built-in default (2 RD candidates + DC plane)
    cfg->sign_hide = 1;
    // Phase 2 is opt-in: the default configuration is the Phase 1 one, so an
    // existing caller and every syntax v1.3 conformance vector keep producing
    // byte-identical streams.  `--inter on` turns the inter path on.
    cfg->eyes = 1;
    cfg->inter = 0;
    cfg->stereo = 0;
    cfg->intra_period = 180;   // PAPER 2.6: 2 s at 90 Hz
    cfg->ref_sel = 0;
    cfg->mv_range = 16;        // PAPER 2.3 step 2
    cfg->skip_thresh = 0;      // built-in default
}

void nxvc_tile_layout_get(uint32_t w, uint32_t h, nxvc_tile_layout *out) {
    nxvc_tile_layout_get_ex(w, h, 1, out);
}

void nxvc_tile_layout_get_ex(uint32_t w, uint32_t h, uint32_t eyes,
                             nxvc_tile_layout *out) {
    if (!out) return;
    if (eyes == 0) eyes = 1;
    out->tiles_x = (w + 63) / 64;
    out->tiles_y = (h + 63) / 64;
    out->tile_count = eyes * out->tiles_x * out->tiles_y;
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
