// nxvc_ref: bitstream syntax, encoder and decoder.  See docs/SYNTAX.md.
#include <algorithm>
#include <cmath>
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
    int ref_sel = 0, tskip = 0, wgt = 0, wm_id = 0;
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
    std::vector<i32> pred;     // size*size, the final prediction
    std::vector<i32> wpred;    // size*size, the inter predictor (empty = intra)
    std::vector<i32> recon;    // size*size, INTRA_DIR: running reconstruction
    std::vector<u8> modes;     // nb*nb, INTRA_DIR: per-block intra mode
};

// ----------------------------------------------------------------- lambda
// ONE rate-distortion slope for the whole encoder.  Every decision that
// trades bits against distortion -- the coefficient trellis, the directional
// intra mode, the transform-skip flag, the per-tile QP offset, the inter mode
// decision and the motion search -- reads its lambda from here.  There is one
// formula, one fitted constant per content class, and no other place in the
// encoder that multiplies a rate by anything.
//
//     lambda(QP, class) = scale * kLambdaClass[class] * qstep(QP)^2
//
// The qstep^2 shape is the standard high-rate result: at the optimum of
// D + lambda*R for a uniform quantiser, -dD/dR is proportional to the square
// of the step.  What is fitted, on tools/quality and not assumed, is `scale`
// and the four class multipliers; ref/RESULTS-rdo-a.md section 2 is the sweep.
//
// Distortion is a sum of SQUARED sample errors and rate is bits, everywhere.
// `sad` is the slope of the one decision whose distortion is a sum of
// ABSOLUTE differences -- the integer motion search, which cannot afford a
// reconstruction per candidate.  sqrt(lambda) is the standard proxy: for a
// fixed error shape SAD grows as the square root of SSE, so a Lagrangian
// stated on SAD needs the square root of the Lagrangian stated on SSE.
struct Lambda {
    double coef = 0;   // D in squared samples, R in bits
    double sad = 0;    // D in absolute samples, R in bits
};

// docs/RATECONTROL.md 3.2: the four classes are not decoration, they measure
// how much distortion a tile can absorb before it is seen.  Flat and text
// tiles show quantisation directly (banding, broken glyphs) and get a lower
// lambda, i.e. more bits than the rate curve alone would give them; texture
// masks it and gets more.
enum TileClass { kClassFlat = 0, kClassTexture, kClassEdge, kClassText,
                 kNumTileClass };

// Fitted on vr-mixed-1024-v2 (ref/RESULTS-rdo-a.md 2.2).  A multiplier of 1
// means "the frame's own slope"; the numbers are deliberately mild, because
// the classifier is a 64x64 statistic and a large per-tile lambda swing shows
// up as a visible tile boundary.
inline constexpr double kLambdaClass[kNumTileClass] = {
    0.70,   // flat:    banding is visible at any rate
    1.15,   // texture: masks its own quantisation noise
    1.00,   // edge
    0.70,   // text:    a broken glyph is not a small error
};

// The measured default of `scale`.  ref/RESULTS-rdo-a.md 2.1 sweeps it; below
// 0.15 the trellis barely fires and above 0.6 it zeroes structure the eye
// sees.  --rdo-lambda overrides it.
inline constexpr double kLambdaScaleDefault = 0.30;

// How much a squared error in one chroma sample is worth relative to one in a
// luma sample.  The encoder used to weigh them equally, which is the one
// weighting nobody uses: the JVET reporting convention is 6:1:1, every
// perceptual model puts chroma lower still, and both anchors here code chroma
// with a quantiser of its own.  Coding the two planes at one lambda and one
// weight is what over-coded chroma.
//
// Every distortion in the encoder is weighed by plane_dist_weight() and every
// lambda by the same number through lambda_for(), which are the same statement
// twice: weighing a plane's D down by w is dividing its lambda by w.  0.25 was
// fitted on the harness against the 6:1:1 PSNR and NOT against PSNR-Y --
// ref/RESULTS-rdo-a.md 3, which reports both and says how much of each number
// is the metric rather than the picture.
inline constexpr double kChromaDistWeight = 0.25;

// Plane index p (0 = Y, 1/2 = Co/Cg, 3 = alpha) to its distortion weight.
static inline double plane_dist_weight(int p, double chroma_weight) {
    return (p == 1 || p == 2) ? chroma_weight : 1.0;
}

