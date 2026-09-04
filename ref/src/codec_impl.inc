// Included by codec.cpp.  Encoder / decoder bodies.
namespace nxvc {

// --------------------------------------------------------- plane accessors
struct PlaneRef {
    const u8 *p = nullptr;
    u8 *w = nullptr;
    int stride = 0, width = 0, height = 0;
};

static inline int fetch_clamped(const PlaneRef &pr, int x, int y) {
    x = clamp_i32(x, 0, pr.width - 1);
    y = clamp_i32(y, 0, pr.height - 1);
    return pr.p[(size_t)y * pr.stride + x];
}

// ------------------------------------------------------- probability tables
// Serialize one table set as `nctx` x 16 five-bit log-domain deltas: 120
// bytes for the 12-context model, 160 for the 16-context one.
static void serialize_table_set(BitW &bw, const u16 target[kNumCtx][kNumSym],
                                int set_index, int nctx, TableSet &decoded) {
    for (int c = 0; c < nctx; ++c) {
        u16 f[kNumSym];
        for (int s = 0; s < kNumSym; ++s) {
            i32 def = default_freq(nctx, set_index, c, s);
            i32 want = target[c][s] < 1 ? 1 : target[c][s];
            int best = 16, bestErr = -1;
            for (int d = 0; d < 32; ++d) {
                i32 v = (def * kDeltaMul[d] + 128) >> 8;
                if (v < 1) v = 1;
                int err = v > want ? v - want : want - v;
                if (bestErr < 0 || err < bestErr) { bestErr = err; best = d; }
            }
            bw.put((u32)best, 5);
            i32 v = (def * kDeltaMul[best] + 128) >> 8;
            f[s] = (u16)(v < 1 ? 1 : (v > 32767 ? 32767 : v));
        }
        normalize_freqs(f);
        for (int s = 0; s < kNumSym; ++s) decoded.ctx[c].freq[s] = f[s];
        finalize_ctx(decoded.ctx[c]);
    }
}

static bool parse_table_set(BitR &br, int set_index, int nctx, TableSet &out) {
    for (int c = 0; c < nctx; ++c) {
        u16 f[kNumSym];
        for (int s = 0; s < kNumSym; ++s) {
            u32 d = br.get(5);
            i32 def = default_freq(nctx, set_index, c, s);
            i32 v = (def * kDeltaMul[d] + 128) >> 8;
            if (v < 1) v = 1;
            if (v > 32767) v = 32767;
            f[s] = (u16)v;
        }
        normalize_freqs(f);
        for (int s = 0; s < kNumSym; ++s) out.ctx[c].freq[s] = f[s];
        if (!finalize_ctx(out.ctx[c])) return false;
    }
    return true;
}

// Cost in Q8 bits of coding `hist` with table set `ts` (encoder side only).
static double table_set_cost(const u32 hist[kNumCtx][kNumSym],
                             const TableSet &ts) {
    double bits = 0;
    for (int c = 0; c < kNumCtx; ++c)
        for (int s = 0; s < kNumSym; ++s)
            if (hist[c][s])
                bits -= (double)hist[c][s] *
                        std::log2((double)ts.ctx[c].freq[s] / 1024.0);
    return bits;
}

// ------------------------------------------------------------ tile helpers
static size_t plane_coef_offset(const TileCoder &tc, int p) {
    size_t off = 0;
    for (int i = 0; i < p; ++i) {
        size_t ndc = (size_t)tc.pl[i].nb * tc.pl[i].nb;
        off += ndc + ndc * 64;
    }
    return off;
}

// warp_dc(): three signed bytes -- dc, gx, gy -- for each of the three coded
// planes of a near-skip tile, in plane order.  A near-skip tile is a skipped
// tile, so its alpha is the derived constant and is not corrected.
constexpr int kWarpDcPlanes = 3;
constexpr size_t kWarpDcBytes = 3u * kWarpDcPlanes;

// mv_quad(): four i8 pairs after the tile vector (docs/SYNTAX.md 4.1, 13.8).
constexpr size_t kMvQuadBytes = 8;

// Number of coded planes in this tile (alpha only when alpha_mode == 2).
static int coded_planes(const TileCoder &tc) {
    int np = tc.nplanes;
    if (tc.tp.alpha_mode != 2) np = np > 3 ? 3 : np;
    return np;
}

// Full (stream-resolution) tile extent of plane p.
static int plane_full_extent(const Geometry &g, int p, int chroma444) {
    if (p == 1 || p == 2) {
        if (g.chroma == NXVC_CHROMA_420) return 32;
        return chroma444 ? 64 : 64;  // 444 source is always 64 wide per tile
    }
    return 64;
}

// Origin of the tile in plane p, in that plane's own EYE-LOCAL coordinates.
static void plane_tile_origin(const Geometry &g, int p, int tx, int ty, int &ox,
                              int &oy) {
    if ((p == 1 || p == 2) && g.chroma == NXVC_CHROMA_420) {
        ox = tx * 32;
        oy = ty * 32;
    } else {
        ox = tx * 64;
        oy = ty * 64;
    }
}

// ------------------------------------------------------------ tile indexing
// Annex D D-3.  cols = eyes * cols_per_eye; the linear index is
// row * cols + eye * cols_per_eye + tile_index, and a frame's tile rows are
// ordered row-major, eye-minor.
static inline int linear_tile(const Geometry &g, int row, int eye, int col) {
    return (int)(row * g.eyes * g.tiles_x + eye * g.tiles_x + col);
}

// ------------------------------------------------------------ reference ring
static void ref_alloc(RefPicture &rp, const Geometry &g) {
    for (int p = 0; p < 4; ++p) {
        int w = (int)(g.pw(p) * g.eyes), h = (int)g.ph(p);
        rp.w[p] = w;
        rp.h[p] = h;
        rp.maxval[p] = g.maxval(p);
        if (p < g.nplanes()) rp.plane[p].assign((size_t)w * h, 0);
        else rp.plane[p].clear();
    }
}

// A RefImage over one eye's sub-picture of one plane of a stored frame.
static nw::RefImage ref_image(const RefPicture &rp, const Geometry &g, int p,
                              int eye) {
    nw::RefImage im{};
    im.data = rp.plane[p].data() + (size_t)eye * g.pw(p);
    im.width = (int)g.pw(p);
    im.height = (int)g.ph(p);
    im.stride = rp.w[p];
    im.channels = 1;
    im.max_value = rp.maxval[p];
    return im;
}

}  // namespace nxvc

// ================================================================= encoder
extern "C" nxvc_encoder *nxvc_encoder_create(const nxvc_config *cfg,
                                             nxvc_status *st) {
    auto fail = [&](nxvc_status s) -> nxvc_encoder * {
        if (st) *st = s;
        return nullptr;
    };
    if (!cfg) return fail(NXVC_ERR_ARG);
    if (cfg->width < 16 || cfg->height < 16 || cfg->width > 4096 ||
        cfg->height > 4096 || (cfg->width & 1) || (cfg->height & 1))
        return fail(NXVC_ERR_ARG);
    if (cfg->bit_depth != 8) return fail(NXVC_ERR_UNSUPPORTED);
    const u32 eyes = cfg->eyes == 0 ? 1u : cfg->eyes;
    if (eyes > 2) return fail(NXVC_ERR_UNSUPPORTED);
    if (cfg->ref_sel > 2) return fail(NXVC_ERR_ARG);
    if (cfg->stereo && (!cfg->inter || eyes != 2)) return fail(NXVC_ERR_ARG);
    if (cfg->chroma > 1 || cfg->color_transform > 1) return fail(NXVC_ERR_ARG);
    if (cfg->base_qp > 63) return fail(NXVC_ERR_ARG);
    if (cfg->color_transform == NXVC_CT_YCOCGR && cfg->chroma != NXVC_CHROMA_444)
        return fail(NXVC_ERR_ARG);
    if (cfg->color_space > 3) return fail(NXVC_ERR_ARG);
    if ((cfg->color_space == NXVC_CS_RGB) !=
        (cfg->color_transform == NXVC_CT_YCOCGR))
        return fail(NXVC_ERR_ARG);
    nxvc_encoder *e = nullptr;
    try {
        e = new nxvc_encoder();
    } catch (...) {
        return fail(NXVC_ERR_NOMEM);
    }
    e->cfg = *cfg;
    if (e->cfg.lossless) {
        e->cfg.base_qp = 0;
        e->cfg.transform_skip = 1;
        e->cfg.quant_matrix = 0;
        // Sign data hiding spends one level step to carry the sign, so it is
        // lossy by construction and cannot coexist with lossless coding.
        e->cfg.sign_hide = 0;
    }
    e->g.width = cfg->width;
    e->g.height = cfg->height;
    e->g.eyes = eyes;
    e->g.chroma = cfg->chroma;
    e->g.color_transform = cfg->color_transform;
    e->g.color_space = cfg->color_space;
    e->g.alpha = cfg->alpha ? 1 : 0;
    derive_geometry(e->g);
    if (e->g.tiles_x > kMaxTilesPerRow) {
        delete e;
        return fail(NXVC_ERR_ARG);
    }
    e->fp.base_qp = (int)e->cfg.base_qp;
    e->fp.chroma_qp_off = e->cfg.chroma_qp_off;
    e->fp.alpha_qp_off = e->cfg.alpha_qp_off;
    e->fp.quant_matrix = (int)e->cfg.quant_matrix;
    if (e->cfg.quant_matrix == 255) {
        if (!e->cfg.custom_matrix) {
            delete e;
            return fail(NXVC_ERR_ARG);
        }
        e->custom_matrix.assign(e->cfg.custom_matrix, e->cfg.custom_matrix + 128);
    }
    resolve_matrices(e->fp, e->custom_matrix.empty() ? nullptr
                                                     : e->custom_matrix.data());
    e->fp.nctx = e->cfg.ctx_v2 ? kNumCtxV2 : kNumCtxV1;
    e->fp.intra_dir = e->cfg.intra_dir ? 1 : 0;
    e->fp.dir_layer = (e->cfg.intra_dir && e->cfg.intra_dir_layer) ? 1 : 0;
    e->fp.sdh = e->cfg.sign_hide ? 1 : 0;
    e->cfg.eyes = eyes;
    e->fp.inter = e->cfg.inter ? 1 : 0;
    e->fp.stereo = e->cfg.stereo ? 1 : 0;
    e->fp.warp_dc = (e->cfg.inter && e->cfg.warp_dc) ? 1 : 0;
    e->fp.mv_quad = (e->cfg.inter && e->cfg.mv_quad) ? 1 : 0;
    for (int i = 0; i < 8; ++i) build_default_set(e->fp.tabs[i], i, e->fp.nctx);
    e->state.assign(e->g.ntiles(), nxvc::PredState{});
    e->state_pre = e->state;
    e->dec.assign(e->g.ntiles(), TileDecision{});
    e->age_since_coded.assign(e->g.ntiles(), 0);
    e->age_since_intra.assign(e->g.ntiles(), 0);
    e->drift.assign(e->g.ntiles(), 0.0f);
    e->views_cur.assign(eyes, nxvc_view{0, 0, 0, 1, -0.8, 0.8, 0.8, -0.8});
    for (int i = 0; i < 4; ++i) e->views_slot[i] = e->views_cur;
    for (int i = 0; i < 4; ++i) ref_alloc(e->ring.slot[i], e->g);
    for (int p = 0; p < 4; ++p) {
        size_t w = (size_t)e->g.pw(p) * eyes, h = e->g.ph(p);
        if (p < e->g.nplanes()) {
            e->shadow_plane[p].assign(w * h, p == 3 ? 255 : 0);
            e->shadow_img.plane[p] = e->shadow_plane[p].data();
            e->shadow_img.stride[p] = (int)w;
        }
    }
    if (st) *st = NXVC_OK;
    return e;
}

extern "C" void nxvc_encoder_destroy(nxvc_encoder *e) { delete e; }

extern "C" void nxvc_encoder_set_pose(nxvc_encoder *e, const uint8_t pose[26]) {
    if (e && pose) std::memcpy(e->pose, pose, 26);
}

extern "C" nxvc_status nxvc_encoder_add_tlv(nxvc_encoder *e, uint16_t type,
                                            const uint8_t *data, uint16_t len) {
    if (!e || (len && !data)) return NXVC_ERR_ARG;
    size_t pad = (4 - (len & 3)) & 3;
    if (e->tlv.size() + 4 + len + pad > 65535) return NXVC_ERR_NOMEM;
    e->tlv.push_back((u8)type);
    e->tlv.push_back((u8)(type >> 8));
    e->tlv.push_back((u8)len);
    e->tlv.push_back((u8)(len >> 8));
    e->tlv.insert(e->tlv.end(), data, data + len);
    for (size_t i = 0; i < pad; ++i) e->tlv.push_back(0);
    return NXVC_OK;
}

static u64 encoder_tools(const nxvc_encoder *e) {
    u64 t = NXVC_TOOL_INTRA_DC_PLANE;
    if (e->cfg.transform_skip || e->cfg.lossless) t |= NXVC_TOOL_TRANSFORM_SKIP;
    if (e->cfg.lossless) t |= NXVC_TOOL_LOSSLESS;
    if (e->cfg.chroma == NXVC_CHROMA_444) t |= NXVC_TOOL_CHROMA444;
    if (e->cfg.alpha) t |= NXVC_TOOL_ALPHA;
    if (e->cfg.custom_tables) t |= NXVC_TOOL_CUSTOM_TABLES;
    if (e->cfg.nsub_log2 != 3) t |= NXVC_TOOL_NSUB_VAR;
    if (e->cfg.tile_chroma420) t |= NXVC_TOOL_PER_TILE_CHROMA;
    if (e->cfg.color_transform == NXVC_CT_YCOCGR) t |= NXVC_TOOL_YCOCGR;
    if (e->cfg.wm_id != 0 && e->cfg.quant_matrix != 255 && !e->cfg.lossless)
        t |= NXVC_TOOL_WM_ID;
    if (e->cfg.intra_dir) t |= NXVC_TOOL_INTRA_DIR;
    if (e->cfg.ctx_v2) t |= NXVC_TOOL_CTX_V2;
    if (e->cfg.sign_hide) t |= NXVC_TOOL_SIGN_HIDE;
    if (e->cfg.inter) t |= NXVC_TOOL_INTER | NXVC_TOOL_WARP;
    if (e->cfg.stereo) t |= NXVC_TOOL_STEREO;
    if (e->cfg.inter && e->cfg.warp_dc) t |= NXVC_TOOL_WARP_DC;
    if (e->cfg.inter && e->cfg.mv_quad) t |= NXVC_TOOL_MV_QUAD;
    t |= NXVC_TOOL_RES_LEVEL;
    return t;
}

extern "C" nxvc_status nxvc_encoder_stream_header(nxvc_encoder *e, uint8_t *buf,
                                                  size_t cap, size_t *out_len) {
    if (!e || !buf || !out_len) return NXVC_ERR_ARG;
    BW bw;
    bw.u32v(kMagic);
    bw.u8v(NXVC_VERSION);
    bw.u8v(e->cfg.profile);
    bw.u8v(e->cfg.level);
    bw.u8v(0);  // tile_size: 0 = 64x64
    bw.u16v(e->g.width);
    bw.u16v(e->g.height);
    bw.u8v((u8)e->g.eyes);
    bw.u8v(8);  // bit_depth
    bw.u8v(1);  // num_layers
    bw.u8v(e->g.chroma);
    for (int i = 0; i < 4; ++i) bw.u32v(i == 0 ? 0u : 0u);  // layer_desc
    bw.u64v(encoder_tools(e));
    bw.u8v(e->g.alpha);
    bw.u8v(e->g.color_transform);
    bw.u8v(e->g.color_space);
    while (bw.size() < 62) bw.u8v(0);
    bw.u16v((u32)e->tlv.size());
    bw.raw(e->tlv.data(), e->tlv.size());
    if (bw.size() > cap) return NXVC_ERR_NOMEM;
    std::memcpy(buf, bw.b.data(), bw.size());
    *out_len = bw.size();
    return NXVC_OK;
}

