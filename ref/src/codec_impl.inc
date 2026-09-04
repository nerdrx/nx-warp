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
// Serialize one table set as 12 x 16 five-bit log-domain deltas (120 bytes).
static void serialize_table_set(BitW &bw, const u16 target[kNumCtx][kNumSym],
                                int set_index, TableSet &decoded) {
    for (int c = 0; c < kNumCtx; ++c) {
        u16 f[kNumSym];
        for (int s = 0; s < kNumSym; ++s) {
            i32 def = kDefaultFreq[set_index][c][s];
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

static bool parse_table_set(BitR &br, int set_index, TableSet &out) {
    for (int c = 0; c < kNumCtx; ++c) {
        u16 f[kNumSym];
        for (int s = 0; s < kNumSym; ++s) {
            u32 d = br.get(5);
            i32 def = kDefaultFreq[set_index][c][s];
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

// Origin of the tile in plane p.
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
    if (cfg->chroma > 1 || cfg->color_transform > 1) return fail(NXVC_ERR_ARG);
    if (cfg->base_qp > 63) return fail(NXVC_ERR_ARG);
    if (cfg->color_transform == NXVC_CT_YCOCGR && cfg->chroma != NXVC_CHROMA_444)
        return fail(NXVC_ERR_ARG);
    if (cfg->color_space > 3) return fail(NXVC_ERR_ARG);
    if ((cfg->color_space == NXVC_CS_RGB) !=
        (cfg->color_transform == NXVC_CT_YCOCGR))
        return fail(NXVC_ERR_ARG);
    auto *e = new (std::nothrow) nxvc_encoder();
    if (!e) return fail(NXVC_ERR_NOMEM);
    e->cfg = *cfg;
    if (e->cfg.lossless) {
        e->cfg.base_qp = 0;
        e->cfg.transform_skip = 1;
        e->cfg.quant_matrix = 0;
    }
    e->g.width = cfg->width;
    e->g.height = cfg->height;
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
    for (int i = 0; i < 8; ++i) build_default_set(e->fp.tabs[i], i);
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
    bw.u8v(1);  // eyes
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
                      int tx, int ty) {
    int np = tc.nplanes;
    PlaneRef ref[4];
    for (int p = 0; p < np; ++p) {
        ref[p].p = img->plane[p];
        ref[p].stride = img->stride[p];
        if (p == 1 || p == 2) { ref[p].width = g.cw; ref[p].height = g.ch; }
        else { ref[p].width = g.width; ref[p].height = g.height; }
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
                       int tx, int ty) {
    int np = tc.nplanes;
    const bool ct = (g.color_transform == NXVC_CT_YCOCGR);
    if (ct) {
        // planes 0..2 are converted back to RGB after upsampling
        int ox = tx * 64, oy = ty * 64;
        int f0 = 64 / tc.pl[0].size;
        int f1 = 64 / tc.pl[1].size;
        for (int y = 0; y < 64; ++y) {
            int gy = oy + y;
            if (gy >= (int)g.height) break;
            for (int x = 0; x < 64; ++x) {
                int gx = ox + x;
                if (gx >= (int)g.width) break;
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
        int pw = (p == 1 || p == 2) ? (int)g.cw : (int)g.width;
        int ph = (p == 1 || p == 2) ? (int)g.ch : (int)g.height;
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

}  // namespace nxvc

extern "C" nxvc_status nxvc_encoder_encode_frame(
    nxvc_encoder *e, const nxvc_image *img, const uint8_t *qp_map,
    const uint8_t *res_map, uint8_t *buf, size_t cap, size_t *out_len) {
    if (!e || !img || !buf || !out_len) return NXVC_ERR_ARG;
    const Geometry &g = e->g;
    const int ntiles = (int)(g.tiles_x * g.tiles_y);
    for (int p = 0; p < g.nplanes(); ++p)
        if (!img->plane[p]) return NXVC_ERR_ARG;

    e->tiles.assign(ntiles, nxvc_tile_info{});
    std::memset(&e->stats, 0, sizeof(e->stats));

    // ---- optional custom probability tables: histogram pass
    FrameParams fp = e->fp;
    fp.frame_number = (u16)e->frame_number;
    std::memcpy(fp.pose, e->pose, 26);
    fp.flags = 1;  // tile-map reset: every v1 intra frame
    std::vector<u8> table_bytes;
    // Table-set selection always scores against the built-in defaults so the
    // histogram pass and the encode pass make identical choices.
    static thread_local TableSet deftabs[8];
    for (int k = 0; k < 8; ++k) build_default_set(deftabs[k], k);
    auto select_set = [&](const u32 th[kNumCtx][kNumSym], int fallback) {
        double best = 0;
        int bestk = fallback;
        for (int k = 0; k < 8; ++k) {
            double c = table_set_cost(th, deftabs[k]);
            if (k == 0 || c < best) { best = c; bestk = k; }
        }
        return bestk;
    };

    auto make_tile_params = [&](int t) {
        TileParams tp;
        tp.tile_index = t % (int)g.tiles_x;
        tp.mode = NXVC_MODE_INTRA;
        tp.res_level = res_map ? clamp_i32(res_map[t], 0, 2) : 0;
        if (e->cfg.lossless) tp.res_level = 0;
        tp.chroma444 = (g.chroma == NXVC_CHROMA_444 && !e->cfg.tile_chroma420);
        int qp = qp_map ? clamp_i32(qp_map[t], 0, 63) : fp.base_qp;
        tp.qp_delta = clamp_i32(qp - fp.base_qp, -32, 31);
        tp.table_set = clamp_i32((fp.base_qp + tp.qp_delta) >> 3, 0, 7);
        tp.nsub_log2 = e->cfg.nsub_log2 == 255 ? 3 : (int)e->cfg.nsub_log2;
        tp.tskip = e->cfg.transform_skip == 1 ? 1 : 0;
        return tp;
    };

    if (e->cfg.custom_tables) {
        u32 hist[8][kNumCtx][kNumSym];
        std::memset(hist, 0, sizeof(hist));
        for (int t = 0; t < ntiles; ++t) {
            TileCoder tc;
            tc.g = &g;
            tc.fp = &fp;
            tc.tp = make_tile_params(t);
            tc.setup();
            load_tile(tc, g, img, t % g.tiles_x, t / g.tiles_x);
            if (e->cfg.transform_skip == 2) tc.tp.tskip = tskip_decision(tc);
            if (e->cfg.alpha) tc.tp.alpha_mode = 2;
            tc.build_units();
            int np = coded_planes(tc);
            for (int p = 0; p < np; ++p)
                analyze_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                              tc.tp.tskip, 1);
            u32 th[kNumCtx][kNumSym];
            std::memset(th, 0, sizeof(th));
            if (count_units(tc.units.data(), (int)tc.units.size(),
                            1 << tc.tp.nsub_log2, th, nullptr)) {
                int bestk = select_set(th, tc.tp.table_set);
                for (int c = 0; c < kNumCtx; ++c)
                    for (int sy = 0; sy < kNumSym; ++sy)
                        hist[bestk][c][sy] += th[c][sy];
            }
        }
        BitW bw;
        for (int k = 0; k < 8; ++k) {
            u32 total = 0;
            for (int c = 0; c < kNumCtx; ++c)
                for (int s = 0; s < kNumSym; ++s) total += hist[k][c][s];
            if (total == 0) continue;
            u16 target[kNumCtx][kNumSym];
            for (int c = 0; c < kNumCtx; ++c) {
                u32 sum = 0;
                for (int s = 0; s < kNumSym; ++s) sum += hist[k][c][s];
                for (int s = 0; s < kNumSym; ++s) {
                    if (sum == 0) {
                        target[c][s] = kDefaultFreq[k][c][s];
                    } else {
                        u32 v = (u32)(((u64)hist[k][c][s] * 1024) / sum);
                        target[c][s] = (u16)(v < 1 ? 1 : v);
                    }
                }
            }
            fp.tables_present |= (1u << k);
            serialize_table_set(bw, target, k, fp.tabs[k]);
        }
        bw.flush();
        table_bytes = bw.b;
    }

    // ---- frame body
    BW body;
    for (u32 row = 0; row < g.tiles_y; ++row) {
        BW rowbuf;
        rowbuf.u16v(fp.frame_number);
        rowbuf.u8v(row);
        rowbuf.u8v(g.tiles_x);
        rowbuf.u64v(0);  // skip bitmap: no skips in an all-intra frame
        for (u32 col = 0; col < g.tiles_x; ++col) {
            int t = (int)(row * g.tiles_x + col);
            TileCoder tc;
            tc.g = &g;
            tc.fp = &fp;
            tc.tp = make_tile_params(t);
            tc.setup();
            load_tile(tc, g, img, col, row);
            if (e->cfg.transform_skip == 2) tc.tp.tskip = tskip_decision(tc);
            if (e->cfg.alpha) {
                // constant / opaque alpha detection
                bool constant = true;
                i32 first = tc.pl[3].samples[0];
                for (size_t i = 1; i < tc.pl[3].samples.size(); ++i)
                    if (tc.pl[3].samples[i] != first) { constant = false; break; }
                if (constant)
                    tc.tp.alpha_mode = (first == 255) ? 0 : 1;
                else
                    tc.tp.alpha_mode = 2;
                tc.tp.alpha_value = first;
            }
            tc.build_units();
            int np = coded_planes(tc);
            for (int p = 0; p < np; ++p)
                analyze_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                              tc.tp.tskip, 1);
            // Pick the table set that codes this tile most cheaply.  The
            // eight sets span the QP range, so this is a content-adaptive
            // choice within a fixed, static-per-frame table family.
            {
                u32 th[kNumCtx][kNumSym];
                std::memset(th, 0, sizeof(th));
                if (count_units(tc.units.data(), (int)tc.units.size(),
                                1 << tc.tp.nsub_log2, th, nullptr)) {
                    tc.tp.table_set = select_set(th, tc.tp.table_set);
                    if (e->cfg.nsub_log2 == 255) {
                        // Each lane costs a 4-byte rANS flush.  Spend lanes
                        // only while the flush stays under a tenth of the
                        // payload, and never more than 8 (one subgroup
                        // cluster).  Cheap periphery tiles end up at 1 lane,
                        // fovea tiles at 8.
                        double bits = table_set_cost(th, fp.tabs[tc.tp.table_set]);
                        int n = 0;
                        while (n < 3 && (double)(32 << (n + 1)) <= bits * 0.10)
                            ++n;
                        tc.tp.nsub_log2 = n;
                    }
                }
            }
            if (e->cfg.collect_stats) {
                // Per-unit symbol streams are independent, so a category's
                // cost can be measured by histogramming just its units.
                const TableSet &ts = fp.tabs[tc.tp.table_set];
                int nplanes_c = coded_planes(tc);
                size_t ui = 0;
                for (int p = 0; p < nplanes_c; ++p) {
                    int ndc = tc.pl[p].nb * tc.pl[p].nb;
                    std::vector<Unit> dc(tc.units.begin() + ui,
                                         tc.units.begin() + ui + 1);
                    std::vector<Unit> blk(tc.units.begin() + ui + 1,
                                          tc.units.begin() + ui + 1 + ndc);
                    ui += 1 + ndc;
                    u32 hd[kNumCtx][kNumSym], hb[kNumCtx][kNumSym];
                    std::memset(hd, 0, sizeof(hd));
                    std::memset(hb, 0, sizeof(hb));
                    count_units(dc.data(), 1, 1, hd, nullptr);
                    count_units(blk.data(), ndc, 1, hb, nullptr);
                    e->stats.bits_dc_plane += (u64)table_set_cost(hd, ts);
                    u64 bb = (u64)table_set_cost(hb, ts);
                    if (p == 0) e->stats.bits_luma_blocks += bb;
                    else if (p == 3) e->stats.bits_alpha_blocks += bb;
                    else e->stats.bits_chroma_blocks += bb;
                }
                int active = 1 << tc.tp.nsub_log2;
                if ((size_t)active > tc.units.size()) active = (int)tc.units.size();
                e->stats.bytes_rans_init += 4u * (u32)active;
                e->stats.lanes_total += (u32)active;
                e->stats.tiles++;
                if (tc.tp.tskip) e->stats.tiles_tskip++;
                e->stats.tiles_res[tc.tp.res_level]++;
            }
            std::vector<u8> payload;
            if (!encode_units(tc.units.data(), (int)tc.units.size(),
                              1 << tc.tp.nsub_log2, fp.tabs[tc.tp.table_set],
                              payload))
                return NXVC_ERR_BITSTREAM;
            if (payload.size() > 65535) return NXVC_ERR_NOMEM;
            tc.tp.payload_len = (int)payload.size();
            pack_tile_header(rowbuf, tc.tp);
            if (tc.tp.mv_present) {
                rowbuf.u8v((u8)tc.tp.mv_x);
                rowbuf.u8v((u8)tc.tp.mv_y);
            }
            if (tc.tp.alpha_mode == 1) rowbuf.u8v((u8)tc.tp.alpha_value);
            rowbuf.raw(payload.data(), payload.size());
            if (e->cfg.collect_stats) {
                e->stats.bytes_tile_headers +=
                    kTileHeaderBytes + (tc.tp.mv_present ? 2 : 0) +
                    (tc.tp.alpha_mode == 1 ? 1 : 0);
                e->stats.bytes_payload += payload.size();
            }

            nxvc_tile_info &ti = e->tiles[t];
            ti.tile_index = (u16)tc.tp.tile_index;
            ti.payload_len = (u16)tc.tp.payload_len;
            ti.layer = 0;
            ti.eye = 0;
            ti.mode = (u8)tc.tp.mode;
            ti.res_level = (u8)tc.tp.res_level;
            ti.chroma444 = (u8)tc.tp.chroma444;
            ti.alpha_mode = (u8)tc.tp.alpha_mode;
            ti.table_set = (u8)tc.tp.table_set;
            ti.nsub_log2 = (u8)tc.tp.nsub_log2;
            ti.tskip = (u8)tc.tp.tskip;
            ti.wgt = 0;
            ti.ref_sel = 0;
            ti.mv_present = 0;
            ti.qp_delta = (i8)tc.tp.qp_delta;
            ti.alpha_value = (u8)tc.tp.alpha_value;
            ti.qp = (u8)clamp_i32(fp.base_qp + tc.tp.qp_delta, 0, 63);
        }
        body.raw(rowbuf.b.data(), rowbuf.b.size());
        if (e->cfg.collect_stats) e->stats.bytes_row_headers += kTileRowHeaderBytes;
    }

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
    size_t extra = (fp.quant_matrix == 255 ? 128 : 0) + table_bytes.size();
    u32 total = (u32)(kFrameHeaderBytes + extra + body.size());
    head.u32v(total);
    if (head.size() != kFrameHeaderBytes) return NXVC_ERR_BITSTREAM;

    if (total > cap) return NXVC_ERR_NOMEM;
    u8 *o = buf;
    std::memcpy(o, head.b.data(), head.size());
    o += head.size();
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
            kFrameHeaderBytes + (fp.quant_matrix == 255 ? 128 : 0);
        e->stats.bytes_tables = table_bytes.size();
    }
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
        load_tile(tc, g, img, t % (int)g.tiles_x, t / (int)g.tiles_x);
        tc.build_units();
        int np = coded_planes(tc);
        for (int p = 0; p < np; ++p)
            analyze_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                          tc.tp.tskip, 1);
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
        load_tile(tc, g, img, t % (int)g.tiles_x, t / (int)g.tiles_x);
        tc.build_units();
        int np = coded_planes(tc);
        for (int p = 0; p < np; ++p)
            analyze_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                          tc.tp.tskip, 1);
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
    auto *d = new (std::nothrow) nxvc_decoder();
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
    if (si.eyes != 1 || si.bit_depth != 8 || si.num_layers != 1)
        return NXVC_ERR_UNSUPPORTED;
    if (si.chroma > 1 || si.color_transform > 1 || si.alpha > 1 ||
        si.color_space > 3)
        return NXVC_ERR_BITSTREAM;
    if ((si.color_space == NXVC_CS_RGB) !=
        (si.color_transform == NXVC_CT_YCOCGR))
        return NXVC_ERR_BITSTREAM;
    if (si.tools & ~(u64)NXVC_TOOLS_SUPPORTED) return NXVC_ERR_VERSION;
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
    d->g.chroma = si.chroma;
    d->g.color_transform = si.color_transform;
    d->g.color_space = si.color_space;
    d->g.alpha = si.alpha;
    derive_geometry(d->g);
    if (d->g.tiles_x > kMaxTilesPerRow) return NXVC_ERR_BITSTREAM;
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
    if (plane == 1 || plane == 2) { *w = d->g.cw; *h = d->g.ch; }
    else { *w = d->g.width; *h = d->g.height; }
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
    u32 frame_bytes = br.u32v();
    if (!br.ok) return NXVC_ERR_TRUNCATED;
    if (fp.base_qp > 63) return NXVC_ERR_BITSTREAM;
    if (fp.quant_matrix > 3 && fp.quant_matrix != 255)
        return NXVC_ERR_BITSTREAM;
    if (frame_bytes < kFrameHeaderBytes || frame_bytes > len)
        return NXVC_ERR_TRUNCATED;
    off = kFrameHeaderBytes;
    const u8 *custom = nullptr;
    if (fp.quant_matrix == 255) {
        if (off + 128 > frame_bytes) return NXVC_ERR_TRUNCATED;
        custom = buf + off;
        off += 128;
    }
    resolve_matrices(fp, custom);
    for (int i = 0; i < 8; ++i) build_default_set(fp.tabs[i], i);
    for (int k = 0; k < 8; ++k) {
        if (!(fp.tables_present & (1u << k))) continue;
        if (off + 120 > frame_bytes) return NXVC_ERR_TRUNCATED;
        BitR bitr{buf + off, 120, 0};
        if (!parse_table_set(bitr, k, fp.tabs[k])) return NXVC_ERR_BITSTREAM;
        off += 120;
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
    const int ntiles = (int)(g.tiles_x * g.tiles_y);
    d->tiles.assign(ntiles, nxvc_tile_info{});

    for (u32 row = 0; row < g.tiles_y; ++row) {
        if (off + kTileRowHeaderBytes > frame_bytes) return NXVC_ERR_TRUNCATED;
        BR br{buf, frame_bytes, off, true};
        u32 fn = br.u16v();
        u32 ri = br.u8v();
        u32 tcount = br.u8v();
        u64 skip = br.u64v();
        if (!br.ok) return NXVC_ERR_TRUNCATED;
        if (fn != fp.frame_number || ri != row) return NXVC_ERR_BITSTREAM;
        if (skip != 0) return NXVC_ERR_UNSUPPORTED;  // no references in v1
        if (tcount != g.tiles_x) return NXVC_ERR_BITSTREAM;
        off = br.i;
        for (u32 k = 0; k < tcount; ++k) {
            if (off + kTileHeaderBytes > frame_bytes) return NXVC_ERR_TRUNCATED;
            BR tb{buf, frame_bytes, off, true};
            u32 w0 = tb.u32v(), w1 = tb.u32v();
            TileParams tp;
            unpack_tile_header(w0, w1, tp);
            if ((w0 >> 3) & 1) return NXVC_ERR_BITSTREAM;  // reserved bit
            if (w1 >> 26) return NXVC_ERR_BITSTREAM;       // reserved field
            if (tp.layer != 0 || tp.eye != 0) return NXVC_ERR_UNSUPPORTED;
            if (tp.mode > NXVC_MODE_STEREO) return NXVC_ERR_BITSTREAM;
            if (tp.mode != NXVC_MODE_INTRA) return NXVC_ERR_UNSUPPORTED;
            if (tp.res_level > 2) return NXVC_ERR_BITSTREAM;
            if (tp.alpha_mode == 3) return NXVC_ERR_BITSTREAM;
            if (tp.nsub_log2 > 5) return NXVC_ERR_BITSTREAM;
            if (tp.nsub_log2 != 3 && !(d->si.tools & NXVC_TOOL_NSUB_VAR))
                return NXVC_ERR_BITSTREAM;
            if (tp.res_level != 0 && !(d->si.tools & NXVC_TOOL_RES_LEVEL))
                return NXVC_ERR_BITSTREAM;
            if (tp.tskip && !(d->si.tools & NXVC_TOOL_TRANSFORM_SKIP))
                return NXVC_ERR_BITSTREAM;
            if (!tp.chroma444 && g.chroma == NXVC_CHROMA_444 &&
                !(d->si.tools & NXVC_TOOL_PER_TILE_CHROMA))
                return NXVC_ERR_BITSTREAM;
            if (tp.chroma444 && g.chroma != NXVC_CHROMA_444)
                return NXVC_ERR_BITSTREAM;
            if (tp.alpha_mode != 0 && !g.alpha) return NXVC_ERR_BITSTREAM;
            if ((u32)tp.tile_index != k) return NXVC_ERR_BITSTREAM;
            off = tb.i;
            if (tp.mv_present) {
                if (off + 2 > frame_bytes) return NXVC_ERR_TRUNCATED;
                tp.mv_x = (i8)buf[off];
                tp.mv_y = (i8)buf[off + 1];
                off += 2;
            }
            if (tp.alpha_mode == 1) {
                if (off + 1 > frame_bytes) return NXVC_ERR_TRUNCATED;
                tp.alpha_value = buf[off];
                off += 1;
            }
            if (off + (size_t)tp.payload_len > frame_bytes)
                return NXVC_ERR_TRUNCATED;

            TileCoder tc;
            tc.g = &g;
            tc.fp = &fp;
            tc.tp = tp;
            tc.setup();
            tc.build_units();
            int np = coded_planes(tc);
            if (!decode_units(tc.units.data(), (int)tc.units.size(),
                              1 << tp.nsub_log2, fp.tabs[tp.table_set],
                              buf + off, tp.payload_len))
                return NXVC_ERR_BITSTREAM;
            for (int p = 0; p < np; ++p)
                reconstruct_plane(tc.pl[p], &tc.coef[plane_coef_offset(tc, p)],
                                  tp.tskip);
            store_tile(tc, g, img, (int)k, (int)row);
            off += tp.payload_len;

            int t = (int)(row * g.tiles_x + k);
            nxvc_tile_info &ti = d->tiles[t];
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
            ti.ref_sel = (u8)tp.ref_sel;
            ti.mv_present = (u8)tp.mv_present;
            ti.qp_delta = (i8)tp.qp_delta;
            ti.mv_x = (i8)tp.mv_x;
            ti.mv_y = (i8)tp.mv_y;
            ti.alpha_value = (u8)tp.alpha_value;
            ti.qp = (u8)clamp_i32(fp.base_qp + tp.qp_delta, 0, 63);
        }
    }
    if (off != frame_bytes) return NXVC_ERR_BITSTREAM;
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