// `dist_weight` is what one squared error in this plane is worth: 1 for luma,
// the chroma weight for Co/Cg.  Weighting the distortion down by w is the same
// statement as dividing the lambda by w, and doing it here keeps it in one
// place instead of at every call site that forms a D.
static inline Lambda lambda_for(int qp, int cls, double scale,
                                double dist_weight = 1.0) {
    const double qstep = (double)kQStep[clamp_i32(qp, 0, 63)] / 16.0;
    Lambda L;
    L.coef = scale * kLambdaClass[clamp_i32(cls, 0, kNumTileClass - 1)] *
             qstep * qstep / (dist_weight > 0 ? dist_weight : 1.0);
    L.sad = std::sqrt(L.coef);
    return L;
}

// docs/RATECONTROL.md 3.1 and 3.3, on one plane of one tile: the statistics
// and the normative class order, without the UI stencil (which is a caller's
// input, not an image property) and without the hysteresis (which needs the
// previous frame's classes and belongs to rc/, not here).  `size` is the
// coded plane size, and the gradient energy is normalised per sample so the
// thresholds do not depend on it.
static int classify_tile(const i32 *s, int size) {
    if (size < 3) return kClassFlat;
    const double n = (double)size * size;
    double sum = 0, sum2 = 0, jxx = 0, jyy = 0, jxy = 0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double v = (double)s[(size_t)y * size + x];
            sum += v;
            sum2 += v * v;
            const int xm = x > 0 ? x - 1 : 0, xp = x + 1 < size ? x + 1 : size - 1;
            const int ym = y > 0 ? y - 1 : 0, yp = y + 1 < size ? y + 1 : size - 1;
            const double gx = ((double)s[(size_t)y * size + xp] -
                               (double)s[(size_t)y * size + xm]) / 2.0;
            const double gy = ((double)s[(size_t)yp * size + x] -
                               (double)s[(size_t)ym * size + x]) / 2.0;
            jxx += gx * gx;
            jyy += gy * gy;
            jxy += gx * gy;
        }
    const double mean = sum / n;
    const double var = sum2 / n - mean * mean;
    const double log_var = std::log2((var > 0 ? var : 0) + 1.0);
    const double trace = jxx + jyy;
    const double G = trace / n;
    const double C = trace > 0 ? std::sqrt((jxx - jyy) * (jxx - jyy) +
                                           4.0 * jxy * jxy) / trace : 0.0;
    const double R = var > 0 ? G / var : 0.0;
    if (G < 12.0 || log_var < 3.0) return kClassFlat;
    if (G >= 2500.0 && log_var >= 11.0 && R <= 0.93) return kClassText;
    if (C >= 0.45) return kClassEdge;
    return kClassTexture;
}

// ----------------------------------------------------------------- effort
// The encode-time budget.  Every knob here costs encoder time and NO
// bitstream: a stream written at `fast` decodes through exactly the same path
// as one written at `slow`, and `nxv-info` cannot tell them apart.  A preset
// is the only place these numbers come from, and an explicit config field
// overrides the preset it came from.  ref/RESULTS-rdo-a.md section 7 is the
// time/quality table the three points were chosen from.
struct Effort {
    double chroma_weight = kChromaDistWeight;  // see kChromaDistWeight
    bool trellis_full = true;  // rdoq_unit's wide candidate set
    bool trellis_dc = true;    // trellis the DC plane, not only the AC blocks
    int dir_cand = 4;          // directional intra modes RD-checked per block
    bool tskip_rd = false;     // transform skip by RD, not by the gradient rule
    int mv_step = 2;           // coarse motion-search step, in samples
    int mv_rd_qpel = 1;        // quarter-pel neighbours re-scored by real RD
    int qp_search = 0;         // per-tile QP offsets tried each side
    // Quantize the tile once to choose its probability table set, then again
    // for real.  The second pass is what the trellis and the directional mode
    // decision need a rate model for, so the first one is not free -- it is
    // the whole tile again.  Without it the tile keeps the table set its QP
    // implies, which is right about two thirds of the time.
    bool tables_two_pass = true;
};

// The three points on the ladder.  Each row is a deliberate choice, not a
// scaling of one number: `fast` gives up the two things that cost the most
// per point of BD-rate (the wide trellis candidate set and the DC-plane
// trellis), `slow` buys the three that cost the most and win the least
// (exhaustive directional modes, RD transform skip, the per-tile QP search).
static inline Effort effort_of_preset(unsigned preset) {
    Effort e;
    switch (preset) {
        case NXVC_PRESET_FAST:
            e.trellis_full = false;
            e.trellis_dc = false;
            e.dir_cand = 2;
            e.tskip_rd = false;
            e.mv_step = 4;
            e.mv_rd_qpel = 0;
            e.qp_search = 0;
            e.tables_two_pass = false;
            break;
        case NXVC_PRESET_SLOW:
            e.trellis_full = true;
            e.trellis_dc = true;
            e.dir_cand = 8;
            e.tskip_rd = true;
            e.mv_step = 1;
            e.mv_rd_qpel = 1;
            e.qp_search = 1;
            break;
        default:   // NXVC_PRESET_MEDIUM
            break;
    }
    return e;
}

