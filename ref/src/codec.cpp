// nxvc_ref: bitstream syntax, encoder and decoder.  See docs/SYNTAX.md.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "common.h"
#include "entropy.h"
#include "entropy_lite.h"
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
    int nctx = kNumCtxV1;   // 12, 16 or 27, from the CTX_V2/CTX_V3 tool bits
    int tab_v2 = 0;         // tool bit 24: the compact table-set coding
    int intra_dir = 0;      // stream tool bit 17
    int split4 = 0;         // stream tool bit 19
    int cfl = 0;            // stream tool bit 24
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
    int split4 = 0;      // word1 bit 28: blocks may carry a 4x4 split flag
    int disparity = 0;   // STEREO only, quarter samples, 12 bits
    int skipped = 0;     // signalled by skip_bitmap, no tile structure at all
    // --- syntax v1.6, tool bits 28 and 29 (docs/SYNTAX.md 13.9 and 13.10)
    int near_skip = 0;      // this tile is named by the row's dc_bitmap: it
                            // is SKIPPED, and `corr` is its whole residual.
                            // Not a word1 bit -- the record and the bitmap
                            // are both in the tile-row header (3.3).
    int quad_mv = 0;        // word1 bit 31: `qmv` refines the tile vector
    i8 corr[3][3] = {};     // [plane][0]=dc, [1]=horizontal, [2]=vertical
    i8 qmv[4][2] = {};      // [quadrant][x,y], quarter samples, TL TR BL BR
};

// The near-skip correction (SYNTAX.md 13.9).  Three signed bytes per coded
// colour plane -- DC, horizontal ramp, vertical ramp -- and alpha is never
// corrected, so a near-skip tile may not carry a coded alpha plane.
//
// The record lives in the TILE-ROW header, not in the tile, and is named by a
// second per-row bitmap; a near-skip tile is a SKIPPED tile with a bias, not
// a very cheap coded one.  That placement is what makes the tool reach a
// warp-only chain at all -- a chain with no coded tiles -- and it is what
// left word1 with room for both transform tools and the quadrant flag.
//
// The three terms are not optional.  A per-tile "DC only" flag was measured
// on both branches and is not merged: the two ramps are three quarters of
// what the correction recovers, and the branch that made them optional never
// chose them on any tested material, so half its syntax went unexercised.
// One record size, always nine bytes, always fitted.
static inline int popcount64(u64 v) {
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}

constexpr int kNearSkipPlanes = 3;
constexpr int kNearSkipBytes = 3 * kNearSkipPlanes;

// Signed nibble, two's complement, -8..+7.  The quad_mv deltas are packed two
// to a byte and this is the only place they are unpacked.
static inline int sign_nibble(u32 v) {
    return (int)(v & 0xfu) - (int)((v & 8u) << 1);
}

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
    // The settled word1 layout, docs/TOOLBITS.md 4: 28 split4x4 (detail
    // merged first and keeps the bit it was authored on), 29-30 the transform
    // size, 31 quad_mv.  The near-skip correction is NOT here -- it lives in
    // the tile-ROW header, which is what left room for all four.
    w1 |= ((u32)t.split4 & 1) << 28;
    w1 |= ((u32)t.xform_size & 3) << 29;
    w1 |= ((u32)t.quad_mv & 1) << 31;
    bw.u32v(w0);
    bw.u32v(w1);
}

// The optional fields that follow the two header words, in the order
// SYNTAX.md 4.1 lists them: the vector, the quadrant deltas, the sub-intra
// quadrant, the constant alpha value, the near-skip correction.  One function
// writes them and one counts them, so a field can never be written in an order
// the size does not account for.
static int tile_field_bytes(const TileParams &t) {
    return (t.mv_present ? 2 : 0) + (t.quad_mv ? 4 : 0) +
           (t.alpha_mode == 1 ? 1 : 0);
}

static void emit_tile_fields(BW &bw, const TileParams &t) {
    if (t.mv_present) {
        if (t.mode == NXVC_MODE_STEREO) {
            bw.u16v((u32)(t.disparity & 0xfff));
        } else {
            bw.u8v((u8)(i8)t.mv_x);
            bw.u8v((u8)(i8)t.mv_y);
        }
    }
    if (t.quad_mv)
        for (int q = 0; q < 4; ++q)
            bw.u8v((u8)(((u32)t.qmv[q][0] & 0xfu) |
                        (((u32)t.qmv[q][1] & 0xfu) << 4)));
    if (t.alpha_mode == 1) bw.u8v((u8)t.alpha_value);
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
    t.split4 = (w1 >> 28) & 1;
    t.xform_size = (w1 >> 29) & 3;
    t.quad_mv = (w1 >> 31) & 1;
}

// ------------------------------------------------------------- tile coding
// Per-plane coding state inside one tile.
// The DC plane is 1/64 of the samples but carries the whole intra predictor,
// so it is quantized at half the tile's QP index (one step per two QP steps).
// Coarse block means make the planar prediction blocky, which the AC residual
// then has to pay for twice over; measured at +3 dB and -10% bits at QP 38.
static inline int dc_qp_of(int qp) { return qp >> 1; }

// The most blocks a plane of one tile can have: a 64x64 tile's luma at the
// smallest transform edge, 8, is 8x8 blocks.  A larger transform has fewer.
constexpr int kMaxBlocksPerPlane = 64;

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
    std::vector<u8> splits;    // nb*nb, XFORM_4X4_SPLIT: per-block split flag
};

// -------------------------------------------------------- chroma from luma
// What mode kIntraCfl needs: this tile's reconstructed luma plane, and how a
// chroma sample maps onto it.  `f` is luma_size / chroma_size and is 1
// (4:4:4) or 2 (4:2:0).  Empty `luma` means the mode is unavailable.
struct CflCtx {
    const i32 *luma = nullptr;
    int size = 0;      // luma plane edge
    int f = 1;
    int maxval = 255;  // of the *chroma* plane
    bool on() const { return luma != nullptr; }
};

struct TileCoder {
    const Geometry *g = nullptr;
    const FrameParams *fp = nullptr;
    TileParams tp;
    int nplanes = 3;
    int intra_dir = 0;   // stream tool bit 17
    int dir_layer = 0;   // frame flag bit 2: predict the DC-plane residual
    int sdh = 0;         // stream tool bit 22
    int split4 = 0;      // tile header bit 28: this tile codes split flags
    int cfl = 0;         // stream tool bit 24: chroma may use kIntraCfl
    int nctx = kNumCtxV1;
    int ctx_v3 = 0;      // stream tool bit 25
    int inter = 0;       // tp.mode != INTRA
    PlaneState pl[4];
    std::vector<i16> coef;
    std::vector<Unit> units;

    void setup();
    // Re-derive the 4x4 split flag after `tskip` has been decided.  The
    // decision happens after setup() has run and after the tile is loaded, so
    // this is the narrow half of setup()'s rule and NOT a second copy of it:
    // both call sites are exactly "tskip just changed".
    void apply_tskip_to_split() {
        if (tp.tskip) tp.split4 = 0;
        split4 = tp.split4;
        for (int p = 0; p < nplanes; ++p)
            if (!split4) pl[p].splits.clear();
    }
    void build_units();
    CflCtx cfl_for(int p) const;
    // Encoder only: drop the tile's split flags once the analysis has chosen
    // none, so the tile pays no flag bits at all.  The reconstruction is
    // unchanged because every flag is already zero.
    void clear_split();
};