namespace nxvc {

// Fill the tile coder's plane sample buffers from the source image.
static void load_tile(TileCoder &tc, const Geometry &g, const nxvc_image *img,
                      int eye, int tx, int ty) {
    int np = tc.nplanes;
    PlaneRef ref[4];
    for (int p = 0; p < np; ++p) {
        // The eye's sub-picture, so fetch_clamped clamps at the eye border and
        // a left-eye tile can never read a right-eye sample.
        ref[p].p = img->plane[p] + (size_t)eye * g.pw(p);
        ref[p].stride = img->stride[p];
        ref[p].width = (int)g.pw(p);
        ref[p].height = (int)g.ph(p);
    }
    const bool ct = (g.color_transform == NXVC_CT_YCOCGR);
    for (int p = 0; p < np; ++p) {
        PlaneState &s = tc.pl[p];
        int full = plane_full_extent(g, p, tc.tp.chroma444);
        int ox, oy;
        plane_tile_origin(g, p, tx, ty, ox, oy);
        int factor = full / s.size;
        for (int y = 0; y < s.size; ++y)
            for (int x = 0; x < s.size; ++x) {
                i32 sum = 0;
                for (int j = 0; j < factor; ++j)
                    for (int i = 0; i < factor; ++i) {
                        int gx = ox + x * factor + i, gy = oy + y * factor + j;
                        if (ct && p < 3) {
                            i32 R = fetch_clamped(ref[0], gx, gy);
                            i32 G = fetch_clamped(ref[1], gx, gy);
                            i32 B = fetch_clamped(ref[2], gx, gy);
                            i32 Co = R - B;
                            i32 t = B + (Co >> 1);
                            i32 Cg = G - t;
                            i32 Y = t + (Cg >> 1);
                            sum += p == 0 ? Y : (p == 1 ? Co + 256 : Cg + 256);
                        } else {
                            sum += fetch_clamped(ref[p], gx, gy);
                        }
                    }
                i32 n = factor * factor;
                s.samples[(size_t)y * s.size + x] = (sum + n / 2) / n;
            }
    }
}

// Write the reconstructed tile back into the output image.
static inline i32 upsampled(const PlaneState &s, int factor, int x, int y) {
    if (factor == 1) return s.samples[(size_t)y * s.size + x];
    i32 mul = 16 / factor, off = mul / 2 - 8;
    return bilinear_q4_i32(s.samples.data(), s.size, s.size, s.size,
                           mul * x + off, mul * y + off);
}

static void store_tile(TileCoder &tc, const Geometry &g, nxvc_image *img,
                       int eye, int tx, int ty) {
    int np = tc.nplanes;
    const bool ct = (g.color_transform == NXVC_CT_YCOCGR);
    const int eoff0 = (int)(eye * g.width);
    if (ct) {
        // planes 0..2 are converted back to RGB after upsampling
        int ox = tx * 64 + eoff0, oy = ty * 64;
        int f0 = 64 / tc.pl[0].size;
        int f1 = 64 / tc.pl[1].size;
        for (int y = 0; y < 64; ++y) {
            int gy = oy + y;
            if (gy >= (int)g.height) break;
            for (int x = 0; x < 64; ++x) {
                int gx = ox + x;
                if (gx >= (int)(g.width * (eye + 1))) break;
                i32 Y = upsampled(tc.pl[0], f0, x, y);
                i32 Co = upsampled(tc.pl[1], f1, x, y) - 256;
                i32 Cg = upsampled(tc.pl[2], f1, x, y) - 256;
                i32 t = Y - (Cg >> 1);
                i32 G = Cg + t;
                i32 B = t - (Co >> 1);
                i32 R = B + Co;
                img->plane[0][(size_t)gy * img->stride[0] + gx] =
                    (u8)clamp_i32(R, 0, 255);
                img->plane[1][(size_t)gy * img->stride[1] + gx] =
                    (u8)clamp_i32(G, 0, 255);
                img->plane[2][(size_t)gy * img->stride[2] + gx] =
                    (u8)clamp_i32(B, 0, 255);
            }
        }
        np = tc.nplanes > 3 ? 4 : 3;
        if (np < 4) return;
    }
    for (int p = ct ? 3 : 0; p < np; ++p) {
        PlaneState &s = tc.pl[p];
        int full = plane_full_extent(g, p, tc.tp.chroma444);
        int ox, oy;
        plane_tile_origin(g, p, tx, ty, ox, oy);
        int pw = (int)g.pw(p);
        int ph = (int)g.ph(p);
        ox += (int)(eye * g.pw(p));
        pw += (int)(eye * g.pw(p));
        int factor = full / s.size;
        u8 *dst = img->plane[p];
        int stride = img->stride[p];
        bool constant_alpha = (p == 3 && tc.tp.alpha_mode != 2);
        i32 cval = tc.tp.alpha_mode == 1 ? tc.tp.alpha_value : 255;
        for (int y = 0; y < full; ++y) {
            int gy = oy + y;
            if (gy >= ph) break;
            for (int x = 0; x < full; ++x) {
                int gx = ox + x;
                if (gx >= pw) break;
                i32 v = constant_alpha ? cval : upsampled(s, factor, x, y);
                dst[(size_t)gy * stride + gx] = (u8)clamp_i32(v, 0, 255);
            }
        }
    }
}

// Write the tile's reconstruction into a reference-ring slot, in the CODED
// sample domain (Y/Co/Cg, never RGB) and at full tile extent, which is the
// domain and the resolution the predictor reads.  Encoder and decoder call
// this with the same `tc`, so the ring contents are identical by construction.
static void store_ref_tile(TileCoder &tc, const Geometry &g, RefPicture &rp,
                           int eye, int tx, int ty) {
    int np = tc.nplanes;
    for (int p = 0; p < np; ++p) {
        PlaneState &s = tc.pl[p];
        int full = plane_full_extent(g, p, tc.tp.chroma444);
        int ox, oy;
        plane_tile_origin(g, p, tx, ty, ox, oy);
        ox += (int)(eye * g.pw(p));
        const int pwe = (int)(g.pw(p) * (eye + 1));
        const int phe = (int)g.ph(p);
        int factor = full / s.size;
        const bool constant_alpha = (p == 3 && tc.tp.alpha_mode != 2);
        const i32 cval = tc.tp.alpha_mode == 1 ? tc.tp.alpha_value : 255;
        u16 *dst = rp.plane[p].data();
        const int stride = rp.w[p];
        for (int y = 0; y < full; ++y) {
            int gy = oy + y;
            if (gy >= phe) break;
            for (int x = 0; x < full; ++x) {
                int gx = ox + x;
                if (gx >= pwe) break;
                i32 v = constant_alpha ? cval : upsampled(s, factor, x, y);
                dst[(size_t)gy * stride + gx] =
                    (u16)clamp_i32(v, 0, g.maxval(p));
            }
        }
    }
}

// Fill every coded plane's `wpred` with the inter predictor of this tile.
//
// `src` is the reference picture, `H` the frame's matrix for this eye (ignored
// by STATIC_MV and STEREO, which are the identity predictor), and `mv` the
// tile vector in quarter LUMA samples.  A plane subsampled by 2 gets the
// conjugated matrix and the halved vector (inter.h).  A res_level > 0 tile
// predicts at full extent and box-downsamples to the coded extent with the
// same kernel load_tile uses on the source, so the residual is measured in one
// domain throughout.
static void predict_tile(TileCoder &tc, const Geometry &g, const RefPicture &rp,
                         const WarpMatrix &wm, int eye, int tx, int ty,
                         int mode, int mv_x, int mv_y, int np_limit = 0,
                         const int (*dmv)[2] = nullptr) {
    const nw::Mode wmode =
        mode_needs_warp(mode) ? nw::kModeWarp : nw::kModeStatic;
    const int np = np_limit > 0 ? np_limit : tc.nplanes;
    // The tile vector, broadcast to the four quadrants, plus the quadrant
    // deltas of `mv_quad` when the tile carries them (docs/SYNTAX.md 13.8).
    i32 mvq[4][2];
    mv_broadcast(mv_x, mv_y, mvq);
    if (dmv)
        for (int q = 0; q < 4; ++q) {
            mvq[q][0] += dmv[q][0];
            mvq[q][1] += dmv[q][1];
        }
    static thread_local std::vector<i32> full_buf;
    for (int p = 0; p < np; ++p) {
        PlaneState &s = tc.pl[p];
        if (s.wpred.empty()) s.wpred.assign((size_t)s.size * s.size, 0);
        const int sub = g.psub(p);
        const int full = plane_full_extent(g, p, tc.tp.chroma444);
        int ox, oy;
        plane_tile_origin(g, p, tx, ty, ox, oy);
        nw::RefImage im = ref_image(rp, g, p, eye);
        nw::Homography H = plane_homography(wm, (int)g.pw(p), (int)g.ph(p), sub);
        i32 mv[4][2];
        for (int q = 0; q < 4; ++q) {
            mv[q][0] = plane_mv(mvq[q][0], sub);
            mv[q][1] = plane_mv(mvq[q][1], sub);
        }
        full_buf.assign((size_t)full * full, 0);
        warp_plane_tile(im, ox, oy, H, mv, wmode, full, full_buf.data());
        const int factor = full / s.size;
        if (factor == 1) {
            for (size_t i = 0; i < s.wpred.size(); ++i) s.wpred[i] = full_buf[i];
        } else {
            const i32 n = factor * factor;
            for (int y = 0; y < s.size; ++y)
                for (int x = 0; x < s.size; ++x) {
                    i32 sum = 0;
                    for (int j = 0; j < factor; ++j)
                        for (int i = 0; i < factor; ++i)
                            sum += full_buf[(size_t)(y * factor + j) * full +
                                            x * factor + i];
                    s.wpred[(size_t)y * s.size + x] = (sum + n / 2) / n;
                }
        }
    }
}

// WARP_SKIP and concealment: the prediction is the whole reconstruction.
// With `dc` (tool bit 24, docs/SYNTAX.md 13.9) the tile is a *near*-skip: the
// same predictor plus one low-frequency correction per plane, before the clamp.
static void reconstruct_skip(TileCoder &tc, const DcCorrection *dc = nullptr) {
    int np = tc.nplanes;
    for (int p = 0; p < np; ++p) {
        PlaneState &s = tc.pl[p];
        if (!dc) {
            for (size_t i = 0; i < s.samples.size(); ++i)
                s.samples[i] = clamp_i32(s.wpred[i], 0, s.maxval);
            continue;
        }
        const int shift = dc_corr_shift(s.size);
        for (int y = 0; y < s.size; ++y)
            for (int x = 0; x < s.size; ++x) {
                const size_t i = (size_t)y * s.size + x;
                s.samples[i] =
                    clamp_i32(s.wpred[i] + dc_corr_at(*dc, p, s.size, shift, x, y),
                              0, s.maxval);
            }
    }
}

// Squared error of the reconstruction this tile's coefficients will produce,
// summed over every coded plane in the *coded* domain (before res_level or
// chroma upsampling).  The encoder runs the real decoder to get it, so the
// number is exact for what the tile codes; for a res_level > 0 tile it is the
// error of the small image, not of the upsampled one, which is the right
// quantity to compare two candidates for the same tile against.
static double tile_distortion(TileCoder &tc) {
    double sse = 0;
    int np = coded_planes(tc);
    std::vector<i32> src;
    for (int p = 0; p < np; ++p) {
        PlaneState &s = tc.pl[p];
        src = s.samples;
        reconstruct_plane(s, &tc.coef[plane_coef_offset(tc, p)], tc.tp.tskip,
                          tc.intra_dir, tc.dir_layer);
        for (size_t i = 0; i < src.size(); ++i) {
            double d = (double)src[i] - (double)s.samples[i];
            sse += d * d;
        }
        s.samples.swap(src);
    }
    return sse;
}

// Cheap transform-skip decision: prefer transform skip when the residual is
// dominated by isolated large jumps (synthetic / text-like content).
static int tskip_decision(const TileCoder &tc) {
    const PlaneState &s = tc.pl[0];
    i64 flat = 0, grad = 0;
    for (int y = 0; y < s.size; ++y)
        for (int x = 1; x < s.size; ++x) {
            i32 d = s.samples[(size_t)y * s.size + x] -
                    s.samples[(size_t)y * s.size + x - 1];
            grad += d < 0 ? -d : d;
            flat += d != 0;
        }
    // High average step over few changed positions -> blocky graphics.
    if (flat == 0) return 0;
    return (grad / (flat + 1)) > 24 ? 1 : 0;
}

// --------------------------------------------------- inter mode decision
// Everything from here to the end of the namespace is ENCODER SIDE and
// non-normative: it chooses a mode and a vector, never how one decodes.
static inline i32 ref_at(const nw::RefImage &im, int x, int y) {
    x = clamp_i32(x, 0, im.width - 1);
    y = clamp_i32(y, 0, im.height - 1);
    return (i32)im.data[(size_t)y * im.stride + x];
}

// The coarse stage searches a pure translation of the reference, because for
// an integer vector the warp *is* a translation of the resampled reference:
// the tile's fractional sampling phase does not change with an integer mv, so
// the search only has to find the right integer offset and the exact kernel
// then decides between its neighbours.  `d0` is the warp's own integer
// displacement, read out of the library's corner function so the search starts
// where the warp already put the tile.
struct SearchCtx {
    const i32 *src = nullptr;
    int size = 0;
    nw::RefImage im{};
    int ox = 0, oy = 0;
    int d0x = 0, d0y = 0;
};

static i64 sad_translate(const SearchCtx &c, int dx, int dy, int step) {
    i64 sad = 0;
    for (int v = 0; v < c.size; v += step)
        for (int u = 0; u < c.size; u += step) {
            i32 a = c.src[(size_t)v * c.size + u];
            i32 b = ref_at(c.im, c.ox + c.d0x + dx + u, c.oy + c.d0y + dy + v);
            sad += a > b ? a - b : b - a;
        }
    return sad;
}

static i64 sad_pred(const PlaneState &s) {
    i64 sad = 0;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        i32 d = s.samples[i] - clamp_i32(s.wpred[i], 0, s.maxval);
        sad += d < 0 ? -d : d;
    }
    return sad;
}

// Squared error of a predictor against the source.  The skip gate is stated on
// this and not on SAD: warp error is impulsive -- a few samples on a hard edge
// carry most of it -- so two tiles with the same SAD can differ by 9 dB, and
// gating on SAD skipped tiles at 41 dB while their SAD said 0.2 LSB.  The
// measurement is in ref/RESULTS-inter.md.
static double sse_pred(const PlaneState &s) {
    double sse = 0;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        double d = (double)s.samples[i] -
                   (double)clamp_i32(s.wpred[i], 0, s.maxval);
        sse += d * d;
    }
    return sse;
}

// ------------------------------------------------------------- warp_dc()
// Fit the low-frequency correction of docs/SYNTAX.md 13.9 to a tile whose
// `wpred` already holds the WARP_SKIP predictor.
//
// The three basis functions -- 1, the horizontal ramp and the vertical ramp --
// are orthogonal over the tile's sample grid (both ramps are zero-mean and
// separable), so the least-squares fit is three independent projections and
// needs no matrix.  Each coefficient is then rounded to the signed byte it
// travels as, which is why the fit is done in double here and the RESULT is
// scored through the normative integer expression: the encoder never assumes
// its own arithmetic is what the decoder will do.
static void fit_dc_correction(const TileCoder &tc, DcCorrection &dc) {
    for (int p = 0; p < kWarpDcPlanes && p < tc.nplanes; ++p) {
        const PlaneState &s = tc.pl[p];
        const int E = s.size;
        const double scale = 1.0 / (2.0 * E);
        double s0 = 0, sa = 0, sb = 0, aa = 0;
        for (int y = 0; y < E; ++y)
            for (int x = 0; x < E; ++x) {
                const double r = (double)s.samples[(size_t)y * E + x] -
                                 (double)clamp_i32(s.wpred[(size_t)y * E + x], 0,
                                                   s.maxval);
                const double a = (2 * x + 1 - E) * scale;
                const double b = (2 * y + 1 - E) * scale;
                s0 += r;
                sa += r * a;
                sb += r * b;
                aa += a * a;
            }
        const double n = (double)E * E;
        auto q = [](double v) {
            return (i8)clamp_i32((i32)std::lround(v), -128, 127);
        };
        dc.dc[p] = q(s0 / n);
        dc.gx[p] = aa > 0 ? q(sa / aa) : (i8)0;
        dc.gy[p] = aa > 0 ? q(sb / aa) : (i8)0;
    }
}