// The preset, then every explicit override.  This is the only place a
// nxvc_config becomes an Effort.
static inline Effort resolve_effort(const nxvc_config &cfg) {
    Effort e = effort_of_preset(cfg.preset);
    if (cfg.chroma_weight_q8)
        e.chroma_weight = (double)cfg.chroma_weight_q8 / 256.0;
    // Lossless has no distortion to weigh, and a plane coded at a weight
    // other than 1 would not be bit exact.
    if (cfg.lossless) e.chroma_weight = 1.0;
    if (cfg.trellis_full) e.trellis_full = cfg.trellis_full >= 2;
    if (cfg.trellis_dc) e.trellis_dc = cfg.trellis_dc >= 2;
    if (cfg.tskip_rd) e.tskip_rd = cfg.tskip_rd >= 2;
    if (cfg.mv_rd_qpel) e.mv_rd_qpel = cfg.mv_rd_qpel >= 2 ? 1 : 0;
    if (cfg.mv_step) e.mv_step = clamp_i32((int)cfg.mv_step, 1, 16);
    if (cfg.intra_dir_cand) e.dir_cand = clamp_i32((int)cfg.intra_dir_cand, 1, 8);
    if (cfg.qp_search == 255) e.qp_search = 0;
    else if (cfg.qp_search) e.qp_search = clamp_i32((int)cfg.qp_search, 1, 8);
    if (cfg.lossless) { e.qp_search = 0; e.tskip_rd = false; }
    return e;
}

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
    // docs/RATECONTROL.md 3.3 on this tile's luma, set by load_tile and read
    // by every lambda in the encoder.  Source-derived, so it does not change
    // with the mode or the QP a candidate is being scored at.
    int cls = kClassTexture;
    PlaneState pl[4];
    std::vector<i16> coef;
    std::vector<Unit> units;

    void setup();
    // The part of setup() that depends only on the QP and the weighting
    // matrix, so a candidate at a different qp_delta or wm_id can be scored
    // without reloading and re-transforming the tile's samples.  One
    // implementation, called by setup() and by the per-tile QP search.
    void requant_params();
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
    for (int p = 0; p < nplanes; ++p) {
        PlaneState &s = pl[p];
        bool chroma = (p == 1 || p == 2);
        s.size = chroma ? tg.chroma_size : tg.coded_size;
        s.nb = s.size / 8;
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
    requant_params();
}