static inline int dequant_step(int qp, int w) {
    // t is the quantizer step in Q4, bounded by 23170 * 32 / 16 = 46340.
    return (kQStep[qp] * w + 8) >> 4;
}
static inline i32 dequant(i32 q, i32 t) { return clamp16((q * t + 8) >> 4); }
// Development hook for the reconstruction-offset measurement in
// ref/RESULTS-detail-a.md section 3, which found the offset to be a loss at
// every operating point and therefore did *not* give it a tool bit.  `d` is
// Q6 of one quantiser step, applied toward zero.  Build with
// -DNXVC_RECON_OFFSET_EXPERIMENT and set NXVC_RECON_OFF on both the encoder
// and the decoder to reproduce; a normal build compiles it out, so no
// conforming stream can depend on it.
#ifdef NXVC_RECON_OFFSET_EXPERIMENT
static int recon_offset_q6() {
    static const int v = [] {
        const char *e = std::getenv("NXVC_RECON_OFF");
        return e ? clamp_i32(std::atoi(e), -32, 32) : 0;
    }();
    return v;
}
static inline i32 dequant_off(i32 q, i32 t, int d) {
    i32 c = clamp16((q * t + 8) >> 4);
    if (!d || !q) return c;
    i32 o = (t * d + 512) >> 10;
    return clamp16(q > 0 ? c - o : c + o);
}
#else
static inline int recon_offset_q6() { return 0; }
static inline i32 dequant_off(i32 q, i32 t, int) { return dequant(q, t); }
#endif
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
    // The 4x4 split is mutually exclusive with transform skip, whose 64 coded
    // values are samples in raster order and have no sub-block structure, and
    // it is meaningful ONLY at the 8x8 transform (SYNTAX.md 4.1, 6.8): a tile
    // that sets both is malformed and the decoder refuses it.
    //
    // The derivation lives here, in setup(), rather than only where a tile's
    // parameters are first chosen, because it is not the only place
    // `xform_size` is written: the per-tile rate-distortion search builds
    // candidates by copying `tp` and overwriting `xform_size`, so a rule
    // applied once at initialisation would be silently undone for every
    // candidate.  setup() runs on every candidate and on the final tile, so
    // the two fields cannot get out of step.  `tp.split4` is normalised too,
    // not just the local copy, because it is what the tile header writes.
    if (tp.xform_size != 0 || tp.tskip) tp.split4 = 0;
    split4 = tp.split4;
    // Chroma from luma predicts the *samples* from the reconstructed luma
    // plane, so it belongs to the replace form of directional intra, and to
    // an intra tile (an inter tile's chroma prediction is the warp).
    cfl = fp->cfl && intra_dir && !dir_layer;
    nctx = fp->nctx;
    ctx_v3 = nctx >= kNumCtxV3 ? 1 : 0;
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
        if (split4) s.splits.assign((size_t)s.nb * s.nb, 0);
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
        u.ucls = (u8)kUclsDc;
        u.ctx_v3 = (u8)ctx_v3;
        u.ngrp = 0;   // a DC plane has no block neighbour and is not one
        u.sdh = (u8)sdh;
        units.push_back(u);
        off += ndc;
        if (intra_dir) {
            Unit mu{};
            mu.kind = UNIT_MODE;
            mu.modes = s.modes.data();
            mu.nbx = (u8)s.nb;
            mu.nmodes = (u8)((cfl && chroma) ? kNumIntraModesCfl
                                             : kNumIntraModes);
            mu.ctx_mode = (u8)mode_context(nctx);
            mu.scan = scan_table(1, false);
            units.push_back(mu);
        }
        const int ncoef = s.bsize * s.bsize;
        for (int b = 0; b < ndc; ++b) {
            Unit v{};
            v.coef = &coef[off];
            v.ncoef = (u16)ncoef;
            v.scan = scan_table(ncoef, tp.tskip != 0);
            // The split flag exists only at xform_size == 8 (SYNTAX.md 4.1),
            // so `split4` is already false at every other size and no unit
            // larger than 64 coefficients ever carries one.
            v.split_present = (u8)split4;
            v.split_out = split4 ? &s.splits[b] : nullptr;
            v.ctx_cbf = ccbf;
            v.ctx_last = clast;
            v.ucls = (u8)(chroma ? kUclsChroma : kUclsLuma);
            v.ctx_v3 = (u8)ctx_v3;
            // The neighbour group is the plane: a lane carries its class
            // across this plane's block units and nothing else.
            v.ngrp = ctx_v3 ? (u8)(p + 1) : (u8)0;
            v.sdh = (u8)sdh;
            units.push_back(v);
            off += ncoef;
        }
    }
}

// The CFL context of plane `p`: this tile's reconstructed luma plane and the
// chroma/luma scale.  Off (a null `luma`) for luma, alpha, and any tile the
// tool is not enabled on.
CflCtx TileCoder::cfl_for(int p) const {
    CflCtx cf;
    if (!cfl || (p != 1 && p != 2)) return cf;
    const PlaneState &y = pl[0], &c = pl[p];
    if (c.size <= 0) return cf;
    if (y.size == c.size) cf.f = 1;
    else if (y.size == 2 * c.size) cf.f = 2;
    else return cf;   // no other luma:chroma ratio is defined
    if ((int)y.recon.size() != y.size * y.size) return cf;
    cf.luma = y.recon.data();
    cf.size = y.size;
    cf.maxval = c.maxval;
    return cf;
}