// Squared error of the near-skip reconstruction, through the same integer
// expression the decoder evaluates.
static double sse_near_skip(const TileCoder &tc, const DcCorrection &dc) {
    double sse = 0;
    for (int p = 0; p < tc.nplanes; ++p) {
        const PlaneState &s = tc.pl[p];
        const int shift = dc_corr_shift(s.size);
        for (int y = 0; y < s.size; ++y)
            for (int x = 0; x < s.size; ++x) {
                const size_t i = (size_t)y * s.size + x;
                const i32 rec = clamp_i32(
                    s.wpred[i] + (p < kWarpDcPlanes
                                      ? dc_corr_at(dc, p, s.size, shift, x, y)
                                      : 0),
                    0, s.maxval);
                const double d = (double)s.samples[i] - (double)rec;
                sse += d * d;
            }
    }
    return sse;
}

// Reconstruct every coded plane from the coefficients already chosen, leaving
// the decoder's samples in the tile so they can go into the reference ring.
static void finalize_recon(TileCoder &tc) {
    int np = coded_planes(tc);
    for (int p = 0; p < np; ++p)
        reconstruct_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                          tc.tp.tskip, tc.intra_dir, tc.dir_layer);
}

// Per-tile mean squared error between the client shadow and the source, in
// the OUTPUT sample domain over every plane the stream carries.  This is the
// error the client is actually showing for this tile, measured rather than
// modelled: `shadow_plane` was written by the same store_tile() the decoder
// runs, and (under no loss, or with nxvc_encoder_set_received_tiles under
// loss) it is bit-exact.  It is what the drift refresh rule of decide_tile
// reads on the NEXT frame.
static void measure_drift(nxvc_encoder *e, const nxvc_image *img) {
    const Geometry &g = e->g;
    const int np = g.nplanes();
    for (u32 row = 0; row < g.tiles_y; ++row)
        for (u32 eye = 0; eye < g.eyes; ++eye)
            for (u32 col = 0; col < g.tiles_x; ++col) {
                double sse = 0;
                i64 n = 0;
                for (int p = 0; p < np; ++p) {
                    const int pw = (int)g.pw(p), ph = (int)g.ph(p);
                    int ox, oy;
                    plane_tile_origin(g, p, (int)col, (int)row, ox, oy);
                    const int full = plane_full_extent(g, p, 1);
                    ox += (int)(eye * pw);
                    const u8 *sh = e->shadow_plane[p].data();
                    const int sstride = (int)(pw * g.eyes);
                    for (int y = 0; y < full; ++y) {
                        const int gy = oy + y;
                        if (gy >= ph) break;
                        for (int x = 0; x < full; ++x) {
                            const int gx = ox + x;
                            if (gx >= (int)(pw * (eye + 1))) break;
                            const double dd =
                                (double)img->plane[p][(size_t)gy *
                                                          img->stride[p] + gx] -
                                (double)sh[(size_t)gy * sstride + gx];
                            sse += dd * dd;
                            ++n;
                        }
                    }
                }
                e->drift[linear_tile(g, (int)row, (int)eye, (int)col)] =
                    (float)(n ? sse / (double)n : 0.0);
            }
}

// Rolling intra refresh (PAPER 2.6, Annex D), written as an AGE with a
// starting phase rather than as a global frame counter, so that the drift rule
// of docs/SYNTAX.md 13.10 can drive the same state.
//
// The phase is a fixed pseudo-random permutation of the tile index rather than
// a column sweep, so there is no visible refresh wave: with the drift rule off,
// 1/T of the tiles are forced INTRA every frame and each tile is refreshed
// exactly every T frames, which is the cadence and the permutation this
// replaces.  The age counts the tile's INTRA frame itself, which is what makes
// "every T frames" mean T and not T + 1, and `intra_period == 1` mean every
// frame.
static inline u32 refresh_phase(u32 tile, u32 period) {
    if (period <= 1) return 0;
    return ((tile * 2654435761u) >> 8) % period;
}

}  // namespace nxvc