void TileCoder::requant_params() {
    const int qp = clamp_i32(fp->base_qp + tp.qp_delta, 0, 63);
    for (int p = 0; p < nplanes; ++p) {
        PlaneState &s = pl[p];
        const bool chroma = (p == 1 || p == 2);
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
        for (int b = 0; b < ndc; ++b) {
            Unit v{};
            v.coef = &coef[off];
            v.ncoef = 64;
            v.scan = scan_table(64, tp.tskip != 0);
            v.ctx_cbf = ccbf;
            v.ctx_last = clast;
            v.sdh = (u8)sdh;
            units.push_back(v);
            off += 64;
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
    i32 a[17];  // a[0] = corner, a[1 + k] = top[k],  k = 0..15
    i32 l[17];  // l[0] = corner, l[1 + k] = left[k], k = 0..15
};

// Development hook for the Pass B wavefront cost study (ref/RESULTS-intra.md
// section 5).  Build with -DNXVC_DIR_SCHED_EXPERIMENT and set NXVC_DIR_SCHED
// to 1 (drop the top-right reference), 2 (confine the dependency to 32x32
// sub-tiles) or 3 (both) to measure what each restriction costs in rate.
//
// A stream produced with a nonzero value is NOT conformant -- the reference
// derivation in SYNTAX.md 7.4 is the one with the full 8x8 raster dependency
// -- which is why the hook is compiled out of a normal build rather than
// merely defaulted off.
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
                       int by, IntraRefs &r) {
    const int x0 = bx * 8, y0 = by * 8;
    const int sched = dir_sched();
    auto at = [&](int x, int y) -> i32 {
        int cx = clamp_i32(x, 0, size - 1), cy = clamp_i32(y, 0, size - 1);
        int nbx = cx >> 3, nby = cy >> 3;
        bool done = (nby < by) || (nby == by && nbx < bx);
        if ((sched & 1) && nbx > bx) done = false;          // no top-right
        if ((sched & 2) && ((nbx >> 2) != (bx >> 2) ||
                            (nby >> 2) != (by >> 2)))
            done = false;                                   // 32x32 sub-tiles
        const i32 *src = done ? recon : fallback;
        return src[(size_t)cy * size + cx];
    };
    r.a[0] = r.l[0] = at(x0 - 1, y0 - 1);
    for (int k = 0; k < 16; ++k) {
        r.a[1 + k] = at(x0 + k, y0 - 1);
        r.l[1 + k] = at(x0 - 1, y0 + k);
    }
}

// P[j * 8 + i] for one 8x8 block.  Every mode but kIntraDcPlane is a weighted
// average of in-range references, so no clamp is needed and none is applied.
static void predict_block(int mode, const IntraRefs &r, const i32 *base,
                          int size, int bx, int by, i32 P[64]) {
    const i32 *A = r.a + 1;  // A[-1] == corner
    const i32 *L = r.l + 1;  // L[-1] == corner
    const i32 tl = r.a[0];
    switch (mode) {
        case kIntraDcPlane:
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    P[j * 8 + i] =
                        base[(size_t)(by * 8 + j) * size + bx * 8 + i];
            return;
        case kIntraDc: {
            i32 sum = 0;
            for (int k = 0; k < 8; ++k) sum += A[k] + L[k];
            i32 dc = (sum + 8) >> 4;
            for (int i = 0; i < 64; ++i) P[i] = dc;
            return;
        }
        case kIntraPlanar:
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    P[j * 8 + i] = ((7 - i) * L[j] + (i + 1) * A[8] +
                                    (7 - j) * A[i] + (j + 1) * L[8] + 8) >> 4;
            return;
        case kIntraH:
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) P[j * 8 + i] = L[j];
            return;
        case kIntraV:
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) P[j * 8 + i] = A[i];
            return;
        case kIntraDdl:
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int k = i + j;
                    P[j * 8 + i] = (k == 14)
                        ? (A[14] + 3 * A[15] + 2) >> 2
                        : (A[k] + 2 * A[k + 1] + A[k + 2] + 2) >> 2;
                }
            return;
        case kIntraDdr:
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    if (i > j) {
                        int k = i - j;
                        P[j * 8 + i] =
                            (A[k - 2] + 2 * A[k - 1] + A[k] + 2) >> 2;
                    } else if (i < j) {
                        int k = j - i;
                        P[j * 8 + i] =
                            (L[k - 2] + 2 * L[k - 1] + L[k] + 2) >> 2;
                    } else {
                        P[j * 8 + i] = (A[0] + 2 * tl + L[0] + 2) >> 2;
                    }
                }
            return;
        case kIntraVr:
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int z = 2 * i - j;
                    int k = i - (j >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        P[j * 8 + i] = (A[k - 1] + A[k] + 1) >> 1;
                    else if (z >= 0)
                        P[j * 8 + i] =
                            (A[k - 2] + 2 * A[k - 1] + A[k] + 2) >> 2;
                    else if (z == -1)
                        P[j * 8 + i] = (L[0] + 2 * tl + A[0] + 2) >> 2;
                    else {
                        int q = j - 2 * i;
                        P[j * 8 + i] =
                            (L[q - 1] + 2 * L[q - 2] + L[q - 3] + 2) >> 2;
                    }
                }
            return;
        default:  // kIntraHd
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int z = 2 * j - i;
                    int k = j - (i >> 1);
                    if (z >= 0 && (z & 1) == 0)
                        P[j * 8 + i] = (L[k - 1] + L[k] + 1) >> 1;
                    else if (z >= 0)
                        P[j * 8 + i] =
                            (L[k - 2] + 2 * L[k - 1] + L[k] + 2) >> 2;
                    else if (z == -1)
                        P[j * 8 + i] = (A[0] + 2 * tl + L[0] + 2) >> 2;
                    else {
                        int q = i - 2 * j;
                        P[j * 8 + i] =
                            (A[q - 1] + 2 * A[q - 2] + A[q - 3] + 2) >> 2;
                    }
                }
            return;
    }
}