void TileCoder::clear_split() {
    split4 = 0;
    tp.split4 = 0;
    for (Unit &u : units) { u.split_present = 0; u.split_out = nullptr; }
    for (int p = 0; p < 4; ++p) pl[p].splits.clear();
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

// The co-located luma of chroma sample (cx, cy), clamped into the tile.  For
// f == 2 it is the rounded 2x2 average, the same kernel SYNTAX.md 5.2 uses to
// subsample chroma, so the two planes are aligned by construction.
static i32 cfl_luma(const CflCtx &cf, int cx, int cy) {
    const int n = cf.size;
    if (cf.f == 1) {
        int x = clamp_i32(cx, 0, n - 1), y = clamp_i32(cy, 0, n - 1);
        return cf.luma[(size_t)y * n + x];
    }
    int x0 = clamp_i32(2 * cx, 0, n - 1), x1 = clamp_i32(2 * cx + 1, 0, n - 1);
    int y0 = clamp_i32(2 * cy, 0, n - 1), y1 = clamp_i32(2 * cy + 1, 0, n - 1);
    return (cf.luma[(size_t)y0 * n + x0] + cf.luma[(size_t)y0 * n + x1] +
            cf.luma[(size_t)y1 * n + x0] + cf.luma[(size_t)y1 * n + x1] + 2) >>
           2;
}

// Fit chroma = base_c + alpha * (luma - base_l) over the block's 16
// neighbour pairs: the two with the smallest co-located luma give `base`, the
// two with the largest give the far end, and both ends are averaged so a
// single noisy neighbour cannot set the slope.  Ties take the lowest index,
// which makes the choice deterministic.  Normative; SYNTAX.md 7.7.
struct CflModel {
    i32 alpha;   // Q8, in [-kCflAlphaMax, kCflAlphaMax - 1]
    i32 base_l, base_c;
};

// `log2b` is the block edge.  The fit reads the block's own 2n reconstructed
// neighbours, so it follows the transform size the way every other predictor
// does; at n == 8 it is character for character the detail package's.
static CflModel cfl_fit(const CflCtx &cf, const IntraRefs &r, int bx, int by,
                        int log2b) {
    const int n = 1 << log2b;
    const int npairs = 2 * n;
    i32 ln[2 * kMaxBlock], cn[2 * kMaxBlock];
    const int x0 = bx * n, y0 = by * n;
    for (int k = 0; k < n; ++k) {
        cn[k] = r.a[1 + k];
        ln[k] = cfl_luma(cf, x0 + k, y0 - 1);
        cn[n + k] = r.l[1 + k];
        ln[n + k] = cfl_luma(cf, x0 - 1, y0 + k);
    }
    int lo0 = 0, lo1 = -1, hi0 = 0, hi1 = -1;
    for (int k = 1; k < npairs; ++k) {
        if (ln[k] < ln[lo0]) lo0 = k;
        if (ln[k] > ln[hi0]) hi0 = k;
    }
    for (int k = 0; k < npairs; ++k) {
        if (k != lo0 && (lo1 < 0 || ln[k] < ln[lo1])) lo1 = k;
        if (k != hi0 && (hi1 < 0 || ln[k] > ln[hi1])) hi1 = k;
    }
    CflModel m{};
    m.base_l = (ln[lo0] + ln[lo1] + 1) >> 1;
    m.base_c = (cn[lo0] + cn[lo1] + 1) >> 1;
    const i32 top_l = (ln[hi0] + ln[hi1] + 1) >> 1;
    const i32 top_c = (cn[hi0] + cn[hi1] + 1) >> 1;
    // Each of the two largest neighbours is at least each of the two
    // smallest, so `dl` cannot be negative; the `<= 0` rather than `== 0`
    // keeps the table index in range without relying on that argument.
    const i32 dl = top_l - m.base_l;          // 0 .. 255
    if (dl <= 0) {
        m.alpha = 0;
        return m;
    }
    // kCflRecip[dl] is 2^15 / dl, so the product is (top_c - base_c)/dl in
    // Q15; >> 7 brings it to Q8.  |top_c - base_c| <= 511 and kCflRecip <=
    // 32768, so the product is at most 1.7e7 -- inside int32.
    const i32 q = (top_c - m.base_c) * (i32)kCflRecip[dl];
    m.alpha = clamp_i32((q + 64) >> 7, -kCflAlphaMax, kCflAlphaMax - 1);
    return m;
}

// P[j * n + i] for one n x n block, n = 1 << log2b.  Every mode but
// kIntraDcPlane and kIntraCfl is a weighted average of in-range references,
// so no clamp is needed and none is applied; kIntraCfl clamps because a
// fitted slope can leave the sample domain.  The formulas are those of
// SYNTAX.md 7.4 with the block edge left as `n`: at n == 8 they are the v1.3
// predictors character for character.
static void predict_block(int mode, const IntraRefs &r, const i32 *base,
                          int size, int bx, int by, int log2b, i32 *P,
                          const CflCtx *cf = nullptr) {
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
        case kIntraCfl: {
            const CflModel m = cfl_fit(*cf, r, bx, by, log2b);
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    i32 l = cfl_luma(*cf, (bx << log2b) + i, (by << log2b) + j);
                    P[j * n + i] = clamp_i32(
                        m.base_c + ((m.alpha * (l - m.base_l) + 128) >> 8), 0,
                        cf->maxval);
                }
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
// transmitted matrix is always the 8x8 one; a larger block replicates it and
// a split 4x4 sub-block subsamples it, so entry (u, v) of an n x n block is
// entry (u >> k, v >> k) of the 8x8 matrix with k = log2(n) - 3.  The roll-off
// therefore covers the same fraction of the frequency plane at every size, and
// no extra matrix is transmitted.  SYNTAX.md 6.5.
static inline int block_weight(const PlaneState &s, int i) {
    const int k = s.log2b - 3;
    const int u = (i >> s.log2b) >> k, v = (i & (s.bsize - 1)) >> k;
    return s.wmat[u * 8 + v];
}

// Dequantize + inverse transform one residual block.  With `split` the block
// is four 4x4 sub-blocks in raster order, each occupying its own quadrant of
// the 64-value coefficient array and quantized with the tile's matrix
// subsampled by two (SYNTAX.md 6.8).  `split` is only ever set at
// `bsize == 8`: the two tools compose by 4.1's rule and never overlap.
static void residual_block(const i16 *c, const PlaneState &s, int tskip,
                           i32 *res, int split = 0) {
    const int ncoef = s.bsize * s.bsize;
    const int ro = recon_offset_q6();
    if (tskip) {
        int t = dequant_step(s.qp, 16);
        for (int i = 0; i < ncoef; ++i) res[i] = dequant(c[i], t);
    } else if (split) {
        for (int sb = 0; sb < 4; ++sb) {
            const int ox = (sb & 1) * 4, oy = (sb >> 1) * 4;
            i32 dq[16], out[16];
            for (int k = 0; k < 16; ++k) {
                int idx = (oy + (k >> 2)) * 8 + ox + (k & 3);
                dq[k] = dequant_off(c[idx],
                                    dequant_step(s.qp, weight4(s.wmat, k)), ro);
            }
            idct_block(dq, out, 4);
            for (int k = 0; k < 16; ++k)
                res[(oy + (k >> 2)) * 8 + ox + (k & 3)] = out[k];
        }
    } else {
        i32 dq[kMaxBlock * kMaxBlock];
        for (int i = 0; i < ncoef; ++i)
            dq[i] = dequant_off(c[i], dequant_step(s.qp, block_weight(s, i)),
                                ro);
        idct_block(dq, res, s.bsize);
    }
}

// The quantizer step of block-local index `i` under the tile's matrix.
static inline int block_step(const PlaneState &s, int i, int tskip, int split) {
    if (tskip) return dequant_step(s.qp, 16);
    if (split) return dequant_step(s.qp, weight4(s.wmat, ((i >> 3) & 3) * 4 +
                                                          (i & 3)));
    return dequant_step(s.qp, s.wmat[i]);
}

// Forward transform of one 8x8 residual into the block's 64 coefficients,
// four 4x4 sub-blocks when `split`.  Encoder side (informative).
static void forward_block(const i32 res[64], i16 co[64], int tskip, int split) {
    if (tskip) {
        for (int i = 0; i < 64; ++i) co[i] = (i16)res[i];
    } else if (split) {
        for (int sb = 0; sb < 4; ++sb) {
            const int ox = (sb & 1) * 4, oy = (sb >> 1) * 4;
            i32 in[16];
            i16 out[16];
            for (int k = 0; k < 16; ++k)
                in[k] = res[(oy + (k >> 2)) * 8 + ox + (k & 3)];
            fdct4x4(in, out);
            for (int k = 0; k < 16; ++k)
                co[(oy + (k >> 2)) * 8 + ox + (k & 3)] = out[k];
        }
    } else {
        fdct8x8(res, co);
    }
}

// SYNTAX.md 7.2 and 13.3: bilinear planar prediction over the block centres,
// then, for an inter tile, added to the warp predictor about the plane's DC
// offset.  Every tile form that produces a `means` field ends here -- the
// coded DC plane, and the near-skip correction of 13.9 -- so there is one
// implementation of the interpolation and of the inter combination.
//
// Block (bx, by)'s mean sits at sample (bx*bs + (bs-1)/2, ...), so the Q4
// coordinate of sample x in the means grid is (16*x - 8*(bs-1)) / bs,
// evaluated as a rounding shift.  At bs == 8 that is exactly 2*x - 7, the v1
// formula; the transform package's re-grid of the DC plane is what makes the
// general form necessary.
static void planar_from_means(PlaneState &s) {
    const int size = s.size, nb = s.nb;
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

// The DC-plane quantiser step of SYNTAX.md 6.5: the DC plane is quantised at
// half the tile's QP index and at unit weight, whatever the tile's weighting
// matrix.  The coded DC plane and the near-skip correction share it, because
// they are the same quantity coded two ways.
static inline int dc_plane_step(const PlaneState &s) {
    return dequant_step(dc_qp_of(s.qp), 16);
}

// ---------------------------------------------------------------- near skip
// SYNTAX.md 13.9.  A near-skip tile has no coefficients at all: its whole
// residual is a per-plane block-mean field -- a DC level and one horizontal
// and one vertical ramp -- built here, after which everything is the ordinary
// skip path: planar interpolation, then the warp predictor.  So a near-skip
// tile's samples are `pred` and nothing is added to them.
//
// `corr` is the plane's three signed bytes, dequantised through the DC-plane
// step, i.e. exactly the levels a coded DC plane would have carried: the
// near-skip form IS the DC plane written in nine bytes instead of an
// entropy-coded unit, so it must quantise the same way.  The ramps span
// +-corr[1] and +-corr[2] dequantised across the tile: `2*bx-nb+1` runs over
// +-(nb-1) and the shift by log2(nb) divides it by nb, so the corner blocks
// sit one quantiser step short of the full amplitude, which is the same
// convention the DC plane's own bilinear interpolation uses.
//
// `nb` follows the tile's transform size like every other block grid, so a
// near-skip tile inside a 32x32-transform stream corrects a 2x2 mean field
// rather than an 8x8 one and the arithmetic is unchanged.
static void reconstruct_near_skip(PlaneState &s, const i8 corr[3]) {
    const int nb = s.nb;
    const int t = dc_plane_step(s);
    const i32 d0 = dequant(corr[0], t);
    const i32 dh = dequant(corr[1], t);
    const i32 dv = dequant(corr[2], t);
    const int nb_log2 = log2_of(nb);
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx)
            s.means[(size_t)by * nb + bx] =
                s.dc_off + d0 + ((dh * (2 * bx - nb + 1)) >> nb_log2) +
                ((dv * (2 * by - nb + 1)) >> nb_log2);
    planar_from_means(s);
    s.samples = s.pred;
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
    const int nb = s.nb;
    const int ndc = nb * nb;
    const int tdc = dc_plane_step(s);
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
    planar_from_means(s);
}


// Reconstruct one plane from its coefficients (normative decode path).
// With `dir` the per-block intra modes in s.modes select the predictor and
// the blocks are reconstructed in raster order, each seeing the ones before
// it; `layer` makes the modes predict the DC-plane residual instead of the
// samples.  Without `dir` this is the v1 predictor exactly.
static void reconstruct_plane(PlaneState &s, const i16 *coefs, int tskip,
                              int dir = 0, int layer = 0,
                              const CflCtx *cf = nullptr) {
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    reconstruct_dc_plane(s, coefs);
    const i16 *bc = coefs + ndc;
    // Empty when the tile does not carry split flags, in which case every
    // block uses the 8x8 transform.
    auto split_of = [&](int b) {
        return (int)s.splits.size() == ndc ? (int)s.splits[b] : 0;
    };
    if (!dir) {
        for (int by = 0; by < nb; ++by)
            for (int bx = 0; bx < nb; ++bx) {
                const i16 *c = bc + ((size_t)by * nb + bx) * ncoef;
                i32 res[kMaxBlock * kMaxBlock];
                residual_block(c, s, tskip, res, split_of(by * nb + bx));
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
            residual_block(c, s, tskip, res, split_of(by * nb + bx));
            IntraRefs r;
            build_refs(s.recon.data(), fallback, size, bx, by, lb, r);
            predict_block(s.modes[(size_t)by * nb + bx], r, fallback, size, bx,
                          by, lb, P, cf);
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

// ------------------------------------------------------------ the lambda
// ONE rate-distortion trade for the whole encoder.  Every decision this
// encoder makes -- which levels to code, which intra mode, which tile mode,
// which vector, which per-tile QP offset, transform skip -- minimises
// D + lambda*R, and they all take that lambda from here.  Before this they
// did not: the trellis used 0.30*qstep^2, the tile-mode decision
// 0.25*0.30*qstep^2 with no stated relation to it, and the motion search
// minimised SAD plus a hand-set two-byte bias in units of qstep, which is a
// third scale again.  Three scales cannot be jointly optimal, and the encoder
// could and did choose a mode its own trellis would then disagree with.
//
// The model.  For a uniform quantiser of step q the high-rate RD slope is
// dD/dR = -k*q^2, so lambda is proportional to q^2 with one dimensionless
// constant.  `kLambdaScale` is that constant, fitted on the quality harness
// (ref/RESULTS-rdo-b.md 2): 0.30 was carried over from v1.2, and the fit puts
// the minimum at 0.20 to 0.25 with 0.36 already 0.5 points the wrong side.  Distortion is squared error in SAMPLE units;
// the 8x8 integer DCT here is orthonormal to within its Q4 scaling, so
// squared COEFFICIENT error is the same currency and the trellis may use it
// directly.  Rate is in bits.
//
// The SAD form.  A motion search compares |e| (first order), not e^2, so its
// lambda is not the same number.  Setting the two costs equal for an error
// that is uniform over a block gives lambda_sad = sqrt(lambda_sse), which is
// the relation every mainstream encoder uses and the one used here.  There
// is no second constant to fit.
//
// The reference-persistence factor, and where it belongs.  A tile's
// reconstruction is the reference for the next `kRefPersist` frames, so
// distortion left in it is displayed that many times while its bits are paid
// once.  That is true of a SKIPPED tile, whose error goes into the reference
// and stays there until the tile is next coded, and it is what the skip
// decision's excess penalty charges (decide_tile, and ref/RESULTS-inter.md 5,
// where charging it was worth 4 dB).  It is NOT separately true of a coded
// tile, whose error is bounded by its quantiser and is corrected the next
// time the tile is coded.
//
// v1.4 charged it twice: the skip penalty, and again as a divisor on the
// per-tile mode decision's lambda (`mode_lambda` defaulting to 1/4).  The
// mode decision therefore ran at a quarter of the trellis's rate weight and
// bought quality the skip penalty had already paid for.  Measured on
// vr-mixed-1024-v2 4:4:4 band A, removing the second charge is worth -3.4
// BD-rate points (ref/RESULTS-rdo-b.md 3).  The mode decision now runs at the
// tile's own lambda and `--mode-lambda` overrides.
enum RdoqEffort { kRdoqFast = 0, kRdoqMedium = 1, kRdoqFull = 2 };

// ------------------------------------------------------------ effort
// One place a config becomes an effort.  The preset is a library concept
// (nxvc_preset), and every individual knob overrides it: 0 in a knob means
// "take the preset's value".  The CLI writes the same fields a caller does,
// so `--preset slow` and a caller setting NXVC_PRESET_SLOW get the identical
// encoder -- which is the property a CLI-only preset cannot have.
struct Effort {
    int rdoq;          // kRdoqFast / kRdoqMedium / kRdoqFull
    int me;            // 1 fast, 2 medium, 3 full
    int dir_cand;      // directional intra modes RD-checked
    int qp_search;     // per-tile QP offset radius
    int qp_step;       // ... and the spacing of its candidates
    double chroma_weight;  // weight of chroma squared error, 1.0 = as sampled
};

static Effort resolve_effort(const nxvc_config &cfg) {
    Effort e{};
    switch (cfg.preset) {
        case NXVC_PRESET_FAST:
            e.rdoq = kRdoqFast;  e.me = 1; e.dir_cand = 1; e.qp_search = 0;
            break;
        case NXVC_PRESET_SLOW:
            e.rdoq = kRdoqFull;  e.me = 3; e.dir_cand = 4; e.qp_search = 2;
            break;
        default:
            e.rdoq = kRdoqMedium; e.me = 2; e.dir_cand = 2; e.qp_search = 0;
            break;
    }
    e.qp_step = 2;
    e.chroma_weight = 1.0;
    if (cfg.rdoq_effort) e.rdoq = clamp_i32((int)cfg.rdoq_effort - 1, 0, 2);
    if (cfg.me_effort) e.me = clamp_i32((int)cfg.me_effort, 1, 3);
    if (cfg.intra_dir_cand) e.dir_cand = clamp_i32((int)cfg.intra_dir_cand, 1, 9);
    if (cfg.qp_search) e.qp_search = clamp_i32((int)cfg.qp_search, 0, 8);
    if (cfg.qp_search_step) e.qp_step = clamp_i32((int)cfg.qp_search_step, 1, 8);
    // The chroma distortion weight is a PERCEPTUAL TUNING KNOB, not a coding
    // gain: it is fitted to the 6:1:1 reporting convention, it buys PSNR-Y at
    // the cost of absolute chroma fidelity, it does nothing at 4:2:0 where
    // chroma already has a quarter of the samples, and it regresses SSIM
    // there.  So it defaults to 1.0 -- chroma weighted as the samples fall --
    // and anything quoted with it must be quoted on both metrics.
    if (cfg.chroma_weight_q8) e.chroma_weight = (double)cfg.chroma_weight_q8 / 256.0;
    return e;
}

constexpr double kLambdaScale = 0.22;
constexpr double kRefPersist = 4.0;
// Multiplier on the DC plane's lambda; see analyze_dc_plane.  0 disables the
// DC trellis entirely and restores the v1.4 dead-zone quantizer there.
constexpr double kDcLambdaGain = 1.00;

struct Lambda {
    double sse = 0;  // cost = D_sse + sse * R
    double sad = 0;  // cost = D_sad + sad * R
    double qstep = 0;
};

static Lambda make_lambda(int qp, double scale) {
    Lambda L;
    L.qstep = (double)kQStep[clamp_i32(qp, 0, 63)] / 16.0;
    L.sse = scale * L.qstep * L.qstep;
    L.sad = std::sqrt(L.sse);
    return L;
}

// ------------------------------------------------------- content classes
// docs/RATECONTROL.md 3.3 classifies every tile from three statistics and
// uses the class to steer the bit allocator.  The same class steers lambda
// here: a flat tile's error is a visible band and is worth spending on, a
// texture tile masks its own error and is not.  This is the encoder's own
// copy of steps 2, 4 and 5 of that classifier -- ref/ does not link rc/, and
// the UI-stencil route of step 1 needs a compositor input the codec does not
// have, so TEXT is not reachable here and its gain is unused.
//
// `G` is the mean squared gradient magnitude and `C` the structure-tensor
// coherence, both over the tile's luma, exactly as RATECONTROL.md 3.1
// defines them.
enum TileClass { kClassFlat = 0, kClassTexture, kClassEdge, kClassText };

static int classify_tile(const i32 *y, int size) {
    double gxx = 0, gyy = 0, gxy = 0, g2 = 0, sum = 0, sum2 = 0;
    const int n = size * size;
    for (int j = 1; j < size - 1; ++j)
        for (int i = 1; i < size - 1; ++i) {
            const i32 *p = y + (size_t)j * size + i;
            double gx = (double)(p[1] - p[-1]) * 0.5;
            double gy = (double)(p[size] - p[-size]) * 0.5;
            gxx += gx * gx;
            gyy += gy * gy;
            gxy += gx * gy;
            g2 += gx * gx + gy * gy;
        }
    for (int i = 0; i < n; ++i) { sum += y[i]; sum2 += (double)y[i] * y[i]; }
    const double inner = (double)(size - 2) * (size - 2);
    const double G = inner > 0 ? g2 / inner : 0.0;
    const double var = sum2 / n - (sum / n) * (sum / n);
    const double log_var = std::log(var > 1e-6 ? var : 1e-6);
    if (G < 12.0 || log_var < 3.0) return kClassFlat;
    const double tr = gxx + gyy;
    const double d = gxx - gyy;
    const double C = tr > 1e-9 ? std::sqrt(d * d + 4 * gxy * gxy) / tr : 0.0;
    return C >= 0.45 ? kClassEdge : kClassTexture;
}

// Per-class multiplier on lambda, fitted on the harness
// (ref/RESULTS-rdo-b.md 2).  1.0 is "spend as the fit says"; below 1.0 the
// class buys quality with bits, above it the class gives bits away.
constexpr double kClassLambdaGain[4] = {1.00, 1.00, 1.00, 1.00};

static inline double class_lambda_gain(int cls, const u32 over[4]) {
    cls &= 3;
    if (over && over[cls]) return (double)over[cls] / 256.0;
    return kClassLambdaGain[cls];
}

// True when no class changes lambda, so the caller can skip classify_tile()
// entirely.  The fit that shipped says exactly that on this material; the
// hook stays because the fit is per content and a caller can set its own.
static inline bool class_lambda_is_flat(const u32 over[4]) {
    for (int i = 0; i < 4; ++i) {
        const double g = over && over[i] ? (double)over[i] / 256.0
                                         : kClassLambdaGain[i];
        if (g != 1.0) return false;
    }
    return true;
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
    // Is a zero never dearer than a one, in every context of this table?
    //
    // The trellis's `last` bound rests on it: above the highest position
    // whose magnitude reaches half a step, level 0 beats level 1 in
    // distortion AND in rate, so those positions are provably zero and are
    // never searched.  The distortion half is an identity; the rate half is a
    // property of the TABLE, and a transmitted table set is whatever the
    // frame chose to send.  So it is checked once per table rather than
    // assumed, and the bound is dropped if it does not hold -- a slower
    // trellis, not a wrong one.
    bool zero_cheapest;
};

static void build_rate_cost(const TableSet &ts, RateCost &rc) {
    for (int c = 0; c < kNumCtx; ++c)
        for (int s = 0; s < kNumSym; ++s) {
            double f = (double)ts.ctx[c].freq[s] / kProbTotal;
            if (f <= 0) f = 1.0 / kProbTotal;
            rc.sym[c][s] = (i32)(-std::log2(f) * 1024.0 + 0.5);
        }
    rc.zero_cheapest = true;
    for (int c = 0; c < kNumCtx; ++c)
        if (rc.sym[c][1] < rc.sym[c][0]) rc.zero_cheapest = false;
}

// The contexts one coding unit is coded in, as the encoder's rate model sees
// them.  Under v1/v2 they are the unit's fixed fields; under v3 they follow
// the unit class and the neighbour class (SYNTAX.md 9.8).  The encoder tracks
// that class in raster order, which is the lane's own chain for every unit
// except the first `nlanes` of a plane -- a rate-model approximation only:
// LaneMachine derives the coded context, and it is always exact.
// Everything about ONE coding unit that the rate model and the entropy coder
// have to agree on.  It carries two independent packages' state:
//
//   * the entropy package (tool 25 CTX_V3): `ucls`, `v3` and the CBF/LAST
//     contexts already shifted by this lane's previous unit of the same
//     class.  The conditioning is per CODING UNIT -- the 8x8 coefficient
//     group -- and never per transform block (docs/SYNTAX.md 9.8).
//   * the detail package (tool 19 XFORM_4X4_SPLIT): `split`, whether this
//     unit is four 4x4 sub-blocks rather than one 8x8 transform, and
//     `split_present`, whether a flag saying so is coded for it.
//
// They meet in exactly one place, `level()`: a split unit's scan positions
// band differently, so the band mapping is applied BEFORE the context is
// chosen.  Neither package reads the other's fields; keeping them in one
// struct is what stops the encoder's rate model and LaneMachine drifting
// apart, which is the failure the whole rate/entropy split is prone to.
struct UnitCtx {
    int cbf = kCtxCbfLuma;
    int last = kCtxLastLuma;
    int level_fixed = kCtxNone;   // kCtxNone: the banded LEVEL contexts
    int ucls = kUclsLuma;
    int v3 = 0;
    int split = 0;                // 1 = four 4x4 sub-blocks (tool 19)
    int split_present = 0;        // 1 = a split flag is coded for this unit
    int band_shift = 0;           // last_shift_of(ncoef), tool 27
    // `last` is the unit's LAST scan position: v3 gives that one coefficient
    // its own contexts, so the rate model has to know where it is.
    // Does the LEVEL context depend on the previous level in the unit?  A
    // DC-plane unit's does not -- under v2 it has one fixed row, under v3 one
    // row plus a separate one for the DC term -- so its trellis has no Markov
    // chain and its three states collapse to one.  That is exactly right and
    // costs nothing to express.
    bool level_markov() const {
        if (ucls == kUclsDc) return false;
        return level_fixed == kCtxNone;
    }
    int level(int scan_pos, int last, int prev_class) const {
        const int bp = band_pos(scan_pos, split != 0, band_shift);
        if (v3) return v3_ctx_level(ucls, scan_pos, bp, last, prev_class);
        return level_fixed != kCtxNone ? level_fixed
                                       : level_ctx(bp, prev_class, 0);
    }
};

// The block-unit contexts of one plane under the active model.
static UnitCtx block_ctx(int nctx, bool chroma) {
    UnitCtx u;
    u.ucls = chroma ? kUclsChroma : kUclsLuma;
    u.v3 = nctx >= kNumCtxV3 ? 1 : 0;
    u.cbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
    u.last = chroma ? kCtxLastChroma : kCtxLastLuma;
    return u;   // neighbour class 0; with_nbr() applies the lane's own
}

// The same unit with the lane's neighbour class applied: v3 shifts CBF and
// LAST, and everything else is unchanged.
static UnitCtx with_nbr(UnitCtx u, int nbr) {
    if (!u.v3) return u;
    u.cbf = v3_ctx_cbf(u.ucls, nbr);
    u.last = v3_ctx_last(u.ucls, nbr);
    return u;
}

// The same unit at one split choice.  `split_present` is a property of the
// tile (does this stream code split flags at all), `split` of the candidate.
static UnitCtx with_split(UnitCtx u, int split, int split_present) {
    u.split = split;
    u.split_present = split_present;
    return u;
}

// The DC-plane unit's contexts under the active model.  It is its own unit
// class: a dense low-frequency image, nothing like the sparse AC blocks it
// shared statistics with in v1.
static UnitCtx dc_plane_ctx(int nctx) {
    UnitCtx u;
    u.ucls = kUclsDc;
    u.v3 = nctx >= kNumCtxV3 ? 1 : 0;
    u.cbf = nctx >= kNumCtxV2 ? kCtxCbfDc : kCtxCbfLuma;
    u.last = nctx >= kNumCtxV2 ? kCtxLastDc : kCtxLastLuma;
    u.level_fixed = nctx >= kNumCtxV2 ? kCtxLevelDc : kCtxNone;
    return u;
}

// The same unit at one transform size.  `band_shift` is what lets a 16x16 or
// 32x32 unit reuse the 64-position LAST classes and the four LEVEL bands by
// naming a scan GROUP rather than a position; SYNTAX.md 9.2.1 and 9.3.1.
static UnitCtx with_ncoef(UnitCtx u, int ncoef) {
    u.band_shift = last_shift_of(ncoef);
    return u;
}

// The neighbour class a quantized unit leaves behind, mirroring
// LaneMachine::finish_coef_unit exactly.  Encoder side.  `scan` is needed
// because the class depends on LAST, which is a scan position.
static int unit_nbr_class(const i16 *c, int ncoef, const u16 *scan) {
    int last = -1;
    for (int p = ncoef - 1; p >= 0; --p)
        if (c[scan[p]] != 0) { last = p; break; }
    return nbr_class_of(last < 0 ? 0 : 1, last < 0 ? 0 : last);
}

// Bypass bits an escape suffix costs for magnitude m >= 15 (Exp-Golomb 3 of
// m - 15), matching eg3_encode in entropy.cpp exactly.
static inline int escape_bits(i32 m) {
    u32 n = (u32)(m - 15) + 8u;
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    return (b - kEscOrder) + 1 + b;  // j ones, one zero, b suffix bits
}

// `last` is the unit's LAST scan position, or -1 for "this position is not
// the unit's last".  Under v3 the coefficient at LAST has contexts of its
// own, so the rate model has to be told which case it is pricing.  The band
// mappings of both transform tools are inside `uc.level()`.
static inline i32 level_rate(const RateCost &rc, const UnitCtx &uc,
                             int scan_pos, int last, int prev_class, i32 m) {
    int ctx = uc.level(scan_pos, last, prev_class);
    i32 sym = m > 14 ? kEscSym : m;
    i32 r = rc.sym[ctx][sym];
    if (m > 14) r += escape_bits(m) << 10;
    if (m != 0) r += 1 << 10;  // sign, one bypass bit
    return r;
}

constexpr double kRdInf = 1e30;

// Effort of the trellis.  The state space is fixed by the syntax; what an
// effort level changes is how many magnitudes per scan position are offered
// to it, which is where the time goes.

// `orig[i]` is the unquantized value at block-local index i, `step[i]` its
// reconstruction step (the dequantizer's t, Q4).  Writes the chosen levels
// back into `coefs`.  `sdh` says the unit will hide its `last` sign, which is
// worth exactly one bit and is part of the LAST decision.
static void rdoq_unit(i16 *coefs, const i32 *orig, const i32 *step, int ncoef,
                      const u16 *scan, const UnitCtx &uc,
                      const RateCost &rc, double lambda, int effort, int sdh) {
    for (int i = 0; i < ncoef; ++i) coefs[i] = 0;

    // The exact upper bound on `last`.  Above the highest position whose
    // magnitude reaches half a step, level 0 beats level 1 in distortion
    // (|a| < st/2 <= st - |a|) *and* in rate, so no trellis is needed to know
    // the answer.  Everything above `hi` is provably zero; only positions
    // 0..hi are searched.  This is not an approximation and it is what makes
    // the trellis affordable on a plane of mostly-empty units.
    //
    // The rate half of that argument is an identity about the table, not
    // about this unit, so it is CHECKED rather than assumed:
    // RateCost::zero_cheapest reads the table and confirms that no context
    // prices a one below a zero.  If a future table ever did, the bound would
    // silently start discarding coefficients the trellis should have kept.
    int hi = -1;
    double energy = 0;
    for (int p = 0; p < ncoef; ++p) {
        int idx = scan[p];
        double c = orig[idx];
        energy += c * c;
        double a = c < 0 ? -c : c;
        if (32.0 * a >= (double)step[idx]) hi = p;  // 2|a| >= st, st = step/16
    }
    if (hi < 0 || !rc.zero_cheapest) hi = hi < 0 ? -1 : ncoef - 1;
    if (hi < 0) return;

    // Buffers for the largest unit the format has, a 32x32 block.  They are
    // thread_local rather than automatic because a 1024-position trellis is
    // 40 kB and this runs once per candidate per block.
    constexpr int kMaxCoef = kMaxBlock * kMaxBlock;
    static thread_local double f[kMaxCoef][3], fnz[kMaxCoef];
    static thread_local i32 best_m[kMaxCoef][3], best_m_nz[kMaxCoef];
    double prev[3] = {0, 0, 0};
    for (int p = 0; p <= hi; ++p) {
        int idx = scan[p];
        double c = orig[idx];
        double a = c < 0 ? -c : c;
        double st = (double)step[idx] / 16.0;
        i32 m0 = (i32)(a / st);
        if (m0 > 32767) m0 = 32767;
        i32 cand[4];
        int nc = 0;
        cand[nc++] = 0;
        if (effort == kRdoqFast) {
            i32 mn = (i32)(a / st + 0.5);
            if (mn > 32767) mn = 32767;
            if (mn > 0) cand[nc++] = mn;
        } else {
            if (effort >= kRdoqFull && m0 >= 2) cand[nc++] = m0 - 1;
            if (m0 > 0) cand[nc++] = m0;
            if (m0 + 1 <= 32767) cand[nc++] = m0 + 1;
        }
        for (int sc = 0; sc < 3; ++sc) {
            double best = kRdInf;
            i32 bm = 0;
            double bestnz = kRdInf;
            i32 bmnz = -1;
            for (int k = 0; k < nc; ++k) {
                i32 m = cand[k];
                double d = a - (double)m * st;
                // Interior position: priced as not-LAST.  A unit whose LEVEL
                // context does not depend on the previous level -- the DC
                // plane -- has no Markov chain and its three states collapse.
                double cost =
                    d * d +
                    lambda * (level_rate(rc, uc, p, -1, sc, m) / 1024.0) +
                    prev[uc.level_markov() ? level_class(m) : 0];
                if (cost < best) { best = cost; bm = m; }
                // The nonzero candidate at p is used only when p is chosen as
                // the unit's LAST, so it is priced in the LAST contexts.
                if (m != 0) {
                    double cnz =
                        d * d +
                        lambda * (level_rate(rc, uc, p, p, sc, m) / 1024.0) +
                        prev[uc.level_markov() ? level_class(m) : 0];
                    if (cnz < bestnz) { bestnz = cnz; bmnz = m; }
                }
            }
            f[p][sc] = best;
            best_m[p][sc] = bm;
            if (sc == 0) { fnz[p] = bestnz; best_m_nz[p] = bmnz; }
        }
        for (int sc = 0; sc < 3; ++sc) prev[sc] = f[p][sc];
    }

    // Choose `last`.
    double best_total = rc.sym[uc.cbf][0] * lambda / 1024.0 + energy;
    int best_last = -1;
    double tail = 0;
    for (int p = ncoef - 1; p > hi; --p) {
        double c = orig[scan[p]];
        tail += c * c;
    }
    for (int p = hi; p >= 0; --p) {
        if (fnz[p] < kRdInf) {
            double r = rc.sym[uc.cbf][1];
            if (ncoef > 1) {
                // A large unit names the scan GROUP with the existing 16
                // LAST classes and the position inside it with `band_shift`
                // raw bypass bits.  SYNTAX.md 9.2.1.
                int cls = last_class_of(p >> uc.band_shift);
                r += rc.sym[uc.last][cls] +
                     ((kLastRawBits[cls] + uc.band_shift) << 10);
            }
            // A hidden sign is one bypass bit this unit does not emit, and
            // whether it is hidden depends on `last`.  unit_bits() charges
            // the same rebate; the trellis used not to see it at all.
            if (sdh && p >= kSdhMinLast) r -= 1 << 10;
            double total = fnz[p] + tail + lambda * (r / 1024.0);
            if (total < best_total) { best_total = total; best_last = p; }
        }
        double c = orig[scan[p]];
        tail += c * c;
    }

    if (best_last < 0) return;
    int sc = 0;
    for (int p = best_last; p >= 0; --p) {
        i32 m = (p == best_last) ? best_m_nz[p] : best_m[p][sc];
        int idx = scan[p];
        coefs[idx] = (i16)(orig[idx] < 0 ? -m : m);
        sc = uc.level_markov() ? level_class(m) : 0;
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
// The move is chosen by the same D + lambda*R the trellis just used, not by
// squared error alone: creating a new nonzero level costs its symbol and its
// sign, and raising a level across a class boundary changes the context of
// the level below it.  `rc`/`lambda` may be null/0 for a rate-blind choice
// (the DC plane, which has no trellis).
static void hide_sign_unit(i16 *coefs, const i32 *orig, const i32 *step,
                           int ncoef, const u16 *scan,
                           const RateCost *rc = nullptr, double lambda = 0,
                           const UnitCtx *uc = nullptr) {
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
            if (rc && lambda > 0) {
                // rate delta of this one level, in its own context; the
                // neighbour's context change is second order and not modelled.
                const int cx = uc ? uc->level(p, last, 0) : level_ctx(p, 0, 0);
                double r1 = (double)rc->sym[cx][m > 14 ? kEscSym : m] +
                            (m > 14 ? (escape_bits(m) << 10) : 0) +
                            (m != 0 ? (1 << 10) : 0);
                double r2 = (double)rc->sym[cx][m2 > 14 ? kEscSym : m2] +
                            (m2 > 14 ? (escape_bits(m2) << 10) : 0) +
                            (m2 != 0 ? (1 << 10) : 0);
                cost += lambda * (r2 - r1) / 1024.0;
            }
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

// The dead-zone offsets actually used, in forty-eighths of a step.
// NXVC_DZ_DC / NXVC_DZ_AC override them with four comma-separated values, the
// development hook the sweep in ref/RESULTS-detail-a.md section 3 was run
// with.  Encoder-only and no syntax change, so a stream produced under them
// is as conformant as any other.
static const u8 *dead_zone_table(bool dc) {
    static const u8 *dcz = [] {
        static u8 v[4];
        std::memcpy(v, kDeadZoneDc, 4);
        if (const char *e = std::getenv("NXVC_DZ_DC"))
            std::sscanf(e, "%hhu,%hhu,%hhu,%hhu", &v[0], &v[1], &v[2], &v[3]);
        return v;
    }();
    static const u8 *acz = [] {
        static u8 v[4];
        std::memcpy(v, kDeadZoneAc, 4);
        if (const char *e = std::getenv("NXVC_DZ_AC"))
            std::sscanf(e, "%hhu,%hhu,%hhu,%hhu", &v[0], &v[1], &v[2], &v[3]);
        return v;
    }();
    return dc ? dcz : acz;
}

// Encoder side: quantize the DC plane and mirror the decoder's
// reconstruction of it into s.means / s.pred.
// `rc`, when given, replaces the dead-zone quantizer of the DC plane with the
// trellis.  The DC plane was left out of RDOQ in v1.2 on the grounds that it
// is the intra predictor, so a level chosen there changes `pred` for all 64
// blocks and a single-unit distortion model is wrong about it.  That is true
// and it is not a reason to leave 30 % of a frame's bits unoptimised: the
// model is wrong in a KNOWN direction.  A DC error the trellis zeroes is not
// free, because the AC pass then has to code it back at the AC quantiser's
// finer step, so the DC plane's true marginal cost is higher than its own
// squared error says.  `dc_gain` is that correction -- a multiplier below 1
// on the DC plane's lambda, fitted on the harness (ref/RESULTS-rdo-b.md 4).
static void analyze_dc_plane(PlaneState &s, i16 *coefs, int sdh,
                             const RateCost *rc = nullptr, double lambda = 0,
                             int nctx = kNumCtxV1, int effort = kRdoqMedium) {
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
        for (int i = 0; i < 64; ++i) orig[i] = out[i];
    } else {
        for (int i = 0; i < ndc; ++i) orig[i] = m[i] - s.dc_off;
    }
    i32 stepv[64];
    for (int i = 0; i < ndc; ++i) stepv[i] = tdc;
    const u16 *dcscan = scan_table(ndc, false);
    const UnitCtx dc_uc = with_ncoef(dc_plane_ctx(nctx), ndc);
    if (rc) {
        // The DC plane through the trellis too.  It is the intra predictor,
        // so re-deciding it with the same D + lambda*R the blocks use keeps
        // the predictor and the residual consistent by construction.
        rdoq_unit(coefs, orig, stepv, ndc, dcscan, dc_uc, *rc, lambda, effort,
                  sdh);
    } else {
        // Per-band dead zone: without a rate model the rounding offsets are
        // the only knob the plane has.  Banded by scan position, as the LEVEL
        // contexts are.
        const u8 *dz = dead_zone_table(true);
        for (int pp = 0; pp < ndc; ++pp) {
            int i = dcscan[pp];
            coefs[i] = (i16)quantize(orig[i], tdc,
                                     dead_zone(tdc, dz[band_of(pp)]));
        }
    }
    if (sdh)
        hide_sign_unit(coefs, orig, stepv, ndc, dcscan, rc, rc ? lambda : 0.0,
                       rc ? &dc_uc : nullptr);
    reconstruct_dc_plane(s, coefs);
    (void)size;
}

// Encoder side: quantize a plane into `coefs` and leave the same
// reconstruction in s.samples that the decoder will produce.
static void analyze_plane(PlaneState &s, i16 *coefs, int tskip, int intra_dz,
                          int sdh) {
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    const u8 *dzac = dead_zone_table(false);
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
            const u16 *scan = scan_table(ncoef, tskip != 0);
            const int band_shift = last_shift_of(ncoef);
            if (tskip) {
                int t = dequant_step(s.qp, 16);
                for (int i = 0; i < ncoef; ++i) {
                    orig[i] = res[i];
                    stepv[i] = t;
                }
            } else {
                i16 co[kMaxBlock * kMaxBlock];
                fdct_block(res, co, bs);
                for (int i = 0; i < ncoef; ++i) {
                    orig[i] = co[i];
                    stepv[i] = dequant_step(s.qp, block_weight(s, i));
                }
            }
            for (int pp = 0; pp < ncoef; ++pp) {
                int i = scan[pp];
                // The dead-zone band follows the scan-group shift, so the
                // four bands cover the same fraction of the scan at every
                // transform size.
                int f = tskip && !intra_dz ? kDeadZoneTskipInter
                                           : dzac[band_of(pp >> band_shift)];
                c[i] = (i16)quantize(orig[i], stepv[i], dead_zone(stepv[i], f));
            }
            if (sdh) hide_sign_unit(c, orig, stepv, ncoef, scan);
        }
}

// Re-quantize the residual blocks of a plane with the RD trellis above.  The
// DC plane is deliberately left on the plain dead-zone quantizer: it is the
// intra predictor, so a level chosen there changes `pred` for all the blocks
// of the plane and the trellis's single-unit distortion model would be wrong
// about it.
static void rdoq_plane(PlaneState &s, i16 *coefs, int tskip, bool chroma,
                       const RateCost &rc, double lambda_scale, int sdh,
                       int nctx, int nlanes, int effort, double dc_gain) {
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    const double lambda = make_lambda(s.qp, lambda_scale).sse;
    // The DC plane first, and through the trellis too: this pass recomputes
    // every block's residual from s.pred below, so re-deciding the plane the
    // predictor is built from stays consistent by construction.
    if (dc_gain > 0)
        analyze_dc_plane(s, coefs, sdh, &rc,
                         make_lambda(dc_qp_of(s.qp), lambda_scale * dc_gain).sse,
                         nctx, effort);
    const u16 *scan = scan_table(ncoef, tskip != 0);
    const UnitCtx base_uc = with_ncoef(block_ctx(nctx, chroma), ncoef);
    u8 nbr[kMaxBlocksPerPlane] = {};
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
            const int bi = by * nb + bx;
            // This lane's previous unit of this class.  bi - nlanes is the
            // unit this lane finished before reaching bi; below nlanes the
            // lane has no predecessor yet and the class is 0.
            const UnitCtx uc = with_nbr(
                base_uc, bi >= nlanes ? nbr[bi - nlanes] : 0);
            rdoq_unit(c, orig, stepv, ncoef, scan, uc, rc, lambda, effort, sdh);
            if (sdh)
                hide_sign_unit(c, orig, stepv, ncoef, scan, &rc, lambda, &uc);
            nbr[bi] = (u8)unit_nbr_class(c, ncoef, scan);
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
static i32 unit_bits(const i16 *c, int ncoef, const u16 *scan,
                     const UnitCtx &uc, const RateCost &rc, int sdh) {
    int last = -1;
    for (int p = ncoef - 1; p >= 0; --p)
        if (c[scan[p]] != 0) { last = p; break; }
    if (last < 0) return rc.sym[uc.cbf][0];
    i32 r = rc.sym[uc.cbf][1];
    // The 4x4 split flag, one bypass bit, coded only after a nonzero CBF.
    if (uc.split_present) r += 1 << 10;
    if (ncoef > 1) {
        int cls = last_class_of(last >> uc.band_shift);
        r += rc.sym[uc.last][cls] +
             ((kLastRawBits[cls] + uc.band_shift) << 10);
    }
    if (last >= kSdhMinLast && sdh) r -= 1 << 10;
    int prev = 0;
    for (int p = last; p >= 0; --p) {
        i32 q = c[scan[p]];
        i32 m = q < 0 ? -q : q;
        r += rc.sym[uc.level(p, last, prev)][m > 14 ? kEscSym : m];
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
                              int nctx, int nlanes,
                              bool chroma, const RateCost &rc,
                              double lambda_scale, bool use_rdo, int ncand,
                              int mode_ctx, int sdh, int split_present,
                              const CflCtx *cf, int nmodes, int effort,
                              bool reuse, double dc_gain) {
    constexpr int kMaxCoef = kMaxBlock * kMaxBlock;
    const int nb = s.nb, size = s.size, bs = s.bsize, lb = s.log2b;
    const int ndc = nb * nb, ncoef = bs * bs;
    // The DC plane's own quantiser step is coarser (dc_qp_of), so its lambda
    // is its own, not the AC planes'; `dc_gain` is the correction
    // analyze_dc_plane documents.
    if (use_rdo && dc_gain > 0)
        analyze_dc_plane(s, coefs, sdh, &rc,
                         make_lambda(dc_qp_of(s.qp), lambda_scale * dc_gain).sse,
                         nctx, effort);
    else
        analyze_dc_plane(s, coefs, sdh);
    if ((int)s.recon.size() != size * size) s.recon.assign((size_t)size * size, 0);
    if ((int)s.modes.size() != ndc) { s.modes.assign((size_t)ndc, 0); reuse = false; }

    std::vector<i32> zero;
    const i32 *fallback = s.pred.data();
    if (layer) {
        zero.assign((size_t)size * size, 0);
        fallback = zero.data();
    }
    const double lambda = make_lambda(s.qp, lambda_scale).sse;
    const UnitCtx base_uc = with_ncoef(block_ctx(nctx, chroma), ncoef);
    // v3 conditions a unit on the previous unit THIS rANS LANE coded.  A lane
    // owns blocks bi, bi+nlanes, bi+2*nlanes, ..., so the encoder's rate model
    // has to walk the same relation the decoder will -- hence nlanes here and
    // the per-block class array, not "the block above".  The class is per
    // plane, so the array starts at zero for every plane, which is the
    // decoder's reset at a group boundary.
    u8 nbr[kMaxBlocksPerPlane] = {};
    // One scan and one step vector per split choice.  Index 0 is the tile's
    // transform (or transform skip) at whatever size it chose; index 1 is the
    // four 4x4 sub-blocks, which by 4.1's rule exist only at bs == 8.  The
    // second row is built anyway and left unused at other sizes rather than
    // conditioned on the size, because the loop below is already bounded by
    // `split_present` and one dead 64-entry array is cheaper than a branch
    // that has to be right.
    const u16 *scan[2] = {scan_table(ncoef, tskip != 0), kScan4Split};
    i32 stepv[2][kMaxCoef];
    for (int i = 0; i < ncoef; ++i)
        stepv[0][i] = tskip ? dequant_step(s.qp, 16)
                            : dequant_step(s.qp, block_weight(s, i));
    if (bs == 8)
        for (int i = 0; i < 64; ++i) stepv[1][i] = block_step(s, i, 0, 1);
    i16 *bc = coefs + ndc;

    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            const int bi = by * nb + bx;
            // This lane's previous unit in this plane (see analyze_plane).
            // The split choice is layered on per candidate, below.
            const UnitCtx uc0 =
                with_nbr(base_uc, bi >= nlanes ? nbr[bi - nlanes] : 0);
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
            // SATD over every mode.  The predictions are NOT kept -- at
            // 32x32 ten of them are 40 kB -- so the candidates are predicted
            // again below; predict_block is a few adds per sample.
            i32 P[kMaxCoef], d[kMaxCoef];
            int cand[kNumIntraModesCfl];
            int nc = 0;
            if (reuse) {
                // The per-tile QP search calls this once per QP candidate on
                // the same picture content.  The best directional mode for a
                // block is a property of its neighbourhood, not of the step
                // it is quantised with, so it is decided once at the tile's
                // own QP and reused here: predict, quantise, reconstruct, no
                // SATD sort and no per-mode RD.  That is what makes the QP
                // search cheap enough to leave on.
                cand[nc++] = s.modes[bi];
            } else {
                i32 cost[kNumIntraModesCfl];
                for (int m = 0; m < nmodes; ++m) {
                    predict_block(m, r, fallback, size, bx, by, lb, P, cf);
                    for (int i = 0; i < ncoef; ++i) d[i] = tgt[i] - P[i];
                    cost[m] = satd_block(d, bs);
                }
                // candidate list: the DC-plane mode plus the best `ncand` by
                // SATD
                cand[nc++] = kIntraDcPlane;
                bool taken[kNumIntraModesCfl] = {};
                taken[kIntraDcPlane] = true;
                for (int k = 0; k < ncand; ++k) {
                    int bm = -1;
                    for (int m = 0; m < nmodes; ++m)
                        if (!taken[m] && (bm < 0 || cost[m] < cost[bm])) bm = m;
                    if (bm < 0) break;
                    taken[bm] = true;
                    cand[nc++] = bm;
                }
            }
            const int mpm = mpm_of(s.modes.data(), nb, bi);
            double best = 0;
            int best_mode = kIntraDcPlane, best_split = 0;
            static thread_local i16 best_c[kMaxCoef];
            static thread_local i32 best_rec[kMaxCoef];
            bool have = false;
            for (int k = 0; k < nc; ++k) {
                const int m = cand[k];
                predict_block(m, r, fallback, size, bx, by, lb, P, cf);
                i32 res[kMaxCoef];
                for (int i = 0; i < ncoef; ++i) res[i] = tgt[i] - P[i];
                // The 4x4 split is a per-block choice INSIDE the tile's
                // transform size, so it is the inner loop; `split_present` is
                // false unless the tile chose the 8x8 transform (SYNTAX.md
                // 4.1), which is what keeps the two tools from ever being
                // asked to describe the same block twice.
                for (int sp = 0; sp <= (split_present ? 1 : 0); ++sp) {
                    i32 orig[kMaxCoef];
                    if (sp) {
                        i16 co[64];
                        forward_block(res, co, tskip, 1);
                        for (int i = 0; i < 64; ++i) orig[i] = co[i];
                    } else if (tskip) {
                        for (int i = 0; i < ncoef; ++i) orig[i] = res[i];
                    } else {
                        i16 co[kMaxCoef];
                        fdct_block(res, co, bs);
                        for (int i = 0; i < ncoef; ++i) orig[i] = co[i];
                    }
                    const i32 *st = stepv[sp];
                    const UnitCtx uc = with_split(uc0, sp, split_present != 0);
                    i16 q[kMaxCoef];
                    if (use_rdo) {
                        rdoq_unit(q, orig, st, ncoef, scan[sp], uc, rc, lambda,
                                  effort, sdh);
                    } else {
                        const u8 *dz = dead_zone_table(false);
                        for (int pp = 0; pp < ncoef; ++pp) {
                            int i = scan[sp][pp];
                            q[i] = (i16)quantize(
                                orig[i], st[i],
                                dead_zone(st[i],
                                          dz[band_of(band_pos(
                                              pp, sp != 0, uc.band_shift))]));
                        }
                    }
                    if (sdh)
                        hide_sign_unit(q, orig, st, ncoef, scan[sp],
                                       use_rdo ? &rc : nullptr,
                                       use_rdo ? lambda : 0.0, &uc);
                    i32 rr[kMaxCoef];
                    residual_block(q, s, tskip, rr, sp);
                    // exact sample-domain distortion of this candidate
                    double d2 = 0;
                    i32 rec[kMaxCoef];
                    for (int j = 0; j < bs; ++j)
                        for (int i = 0; i < bs; ++i) {
                            int y = (by << lb) + j, x = (bx << lb) + i;
                            i32 v = P[j * bs + i] + rr[j * bs + i];
                            i32 full =
                                layer ? clamp_i32(s.pred[(size_t)y * size + x] + v,
                                                  0, s.maxval)
                                      : clamp_i32(v, 0, s.maxval);
                            rec[j * bs + i] =
                                layer ? full - s.pred[(size_t)y * size + x] : full;
                            double e = (double)s.samples[(size_t)y * size + x] -
                                       (double)full;
                            d2 += e * e;
                        }
                    double bits =
                        unit_bits(q, ncoef, scan[sp], uc, rc, sdh) / 1024.0;
                    // mode signalling, exactly as the LaneMachine will code it
                    if (m == mpm) {
                        bits += mode_ctx ? rc.sym[mode_ctx][0] / 1024.0 : 1.0;
                    } else {
                        bits += mode_ctx ? rc.sym[mode_ctx][1 + nonmpm_index(
                                               mpm, m, nmodes)] /
                                               1024.0
                                         : 4.0;
                    }
                    double tc = d2 + lambda * bits;
                    if (!have || tc < best) {
                        have = true;
                        best = tc;
                        best_mode = m;
                        best_split = sp;
                        std::memcpy(best_c, q, sizeof(i16) * ncoef);
                        std::memcpy(best_rec, rec, sizeof(i32) * ncoef);                    }
                }
            }
#ifdef NXVC_XFORM_CTX_EXPERIMENT
            g_mode_hist[s.log2b - 3][best_mode]++;
#endif
            // A block with no coefficients codes no split flag, so its stored
            // flag must be 0 -- exactly what the decoder will reconstruct.
            if (split_present) {
                bool any = false;
                for (int i = 0; i < ncoef; ++i)
                    if (best_c[i]) { any = true; break; }
                s.splits[bi] = (u8)(any ? best_split : 0);
            }
            s.modes[bi] = (u8)best_mode;
            // The neighbour class this unit leaves for the lane, mirroring
            // LaneMachine::finish_coef_unit exactly.
            nbr[bi] = (u8)unit_nbr_class(best_c, ncoef, scan[best_split]);
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
    int near_skip = 0, quad_mv = 0;
    i8 corr[3][3] = {};
    i8 qmv[4][2] = {};
    // Mean absolute difference per luma sample of the WARP_SKIP predictor with
    // this tile's stored vector, Q8.  This is the `complexity` input
    // docs/RATECONTROL.md 4.1 asks the rate controller for; it is measured
    // here because the mode search computes the predictor anyway.
    // kWarpMadUnmeasured when there was no reference to measure against.
    unsigned warp_mad_q8 = 0xFFFFu;
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
    std::vector<u8> wm_map;                  // rc/'s per-tile weighting matrix
    std::vector<u16> age_since_coded;        // per tile position per eye
    // Drift-driven refresh (docs/SYNTAX.md 13.8).  `age_since_intra` is the
    // hard-cap clock; `drift` is the mean squared error, per luma sample, of
    // the client shadow this encoder holds against the source it was meant to
    // reproduce, measured on the frame just encoded.
    std::vector<u16> age_since_intra;
    std::vector<double> drift;
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
    // The source of the most recently encoded frame, kept so that
    // nxvc_encoder_set_received_tiles() can re-measure the drift of a tile it
    // has just concealed.  The gate reads the shadow against the source, and
    // concealment moves the shadow after the source is gone.
    std::vector<u8> last_src[4];
    nxvc_image last_src_img{};
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
    cfg->qp_search_step = 0;   // built-in default (2)
    cfg->dc_lambda_q8 = 0;     // built-in default
    cfg->dc_rdoq_off = 0;      // the DC plane goes through the trellis
    cfg->rdoq_effort = 0;      // built-in default (medium)
    cfg->me_effort = 0;        // built-in default (medium)
    cfg->lambda_class_off = 0; // per-class lambda on
    for (int i = 0; i < 4; ++i) cfg->lambda_class_q8[i] = 0;
    cfg->wm_id = 0;           // frame matrix everywhere (see --wm)
    // The v2 intra tools are on by default because they win on the quality
    // harness: together they are worth about -13 % BD-rate at the Phase 1
    // operating point (ref/RESULTS-intra.md).  Both set a tool bit, so a
    // decoder without them refuses the stream at the handshake rather than
    // misparsing it; `--intra-dir off --ctx v1` gets a v1.2 stream back.
    cfg->intra_dir = 1;
    cfg->intra_dir_layer = 0;  // the replace form, measured better than layer
    cfg->ctx_v2 = 1;
    // The entropy and context package ships OFF, unlike every other tool that
    // wins on the harness.  vk/decoder/passA does not implement either bit, so
    // the Vulkan decoder refuses such a stream with VERSION; an encoder
    // default the project's own GPU decoder rejects would make "a default
    // stream" mean two different things.  docs/TOOLBITS.md 7.
    cfg->ctx_v3 = 0;
    cfg->tab_v2 = 0;
    // `table_iters` means what it says: 0 is OFF.  A zeroed nxvc_config -- how
    // every caller starts -- must still get the default, so "the caller set
    // it" is a separate flag rather than a sentinel value inside the field.
    cfg->table_iters = kDefaultTableIters;
    cfg->table_iters_set = 1;
    cfg->intra_dir_cand = 0;   // built-in default (2 RD candidates + DC plane)
    cfg->sign_hide = 1;
    // The v1.5 detail tools, on for the same reason the v2 ones are: measured
    // on the quality harness they are worth -18.8 BD-rate points on 4:4:4 and
    // -16.0 on 4:2:0 together, for 1.35x encode time and nothing the decoder
    // can measure (ref/RESULTS-detail-a.md).  Each sets a tool bit, so a
    // decoder without them refuses the stream at the handshake rather than
    // misparsing it; `--split4x4 off --cfl off` gets a v1.4 stream back.
    cfg->split4x4 = 1;
    cfg->chroma_from_luma = 1;
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
    // The inter-efficiency tools (syntax v1.5) follow the same rule the v2
    // intra tools follow: on by default when the measurement says so, each
    // behind its own tool bit so a decoder without it refuses the stream at
    // the handshake rather than misparsing it.  They only ever engage on an
    // inter stream, so a Phase 1 caller is unaffected either way.
    //
    //   drift_refresh   -7.8 / -46.9 points of BD-rate  (13.8)
    //   near_skip       -5.4 /  -4.7                    (13.9)
    //   quad_mv         -9.5 /  -8.5                    (13.10)
    //   subtile_intra   -0.5 /  +0.6  -- OFF.  It needs disocclusion and this
    //                                  corpus is rotation-only by
    //                                  construction, so the measurement does
    //                                  not justify the byte.  --sub-intra on.
    // ref/RESULTS-inter-a.md section 2 is the sweep.
    cfg->drift_refresh = 1;
    cfg->drift_gate_q8 = 0;    // built-in default, 4x the quantiser floor
    cfg->near_skip = 1;
    cfg->quad_mv = 1;
    // ENTROPY_LITE ships OFF.  It is a NEGOTIATED tool, not a default: it buys
    // Pass A time with bits, and whether that trade is worth making depends on
    // a number only the DECODER has -- its own measured Pass A.  On a Pico 4
    // it is 7.5x and the only lever that reaches the frame budget at all; on a
    // desktop GPU, where Pass A already fits, it is +40-50 % bits for nothing
    // anyone needed.  So the decoder asks for it at the handshake and the
    // encoder obliges; the encoder does not guess.  docs/SYNTAX.md 9.10.
    cfg->entropy_lite = 0;
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