extern "C" nxvc_status nxvc_encoder_encode_frame(
    nxvc_encoder *e, const nxvc_image *img, const uint8_t *qp_map,
    const uint8_t *res_map, uint8_t *buf, size_t cap, size_t *out_len) {
    if (!e || !img || !buf || !out_len) return NXVC_ERR_ARG;
    const Geometry &g = e->g;
    const int ntiles = (int)g.ntiles();
    for (int p = 0; p < g.nplanes(); ++p)
        if (!img->plane[p]) return NXVC_ERR_ARG;

    e->tiles.assign(ntiles, nxvc_tile_info{});
    std::memset(&e->stats, 0, sizeof(e->stats));

    FrameParams fp = e->fp;
    fp.frame_number = (u16)e->frame_number;
    std::memcpy(fp.pose, e->pose, 26);
    // Annex D D-10: the field is a bitmask and in version 1 it must be
    // 1 << (frame_number mod 4), which is the ring slot the frame overwrites.
    //
    // It is written, and enforced on parse, only for a stream that declares
    // the INTER tool.  A stream with no inter tools has no reference ring for
    // the field to describe, and every syntax v1.1-v1.3 stream ever produced
    // carries 0 there; making those retroactively malformed would buy nothing
    // and break every committed conformance vector.  See docs/SYNTAX.md 3.1.
    const int slot = (int)(fp.frame_number & 3);
    fp.ref_slots = e->cfg.inter ? (1u << slot) : 0u;

    // ---- reference availability.  ref_sel = d selects frame N-1-d.
    const int want_d = e->cfg.inter ? (int)e->cfg.ref_sel : 0;
    const int ref_slot = (int)((fp.frame_number - 1 - want_d) & 3u);
    const u16 ref_fn = (u16)(fp.frame_number - 1 - want_d);
    const bool have_ref = e->cfg.inter && e->ring.slot[ref_slot].valid &&
                          e->ring.slot[ref_slot].frame_number == ref_fn &&
                          e->frame_number >= (u32)(1 + want_d);
    const RefPicture &refpic = e->ring.slot[ref_slot];

    const u32 intra_period = e->cfg.intra_period ? e->cfg.intra_period : 180u;
    // The hard cap defaults to the eligibility period, which makes the two
    // thresholds coincide and the rule exactly the fixed cadence.
    const u32 max_age = e->cfg.refresh_max_age > intra_period
                            ? e->cfg.refresh_max_age
                            : intra_period;
    fp.flags = have_ref ? 0u : 1u;   // bit 0: tile-map reset
    if (fp.dir_layer) fp.flags |= 4;
    if (!have_ref) {
        for (auto &st : e->state) st = nxvc::PredState{};
        for (int t = 0; t < ntiles; ++t) e->drift[t] = 0.0f;
    }
    e->state_pre = e->state;

    // ---- warp_ext(): one matrix per eye, derived from the views of the
    // reference frame and of this frame.  A failure of derive_homography (an
    // entry outside its format, or a denominator that leaves [2^28, 2^30)
    // somewhere in the picture) leaves the identity, which is always legal.
    if (have_ref) {
        fp.warp_present = 1;
        fp.flags |= 8;   // bit 3
        for (u32 eye = 0; eye < g.eyes; ++eye) {
            nw::Homography H{};
            const nxvc_view &a = e->views_slot[ref_slot][eye],
                            &b = e->views_cur[eye];
            nw::Quat qa{a.qx, a.qy, a.qz, a.qw}, qb{b.qx, b.qy, b.qz, b.qw};
            nw::Fov fa{a.fov_left, a.fov_right, a.fov_up, a.fov_down};
            nw::Fov fb{b.fov_left, b.fov_right, b.fov_up, b.fov_down};
            if (!nw::derive_homography(qa, fa, qb, fb, (i32)g.width,
                                       (i32)g.height, &H))
                H = nw::identity_homography((i32)g.width / 2, (i32)g.height / 2);
            for (int i = 0; i < 9; ++i) fp.warp[eye].h[i] = H.h[i];
            fp.warp[eye].h[8] = kH22;
        }
    }

    std::vector<u8> table_bytes;
    static thread_local TableSet deftabs[8];
    for (int k = 0; k < 8; ++k) build_default_set(deftabs[k], k, fp.nctx);
    auto select_set = [&](const u32 th[kNumCtx][kNumSym], int fallback) {
        double best = 0;
        int bestk = fallback;
        for (int k = 0; k < 8; ++k) {
            double c = table_set_cost(th, deftabs[k]);
            if (k == 0 || c < best) { best = c; bestk = k; }
        }
        return bestk;
    };

    // Lambda for the RD trellis, as a multiple of the tile's squared quantizer
    // step.  0.30 was measured on the quality harness (tools/quality) over the
    // 100-400 Mbit band; below 0.15 the trellis barely fires, above 0.6 it
    // starts zeroing structure the eye sees.
    const double rdo_lambda_scale =
        e->cfg.rdo_lambda_q8 ? (double)e->cfg.rdo_lambda_q8 / 256.0 : 0.30;
    const bool use_rdo = e->cfg.rdo != 0 && !e->cfg.lossless;
    const int qp_search = e->cfg.lossless ? 0 : (int)(e->cfg.qp_search > 8
                                                          ? 8u : e->cfg.qp_search);
    const bool wm_search = e->cfg.wm_id == 255 && fp.quant_matrix != 255 &&
                           !e->cfg.lossless;
    const int dir_cand = e->cfg.intra_dir_cand ? (int)(e->cfg.intra_dir_cand > 8
                                                          ? 8u
                                                          : e->cfg.intra_dir_cand)
                                                : 2;
    const int mv_range = (int)(e->cfg.mv_range ? e->cfg.mv_range : 16u);
    // Drift-driven refresh: the threshold is a Q8 multiple of the quantiser's
    // own noise floor per sample, so it is scale-free across QP exactly as the
    // skip gate is.  Zero turns the rule off, which leaves `intra_period` as
    // the fixed cadence it was.
    const u32 refresh_drift_q8 = e->cfg.refresh_drift_q8;
    const double refresh_drift_thresh = (double)refresh_drift_q8 / 256.0;

    auto rdoq_tile = [&](TileCoder &tc, const TableSet *tabs) {
        if (!use_rdo) return;
        RateCost rc;
        build_rate_cost(tabs[tc.tp.table_set], rc);
        int np = coded_planes(tc);
        for (int p = 0; p < np; ++p)
            rdoq_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)], tc.tp.tskip,
                       p == 1 || p == 2, rc, rdo_lambda_scale, tc.sdh);
    };

    auto analyze_tile_dir = [&](TileCoder &tc, const TableSet *tabs, int pass) {
        RateCost rc;
        build_rate_cost(tabs[tc.tp.table_set], rc);
        int np = coded_planes(tc);
        int mode_ctx = tc.nctx >= kNumCtxV2 ? kCtxMode : 0;
        for (int p = 0; p < np; ++p)
            analyze_plane_dir(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                              tc.tp.tskip, tc.dir_layer, p == 1 || p == 2, rc,
                              rdo_lambda_scale, pass && use_rdo,
                              pass ? dir_cand : 1, mode_ctx, tc.sdh);
    };

    // Quantize one already-loaded tile: plain pass, table-set choice, then the
    // RD trellis and a second table-set choice.  Leaves `tc` ready to encode
    // and returns the coefficient rate in bits (payload only).
    auto quantize_tile = [&](TileCoder &tc, const TableSet *tabs) -> double {
        int np = coded_planes(tc);
        if (tc.intra_dir) {
            analyze_tile_dir(tc, tabs, 0);
        } else {
            for (int p = 0; p < np; ++p)
                analyze_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                              tc.tp.tskip, 1, tc.sdh);
        }
        u32 th[kNumCtx][kNumSym];
        std::memset(th, 0, sizeof(th));
        if (count_units(tc.units.data(), (int)tc.units.size(),
                        1 << tc.tp.nsub_log2, th, nullptr))
            tc.tp.table_set = select_set(th, tc.tp.table_set);
        if (tc.intra_dir) {
            analyze_tile_dir(tc, tabs, 1);
            std::memset(th, 0, sizeof(th));
            if (!count_units(tc.units.data(), (int)tc.units.size(),
                             1 << tc.tp.nsub_log2, th, nullptr))
                return 0;
            tc.tp.table_set = select_set(th, tc.tp.table_set);
        } else if (use_rdo) {
            rdoq_tile(tc, tabs);
            std::memset(th, 0, sizeof(th));
            if (!count_units(tc.units.data(), (int)tc.units.size(),
                             1 << tc.tp.nsub_log2, th, nullptr))
                return 0;
            tc.tp.table_set = select_set(th, tc.tp.table_set);
        }
        return table_set_cost(th, tabs[tc.tp.table_set]);
    };

    auto make_tile_params = [&](int t, int eye, int col) {
        TileParams tp;
        tp.tile_index = col;
        tp.eye = eye;
        tp.mode = NXVC_MODE_INTRA;
        tp.res_level = res_map ? clamp_i32(res_map[t], 0, 2) : 0;
        if (e->cfg.lossless) tp.res_level = 0;
        tp.chroma444 = (g.chroma == NXVC_CHROMA_444 && !e->cfg.tile_chroma420);
        int qp = qp_map ? clamp_i32(qp_map[t], 0, 63) : fp.base_qp;
        tp.qp_delta = clamp_i32(qp - fp.base_qp, -32, 31);
        tp.table_set = clamp_i32((fp.base_qp + tp.qp_delta) >> 3, 0, 7);
        tp.nsub_log2 = e->cfg.nsub_log2 == 255 ? 3 : (int)e->cfg.nsub_log2;
        tp.tskip = e->cfg.transform_skip == 1 ? 1 : 0;
        if (fp.quant_matrix != 255 && e->cfg.wm_id != 0 && !e->cfg.lossless)
            tp.wm_id = e->cfg.wm_id == 255 ? 0 : clamp_i32((int)e->cfg.wm_id, 0, 3);
        return tp;
    };

    // Apply a decision to a tile's header fields.
    auto apply_decision = [&](TileParams &tp, const TileDecision &d) {
        tp.mode = d.mode;
        tp.mv_x = d.mv_x;
        tp.mv_y = d.mv_y;
        tp.disparity = d.disparity;
        tp.skipped = d.skipped;
        tp.ref_sel = 0;
        tp.mv_present = 0;
        tp.mv_quad = 0;
        if (d.mode == NXVC_MODE_WARP_MV || d.mode == NXVC_MODE_STATIC_MV) {
            tp.mv_present = 1;
            tp.ref_sel = d.ref_sel;
            tp.mv_quad = d.mv_quad;
            for (int q = 0; q < 4; ++q) {
                tp.dmv[q][0] = d.dmv[q][0];
                tp.dmv[q][1] = d.dmv[q][1];
            }
        } else if (d.mode == NXVC_MODE_STEREO) {
            tp.mv_present = 1;   // the disparity field lives in the mv slot
        }
        if (d.mode == NXVC_MODE_WARP_SKIP) {
            // A skipped tile carries no header, so every parameter of it has
            // to be derivable: res_level 0, full-resolution chroma, opaque
            // alpha.  The encoder therefore never chooses skip on a stream
            // with a coded alpha plane (see decide_tile).
            tp.res_level = 0;
            tp.chroma444 = (g.chroma == NXVC_CHROMA_444) ? 1 : 0;
            tp.alpha_mode = 0;
        }
    };

    // ------------------------------------------------------------------
    // The per-tile mode decision.  `cur` is the ring slot this frame writes,
    // which already holds this frame's reconstructed left eye by the time a
    // right-eye tile of the same row is reached -- the one intra-frame
    // dependency in the format (Annex D D-3).
    // ------------------------------------------------------------------
    auto decide_tile = [&](int t, int eye, int col, int row,
                           const TableSet *tabs) -> TileDecision {
        TileDecision d;
        d.mode = NXVC_MODE_INTRA;
        // The two correctness overrides of the rate controller's
        // force_warp_skip request (docs/RATECONTROL.md 8.7) are no eligible
        // reference -- this line -- and a refresh due, which is the block
        // below.  A skipped tile also cannot carry a coded alpha plane, which
        // is checked further down.
        if (!have_ref) return d;
        const nxvc::PredState &ps = e->state[t];
        const int qp = qp_map ? clamp_i32(qp_map[t], 0, 63) : fp.base_qp;
        const double qstep = (double)kQStep[qp] / 16.0;
        const double qnoise = qstep * qstep / 12.0;

        // Refresh, docs/SYNTAX.md 13.10.  One age counter, two thresholds:
        //
        //   * at `intra_period` the tile becomes ELIGIBLE.  With no drift rule
        //     it refreshes there, which is the fixed 1-in-T cadence written as
        //     an age; with the drift rule it refreshes there only if the
        //     client shadow says it needs to.
        //   * at `max_age` it refreshes unconditionally, whatever the pictures
        //     look like.  That bound is what a client which joined late or
        //     lost a run of frames recovers on, so it must not depend on the
        //     content, and it is the only thing the drift rule may not defer.
        //
        // The encoder holds an exact shadow of what the client displays, so
        // "needs to" is measured rather than assumed: `drift[t]` is the mean
        // squared error that shadow actually carried for this tile after the
        // previous frame, and the threshold is a multiple of the quantiser's
        // own noise floor, which is the error the tile's rate was meant to buy.
        if (e->age_since_intra[t] >= max_age) return d;
        if (e->age_since_intra[t] >= intra_period &&
            (!refresh_drift_q8 ||
             (double)e->drift[t] > refresh_drift_thresh * qnoise))
            return d;
        const bool force_skip =
            !e->cfg.alpha && t < (int)e->skip_map.size() && e->skip_map[t] != 0;

        // The MODE decision runs at a lower lambda than the within-tile
        // trellis.  Every frame this codec produces is a reference for the
        // next three, so a mode that saves bits by leaving distortion in the
        // picture is charged for that distortion once and paid for it four
        // times.  The trellis's lambda is a within-tile trade at fixed mode
        // and is unaffected.
        const double mode_scale =
            e->cfg.mode_lambda_q8 ? (double)e->cfg.mode_lambda_q8 / 256.0 : 0.25;
        const double lambda = mode_scale * rdo_lambda_scale * qstep * qstep;
        // warp_dc() is a rate-for-distortion trade at a FIXED mode, which is
        // what the trellis's lambda prices; `lambda` above is the mode
        // decision's, deliberately lower because a mode that leaves
        // distortion in the reference pays for it four times.  Nine bytes of
        // correction do not, so they are charged at the trellis's rate.
        const double dc_rate =
            rdo_lambda_scale * qstep * qstep * 8.0 * (double)kWarpDcBytes;

        TileCoder probe;
        probe.g = &g;
        probe.fp = &fp;
        probe.tp = make_tile_params(t, eye, col);
        probe.tp.res_level = 0;
        probe.tp.mode = NXVC_MODE_WARP_MV;   // allocates wpred
        probe.setup();
        load_tile(probe, g, img, eye, col, row);
        const int lsz = probe.pl[0].size;
        const i64 npix = (i64)lsz * lsz;

        // --- WARP_SKIP with the stored vector: no bits at all.
        predict_tile(probe, g, refpic, fp.warp[eye], eye, col, row,
                     NXVC_MODE_WARP_SKIP, ps.last_mv_x, ps.last_mv_y);
        const double skip_sse = sse_pred(probe.pl[0]);
        // The gate is a SPEED early-out, not a decision: it takes the skip
        // without searching when the predictor's error is already below the
        // quantiser's own noise floor, which no coded residual could improve
        // on.  qstep^2/12 is that floor for a uniform quantiser, and
        // `skip_thresh` is the multiple of it the encoder will accept.
        // Everything else goes to the rate-distortion comparison below.
        const double skip_gate =
            qnoise * (e->cfg.skip_thresh ? (double)e->cfg.skip_thresh / 256.0
                                         : 1.0);
        TileDecision skip_cand;
        skip_cand.mode = NXVC_MODE_WARP_SKIP;
        skip_cand.mv_x = ps.last_mv_x;
        skip_cand.mv_y = ps.last_mv_y;
        skip_cand.skipped = 1;

        // The near-skip of docs/SYNTAX.md 13.9.  `probe` holds the WARP_SKIP
        // predictor, so the fit is free; the rule is stated once here and used
        // at both places a skip can be chosen -- the early-out just below and
        // the rate-distortion tail at the end.  It returns the squared error
        // the correction removes, which is what has to pay for its nine bytes.
        auto fit_near = [&](DcCorrection &dc) -> double {
            if (!fp.warp_dc || e->cfg.alpha) return 0.0;
            fit_dc_correction(probe, dc);
            return sse_near_skip(probe, DcCorrection{}) -
                   sse_near_skip(probe, dc);
        };
        auto near_of = [&](TileDecision cand, const DcCorrection &dc,
                           double gain) {
            if (gain > dc_rate) {
                cand.dc_corr = 1;
                cand.dc = dc;
            }
            return cand;
        };

        if (force_skip ||
            (!e->cfg.alpha && skip_sse <= skip_gate * (double)npix)) {
            DcCorrection dc;
            const double gain = fit_near(dc);
            return near_of(skip_cand, dc, gain);
        }
        i64 best_sad = e->cfg.alpha ? (i64)1 << 62 : sad_pred(probe.pl[0]);
        TileDecision best_inter;
        best_inter.mode = NXVC_MODE_WARP_SKIP;
        best_inter.mv_x = ps.last_mv_x;
        best_inter.mv_y = ps.last_mv_y;
        best_inter.skipped = 1;

        // --- the two searched modes, and STEREO on the right eye.
        struct Cand { int mode; const RefPicture *ref; };
        Cand cands[3];
        int ncand = 0;
        cands[ncand++] = {NXVC_MODE_WARP_MV, &refpic};
        cands[ncand++] = {NXVC_MODE_STATIC_MV, &refpic};
        if (e->cfg.stereo && eye == 1)
            cands[ncand++] = {NXVC_MODE_STEREO, &e->ring.slot[slot]};

        for (int ci = 0; ci < ncand; ++ci) {
            const int mode = cands[ci].mode;
            const RefPicture &rp = *cands[ci].ref;
            const bool warped = mode_needs_warp(mode);
            nw::Homography H =
                plane_homography(fp.warp[eye], (int)g.width, (int)g.height, 1);
            nw::TileCorners c = nw::warp_tile_corners(
                H, col * 64, row * 64, warped ? nw::kModeWarp : nw::kModeStatic);
            SearchCtx sc;
            sc.src = probe.pl[0].samples.data();
            sc.size = lsz;
            sc.im = ref_image(rp, g, 0, mode == NXVC_MODE_STEREO ? 0 : eye);
            sc.ox = col * 64;
            sc.oy = row * 64;
            sc.d0x = (c.x[0] >> 6) - sc.ox;
            sc.d0y = (c.y[0] >> 6) - sc.oy;

            // Coarse: the seeds of PAPER 2.3 step 1 (zero and the tile's
            // previous vector), then a +-mv_range integer sweep at step 2 and
            // a step-1 sweep around the winner.
            // A motion vector is s8 quarter samples, so |mv| <= 32 samples;
            // a STEREO disparity is unsigned 12-bit quarter samples.
            const bool st = (mode == NXVC_MODE_STEREO);
            const bool want_quad = fp.mv_quad && !st;
            auto vec_ok = [&](int mx, int my) {
                if (st) return my == 0 && mx >= 0 && mx <= 4095;
                return mx >= -128 && mx <= 127 && my >= -128 && my <= 127;
            };
            int bx = 0, by = 0;
            i64 bsad = -1;
            auto try_int = [&](int dx, int dy) {
                if (!vec_ok(dx * 4, dy * 4)) return;
                i64 v = sad_translate(sc, dx, dy, 4);
                if (bsad < 0 || v < bsad) { bsad = v; bx = dx; by = dy; }
            };
            try_int(0, 0);
            if (mode != NXVC_MODE_STEREO)
                try_int(ps.last_mv_x >> 2, ps.last_mv_y >> 2);
            if (st) {
                // Horizontal only, and the disparity is unsigned: the left-eye
                // source lies to the RIGHT of the right-eye position.
                try_int((int)(ps.last_disp >> 2), 0);
                for (int dx = 0; dx <= mv_range * 2; dx += 2) try_int(dx, 0);
                int c0 = bx;
                for (int dx = c0 - 2; dx <= c0 + 2; ++dx)
                    if (dx >= 0) try_int(dx, 0);
            } else {
                for (int dy = -mv_range; dy <= mv_range; dy += 2)
                    for (int dx = -mv_range; dx <= mv_range; dx += 2)
                        try_int(dx, dy);
                int cx = bx, cy = by;
                for (int dy = cy - 1; dy <= cy + 1; ++dy)
                    for (int dx = cx - 1; dx <= cx + 1; ++dx) try_int(dx, dy);
            }

            // Exact stage: the real predictor, integer neighbours then the
            // eight quarter-pel neighbours of the winner.
            int qx = bx * 4, qy = by * 4;
            i64 qsad = -1;
            // Tool 25's per-quadrant search rides on this one: a candidate
            // vector applied to the whole tile predicts every quadrant with
            // that vector at once, and the quadrants are independent given the
            // vector, so the best vector per quadrant falls out of the same
            // scan at no extra predictor calls (docs/SYNTAX.md 13.8).
            i64 quad_sad[4] = {-1, -1, -1, -1};
            int quad_x[4] = {}, quad_y[4] = {};
            auto try_exact = [&](int mx, int my) {
                if (!vec_ok(mx, my)) return;
                predict_tile(probe, g, rp, fp.warp[eye],
                             st ? 0 : eye, col, row, mode, mx, my, 1);
                i64 v;
                if (!want_quad) {
                    v = sad_pred(probe.pl[0]);
                } else {
                    // The same sum, split four ways.  A whole-tile SAD is the
                    // sum of its quadrants', so this is sad_pred() written so
                    // that the parts survive -- one pass, not two.
                    i64 qs[4] = {0, 0, 0, 0};
                    const PlaneState &sp = probe.pl[0];
                    const int half = sp.size / 2;
                    for (int y = 0; y < sp.size; ++y)
                        for (int x = 0; x < sp.size; ++x) {
                            const size_t i = (size_t)y * sp.size + x;
                            const i32 dd = sp.samples[i] -
                                           clamp_i32(sp.wpred[i], 0, sp.maxval);
                            qs[(y >= half ? 2 : 0) + (x >= half ? 1 : 0)] +=
                                dd < 0 ? -dd : dd;
                        }
                    v = qs[0] + qs[1] + qs[2] + qs[3];
                    for (int q = 0; q < 4; ++q)
                        if (quad_sad[q] < 0 || qs[q] < quad_sad[q]) {
                            quad_sad[q] = qs[q];
                            quad_x[q] = mx;
                            quad_y[q] = my;
                        }
                }
                if (qsad < 0 || v < qsad) { qsad = v; qx = mx; qy = my; }
            };
            const int seed_x = qx, seed_y = qy;
            if (st) {
                for (int dx = -4; dx <= 4; dx += 4) try_exact(seed_x + dx, 0);
                int s2 = qx;
                for (int dx = -3; dx <= 3; ++dx) try_exact(s2 + dx, 0);
            } else {
                for (int dy = -4; dy <= 4; dy += 4)
                    for (int dx = -4; dx <= 4; dx += 4)
                        try_exact(seed_x + dx, seed_y + dy);
                int s2x = qx, s2y = qy;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        try_exact(s2x + dx, s2y + dy);
            }
            // The four quadrant vectors, as deltas from the tile vector, when
            // they are worth their eight bytes.  The charge is stated the same
            // way as the two-byte vector's below, so one rule covers both.
            int use_quad = 0;
            if (want_quad && qsad >= 0) {
                i64 sum = 0;
                bool inrange = true;
                for (int q = 0; q < 4; ++q) {
                    sum += quad_sad[q];
                    const int dx = quad_x[q] - qx, dy = quad_y[q] - qy;
                    if (dx < -128 || dx > 127 || dy < -128 || dy > 127)
                        inrange = false;
                }
                if (inrange &&
                    sum + (i64)((double)kMvQuadBytes * 8.0 * qstep) < qsad) {
                    use_quad = 1;
                    qsad = sum;
                }
            }
            // A coded vector costs about two bytes of header; charge it so a
            // mode only wins when it is actually better.
            const i64 bias = (i64)(2.0 * qstep * 8.0);
            if (qsad >= 0 && qsad + bias < best_sad) {
                best_sad = qsad + bias;
                best_inter.mode = mode;
                best_inter.skipped = 0;
                best_inter.ref_sel = mode == NXVC_MODE_STEREO ? 0 : want_d;
                best_inter.mv_quad = use_quad;
                for (int q = 0; q < 4; ++q) {
                    best_inter.dmv[q][0] = use_quad ? quad_x[q] - qx : 0;
                    best_inter.dmv[q][1] = use_quad ? quad_y[q] - qy : 0;
                }
                if (mode == NXVC_MODE_STEREO) {
                    best_inter.disparity = qx;
                    best_inter.mv_x = 0;
                    best_inter.mv_y = 0;
                } else {
                    best_inter.mv_x = qx;
                    best_inter.mv_y = qy;
                    best_inter.disparity = 0;
                }
            }
        }

        auto score = [&](const TileDecision &cand, double *dist_out) -> double {
            TileCoder tc;
            tc.g = &g;
            tc.fp = &fp;
            tc.tp = make_tile_params(t, eye, col);
            apply_decision(tc.tp, cand);
            tc.setup();
            load_tile(tc, g, img, eye, col, row);
            if (e->cfg.transform_skip == 2) tc.tp.tskip = tskip_decision(tc);
            if (cand.mode != NXVC_MODE_INTRA) {
                const RefPicture &rp = cand.mode == NXVC_MODE_STEREO
                                           ? e->ring.slot[slot]
                                           : refpic;
                predict_tile(tc, g, rp, fp.warp[eye],
                             cand.mode == NXVC_MODE_STEREO ? 0 : eye, col, row,
                             cand.mode,
                             cand.mode == NXVC_MODE_STEREO ? cand.disparity
                                                           : cand.mv_x,
                             cand.mode == NXVC_MODE_STEREO ? 0 : cand.mv_y, 0,
                             cand.mv_quad ? cand.dmv : nullptr);
            }
            if (cand.mode == NXVC_MODE_WARP_SKIP) {
                double sse = 0;
                int np = coded_planes(tc);
                for (int p = 0; p < np; ++p) {
                    PlaneState &s = tc.pl[p];
                    for (size_t i = 0; i < s.samples.size(); ++i) {
                        double dd = (double)s.samples[i] -
                                    (double)clamp_i32(s.wpred[i], 0, s.maxval);
                        sse += dd * dd;
                    }
                }
                if (dist_out) *dist_out = sse;
                return sse + lambda * 1.0;
            }
            tc.build_units();
            double bits = quantize_tile(tc, tabs);
            int active = 1 << tc.tp.nsub_log2;
            if ((size_t)active > tc.units.size()) active = (int)tc.units.size();
            bits += 8 * (kTileHeaderBytes + 4 * active +
                         (tc.tp.mv_present ? 2 : 0) +
                         (tc.tp.mv_quad ? (int)kMvQuadBytes : 0));
            const double d = tile_distortion(tc);
            if (dist_out) *dist_out = d;
            return d + lambda * bits;
        };

        // A coded tile's error is bounded by its quantiser and is corrected
        // again next frame.  A skipped tile's error goes straight into the
        // reference and stays there until the tile is next coded, so the part
        // of it that a coded tile would NOT have left behind is paid for
        // several times over.  Charging skip only its own distortion, once, is
        // what makes a locally optimal RD decision globally wrong: measured on
        // vr-turn-256 at QP 0 it gave up 4 dB to save 12 % of the rate.
        //
        // The penalty is on the EXCESS over the best coded candidate, not on
        // the distortion itself.  A skipped tile whose error merely repeats
        // the error a coded tile would have made costs nothing extra -- which
        // is exactly the repeated-frame case, where every tile must skip.
        constexpr double kSkipPersist = 4.0;
        TileDecision intra;
        double d_intra = 0, d_skip = 0, d_inter = 0;
        const double c_intra = score(intra, &d_intra);
        double best_cost = c_intra;
        TileDecision best = intra;
        double d_ref = d_intra;
        if (!best_inter.skipped) {
            const double c_inter = score(best_inter, &d_inter);
            if (c_inter < best_cost) {
                best_cost = c_inter;
                best = best_inter;
            }
            if (d_inter < d_ref) d_ref = d_inter;
        }
        score(skip_cand, &d_skip);
        const double excess = d_skip > d_ref ? d_skip - d_ref : 0.0;
        const double c_skip = d_skip + (kSkipPersist - 1.0) * excess + lambda;
        if (c_skip < best_cost) {
            best_cost = c_skip;
            best = skip_cand;
        }

        // The same near-skip rule at the other place a skip can be chosen.
        // It is charged the SAME persistence penalty as a plain skip, because
        // a near-skip's error also goes into the reference and stays there.
        if (fp.warp_dc && !e->cfg.alpha) {
            // The search above left `probe` holding its last candidate, and
            // only its luma plane; put the skip predictor back, every plane.
            predict_tile(probe, g, refpic, fp.warp[eye], (int)eye, col, row,
                         NXVC_MODE_WARP_SKIP, skip_cand.mv_x, skip_cand.mv_y);
            DcCorrection dc;
            const double gain = fit_near(dc);
            if (gain > dc_rate) {
                const double d_near = d_skip - gain;
                const double ex = d_near > d_ref ? d_near - d_ref : 0.0;
                const double c_near =
                    d_near + (kSkipPersist - 1.0) * ex + lambda + dc_rate;
                if (c_near < best_cost) {
                    best_cost = c_near;
                    best = near_of(skip_cand, dc, gain);
                }
            }
        }
        return best;
    };

    // ------------------------------------------------------------------
    // One pass over the frame, in the normative order: row-major, eye-minor.
    // `decide` runs the mode decision and caches it; `emit` writes bytes.
    // ------------------------------------------------------------------
    u32 hist[8][kNumCtx][kNumSym];
    std::memset(hist, 0, sizeof(hist));
    BW body;
    nxvc_status err = NXVC_OK;

    auto run_pass = [&](bool decide, bool emit, const TableSet *tabs) {
        RefPicture &cur = e->ring.slot[slot];
        for (u32 row = 0; row < g.tiles_y && err == NXVC_OK; ++row) {
            for (u32 eye = 0; eye < g.eyes && err == NXVC_OK; ++eye) {
                BW rowbuf;
                u64 skipmap = 0;
                u32 coded = 0;
                u64 dcmap = 0;
                for (u32 col = 0; col < g.tiles_x; ++col) {
                    const int t = linear_tile(g, (int)row, (int)eye, (int)col);
                    if (decide) e->dec[t] = decide_tile(t, (int)eye, (int)col,
                                                        (int)row, tabs);
                    if (e->dec[t].skipped) {
                        skipmap |= 1ull << col;
                        if (e->dec[t].dc_corr) dcmap |= 1ull << col;
                    } else ++coded;
                }
                if (emit) {
                    rowbuf.u16v(fp.frame_number);
                    rowbuf.u8v(row);
                    rowbuf.u8v(coded | (dcmap ? 0x80u : 0u));
                    rowbuf.u64v(skipmap);
                    if (dcmap) {
                        rowbuf.u64v(dcmap);
                        for (u32 col = 0; col < g.tiles_x; ++col) {
                            if (!((dcmap >> col) & 1u)) continue;
                            const nxvc::DcCorrection &c =
                                e->dec[linear_tile(g, (int)row, (int)eye,
                                                   (int)col)].dc;
                            for (int q = 0; q < kWarpDcPlanes; ++q) {
                                rowbuf.u8v((u8)c.dc[q]);
                                rowbuf.u8v((u8)c.gx[q]);
                                rowbuf.u8v((u8)c.gy[q]);
                            }
                        }
                    }
                }
                for (u32 col = 0; col < g.tiles_x && err == NXVC_OK; ++col) {
                    const int t = linear_tile(g, (int)row, (int)eye, (int)col);
                    const TileDecision &d = e->dec[t];
                    TileCoder tc;
                    tc.g = &g;
                    tc.fp = &fp;
                    tc.tp = make_tile_params(t, (int)eye, (int)col);
                    apply_decision(tc.tp, d);
                    tc.setup();
                    load_tile(tc, g, img, (int)eye, (int)col, (int)row);
                    if (e->cfg.transform_skip == 2)
                        tc.tp.tskip = tskip_decision(tc);
                    if (e->cfg.alpha) {
                        bool constant = true;
                        i32 first = tc.pl[3].samples[0];
                        for (size_t i = 1; i < tc.pl[3].samples.size(); ++i)
                            if (tc.pl[3].samples[i] != first) {
                                constant = false;
                                break;
                            }
                        tc.tp.alpha_mode = constant ? ((first == 255) ? 0 : 1) : 2;
                        tc.tp.alpha_value = first;
                    }
                    if (d.mode != NXVC_MODE_INTRA) {
                        const RefPicture &rp =
                            d.mode == NXVC_MODE_STEREO ? cur : refpic;
                        predict_tile(tc, g, rp, fp.warp[eye],
                                     d.mode == NXVC_MODE_STEREO ? 0 : (int)eye,
                                     (int)col, (int)row, d.mode,
                                     d.mode == NXVC_MODE_STEREO ? d.disparity
                                                                : d.mv_x,
                                     d.mode == NXVC_MODE_STEREO ? 0 : d.mv_y, 0,
                                     d.mv_quad ? d.dmv : nullptr);
                    }
                    if (d.skipped) {
                        reconstruct_skip(tc, d.dc_corr ? &d.dc : nullptr);
                        store_ref_tile(tc, g, cur, (int)eye, (int)col, (int)row);
                        if (emit)
                            store_tile(tc, g, &e->shadow_img, (int)eye, (int)col,
                                       (int)row);
                        nxvc_tile_info &ti = e->tiles[t];
                        ti.tile_index = (u16)col;
                        ti.eye = (u8)eye;
                        ti.mode = NXVC_MODE_WARP_SKIP;
                        ti.skipped = 1;
                        ti.dc_corr = (u8)d.dc_corr;
                        ti.mv_x = (i8)d.mv_x;
                        ti.mv_y = (i8)d.mv_y;
                        ti.ref_delta = 0;
                        ti.res_level = 0;
                        ti.chroma444 = (u8)tc.tp.chroma444;
                        ti.nsub_log2 = (u8)tc.tp.nsub_log2;
                        ti.table_set = (u8)tc.tp.table_set;
                        ti.qp_delta = (i8)tc.tp.qp_delta;
                        ti.qp = (u8)clamp_i32(fp.base_qp + tc.tp.qp_delta, 0, 63);
                        continue;
                    }
                    tc.build_units();
                    if (qp_search > 0 || wm_search) {
                        const double base =
                            (double)kQStep[clamp_i32(fp.base_qp + tc.tp.qp_delta,
                                                     0, 63)] / 16.0;
                        const double lambda = rdo_lambda_scale * base * base;
                        const int qd0 = tc.tp.qp_delta;
                        double best = 0;
                        bool have = false;
                        int best_qd = qd0, best_wm = tc.tp.wm_id;
                        for (int dq = -qp_search; dq <= qp_search; ++dq) {
                            for (int wm = 0; wm <= (wm_search ? 3 : 0); ++wm) {
                                TileCoder cand;
                                cand.g = &g;
                                cand.fp = &fp;
                                cand.tp = tc.tp;
                                cand.tp.qp_delta = clamp_i32(qd0 + dq, -32, 31);
                                cand.tp.wm_id = wm;
                                cand.setup();
                                load_tile(cand, g, img, (int)eye, (int)col,
                                          (int)row);
                                if (d.mode != NXVC_MODE_INTRA)
                                    for (int p = 0; p < 4; ++p)
                                        cand.pl[p].wpred = tc.pl[p].wpred;
                                cand.build_units();
                                double bits = quantize_tile(cand, fp.tabs);
                                int active = 1 << cand.tp.nsub_log2;
                                if ((size_t)active > cand.units.size())
                                    active = (int)cand.units.size();
                                bits += 8 * (kTileHeaderBytes + 4 * active);
                                double cost =
                                    tile_distortion(cand) + lambda * bits;
                                if (!have || cost < best) {
                                    best = cost;
                                    have = true;
                                    best_qd = cand.tp.qp_delta;
                                    best_wm = wm;
                                }
                            }
                        }
                        if (have && (best_qd != tc.tp.qp_delta ||
                                     best_wm != tc.tp.wm_id)) {
                            std::vector<i32> keep[4];
                            for (int p = 0; p < 4; ++p) keep[p] = tc.pl[p].wpred;
                            tc.tp.qp_delta = best_qd;
                            tc.tp.wm_id = best_wm;
                            tc.setup();
                            load_tile(tc, g, img, (int)eye, (int)col, (int)row);
                            for (int p = 0; p < 4; ++p) tc.pl[p].wpred = keep[p];
                            tc.build_units();
                        }
                    }
                    quantize_tile(tc, tabs);
                    u32 th[kNumCtx][kNumSym];
                    std::memset(th, 0, sizeof(th));
                    if (count_units(tc.units.data(), (int)tc.units.size(),
                                    1 << tc.tp.nsub_log2, th, nullptr)) {
                        tc.tp.table_set = select_set(th, tc.tp.table_set);
                        if (emit && e->cfg.nsub_log2 == 255) {
                            double bits =
                                table_set_cost(th, tabs[tc.tp.table_set]);
                            int n = 0;
                            while (n < 3 && (double)(32 << (n + 1)) <= bits * 0.10)
                                ++n;
                            tc.tp.nsub_log2 = n;
                        }
                        if (!emit) {
                            for (int c = 0; c < fp.nctx; ++c)
                                for (int sy = 0; sy < kNumSym; ++sy)
                                    hist[tc.tp.table_set][c][sy] += th[c][sy];
                        }
                    }
                    if (emit && e->cfg.collect_stats) {
                        const TableSet &ts = tabs[tc.tp.table_set];
                        int nplanes_c = coded_planes(tc);
                        size_t ui = 0;
                        const size_t nhead = tc.intra_dir ? 2 : 1;
                        for (int p = 0; p < nplanes_c; ++p) {
                            int ndc = tc.pl[p].nb * tc.pl[p].nb;
                            std::vector<Unit> dcu(tc.units.begin() + ui,
                                                  tc.units.begin() + ui + nhead);
                            std::vector<Unit> blk(
                                tc.units.begin() + ui + nhead,
                                tc.units.begin() + ui + nhead + ndc);
                            ui += nhead + ndc;
                            u32 hd[kNumCtx][kNumSym], hb[kNumCtx][kNumSym];
                            std::memset(hd, 0, sizeof(hd));
                            std::memset(hb, 0, sizeof(hb));
                            count_units(dcu.data(), (int)nhead, 1, hd, nullptr);
                            count_units(blk.data(), ndc, 1, hb, nullptr);
                            e->stats.bits_dc_plane += (u64)table_set_cost(hd, ts);
                            u64 bb = (u64)table_set_cost(hb, ts);
                            if (p == 0) e->stats.bits_luma_blocks += bb;
                            else if (p == 3) e->stats.bits_alpha_blocks += bb;
                            else e->stats.bits_chroma_blocks += bb;
                        }
                        int active = 1 << tc.tp.nsub_log2;
                        if ((size_t)active > tc.units.size())
                            active = (int)tc.units.size();
                        e->stats.bytes_rans_init += 4u * (u32)active;
                        e->stats.lanes_total += (u32)active;
                        e->stats.tiles++;
                        if (tc.tp.tskip) e->stats.tiles_tskip++;
                        e->stats.tiles_res[tc.tp.res_level]++;
                    }
                    if (emit) {
                        std::vector<u8> payload;
                        if (!encode_units(tc.units.data(), (int)tc.units.size(),
                                          1 << tc.tp.nsub_log2,
                                          tabs[tc.tp.table_set], payload)) {
                            err = NXVC_ERR_BITSTREAM;
                            break;
                        }
                        if (payload.size() > 65535) {
                            err = NXVC_ERR_NOMEM;
                            break;
                        }
                        tc.tp.payload_len = (int)payload.size();
                        pack_tile_header(rowbuf, tc.tp);
                        if (tc.tp.mv_present) {
                            if (tc.tp.mode == NXVC_MODE_STEREO) {
                                rowbuf.u16v((u32)(tc.tp.disparity & 0xfff));
                            } else {
                                rowbuf.u8v((u8)(i8)tc.tp.mv_x);
                                rowbuf.u8v((u8)(i8)tc.tp.mv_y);
                            }
                        }
                        if (tc.tp.mv_quad)
                            for (int q = 0; q < 4; ++q) {
                                rowbuf.u8v((u8)(i8)tc.tp.dmv[q][0]);
                                rowbuf.u8v((u8)(i8)tc.tp.dmv[q][1]);
                            }
                        if (tc.tp.alpha_mode == 1)
                            rowbuf.u8v((u8)tc.tp.alpha_value);
                        rowbuf.raw(payload.data(), payload.size());
                        if (e->cfg.collect_stats) {
                            e->stats.bytes_tile_headers +=
                                kTileHeaderBytes + (tc.tp.mv_present ? 2 : 0) +
                                (tc.tp.mv_quad ? kMvQuadBytes : 0) +
                                (tc.tp.alpha_mode == 1 ? 1 : 0);
                            e->stats.bytes_payload += payload.size();
                        }
                    }
                    finalize_recon(tc);
                    store_ref_tile(tc, g, cur, (int)eye, (int)col, (int)row);
                    if (emit)
                        store_tile(tc, g, &e->shadow_img, (int)eye, (int)col,
                                   (int)row);

                    nxvc_tile_info &ti = e->tiles[t];
                    ti.tile_index = (u16)tc.tp.tile_index;
                    ti.payload_len = (u16)tc.tp.payload_len;
                    ti.layer = 0;
                    ti.eye = (u8)eye;
                    ti.mode = (u8)tc.tp.mode;
                    ti.res_level = (u8)tc.tp.res_level;
                    ti.chroma444 = (u8)tc.tp.chroma444;
                    ti.alpha_mode = (u8)tc.tp.alpha_mode;
                    ti.table_set = (u8)tc.tp.table_set;
                    ti.nsub_log2 = (u8)tc.tp.nsub_log2;
                    ti.tskip = (u8)tc.tp.tskip;
                    ti.wgt = 0;
                    ti.wm_id = (u8)tc.tp.wm_id;
                    ti.intra_dir = (u8)tc.intra_dir;
                    ti.ref_sel = (u8)tc.tp.ref_sel;
                    ti.mv_present = (u8)tc.tp.mv_present;
                    ti.mv_quad = (u8)tc.tp.mv_quad;
                    for (int q = 0; q < 4; ++q) {
                        ti.dmv[q][0] = (i8)tc.tp.dmv[q][0];
                        ti.dmv[q][1] = (i8)tc.tp.dmv[q][1];
                    }
                    ti.qp_delta = (i8)tc.tp.qp_delta;
                    ti.mv_x = (i8)tc.tp.mv_x;
                    ti.mv_y = (i8)tc.tp.mv_y;
                    ti.disparity = (u16)tc.tp.disparity;
                    ti.ref_delta = (tc.tp.mode == NXVC_MODE_INTRA ||
                                    tc.tp.mode == NXVC_MODE_STEREO)
                                       ? 3u
                                       : (u8)tc.tp.ref_sel;
                    ti.alpha_value = (u8)tc.tp.alpha_value;
                    ti.skipped = 0;
                    ti.qp = (u8)clamp_i32(fp.base_qp + tc.tp.qp_delta, 0, 63);
                }
                if (emit) {
                    body.raw(rowbuf.b.data(), rowbuf.b.size());
                    if (e->cfg.collect_stats)
                        e->stats.bytes_row_headers +=
                            kTileRowHeaderBytes + (dcmap ? 8u : 0u);
                }
            }
        }
    };

    // Pass 0 decides the modes and, when custom tables are on, trains them.
    // It is skipped for an all-intra frame with no custom tables, where there
    // is nothing to decide and nothing to train.
    const bool need_pass0 = e->cfg.custom_tables || have_ref;
    if (need_pass0) {
        run_pass(true, false, deftabs);
        if (err != NXVC_OK) return err;
    } else {
        for (int t = 0; t < ntiles; ++t) e->dec[t] = TileDecision{};
    }

    if (e->cfg.custom_tables) {
        BitW bw;
        for (int k = 0; k < 8; ++k) {
            u32 total = 0;
            for (int c = 0; c < fp.nctx; ++c)
                for (int s2 = 0; s2 < kNumSym; ++s2) total += hist[k][c][s2];
            if (total == 0) continue;
            u16 target[kNumCtx][kNumSym];
            for (int c = 0; c < fp.nctx; ++c) {
                u32 sum = 0;
                for (int s2 = 0; s2 < kNumSym; ++s2) sum += hist[k][c][s2];
                for (int s2 = 0; s2 < kNumSym; ++s2) {
                    if (sum == 0) {
                        target[c][s2] = default_freq(fp.nctx, k, c, s2);
                    } else {
                        u32 v = (u32)(((u64)hist[k][c][s2] * 1024) / sum);
                        target[c][s2] = (u16)(v < 1 ? 1 : v);
                    }
                }
            }
            fp.tables_present |= (1u << k);
            serialize_table_set(bw, target, k, fp.nctx, fp.tabs[k]);
        }
        bw.flush();
        table_bytes = bw.b;
    }

    run_pass(false, true, fp.tabs);
    if (err != NXVC_OK) return err;

    // ---- frame header
    BW head;
    head.u16v(fp.frame_number);
    head.raw(fp.pose, 26);
    head.u8v(fp.base_qp);
    head.u8v((u8)(i8)fp.chroma_qp_off);
    head.u8v((u8)(i8)fp.alpha_qp_off);
    head.u8v(fp.quant_matrix);
    head.u8v(fp.tables_present);
    head.u8v(fp.ref_slots);
    head.u8v(fp.flags);
    head.u8v(0);
    const size_t warp_bytes = fp.warp_present ? 36u * g.eyes : 0u;
    size_t extra = warp_bytes + (fp.quant_matrix == 255 ? 128 : 0) +
                   table_bytes.size();
    u32 total = (u32)(kFrameHeaderBytes + extra + body.size());
    head.u32v(total);
    if (head.size() != kFrameHeaderBytes) return NXVC_ERR_BITSTREAM;

    if (total > cap) return NXVC_ERR_NOMEM;
    u8 *o = buf;
    std::memcpy(o, head.b.data(), head.size());
    o += head.size();
    if (fp.warp_present) {
        BW wb;
        for (u32 eye = 0; eye < g.eyes; ++eye)
            for (int i = 0; i < 9; ++i) wb.u32v((u32)fp.warp[eye].h[i]);
        std::memcpy(o, wb.b.data(), wb.size());
        o += wb.size();
    }
    if (fp.quant_matrix == 255) {
        std::memcpy(o, e->custom_matrix.data(), 128);
        o += 128;
    }
    if (!table_bytes.empty()) {
        std::memcpy(o, table_bytes.data(), table_bytes.size());
        o += table_bytes.size();
    }
    std::memcpy(o, body.b.data(), body.size());
    o += body.size();
    *out_len = (size_t)(o - buf);
    if (e->cfg.collect_stats) {
        e->stats.bytes_total = *out_len;
        e->stats.bytes_frame_header =
            kFrameHeaderBytes + warp_bytes + (fp.quant_matrix == 255 ? 128 : 0);
        e->stats.bytes_tables = table_bytes.size();
    }

    // ---- the frame is now a reference; update the ring and the prediction
    // state by the rules of Annex D D-9.
    e->ring.slot[slot].valid = true;
    e->ring.slot[slot].frame_number = (u16)fp.frame_number;
    for (int t = 0; t < ntiles; ++t) {
        const TileDecision &d = e->dec[t];
        nxvc::update_pred_state(e->state[t], d.mode, d.mv_x, d.mv_y, d.disparity);
        if (d.skipped) {
            if (e->age_since_coded[t] < 0xffffu) ++e->age_since_coded[t];
        } else {
            e->age_since_coded[t] = 0;
        }
        // Everything is INTRA on a reset frame, so without a starting phase
        // every tile would come due on the same later frame.  Seeding the age
        // from the permutation there is what keeps the cap's refreshes spread
        // over the period, as the 1-in-T cadence it replaces was.
        if (d.mode == NXVC_MODE_INTRA)
            e->age_since_intra[t] =
                (u16)(1 + ((fp.flags & 1) ? refresh_phase((u32)t, max_age) : 0));
        else if (e->age_since_intra[t] < 0xffffu) ++e->age_since_intra[t];
        e->tiles[t].age_since_coded = e->age_since_coded[t];
    }
    measure_drift(e, img);
    e->skip_map.clear();
    e->last_slot = slot;
    e->last_frame_number = fp.frame_number;
    e->last_warp_present = fp.warp_present;
    for (u32 eye = 0; eye < g.eyes; ++eye) e->last_warp[eye] = fp.warp[eye];
    e->views_slot[slot] = e->views_cur;
    e->frame_number++;
    return NXVC_OK;
}