// Dequantize + inverse transform one residual block.
static void residual_block(const i16 *c, const PlaneState &s, int tskip,
                           i32 res[64]) {
    if (tskip) {
        int t = dequant_step(s.qp, 16);
        for (int i = 0; i < 64; ++i) res[i] = dequant(c[i], t);
    } else {
        i32 dq[64];
        for (int i = 0; i < 64; ++i)
            dq[i] = dequant(c[i], dequant_step(s.qp, s.wmat[i]));
        idct8x8(dq, res);
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
    // planar prediction: bilinear over block centres (8x8 blocks)
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            s.pred[(size_t)y * size + x] = bilinear_q4_i32(
                s.means.data(), nb, nb, nb, 2 * x - 7, 2 * y - 7);
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
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    reconstruct_dc_plane(s, coefs);
    const i16 *bc = coefs + ndc;
    if (!dir) {
        for (int by = 0; by < nb; ++by)
            for (int bx = 0; bx < nb; ++bx) {
                const i16 *c = bc + ((size_t)by * nb + bx) * 64;
                i32 res[64];
                residual_block(c, s, tskip, res);
                for (int j = 0; j < 8; ++j)
                    for (int i = 0; i < 8; ++i) {
                        int y = by * 8 + j, x = bx * 8 + i;
                        s.samples[(size_t)y * size + x] = clamp_i32(
                            s.pred[(size_t)y * size + x] + res[j * 8 + i], 0,
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
            const i16 *c = bc + ((size_t)by * nb + bx) * 64;
            i32 res[64], P[64];
            residual_block(c, s, tskip, res);
            IntraRefs r;
            build_refs(s.recon.data(), fallback, size, bx, by, r);
            predict_block(s.modes[(size_t)by * nb + bx], r, fallback, size, bx,
                          by, P);
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    i32 v = P[j * 8 + i] + res[j * 8 + i];
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
    // True when coding a zero is at least as cheap as coding a one in every
    // context.  It is what makes the trellis's scan truncation below exact
    // rather than approximate, so it is measured off the table rather than
    // assumed: a trained table could in principle put level 1 above level 0.
    bool zero_cheapest;
};

static void build_rate_cost(const TableSet &ts, RateCost &rc) {
    for (int c = 0; c < kNumCtx; ++c)
        for (int s = 0; s < kNumSym; ++s) {
            double f = (double)ts.ctx[c].freq[s] / 1024.0;
            if (f <= 0) f = 1.0 / 1024.0;
            rc.sym[c][s] = (i32)(-std::log2(f) * 1024.0 + 0.5);
        }
    rc.zero_cheapest = true;
    for (int c = 0; c < kNumCtx; ++c)
        if (rc.sym[c][1] < rc.sym[c][0]) rc.zero_cheapest = false;
}

// Bypass bits an escape suffix costs for magnitude m >= 15 (Exp-Golomb 3 of
// m - 15), matching eg3_encode in entropy.cpp exactly.
static inline int escape_bits(i32 m) {
    u32 n = (u32)(m - 15) + 8u;
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    return (b - kEscOrder) + 1 + b;  // j ones, one zero, b suffix bits
}

// `ctx_level` is kCtxNone for the position/class-dependent LEVEL model and a
// fixed context id for a unit that codes every level in one context (the DC
// plane under CTX_V2, SYNTAX.md 9.3).
static inline i32 level_rate(const RateCost &rc, int scan_pos, int prev_class,
                             i32 m, int ctx_level) {
    int ctx = ctx_level ? ctx_level : level_ctx(scan_pos, prev_class);
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
                      const u8 *scan, int ctx_cbf, int ctx_last, int ctx_level,
                      const RateCost &rc, double lambda, bool full) {
    double f[64][3], fnz[64];
    i32 best_m[64][3], best_m_nz[64];
    double tail = 0;               // energy of scan positions above p
    double energy = 0;
    for (int i = 0; i < ncoef; ++i) {
        double c = orig[i];
        energy += c * c;
    }

    // A scan position whose magnitude is below half its step can only be
    // zero: (a - st)^2 > a^2 there, so level 1 costs it more distortion than
    // level 0, and `zero_cheapest` says it costs at least as much rate too.
    // No lambda makes it win.  Everything above the last position that clears
    // that bar is therefore zero, `last` can never reach it, and the trellis
    // does not have to visit it.  With `zero_cheapest` this is an identity
    // rather than a heuristic, and it is what makes the trellis affordable at
    // the QPs where most of a frame is zeroes.
    int pmax = ncoef - 1;
    if (rc.zero_cheapest) {
        pmax = -1;
        for (int p = 0; p < ncoef; ++p) {
            int idx = scan[p];
            double a = orig[idx] < 0 ? -(double)orig[idx] : (double)orig[idx];
            if (2.0 * a * 16.0 >= (double)step[idx]) pmax = p;
        }
    }
    for (int i = 0; i < ncoef; ++i) coefs[i] = 0;
    if (pmax < 0) return;

    double prev[3] = {0, 0, 0};
    for (int p = 0; p <= pmax; ++p) {
        int idx = scan[p];
        double c = orig[idx];
        double a = c < 0 ? -c : c;
        double st = (double)step[idx] / 16.0;
        i32 m0 = (i32)(a / st);
        if (m0 > 32767) m0 = 32767;
        // The candidate set.  {0, floor, floor+1} brackets the unquantized
        // value; `full` adds the two levels a Markov rate model can prefer
        // over a closer one -- floor-1, which may sit in a cheaper magnitude
        // class, and 14, the largest level that is not an Exp-Golomb escape
        // (entropy.cpp eg3_encode) and so the last cheap one before the rate
        // jumps by several bits.
        i32 cand[6];
        int nc = 0;
        cand[nc++] = 0;
        if (m0 > 0) cand[nc++] = m0;
        if (m0 < 32767) cand[nc++] = m0 + 1;
        if (full) {
            if (m0 >= 2) cand[nc++] = m0 - 1;
            if (m0 >= 16) cand[nc++] = 14;
        }
        if (rc.zero_cheapest && 2.0 * a < st) nc = 1;   // the identity above
        for (int s = 0; s < 3; ++s) {
            double best = kRdInf;
            i32 bm = 0;
            double bestnz = kRdInf;
            i32 bmnz = -1;
            for (int k = 0; k < nc; ++k) {
                i32 m = cand[k];
                double d = a - (double)m * st;
                double cost = d * d +
                              lambda * (level_rate(rc, p, s, m, ctx_level) / 1024.0) +
                              prev[level_class(m)];
                if (cost < best) { best = cost; bm = m; }
                if (m != 0 && cost < bestnz) { bestnz = cost; bmnz = m; }
            }
            f[p][s] = best;
            best_m[p][s] = bm;
            if (s == 0) { fnz[p] = bestnz; best_m_nz[p] = bmnz; }
            // With one LEVEL context for the whole unit the three states are
            // the same problem; solve it once.
            if (ctx_level) {
                for (int t = 1; t < 3; ++t) { f[p][t] = best; best_m[p][t] = bm; }
                break;
            }
        }
        for (int s = 0; s < 3; ++s) prev[s] = f[p][s];
    }

    // Choose `last`.
    double best_total = rc.sym[ctx_cbf][0] * lambda / 1024.0 + energy;
    int best_last = -1;
    tail = 0;
    for (int p = ncoef - 1; p > pmax; --p) {
        double c = orig[scan[p]];
        tail += c * c;
    }
    for (int p = pmax; p >= 0; --p) {
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
                           int ncoef, const u8 *scan) {
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
// The trellis parameters for the DC plane, or nullptr for the plain dead-zone
// quantizer.  The DC plane is the intra predictor and, for an inter tile, the
// residual's mean: one level chosen there moves the prediction of all 64
// blocks of the plane, and the unit's own distortion is stated in block-MEAN
// units rather than sample units.  A mean error e raises the block's sample
// SSE by 64 e^2 -- the block area -- before the AC blocks correct part of it
// at a rate cost of their own.  kDcPropagation is that factor, and the DC
// plane is trellised at lambda / kDcPropagation so its decisions are in the
// same currency as every other one in the encoder.
//
// The geometric argument gives 64.  32 is the fitted value: the AC blocks
// recover about half the mean error, and 32 is worth 0.5 BD-rate points more
// than 64 on the harness.  It is also the floor of what is safe -- at 16 the
// trellis takes 43 % off a QP 48 frame for 2.4 dB, which is a good RD trade
// and a bad picture, and `ref.codec`'s quality floors catch it.
// ref/RESULTS-rdo-a.md 3.
struct DcRdoq {
    const RateCost *rc;
    double lambda;
    int ctx_cbf, ctx_last, ctx_level;
    bool full;
};
inline constexpr double kDcPropagation = 32.0;

static void analyze_dc_plane(PlaneState &s, i16 *coefs, int sdh,
                             const DcRdoq *rd) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    std::vector<i32> m(ndc);
    const bool inter = !s.wpred.empty();
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            i32 sum = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    size_t k = (size_t)(by * 8 + j) * size + bx * 8 + i;
                    // For an inter tile the DC plane codes the mean of the
                    // residual, offset back so the quantized quantity is the
                    // same `m - dc_off` in both paths.
                    sum += s.samples[k] - (inter ? s.wpred[k] - s.dc_off : 0);
                }
            i32 mean = sum >= 0 ? (sum + 32) >> 6 : -((-sum + 32) >> 6);
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
    if (rd) {
        rdoq_unit(coefs, orig, stepv, ndc, scan_table(ndc, false), rd->ctx_cbf,
                  rd->ctx_last, rd->ctx_level, *rd->rc, rd->lambda, rd->full);
    } else {
        for (int i = 0; i < ndc; ++i)
            coefs[i] = (i16)quantize(orig[i], tdc, tdc / 3);
    }
    if (sdh) hide_sign_unit(coefs, orig, stepv, ndc, scan_table(ndc, false));
    reconstruct_dc_plane(s, coefs);
    (void)size;
}

// Encoder side: quantize a plane into `coefs` and leave the same
// reconstruction in s.samples that the decoder will produce.
static void analyze_plane(PlaneState &s, i16 *coefs, int tskip, int intra_dz,
                          int sdh) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    analyze_dc_plane(s, coefs, sdh, nullptr);
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
            i32 orig[64], stepv[64];
            if (tskip) {
                int t = dequant_step(s.qp, 16);
                for (int i = 0; i < 64; ++i) {
                    orig[i] = res[i];
                    stepv[i] = t;
                    c[i] = (i16)quantize(res[i], t, intra_dz ? t / 3 : t / 2);
                }
            } else {
                i16 co[64];
                fdct8x8(res, co);
                for (int i = 0; i < 64; ++i) {
                    int t = dequant_step(s.qp, s.wmat[i]);
                    orig[i] = co[i];
                    stepv[i] = t;
                    c[i] = (i16)quantize(co[i], t, t / 3);
                }
            }
            if (sdh)
                hide_sign_unit(c, orig, stepv, 64, scan_table(64, tskip != 0));
        }
}

// Re-quantize the residual blocks of a plane with the RD trellis above.  The
// DC plane is deliberately left on the plain dead-zone quantizer: it is the
// intra predictor, so a level chosen there changes `pred` for all 64 blocks
// and the trellis's single-unit distortion model would be wrong about it.
// The DC plane's trellis parameters.  `nctx` decides which contexts the
// plane's unit is coded in (build_units, SYNTAX.md 9.3), so the rate model
// here is the one the entropy coder will actually use.
static DcRdoq dc_rdoq(bool chroma, int nctx, const RateCost &rc, double lambda,
                      const Effort &eff) {
    DcRdoq rd;
    rd.rc = &rc;
    rd.lambda = lambda / kDcPropagation;
    const bool v2 = nctx >= kNumCtxV2;
    rd.ctx_cbf = v2 ? kCtxCbfDc : (chroma ? kCtxCbfChroma : kCtxCbfLuma);
    rd.ctx_last = v2 ? kCtxLastDc : (chroma ? kCtxLastChroma : kCtxLastLuma);
    rd.ctx_level = v2 ? kCtxLevelDc : kCtxNone;
    rd.full = eff.trellis_full;
    return rd;
}

static void rdoq_plane(PlaneState &s, i16 *coefs, int tskip, bool chroma,
                       int nctx, const RateCost &rc, double lambda_scale,
                       int cls, int sdh, const Effort &eff) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    const double lambda =
        lambda_for(s.qp, cls, lambda_scale,
                   plane_dist_weight(chroma ? 1 : 0, eff.chroma_weight)).coef;
    const bool full = eff.trellis_full;
    const u8 *scan = scan_table(64, tskip != 0);
    const int ctx_cbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
    const int ctx_last = chroma ? kCtxLastChroma : kCtxLastLuma;
    // The DC plane first: it is the predictor the residual blocks below are
    // measured against, so re-choosing its levels has to happen before those
    // residuals are formed, not after.
    if (eff.trellis_dc) {
        DcRdoq rd = dc_rdoq(chroma, nctx, rc, lambda, eff);
        analyze_dc_plane(s, coefs, sdh, &rd);
    }
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
            rdoq_unit(c, orig, stepv, 64, scan, ctx_cbf, ctx_last, kCtxNone, rc,
                      lambda, full);
            if (sdh) hide_sign_unit(c, orig, stepv, 64, scan);
        }
}

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

// Q10 bits one coding unit costs under `rc`, mirroring the LaneMachine.
static i32 unit_bits(const i16 *c, int ncoef, const u8 *scan, int ctx_cbf,
                     int ctx_last, int ctx_level, const RateCost &rc,
                     int sdh) {
    int last = -1;
    for (int p = ncoef - 1; p >= 0; --p)
        if (c[scan[p]] != 0) { last = p; break; }
    if (last < 0) return rc.sym[ctx_cbf][0];
    i32 r = rc.sym[ctx_cbf][1];
    if (ncoef > 1) {
        int cls = last_class_of(last);
        r += rc.sym[ctx_last][cls] + (kLastRawBits[cls] << 10);
    }
    if (last >= kSdhMinLast && sdh) r -= 1 << 10;
    int prev = 0;
    for (int p = last; p >= 0; --p) {
        i32 q = c[scan[p]];
        i32 m = q < 0 ? -q : q;
        int ctx = ctx_level ? ctx_level : level_ctx(p, prev);
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
                              int nctx, double lambda_scale, int cls,
                              bool use_rdo, int ncand, int mode_ctx, int sdh,
                              const Effort &eff) {
    const int nb = s.nb, size = s.size;
    const int ndc = nb * nb;
    const double lambda =
        lambda_for(s.qp, cls, lambda_scale,
                   plane_dist_weight(chroma ? 1 : 0, eff.chroma_weight)).coef;
    const bool full = eff.trellis_full;
    if (use_rdo && eff.trellis_dc) {
        DcRdoq rd = dc_rdoq(chroma, nctx, rc, lambda, eff);
        analyze_dc_plane(s, coefs, sdh, &rd);
    } else {
        analyze_dc_plane(s, coefs, sdh, nullptr);
    }
    if ((int)s.recon.size() != size * size) s.recon.assign((size_t)size * size, 0);
    if ((int)s.modes.size() != ndc) s.modes.assign((size_t)ndc, 0);

    std::vector<i32> zero;
    const i32 *fallback = s.pred.data();
    if (layer) {
        zero.assign((size_t)size * size, 0);
        fallback = zero.data();
    }
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
            const int bi = by * nb + bx;
            i16 *c = bc + (size_t)bi * 64;
            IntraRefs r;
            build_refs(s.recon.data(), fallback, size, bx, by, r);
            // target: the samples this block must reproduce, in the domain
            // the modes predict (samples, or the DC-plane residual).
            i32 tgt[64];
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    int y = by * 8 + j, x = bx * 8 + i;
                    i32 v = s.samples[(size_t)y * size + x];
                    if (layer) v -= s.pred[(size_t)y * size + x];
                    tgt[j * 8 + i] = v;
                }
            i32 P[kNumIntraModes][64];
            i32 cost[kNumIntraModes];
            for (int m = 0; m < kNumIntraModes; ++m) {
                predict_block(m, r, fallback, size, bx, by, P[m]);
                i32 d[64];
                for (int i = 0; i < 64; ++i) d[i] = tgt[i] - P[m][i];
                cost[m] = satd8x8(d);
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
            i16 best_c[64];
            i32 best_rec[64];
            bool have = false;
            for (int k = 0; k < nc; ++k) {
                const int m = cand[k];
                i32 res[64];
                for (int i = 0; i < 64; ++i) res[i] = tgt[i] - P[m][i];
                i32 orig[64];
                if (tskip) {
                    for (int i = 0; i < 64; ++i) orig[i] = res[i];
                } else {
                    i16 co[64];
                    fdct8x8(res, co);
                    for (int i = 0; i < 64; ++i) orig[i] = co[i];
                }
                i16 q[64];
                if (use_rdo) {
                    rdoq_unit(q, orig, stepv, 64, scan, ctx_cbf, ctx_last,
                              kCtxNone, rc, lambda, full);
                } else {
                    for (int i = 0; i < 64; ++i)
                        q[i] = (i16)quantize(orig[i], stepv[i], stepv[i] / 3);
                }
                if (sdh) hide_sign_unit(q, orig, stepv, 64, scan);
                i32 rr[64];
                residual_block(q, s, tskip, rr);
                // exact sample-domain distortion of this candidate
                double d2 = 0;
                i32 rec[64];
                for (int j = 0; j < 8; ++j)
                    for (int i = 0; i < 8; ++i) {
                        int y = by * 8 + j, x = bx * 8 + i;
                        i32 v = P[m][j * 8 + i] + rr[j * 8 + i];
                        i32 full = layer
                                       ? clamp_i32(s.pred[(size_t)y * size + x] + v,
                                                   0, s.maxval)
                                       : clamp_i32(v, 0, s.maxval);
                        rec[j * 8 + i] =
                            layer ? full - s.pred[(size_t)y * size + x] : full;
                        double e = (double)s.samples[(size_t)y * size + x] -
                                   (double)full;
                        d2 += e * e;
                    }
                double bits =
                    unit_bits(q, 64, scan, ctx_cbf, ctx_last, 0, rc, sdh) /
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
                    std::memcpy(best_c, q, sizeof best_c);
                    std::memcpy(best_rec, rec, sizeof best_rec);
                }
            }
            s.modes[bi] = (u8)best_mode;
            std::memcpy(c, best_c, sizeof best_c);
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    s.recon[(size_t)(by * 8 + j) * size + bx * 8 + i] =
                        best_rec[j * 8 + i];
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