// Development hook used by tests/ref/gentables.cpp to regenerate the built-in
// probability tables.  Not part of the public ABI and not declared in nxvc.h.
extern "C" int nxvc_debug_histogram(const nxvc_config *cfg,
                                    const nxvc_image *img,
                                    const uint8_t *qp_map,
                                    const uint8_t *res_map,
                                    uint32_t hist[8][kNumCtx][kNumSym]) {
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(cfg, &st);
    if (!e) return -1;
    const Geometry &g = e->g;
    FrameParams fp = e->fp;
    const int ntiles = (int)(g.tiles_x * g.tiles_y);
    for (int t = 0; t < ntiles; ++t) {
        TileCoder tc;
        tc.g = &g;
        tc.fp = &fp;
        TileParams tp;
        tp.tile_index = t % (int)g.tiles_x;
        tp.mode = NXVC_MODE_INTRA;
        tp.res_level = res_map ? clamp_i32(res_map[t], 0, 2) : 0;
        tp.chroma444 = (g.chroma == NXVC_CHROMA_444 && !e->cfg.tile_chroma420);
        int qp = qp_map ? clamp_i32(qp_map[t], 0, 63) : fp.base_qp;
        tp.qp_delta = clamp_i32(qp - fp.base_qp, -32, 31);
        tp.table_set = clamp_i32((fp.base_qp + tp.qp_delta) >> 3, 0, 7);
        tp.nsub_log2 = 3;
        tp.tskip = e->cfg.transform_skip == 1 ? 1 : 0;
        if (e->cfg.alpha) tp.alpha_mode = 2;
        tc.tp = tp;
        tc.setup();
        load_tile(tc, g, img, 0, t % (int)g.tiles_x, t / (int)g.tiles_x);
        tc.build_units();
        int np = coded_planes(tc);
        for (int p = 0; p < np; ++p)
            analyze_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                          tc.tp.tskip, 1, tc.sdh);
        count_units(tc.units.data(), (int)tc.units.size(), 8,
                    hist[tc.tp.table_set], nullptr);
    }
    nxvc_encoder_destroy(e);
    return 0;
}

// Development hook: per-tile symbol histograms, used by gentables to cluster
// the eight built-in table sets.  Writes ntiles * (kNumCtx * kNumSym) u32.
extern "C" int nxvc_debug_tile_histograms(const nxvc_config *cfg,
                                          const nxvc_image *img,
                                          const uint8_t *res_map, uint32_t *out,
                                          uint32_t max_tiles) {
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(cfg, &st);
    if (!e) return -1;
    const Geometry &g = e->g;
    FrameParams fp = e->fp;
    const int ntiles = (int)(g.tiles_x * g.tiles_y);
    if ((u32)ntiles > max_tiles) { nxvc_encoder_destroy(e); return -1; }
    for (int t = 0; t < ntiles; ++t) {
        TileCoder tc;
        tc.g = &g;
        tc.fp = &fp;
        TileParams tp;
        tp.tile_index = t % (int)g.tiles_x;
        tp.mode = NXVC_MODE_INTRA;
        tp.res_level = res_map ? clamp_i32(res_map[t], 0, 2) : 0;
        tp.chroma444 = (g.chroma == NXVC_CHROMA_444 && !e->cfg.tile_chroma420);
        tp.qp_delta = 0;
        tp.nsub_log2 = 3;
        tp.tskip = e->cfg.transform_skip == 1 ? 1 : 0;
        tc.tp = tp;
        tc.setup();
        load_tile(tc, g, img, 0, t % (int)g.tiles_x, t / (int)g.tiles_x);
        tc.build_units();
        int np = coded_planes(tc);
        if (tc.intra_dir) {
            RateCost rc;
            build_rate_cost(fp.tabs[tc.tp.table_set], rc);
            int mctx = tc.nctx >= kNumCtxV2 ? kCtxMode : 0;
            for (int p = 0; p < np; ++p)
                analyze_plane_dir(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                                  tc.tp.tskip, tc.dir_layer, p == 1 || p == 2,
                                  rc, 0.30, true, 2, mctx, tc.sdh);
        } else {
            for (int p = 0; p < np; ++p)
                analyze_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                              tc.tp.tskip, 1, tc.sdh);
        }
        u32 (*h)[kNumSym] = (u32 (*)[kNumSym])(out + (size_t)t * kNumCtx * kNumSym);
        std::memset(h, 0, sizeof(u32) * kNumCtx * kNumSym);
        count_units(tc.units.data(), (int)tc.units.size(), 8, h, nullptr);
    }
    nxvc_encoder_destroy(e);
    return ntiles;
}

extern "C" nxvc_status nxvc_encoder_stats(const nxvc_encoder *e,
                                          nxvc_encode_stats *out) {
    if (!e || !out) return NXVC_ERR_ARG;
    *out = e->stats;
    return NXVC_OK;
}

extern "C" const nxvc_tile_info *nxvc_encoder_tiles(const nxvc_encoder *e,
                                                    uint32_t *count) {
    if (!e) return nullptr;
    if (count) *count = (u32)e->tiles.size();
    return e->tiles.data();
}

// ================================================================= decoder
extern "C" nxvc_decoder *nxvc_decoder_create(nxvc_status *st) {
    nxvc_decoder *d = nullptr;
    try {
        d = new nxvc_decoder();
    } catch (...) {
        d = nullptr;
    }
    if (st) *st = d ? NXVC_OK : NXVC_ERR_NOMEM;
    return d;
}
extern "C" void nxvc_decoder_destroy(nxvc_decoder *d) { delete d; }

extern "C" nxvc_status nxvc_decoder_parse_stream_header(nxvc_decoder *d,
                                                        const uint8_t *buf,
                                                        size_t len,
                                                        size_t *consumed) {
    if (!d || !buf) return NXVC_ERR_ARG;
    if (len < kStreamHeaderBytes) return NXVC_ERR_TRUNCATED;
    BR br{buf, len, 0, true};
    nxvc_stream_info si{};
    si.magic = br.u32v();
    if (si.magic != kMagic) return NXVC_ERR_VERSION;
    si.version = br.u8v();
    if (si.version != NXVC_VERSION) return NXVC_ERR_VERSION;
    si.profile = br.u8v();
    si.level = br.u8v();
    si.tile_size = br.u8v();
    if (si.tile_size & 0xfe) return NXVC_ERR_BITSTREAM;
    if (si.tile_size & 1) return NXVC_ERR_UNSUPPORTED;  // 32x32 profile
    si.width = br.u16v();
    si.height = br.u16v();
    si.eyes = br.u8v();
    si.bit_depth = br.u8v();
    si.num_layers = br.u8v();
    si.chroma = br.u8v();
    for (int i = 0; i < 4; ++i) si.layer_desc[i] = br.u32v();
    si.tools = br.u64v();
    si.alpha = br.u8v();
    si.color_transform = br.u8v();
    si.color_space = br.u8v();
    br.i = 62;
    si.ext_len = br.u16v();
    if (!br.ok) return NXVC_ERR_TRUNCATED;
    if (len < kStreamHeaderBytes + si.ext_len) return NXVC_ERR_TRUNCATED;
    if (si.width < 16 || si.height < 16 || si.width > 4096 || si.height > 4096 ||
        (si.width & 1) || (si.height & 1))
        return NXVC_ERR_BITSTREAM;
    if (si.eyes < 1 || si.eyes > 2 || si.bit_depth != 8 || si.num_layers != 1)
        return NXVC_ERR_UNSUPPORTED;
    // Annex D D-16: version 1 is 8-bit.  Tool bit 14 is undefined and the
    // depth field cannot say 10 either.
    if (si.tools & NXVC_TOOL_BITDEPTH10) return NXVC_ERR_VERSION;
    // Annex D D-1: `warp_present` and the warped tile modes need the WARP
    // tool, and WARP without INTER says nothing.
    if ((si.tools & NXVC_TOOL_WARP) && !(si.tools & NXVC_TOOL_INTER))
        return NXVC_ERR_BITSTREAM;
    if ((si.tools & NXVC_TOOL_STEREO) && si.eyes != 2)
        return NXVC_ERR_BITSTREAM;
    if (si.chroma > 1 || si.color_transform > 1 || si.alpha > 1 ||
        si.color_space > 3)
        return NXVC_ERR_BITSTREAM;
    if ((si.color_space == NXVC_CS_RGB) !=
        (si.color_transform == NXVC_CT_YCOCGR))
        return NXVC_ERR_BITSTREAM;
    // SYNTAX.md 2: color_transform == 1 requires chroma_format == 1.  YCoCg-R
    // chroma is 9-bit and biased by 256 and the transform runs before
    // subsampling, so a 4:2:0 YCoCg-R stream would push 9-bit chroma through
    // an 8-bit plane.  The encoder already refuses it; so must the decoder.
    if (si.color_transform == NXVC_CT_YCOCGR && si.chroma != NXVC_CHROMA_444)
        return NXVC_ERR_BITSTREAM;
    if (si.tools & ~(u64)NXVC_TOOLS_SUPPORTED) return NXVC_ERR_VERSION;
    // Sign data hiding moves a level by one step; a lossless stream cannot
    // carry it, and a decoder that accepted both would not know which.
    if ((si.tools & NXVC_TOOL_LOSSLESS) && (si.tools & NXVC_TOOL_SIGN_HIDE))
        return NXVC_ERR_BITSTREAM;
    // Walk the TLV area; unknown types are skipped (forward compatibility).
    size_t p = kStreamHeaderBytes, end = kStreamHeaderBytes + si.ext_len;
    while (p + 4 <= end) {
        u32 type = buf[p] | (buf[p + 1] << 8);
        u32 tl = buf[p + 2] | (buf[p + 3] << 8);
        size_t adv = 4 + tl + ((4 - (tl & 3)) & 3);
        if (p + adv > end) return NXVC_ERR_BITSTREAM;
        si.ext_tlv_count++;
        if (type >= 0x8000 || type == 0) si.ext_unknown_count++;
        p += adv;
    }
    if (p != end) return NXVC_ERR_BITSTREAM;

    d->si = si;
    d->g = Geometry{};
    d->g.width = si.width;
    d->g.height = si.height;
    d->g.eyes = si.eyes;
    d->g.chroma = si.chroma;
    d->g.color_transform = si.color_transform;
    d->g.color_space = si.color_space;
    d->g.alpha = si.alpha;
    derive_geometry(d->g);
    if (d->g.tiles_x > kMaxTilesPerRow) return NXVC_ERR_BITSTREAM;
    for (int i = 0; i < 4; ++i) ref_alloc(d->ring.slot[i], d->g);
    d->state.assign(d->g.ntiles(), nxvc::PredState{});
    d->lost.clear();
    d->have_stream = true;
    if (consumed) *consumed = kStreamHeaderBytes + si.ext_len;
    return NXVC_OK;
}

extern "C" nxvc_status nxvc_decoder_stream_info(const nxvc_decoder *d,
                                                nxvc_stream_info *out) {
    if (!d || !out) return NXVC_ERR_ARG;
    if (!d->have_stream) return NXVC_ERR_BITSTREAM;
    *out = d->si;
    return NXVC_OK;
}

extern "C" nxvc_status nxvc_decoder_plane_size(const nxvc_decoder *d, int plane,
                                               uint32_t *w, uint32_t *h) {
    if (!d || !w || !h || plane < 0 || plane > 3) return NXVC_ERR_ARG;
    if (!d->have_stream) return NXVC_ERR_BITSTREAM;
    if (plane == 1 || plane == 2) { *w = d->g.cw * d->g.eyes; *h = d->g.ch; }
    else { *w = d->g.width * d->g.eyes; *h = d->g.height; }
    return NXVC_OK;
}

extern "C" const nxvc_tile_info *nxvc_decoder_tiles(const nxvc_decoder *d,
                                                    uint32_t *count) {
    if (!d) return nullptr;
    if (count) *count = (u32)d->tiles.size();
    return d->tiles.data();
}

extern "C" nxvc_status nxvc_decoder_frame_info(const nxvc_decoder *d,
                                               nxvc_frame_info *out) {
    if (!d || !out) return NXVC_ERR_ARG;
    *out = d->fi;
    return NXVC_OK;
}

namespace nxvc {

// Parse the frame header (and its optional matrix / table payloads).
static nxvc_status parse_frame_header(nxvc_decoder *d, const u8 *buf,
                                      size_t len, FrameParams &fp,
                                      nxvc_frame_info &fi, size_t &off) {
    if (len < kFrameHeaderBytes) return NXVC_ERR_TRUNCATED;
    BR br{buf, len, 0, true};
    fp.frame_number = br.u16v();
    for (int i = 0; i < 26; ++i) fp.pose[i] = (u8)br.u8v();
    fp.base_qp = (int)br.u8v();
    fp.chroma_qp_off = (i8)br.u8v();
    fp.alpha_qp_off = (i8)br.u8v();
    fp.quant_matrix = (int)br.u8v();
    fp.tables_present = br.u8v();
    fp.ref_slots = br.u8v();
    fp.flags = br.u8v();
    br.u8v();  // reserved
    fp.nctx = (d->si.tools & NXVC_TOOL_CTX_V2) ? kNumCtxV2 : kNumCtxV1;
    fp.intra_dir = (d->si.tools & NXVC_TOOL_INTRA_DIR) ? 1 : 0;
    fp.sdh = (d->si.tools & NXVC_TOOL_SIGN_HIDE) ? 1 : 0;
    fp.dir_layer = (fp.flags >> 2) & 1;
    fp.inter = (d->si.tools & NXVC_TOOL_INTER) ? 1 : 0;
    fp.stereo = (d->si.tools & NXVC_TOOL_STEREO) ? 1 : 0;
    fp.warp_dc = (d->si.tools & NXVC_TOOL_WARP_DC) ? 1 : 0;
    fp.mv_quad = (d->si.tools & NXVC_TOOL_MV_QUAD) ? 1 : 0;
    fp.warp_present = (fp.flags >> 3) & 1;
    // flags bit 2 selects the layered form of directional intra; it is
    // meaningless, and therefore illegal, without the tool.
    if (fp.dir_layer && !fp.intra_dir) return NXVC_ERR_BITSTREAM;
    if (fp.flags & 0xf0) return NXVC_ERR_BITSTREAM;   // reserved bits 4-7
    // Annex D D-1: warp_present requires the WARP tool bit.
    if (fp.warp_present && !(d->si.tools & NXVC_TOOL_WARP))
        return NXVC_ERR_BITSTREAM;
    // Annex D D-10: ref_slots is a bitmask and in version 1 it must name
    // exactly the slot this frame's number addresses.  Only an inter stream
    // has a ring for it to describe; see the note in the encoder.
    if (fp.inter && fp.ref_slots != (1u << (fp.frame_number & 3)))
        return NXVC_ERR_BITSTREAM;
    u32 frame_bytes = br.u32v();
    if (!br.ok) return NXVC_ERR_TRUNCATED;
    if (fp.base_qp > 63) return NXVC_ERR_BITSTREAM;
    if (fp.quant_matrix > 3 && fp.quant_matrix != 255)
        return NXVC_ERR_BITSTREAM;
    if (frame_bytes < kFrameHeaderBytes || frame_bytes > len)
        return NXVC_ERR_TRUNCATED;
    off = kFrameHeaderBytes;
    // ---- warp_ext(): 36 bytes per eye, nine little-endian int32, ascending
    // eye order.  Annex D D-1 states four MUST-reject conditions and they are
    // all checked here, before a single sample is predicted.
    if (fp.warp_present) {
        const size_t need = 36u * d->g.eyes;
        if (off + need > frame_bytes) return NXVC_ERR_TRUNCATED;
        BR wr{buf, frame_bytes, off, true};
        for (u32 eye = 0; eye < d->g.eyes; ++eye) {
            for (int i = 0; i < 9; ++i) fp.warp[eye].h[i] = (i32)wr.u32v();
            if (fp.warp[eye].h[8] != kH22) return NXVC_ERR_BITSTREAM;
            for (int i = 0; i < 9; ++i)
                if (fp.warp[eye].h[i] < -(i32)nw::kEntryMax ||
                    fp.warp[eye].h[i] > (i32)nw::kEntryMax)
                    return NXVC_ERR_BITSTREAM;
            // den = h20*cx + h21*cy + h22 is affine in (cx, cy), so the four
            // picture corners bound the whole picture.  Accumulated in 64 bits
            // and required to fit int32 and to lie in [2^28, 2^30).
            const i32 ox = (i32)(d->g.width >> 1), oy = (i32)(d->g.height >> 1);
            const i32 cxs[2] = {-ox, (i32)d->g.width - ox};
            const i32 cys[2] = {-oy, (i32)d->g.height - oy};
            for (int a = 0; a < 2; ++a)
                for (int b = 0; b < 2; ++b) {
                    i64 den = (i64)fp.warp[eye].h[6] * cxs[a] +
                              (i64)fp.warp[eye].h[7] * cys[b] +
                              (i64)fp.warp[eye].h[8];
                    if (den < (i64)nw::kDenMin || den >= (i64)nw::kDenMax)
                        return NXVC_ERR_BITSTREAM;
                }
        }
        off += need;
    }
    const u8 *custom = nullptr;
    if (fp.quant_matrix == 255) {
        if (off + 128 > frame_bytes) return NXVC_ERR_TRUNCATED;
        custom = buf + off;
        off += 128;
    }
    resolve_matrices(fp, custom);
    for (int i = 0; i < 8; ++i) build_default_set(fp.tabs[i], i, fp.nctx);
    for (int k = 0; k < 8; ++k) {
        if (!(fp.tables_present & (1u << k))) continue;
        const size_t tbytes = (size_t)fp.nctx * kNumSym * 5 / 8;
        if (off + tbytes > frame_bytes) return NXVC_ERR_TRUNCATED;
        BitR bitr{buf + off, tbytes, 0};
        if (!parse_table_set(bitr, k, fp.nctx, fp.tabs[k]))
            return NXVC_ERR_BITSTREAM;
        off += tbytes;
    }
    fi.frame_number = fp.frame_number;
    fi.base_qp = fp.base_qp;
    fi.chroma_qp_off = fp.chroma_qp_off;
    fi.alpha_qp_off = fp.alpha_qp_off;
    fi.quant_matrix = fp.quant_matrix;
    fi.tables_present = fp.tables_present;
    fi.ref_slots = fp.ref_slots;
    fi.flags = fp.flags;
    fi.frame_bytes = frame_bytes;
    std::memcpy(fi.pose, fp.pose, 26);
    fi.tile_count = 0;
    fi.warp_present = fp.warp_present;
    for (int eye = 0; eye < 2; ++eye)
        for (int i = 0; i < 9; ++i) fi.warp[eye][i] = fp.warp[eye].h[i];
    return NXVC_OK;
}

}  // namespace nxvc

extern "C" nxvc_status nxvc_decoder_decode_frame(nxvc_decoder *d,
                                                 const uint8_t *buf, size_t len,
                                                 nxvc_image *img,
                                                 size_t *consumed) {
    if (!d || !buf || !img) return NXVC_ERR_ARG;
    if (!d->have_stream) return NXVC_ERR_BITSTREAM;
    const Geometry &g = d->g;
    for (int p = 0; p < g.nplanes(); ++p)
        if (!img->plane[p]) return NXVC_ERR_ARG;

    FrameParams fp;
    size_t off = 0;
    nxvc_status st = parse_frame_header(d, buf, len, fp, d->fi, off);
    if (st != NXVC_OK) return st;
    const size_t frame_bytes = d->fi.frame_bytes;
    const int ntiles = (int)g.ntiles();
    d->tiles.assign(ntiles, nxvc_tile_info{});

    // Annex D D-9: the prediction state is cleared entirely on tile_map_reset.
    if (fp.flags & 1)
        for (auto &ps : d->state) ps = nxvc::PredState{};

    const int slot = (int)(fp.frame_number & 3);
    RefPicture &cur = d->ring.slot[slot];
    // A frame that overwrites a slot invalidates whatever it held; from here
    // on the slot is this frame, filled tile by tile.
    cur.valid = false;
    cur.frame_number = (u16)fp.frame_number;

    auto ref_for = [&](int ref_sel, const RefPicture **out) -> bool {
        const int rs = (int)((fp.frame_number - 1 - (u32)ref_sel) & 3u);
        const RefPicture &rp = d->ring.slot[rs];
        if (!rp.valid || rp.frame_number != (u16)(fp.frame_number - 1 - ref_sel))
            return false;
        *out = &rp;
        return true;
    };

    const std::vector<u8> lost = d->lost;
    d->lost.clear();
    auto tile_lost = [&](int t) {
        return t < (int)lost.size() && lost[t] != 0;
    };

    // Clause 6.11 concealment, and the WARP_SKIP reconstruction it reuses.
    auto reconstruct_skip_tile = [&](int t, int eye, int col, int row,
                                     int mv_x, int mv_y,
                                     const DcCorrection *dc = nullptr)
        -> nxvc_status {
        const RefPicture *rp = nullptr;
        TileCoder tc;
        tc.g = &g;
        tc.fp = &fp;
        tc.tp = TileParams{};
        tc.tp.eye = eye;
        tc.tp.tile_index = col;
        tc.tp.mode = NXVC_MODE_WARP_SKIP;
        tc.tp.chroma444 = (g.chroma == NXVC_CHROMA_444);
        tc.setup();
        if (!ref_for(0, &rp)) {
            // Nothing to conceal from: leave mid-grey, which is what a decoder
            // that has never held a reference can honestly show.
            for (int p = 0; p < tc.nplanes; ++p)
                for (auto &v : tc.pl[p].samples) v = tc.pl[p].dc_off;
        } else {
            predict_tile(tc, g, *rp, fp.warp[eye], eye, col, row,
                         NXVC_MODE_WARP_SKIP, mv_x, mv_y);
            reconstruct_skip(tc, dc);
        }
        store_tile(tc, g, img, eye, col, row);
        store_ref_tile(tc, g, cur, eye, col, row);
        return NXVC_OK;
    };

    for (u32 row = 0; row < g.tiles_y; ++row) {
        for (u32 eye = 0; eye < g.eyes; ++eye) {
            if (off + kTileRowHeaderBytes > frame_bytes)
                return NXVC_ERR_TRUNCATED;
            BR br{buf, frame_bytes, off, true};
            u32 fn = br.u16v();
            u32 ri = br.u8v();
            u32 tc8 = br.u8v();
            // docs/SYNTAX.md 3.3: bit 7 of `tile_count` is `dc_present`.  A
            // row that carries no correction carries no bitmap either, so the
            // WARP_DC tool costs nothing on the rows that do not use it.
            const u32 tcount = tc8 & 0x7fu;
            const u32 dc_present = tc8 >> 7;
            if (dc_present && !fp.warp_dc) return NXVC_ERR_BITSTREAM;
            u64 skip = br.u64v();
            u64 dcmap = dc_present ? br.u64v() : 0ull;
            if (!br.ok) return NXVC_ERR_TRUNCATED;
            // `dc_present` with an empty bitmap has two encodings for one
            // stream; only the short one is legal.
            if (dc_present && dcmap == 0) return NXVC_ERR_BITSTREAM;
            if (fn != fp.frame_number || ri != row) return NXVC_ERR_BITSTREAM;
            // skip_bitmap covers one tile row of ONE eye (Annex D D-3), so the
            // bits above cols_per_eye must be zero.
            if (g.tiles_x < 64 && (skip >> g.tiles_x) != 0)
                return NXVC_ERR_BITSTREAM;
            u32 nskip = 0;
            for (u32 c = 0; c < g.tiles_x; ++c) nskip += (skip >> c) & 1u;
            if (nskip && !fp.inter) return NXVC_ERR_BITSTREAM;
            if (nskip && !fp.warp_present) return NXVC_ERR_BITSTREAM;
            if (tcount != g.tiles_x - nskip) return NXVC_ERR_BITSTREAM;
            // docs/SYNTAX.md 13.9: a near-skip tile IS a skipped tile, so
            // every dc_bitmap bit must name a skipped tile of this row.
            if (dcmap & ~skip) return NXVC_ERR_BITSTREAM;
            off = br.i;
            // The warp_dc() records, in ascending column order, immediately
            // after the row header and before the first tile structure.
            DcCorrection dcrec[kMaxTilesPerRow];
            for (u32 c = 0; c < g.tiles_x; ++c) {
                if (!((dcmap >> c) & 1u)) continue;
                if (off + kWarpDcBytes > frame_bytes) return NXVC_ERR_TRUNCATED;
                for (int q = 0; q < kWarpDcPlanes; ++q) {
                    dcrec[c].dc[q] = (i8)buf[off + 3 * q];
                    dcrec[c].gx[q] = (i8)buf[off + 3 * q + 1];
                    dcrec[c].gy[q] = (i8)buf[off + 3 * q + 2];
                }
                off += kWarpDcBytes;
            }

            for (u32 col = 0; col < g.tiles_x; ++col) {
                const int t = linear_tile(g, (int)row, (int)eye, (int)col);
                nxvc_tile_info &ti = d->tiles[t];
                if ((skip >> col) & 1u) {
                    // A skipped tile carries no structure at all: the
                    // predictor runs with the tile's stored vector.
                    st = reconstruct_skip_tile(
                        t, (int)eye, (int)col, (int)row, d->state[t].last_mv_x,
                        d->state[t].last_mv_y,
                        ((dcmap >> col) & 1u) ? &dcrec[col] : nullptr);
                    if (st != NXVC_OK) return st;
                    ti.tile_index = (u16)col;
                    ti.eye = (u8)eye;
                    ti.mode = NXVC_MODE_WARP_SKIP;
                    ti.skipped = 1;
                    ti.dc_corr = (u8)((dcmap >> col) & 1u);
                    ti.chroma444 = (g.chroma == NXVC_CHROMA_444) ? 1 : 0;
                    ti.mv_x = (i8)d->state[t].last_mv_x;
                    ti.mv_y = (i8)d->state[t].last_mv_y;
                    ti.qp = (u8)clamp_i32(fp.base_qp, 0, 63);
                    // Losing a skipped tile is a no-op: concealment IS the
                    // WARP_SKIP predictor with the same vector, which is the
                    // point of clause 6.11.  It is still reported, so a caller
                    // can account for every tile it marked lost.
                    ti.concealed = tile_lost(t) ? 1 : 0;
                    continue;
                }
                if (off + kTileHeaderBytes > frame_bytes) return NXVC_ERR_TRUNCATED;
                BR tb{buf, frame_bytes, off, true};
                u32 w0 = tb.u32v(), w1 = tb.u32v();
                TileParams tp;
                unpack_tile_header(w0, w1, tp);
                if ((w0 >> 3) & 1) return NXVC_ERR_BITSTREAM;  // reserved bit
                if (w1 >> 29) return NXVC_ERR_BITSTREAM;       // reserved field
                // docs/SYNTAX.md 13.8: mv_quad needs its tool bit and a tile
                // vector to be a delta from, so it lives only on the two
                // vector-carrying temporal modes.
                if (tp.mv_quad) {
                    if (!fp.mv_quad) return NXVC_ERR_BITSTREAM;
                    if (tp.mode != NXVC_MODE_WARP_MV &&
                        tp.mode != NXVC_MODE_STATIC_MV)
                        return NXVC_ERR_BITSTREAM;
                    if (!tp.mv_present) return NXVC_ERR_BITSTREAM;
                }
                if (tp.layer != 0) return NXVC_ERR_UNSUPPORTED;
                // Annex D D-3: the eye field must agree with the eye the
                // tile's position in the frame derives.
                if ((u32)tp.eye != eye) return NXVC_ERR_BITSTREAM;
                if (tp.mode > NXVC_MODE_STEREO) return NXVC_ERR_BITSTREAM;
                if (tp.mode != NXVC_MODE_INTRA && !fp.inter)
                    return NXVC_ERR_UNSUPPORTED;
                if (mode_needs_warp(tp.mode) && !fp.warp_present)
                    return NXVC_ERR_BITSTREAM;
                if (tp.mode == NXVC_MODE_STEREO) {
                    if (!fp.stereo || eye != 1) return NXVC_ERR_BITSTREAM;
                    if (!tp.mv_present) return NXVC_ERR_BITSTREAM;
                }
                // Annex D D-12: ref_sel 3 is reserved; INTRA and STEREO must
                // carry 0 and the decoding process ignores it.
                if (tp.ref_sel == 3) return NXVC_ERR_BITSTREAM;
                if ((tp.mode == NXVC_MODE_INTRA || tp.mode == NXVC_MODE_STEREO) &&
                    tp.ref_sel != 0)
                    return NXVC_ERR_BITSTREAM;
                if (tp.res_level > 2) return NXVC_ERR_BITSTREAM;
                if (tp.alpha_mode == 3) return NXVC_ERR_BITSTREAM;
                if (tp.nsub_log2 > 5) return NXVC_ERR_BITSTREAM;
                if (tp.nsub_log2 != 3 && !(d->si.tools & NXVC_TOOL_NSUB_VAR))
                    return NXVC_ERR_BITSTREAM;
                if (tp.res_level != 0 && !(d->si.tools & NXVC_TOOL_RES_LEVEL))
                    return NXVC_ERR_BITSTREAM;
                if (tp.tskip && !(d->si.tools & NXVC_TOOL_TRANSFORM_SKIP))
                    return NXVC_ERR_BITSTREAM;
                if (tp.wm_id != 0 && !(d->si.tools & NXVC_TOOL_WM_ID))
                    return NXVC_ERR_BITSTREAM;
                if (tp.wm_id != 0 && fp.quant_matrix == 255)
                    return NXVC_ERR_BITSTREAM;
                if (!tp.chroma444 && g.chroma == NXVC_CHROMA_444 &&
                    !(d->si.tools & NXVC_TOOL_PER_TILE_CHROMA))
                    return NXVC_ERR_BITSTREAM;
                if (tp.chroma444 && g.chroma != NXVC_CHROMA_444)
                    return NXVC_ERR_BITSTREAM;
                if (tp.alpha_mode != 0 && !g.alpha) return NXVC_ERR_BITSTREAM;
                if ((u32)tp.tile_index != col) return NXVC_ERR_BITSTREAM;
                off = tb.i;
                if (tp.mv_present) {
                    if (off + 2 > frame_bytes) return NXVC_ERR_TRUNCATED;
                    if (tp.mode == NXVC_MODE_STEREO) {
                        u32 disp = (u32)buf[off] | ((u32)buf[off + 1] << 8);
                        // Annex D D-4: bits 15:12 are reserved.
                        if (disp & 0xf000u) return NXVC_ERR_BITSTREAM;
                        tp.disparity = (int)disp;
                    } else {
                        tp.mv_x = (i8)buf[off];
                        tp.mv_y = (i8)buf[off + 1];
                    }
                    off += 2;
                }
                if (tp.mv_quad) {
                    if (off + kMvQuadBytes > frame_bytes)
                        return NXVC_ERR_TRUNCATED;
                    for (int q = 0; q < 4; ++q) {
                        tp.dmv[q][0] = (i8)buf[off + 2 * q];
                        tp.dmv[q][1] = (i8)buf[off + 2 * q + 1];
                        // The quadrant vector is the tile vector plus its
                        // delta and must stay inside the tile vector's own
                        // signed 8-bit envelope, so one range rule covers
                        // every vector the predictor ever sees.
                        const int ax = tp.mv_x + tp.dmv[q][0];
                        const int ay = tp.mv_y + tp.dmv[q][1];
                        if (ax < -128 || ax > 127 || ay < -128 || ay > 127)
                            return NXVC_ERR_BITSTREAM;
                    }
                    off += kMvQuadBytes;
                }
                if (tp.alpha_mode == 1) {
                    if (off + 1 > frame_bytes) return NXVC_ERR_TRUNCATED;
                    tp.alpha_value = buf[off];
                    off += 1;
                }
                if (off + (size_t)tp.payload_len > frame_bytes)
                    return NXVC_ERR_TRUNCATED;

                const size_t payload_off = off;
                off += tp.payload_len;

                if (tile_lost(t)) {
                    // The tile's bytes are parsed (so the frame stays
                    // self-delimiting) and then discarded: the client does not
                    // hold them.  Clause 6.11 runs instead, with last_mv.
                    st = reconstruct_skip_tile(t, (int)eye, (int)col, (int)row,
                                               d->state[t].last_mv_x,
                                               d->state[t].last_mv_y);
                    if (st != NXVC_OK) return st;
                    ti.tile_index = (u16)col;
                    ti.eye = (u8)eye;
                    ti.mode = NXVC_MODE_WARP_SKIP;
                    ti.skipped = 1;
                    ti.concealed = 1;
                    ti.mv_x = (i8)d->state[t].last_mv_x;
                    ti.mv_y = (i8)d->state[t].last_mv_y;
                    ti.qp = (u8)clamp_i32(fp.base_qp + tp.qp_delta, 0, 63);
                    continue;
                }

                TileCoder tc;
                tc.g = &g;
                tc.fp = &fp;
                tc.tp = tp;
                tc.setup();
                if (tp.mode != NXVC_MODE_INTRA) {
                    const RefPicture *rp = nullptr;
                    if (tp.mode == NXVC_MODE_STEREO) {
                        rp = &cur;   // this frame's already-decoded left eye
                    } else if (!ref_for(tp.ref_sel, &rp)) {
                        return NXVC_ERR_BITSTREAM;
                    }
                    predict_tile(tc, g, *rp, fp.warp[eye],
                                 tp.mode == NXVC_MODE_STEREO ? 0 : (int)eye,
                                 (int)col, (int)row, tp.mode,
                                 tp.mode == NXVC_MODE_STEREO ? tp.disparity
                                                             : tp.mv_x,
                                 tp.mode == NXVC_MODE_STEREO ? 0 : tp.mv_y, 0,
                                 tp.mv_quad ? tp.dmv : nullptr);
                }
                tc.build_units();
                int np = coded_planes(tc);
                if (!decode_units(tc.units.data(), (int)tc.units.size(),
                                  1 << tp.nsub_log2, fp.tabs[tp.table_set],
                                  buf + payload_off, tp.payload_len))
                    return NXVC_ERR_BITSTREAM;
                for (int p = 0; p < np; ++p)
                    reconstruct_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                                      tp.tskip, tc.intra_dir, tc.dir_layer);
                store_tile(tc, g, img, (int)eye, (int)col, (int)row);
                store_ref_tile(tc, g, cur, (int)eye, (int)col, (int)row);
                nxvc::update_pred_state(d->state[t], tp.mode, tp.mv_x, tp.mv_y,
                                        tp.disparity);

                ti.tile_index = (u16)tp.tile_index;
                ti.payload_len = (u16)tp.payload_len;
                ti.layer = (u8)tp.layer;
                ti.eye = (u8)tp.eye;
                ti.mode = (u8)tp.mode;
                ti.res_level = (u8)tp.res_level;
                ti.chroma444 = (u8)tp.chroma444;
                ti.alpha_mode = (u8)tp.alpha_mode;
                ti.table_set = (u8)tp.table_set;
                ti.nsub_log2 = (u8)tp.nsub_log2;
                ti.tskip = (u8)tp.tskip;
                ti.wgt = (u8)tp.wgt;
                ti.wm_id = (u8)tp.wm_id;
                ti.intra_dir = (u8)tc.intra_dir;
                ti.ref_sel = (u8)tp.ref_sel;
                ti.mv_present = (u8)tp.mv_present;
                ti.mv_quad = (u8)tp.mv_quad;
                for (int q = 0; q < 4; ++q) {
                    ti.dmv[q][0] = (i8)tp.dmv[q][0];
                    ti.dmv[q][1] = (i8)tp.dmv[q][1];
                }
                ti.qp_delta = (i8)tp.qp_delta;
                ti.mv_x = (i8)tp.mv_x;
                ti.mv_y = (i8)tp.mv_y;
                ti.disparity = (u16)tp.disparity;
                ti.ref_delta = (tp.mode == NXVC_MODE_INTRA ||
                                tp.mode == NXVC_MODE_STEREO)
                                   ? 3u
                                   : (u8)tp.ref_sel;
                ti.alpha_value = (u8)tp.alpha_value;
                ti.qp = (u8)clamp_i32(fp.base_qp + tp.qp_delta, 0, 63);
            }
        }
    }
    if (off != frame_bytes) return NXVC_ERR_BITSTREAM;
    cur.valid = true;
    d->fi.tile_count = (u32)ntiles;
    if (consumed) *consumed = frame_bytes;
    return NXVC_OK;
}

extern "C" nxvc_status nxvc_decoder_scan_frame(nxvc_decoder *d,
                                               const uint8_t *buf, size_t len,
                                               nxvc_frame_info *fi,
                                               size_t *consumed) {
    if (!d || !buf || !fi) return NXVC_ERR_ARG;
    if (!d->have_stream) return NXVC_ERR_BITSTREAM;
    FrameParams fp;
    size_t off = 0;
    nxvc_status st = parse_frame_header(d, buf, len, fp, d->fi, off);
    if (st != NXVC_OK) return st;
    *fi = d->fi;
    if (consumed) *consumed = d->fi.frame_bytes;
    return NXVC_OK;
}

// =============================================== Phase 2 API (encoder side)
extern "C" nxvc_status nxvc_encoder_set_views(nxvc_encoder *e,
                                              const nxvc_view *views,
                                              uint32_t count) {
    if (!e || !views) return NXVC_ERR_ARG;
    if (count != e->g.eyes) return NXVC_ERR_ARG;
    for (u32 i = 0; i < count; ++i) e->views_cur[i] = views[i];
    e->have_views = true;
    // Before the first encoded frame there is no history, so seed every slot
    // with this view: the first inter frame then sees the true previous view.
    if (e->frame_number == 0)
        for (int i = 0; i < 4; ++i) e->views_slot[i] = e->views_cur;
    return NXVC_OK;
}

extern "C" uint32_t nxvc_encoder_tile_count(const nxvc_encoder *e) {
    return e ? e->g.ntiles() : 0u;
}

extern "C" nxvc_status nxvc_encoder_set_skip_map(nxvc_encoder *e,
                                                 const uint8_t *skip,
                                                 uint32_t count) {
    if (!e || !skip) return NXVC_ERR_ARG;
    if (count != e->g.ntiles()) return NXVC_ERR_ARG;
    e->skip_map.assign(skip, skip + count);
    return NXVC_OK;
}

extern "C" nxvc_status nxvc_encoder_shadow_image(const nxvc_encoder *e,
                                                 nxvc_image *img) {
    if (!e || !img) return NXVC_ERR_ARG;
    const Geometry &g = e->g;
    for (int p = 0; p < g.nplanes(); ++p) {
        if (!img->plane[p]) return NXVC_ERR_ARG;
        const size_t w = (size_t)g.pw(p) * g.eyes, h = g.ph(p);
        for (size_t y = 0; y < h; ++y)
            std::memcpy(img->plane[p] + y * (size_t)img->stride[p],
                        e->shadow_plane[p].data() + y * w, w);
    }
    return NXVC_OK;
}

extern "C" nxvc_status nxvc_encoder_set_received_tiles(nxvc_encoder *e,
                                                       const uint8_t *received,
                                                       uint32_t count) {
    if (!e || !received) return NXVC_ERR_ARG;
    if (count != e->g.ntiles()) return NXVC_ERR_ARG;
    if (e->last_slot < 0) return NXVC_ERR_BITSTREAM;   // nothing encoded yet
    const Geometry &g = e->g;
    FrameParams fp = e->fp;
    fp.frame_number = (u16)e->last_frame_number;
    // The matrix the frame emitted, so the replayed predictor is the client's.
    fp.warp_present = e->last_warp_present;
    for (u32 eye = 0; eye < g.eyes; ++eye) fp.warp[eye] = e->last_warp[eye];
    RefPicture &cur = e->ring.slot[e->last_slot];
    const int prev_slot = (int)((e->last_frame_number - 1) & 3u);
    const RefPicture &prev = e->ring.slot[prev_slot];
    const bool have_prev =
        prev.valid && prev.frame_number == (u16)(e->last_frame_number - 1) &&
        e->last_frame_number > 0;

    for (u32 t = 0; t < count; ++t) {
        if (received[t]) continue;
        const u32 cols = g.eyes * g.tiles_x;
        const int row = (int)(t / cols);
        const int col_all = (int)(t % cols);
        const int eye = col_all / (int)g.tiles_x;
        const int col = col_all % (int)g.tiles_x;
        // Concealing a left-eye tile changes what a STEREO tile of the same
        // row was predicted from, and re-deriving that needs a full-frame
        // replay this reference does not do.  Say so rather than diverge.
        if (e->cfg.stereo && eye == 0) return NXVC_ERR_UNSUPPORTED;
        TileCoder tc;
        tc.g = &g;
        tc.fp = &fp;
        tc.tp = TileParams{};
        tc.tp.eye = eye;
        tc.tp.tile_index = col;
        tc.tp.mode = NXVC_MODE_WARP_SKIP;
        tc.tp.chroma444 = (g.chroma == NXVC_CHROMA_444) ? 1 : 0;
        tc.setup();
        if (!have_prev) {
            for (int p = 0; p < tc.nplanes; ++p)
                for (auto &v : tc.pl[p].samples) v = tc.pl[p].dc_off;
        } else {
            predict_tile(tc, g, prev, fp.warp[eye], eye, col, row,
                         NXVC_MODE_WARP_SKIP, e->state_pre[t].last_mv_x,
                         e->state_pre[t].last_mv_y);
            // A tile that was SKIPPED has no bytes of its own to lose: its
            // vector is stored state and its warp_dc() record travels in the
            // row header, which the transport replicates into every datagram
            // of the row.  So concealing one must reproduce it exactly,
            // correction included, or "losing a skipped tile is a no-op"
            // (clause 6.11) stops being true.  `dc_corr` is only ever set on
            // a skipped tile.
            reconstruct_skip(tc, e->dec[t].dc_corr ? &e->dec[t].dc : nullptr);
        }
        store_tile(tc, g, &e->shadow_img, eye, col, row);
        store_ref_tile(tc, g, cur, eye, col, row);
        // The tile was not decoded, so its prediction state never advanced.
        e->state[t] = e->state_pre[t];
    }
    return NXVC_OK;
}

// =============================================== Phase 2 API (decoder side)
extern "C" nxvc_status nxvc_decoder_set_lost_tiles(nxvc_decoder *d,
                                                   const uint8_t *lost,
                                                   uint32_t count) {
    if (!d || !lost) return NXVC_ERR_ARG;
    if (!d->have_stream) return NXVC_ERR_BITSTREAM;
    if (count != d->g.ntiles()) return NXVC_ERR_ARG;
    d->lost.assign(lost, lost + count);
    return NXVC_OK;
}

extern "C" uint32_t nxvc_decoder_tile_count(const nxvc_decoder *d) {
    return d && d->have_stream ? d->g.ntiles() : 0u;
}
