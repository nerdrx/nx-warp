// Conformance vector generator and checker.
//
//   nxv-vectors --generate DIR   write DIR/*.nxv and DIR/vectors.md5
//   nxv-vectors --check DIR      decode each vector and compare MD5s
//
// The manifest pins, for every vector, the MD5 of the bitstream and the MD5 of
// the decoded planes in output order (Y, Co, Cg, [A] per frame).  A future GPU
// decoder is conformant exactly when it reproduces the decoded MD5.
#include "test_util.h"
#include "nxvc/nxvc.h"

// The saturating vector is assembled from hand-made coefficients, so it needs
// the internal coding-unit and rANS interfaces the encoder uses.
#include "common.h"
#include "entropy.h"

struct VecSpec {
    const char *name;
    int w, h;
    int c444;
    int kind;       // make_image kind
    int qp;
    int lossless;
    int alpha;
    int tskip;      // 0 off 1 on 2 auto
    int nsub;       // 0..5, 255 = auto
    int tables;     // custom probability tables
    int t420;       // 4:2:0 tiles inside a 4:4:4 stream
    int ct;         // color transform
    int matrix;
    int res_pattern;  // 0 none, 1 cycling 0/1/2, 2 all level 2
    int qp_pattern;   // 0 none, 1 cycling
    int frames;
    int wm;           // per-tile weighting-matrix id (0 = frame's matrix)
    int raw;          // != 0: built by build_raw(raw) instead of the encoder
    int dir;          // 0 off, 1 directional intra, 2 its layered form
    int ctx;          // 0 = 12 contexts, 1 = 16 (CTX_V2)
    int sdh;          // sign data hiding (tool 22)
    int xf;           // largest transform the encoder may pick (tool 24)
};

static const VecSpec kVectors[] = {
    // name                     w    h  444 kind qp  ll  a  ts nsub tab t420 ct mat res qpp fr
    {"v01_intra420_qp12",      192, 128, 0,  1, 12,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v02_intra420_qp24",      192, 128, 0,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v03_intra420_qp36",      192, 128, 0,  1, 36,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v04_intra420_qp51",      192, 128, 0,  1, 51,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v05_intra444_qp24",      192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v06_gradient420_qp20",   192, 128, 0,  0, 20,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v07_checker420_qp28",    192, 128, 0,  2, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v08_noise420_qp28",      192, 128, 0,  3, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v09_flat420_qp28",       192, 128, 0,  4, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v10_lossless420",        192, 128, 0,  1,  0,  1, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v11_lossless444",        192, 128, 1,  1,  0,  1, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v12_lossless444_alpha",  192, 128, 1,  2,  0,  1, 1,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v13_tskip420_qp16",      192, 128, 0,  2, 16,  0, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v14_alpha420_qp24",      192, 128, 0,  1, 24,  0, 1,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v15_res_cycle420",       192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  1,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v16_res_level2_420",     192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  2,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v17_res_cycle444",       192, 128, 1,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  1,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v18_qpmap420",           192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  1, 1, 0, 0, 0, 0, 0, 0},
    {"v19_qp_res_map420",      192, 128, 0,  1, 30,  0, 0,  0,  3,  0,  0, 0,  2,  1,  1, 1, 0, 0, 0, 0, 0, 0},
    {"v20_tile420_in444",      192, 128, 1,  1, 26,  0, 0,  0,  3,  0,  1, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v21_ycocgr444_qp24",     192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 1,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v22_ycocgr_lossless",    192, 128, 1,  1,  0,  1, 0,  1,  3,  0,  0, 1,  0,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v23_custom_tables420",   192, 128, 0,  1, 28,  0, 0,  0,  3,  1,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v24_nsub0_420",          192, 128, 0,  1, 28,  0, 0,  0,  0,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v25_nsub5_420",          192, 128, 0,  1, 28,  0, 0,  0,  5,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v26_nsub_auto_420",      192, 128, 0,  1, 28,  0, 0,  0,255,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v27_matrix0_420",        192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v28_matrix3_420",        192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  3,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v29_odd_size_200x140",   200, 140, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v30_tiny_64x64",          64,  64, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v31_wide_320x64",        320,  64, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 0},
    {"v32_multiframe420",      128, 128, 0,  1, 30,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 3, 0, 0, 0, 0, 0, 0},
    // v1.2 additions.
    {"v33_wm_id444",           192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 2, 0, 0, 0, 0, 0},
    {"v34_wm_id420_tables",    192, 128, 0,  1, 30,  0, 0,  0,  3,  1,  0, 0,  2,  0,  0, 1, 3, 0, 0, 0, 0, 0},
    // Hand-built: every dequantized coefficient saturates the int16 clamp, so
    // the IDCT runs at its documented worst case (SYNTAX.md 6.3).  This is the
    // vector that pins the odd-part rotation's range; the reference decoder
    // must run it clean under -fsanitize=undefined (ctest ref.saturate).
    {"v35_saturate420",         64,  64, 0,  4, 63,  0, 0,  0,  3,  0,  0, 0,  2,  0,  0, 1, 0, 1, 0, 0, 0, 0},
    // v1.3 additions: the v2 intra tools.  dir/ctx are the last two columns.
    {"v36_dir444_qp16",        192, 128, 1,  1, 16,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 1, 0, 0, 0},
    {"v37_dir420_qp28",        192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 1, 0, 0, 0},
    {"v38_dir_ctxv2_444",      192, 128, 1,  1, 20,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 1, 1, 0, 0},
    {"v39_ctxv2_only_420",     192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 1, 0, 0},
    {"v40_dir_layer420",       192, 128, 0,  2, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 2, 0, 0, 0},
    // every v2 feature at once: layered modes, 16 contexts, transmitted
    // tables (160 bytes a set now), res_level cycling and 1 lane per tile.
    {"v41_dir_ctxv2_tables",   192, 128, 1,  2, 22,  0, 0,  0,  0,  1,  0, 0,  1,  1,  0, 2, 0, 0, 1, 1, 0, 0},
    {"v42_dir_res_tskip420",   192, 128, 0,  2, 18,  0, 0,  1,255,  1,  0, 0,  0,  1,  1, 1, 0, 0, 1, 1, 0, 0},
    // sign data hiding, alone and stacked on the rest.  `sdh` is the last
    // column; v44 is the encoder's shipped default configuration.
    {"v43_sdh_only420",        192, 128, 0,  1, 20,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 1, 0},
    {"v44_default444",         192, 128, 1,  1, 20,  0, 0,  0,255,  1,  0, 0,  1,  0,  0, 1, 0, 0, 1, 1, 1, 0},
    // v1.5 additions: the larger transforms (tool 24).  `xf` is the last
    // column and is the largest size the encoder may pick per tile, so a
    // vector exercises the RD choice as well as the transform itself.  v45 to
    // v56 are the inter set and are unchanged.
    // The image kind and QP are chosen so that between them the six vectors
    // carry every transform size and the per-tile mix: v57 is 16x16 in every
    // tile, v58 mixes 32x32 with 8x8, v59 is 32x32 in every tile.
    {"v57_xform16_420",        192, 128, 0,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 1},
    {"v58_xform32_420",        192, 128, 0,  0, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 2},
    {"v59_xform32_444",        192, 128, 1,  0, 16,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0, 0, 0, 0, 2},
    // res_level cycling makes the per-plane cap of SYNTAX.md 6.7 bite: a
    // res_level 2 tile's chroma plane is 8 samples wide and stays at 8x8
    // whatever the tile header says.
    {"v60_xform32_res420",     192, 128, 0,  1, 26,  0, 0,  0,255,  1,  0, 0,  1,  1,  0, 1, 0, 0, 0, 0, 0, 2},
    // INTRA_DIR and XFORM_LARGE both declared: every tile chooses one.
    {"v61_xform_dir444",       192, 128, 1,  2, 18,  0, 0,  0,255,  1,  0, 0,  1,  0,  0, 1, 0, 0, 1, 1, 1, 2},
    // Hand-built, the 32x32 counterpart of v35: every dequantized coefficient
    // saturates the int16 clamp with the signs aligned to a column of T_32,
    // which is the pattern that attains the 3.5e8 pass-1 bound of SYNTAX.md
    // 6.2.  Run under -fsanitize=undefined (ctest ref.saturate).
    {"v62_xform32_saturate",    64,  64, 0,  4, 63,  0, 0,  0,  3,  0,  0, 0,  2,  0,  0, 1, 0, 2, 0, 0, 0, 2},
};
static const int kNumVectors = (int)(sizeof(kVectors) / sizeof(kVectors[0]));

struct Result {
    std::vector<uint8_t> stream;
    std::string stream_md5, decoded_md5;
    bool ok = false;
    std::string err;
};

// ---------------------------------------------------------------- raw build
// Replace a one-tile frame's payload with hand-made coefficients whose
// dequantized values all saturate the int16 clamp, so that the inverse
// transform runs at the worst case SYNTAX.md 6.3 documents.  The pattern
// (+q, -q alternating over the odd frequencies) is the one that maximises the
// odd-part rotation operand |P + Q|; with QP 63 and matrix 2's weights any
// |level| above about 12 already saturates, so 40 is used throughout.
static void fill_saturating(nxvc::i16 *coef, int ncoef) {
    for (int i = 0; i < ncoef; ++i) {
        int v = ((i >> 1) & 1) ? -40 : 40;
        coef[i] = (nxvc::i16)((i & 1) ? v : 0);
    }
    coef[0] = 40;  // a nonzero DC as well
}

static bool build_saturating_payload(std::vector<uint8_t> &out) {
    using namespace nxvc;
    // 64x64 4:2:0 tile at res_level 0: luma nb = 8, chroma nb = 4.
    const int nbl = 8, nbc = 4;
    const int nunits = (1 + nbl * nbl) + 2 * (1 + nbc * nbc);
    std::vector<i16> coef((size_t)(nbl * nbl + nbl * nbl * 64) +
                          2 * (size_t)(nbc * nbc + nbc * nbc * 64), 0);
    std::vector<Unit> units;
    size_t off = 0;
    for (int p = 0; p < 3; ++p) {
        const int nb = (p == 0) ? nbl : nbc;
        const bool chroma = (p != 0);
        const int ndc = nb * nb;
        Unit u{};
        u.coef = &coef[off];
        u.ncoef = (u16)ndc;
        u.scan = scan_table(ndc, false);
        u.ctx_cbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
        u.ctx_last = chroma ? kCtxLastChroma : kCtxLastLuma;
        fill_saturating(u.coef, ndc);
        units.push_back(u);
        off += ndc;
        for (int b = 0; b < ndc; ++b) {
            Unit w{};
            w.coef = &coef[off];
            w.ncoef = 64;
            w.scan = scan_table(64, false);
            w.ctx_cbf = u.ctx_cbf;
            w.ctx_last = u.ctx_last;
            fill_saturating(w.coef, 64);
            units.push_back(w);
            off += 64;
        }
    }
    if ((int)units.size() != nunits) return false;
    TableSet ts;
    build_default_set(ts, 0);
    return encode_units(units.data(), nunits, 8, ts, out);
}

// The 32x32 counterpart.  All levels have the SAME sign, which is the pattern
// aligned with column 0 of T_32 -- every entry of that column is positive --
// and is therefore the one that attains the 3.5e8 inverse pass-1 bound of
// SYNTAX.md 6.2.  The DC planes keep the 8x8 grid (7.7) and the v35 pattern.
static bool build_saturating_payload_x32(std::vector<uint8_t> &out) {
    using namespace nxvc;
    // 64x64 4:2:0 tile at res_level 0 with xform == 2: luma is 64 wide so it
    // is 2x2 blocks of 32x32, chroma is 32 wide so it is one block; both have
    // 16 coefficient groups per block.  The DC planes are unchanged: nb 8 and
    // nb 4 (SYNTAX.md 7.7).
    const int nbl = 8, nbc = 4;
    const int ngrp = 16;
    const int nunits = (1 + nbl * nbl) + 2 * (1 + nbc * nbc);
    std::vector<i16> coef((size_t)(nbl * nbl + nbl * nbl * 64) +
                          2 * (size_t)(nbc * nbc + nbc * nbc * 64), 0);
    std::vector<Unit> units;
    size_t off = 0;
    for (int p = 0; p < 3; ++p) {
        const int nb = (p == 0) ? nbl : nbc;
        const bool chroma = (p != 0);
        const int ndc = nb * nb;
        Unit u{};
        u.coef = &coef[off];
        u.ncoef = (u16)ndc;
        u.scan = scan_table(ndc, false);
        u.ctx_cbf = chroma ? kCtxCbfChroma : kCtxCbfLuma;
        u.ctx_last = chroma ? kCtxLastChroma : kCtxLastLuma;
        fill_saturating(u.coef, ndc);
        units.push_back(u);
        off += ndc;
        for (int b = 0; b < ndc; ++b) {
            Unit w{};
            w.coef = &coef[off];
            w.ncoef = 64;
            w.scan = scan_table(64, false);
            w.ctx_cbf = u.ctx_cbf;
            w.ctx_last = u.ctx_last;
            w.band_min = (u8)group_band_min(b % ngrp);
            for (int i = 0; i < 64; ++i) w.coef[i] = 40;
            units.push_back(w);
            off += 64;
        }
    }
    if ((int)units.size() != nunits) return false;
    TableSet ts;
    build_default_set(ts, 0);
    return encode_units(units.data(), nunits, 8, ts, out);
}

// raw == 1: the 8x8 saturating vector; raw == 2: its 32x32 counterpart.
// Everything but the tile payload comes from a real encode of the same
// geometry, so the headers stay exactly conformant.
static Result build_raw(const VecSpec &v, Result base) {
    Result r;
    if (!base.ok && base.err.empty()) base.err = "base build failed";
    if (!base.err.empty()) { r.err = base.err; return r; }
    std::vector<uint8_t> payload;
    const bool ok = v.raw == 2 ? build_saturating_payload_x32(payload)
                               : build_saturating_payload(payload);
    if (!ok) { r.err = "payload build"; return r; }
    if (payload.size() > 65535) { r.err = "payload too long"; return r; }
    // Layout for one 64x64 tile with no custom matrices or tables:
    //   [64 stream header][40 frame header][12 row header][8 tile header][payload]
    const size_t kSH = 64, kFH = 40, kRH = 12, kTH = 8;
    if (base.stream.size() < kSH + kFH + kRH + kTH) { r.err = "short base"; return r; }
    std::vector<uint8_t> out(base.stream.begin(),
                             base.stream.begin() + kSH + kFH + kRH + kTH);
    // tile header word0: keep layer/eye/tile_index, set the new payload_len.
    uint32_t w0 = 0;
    for (int i = 0; i < 4; ++i) w0 |= (uint32_t)out[kSH + kFH + kRH + i] << (8 * i);
    w0 = (w0 & 0x0000ffffu) | ((uint32_t)payload.size() << 16);
    for (int i = 0; i < 4; ++i) out[kSH + kFH + kRH + i] = (uint8_t)(w0 >> (8 * i));
    // tile header word1: force table_set 0 and 8 lanes, keep the rest.
    uint32_t w1 = 0;
    for (int i = 0; i < 4; ++i) w1 |= (uint32_t)out[kSH + kFH + kRH + 4 + i] << (8 * i);
    w1 &= ~((7u << 14) | (7u << 17) | (3u << 28));
    w1 |= (3u << 17);
    if (v.raw == 2) w1 |= (2u << 28);   // xform = 2 (32x32)
    for (int i = 0; i < 4; ++i) out[kSH + kFH + kRH + 4 + i] = (uint8_t)(w1 >> (8 * i));
    out.insert(out.end(), payload.begin(), payload.end());
    // frame header frame_bytes at offset 36 of the frame header.
    uint32_t fb = (uint32_t)(out.size() - kSH);
    for (int i = 0; i < 4; ++i) out[kSH + 36 + i] = (uint8_t)(fb >> (8 * i));
    r.stream = out;
    r.ok = true;
    return r;
}

static Result build(const VecSpec &v) {
    Result r;
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)v.w;
    cfg.height = (uint32_t)v.h;
    cfg.chroma = v.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)v.qp;
    cfg.lossless = (uint32_t)v.lossless;
    cfg.alpha = (uint32_t)v.alpha;
    cfg.transform_skip = (uint32_t)v.tskip;
    cfg.nsub_log2 = (uint32_t)v.nsub;
    cfg.custom_tables = (uint32_t)v.tables;
    cfg.tile_chroma420 = (uint32_t)v.t420;
    cfg.color_transform = (uint32_t)v.ct;
    cfg.color_space = v.ct ? (uint32_t)NXVC_CS_RGB : (uint32_t)NXVC_CS_YCBCR_709_LIMITED;
    cfg.quant_matrix = (uint32_t)v.matrix;
    cfg.wm_id = (uint32_t)v.wm;
    cfg.intra_dir = v.dir ? 1u : 0u;
    cfg.intra_dir_layer = v.dir == 2 ? 1u : 0u;
    cfg.ctx_v2 = (uint32_t)v.ctx;
    cfg.sign_hide = (uint32_t)v.sdh;
    cfg.xform_large = (uint32_t)v.xf;

    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { r.err = nxvc_status_string(st); return r; }
    // A fixed, non-zero pose so the field is exercised by the vectors.
    uint8_t pose[26];
    for (int i = 0; i < 26; ++i) pose[i] = (uint8_t)(0x10 + i * 7);
    nxvc_encoder_set_pose(e, pose);

    std::vector<uint8_t> buf(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, buf.data(), buf.size(), &hl);
    if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
    r.stream.assign(buf.begin(), buf.begin() + hl);

    nxvc_tile_layout tl;
    nxvc_tile_layout_get(cfg.width, cfg.height, &tl);
    std::vector<uint8_t> qmap(tl.tile_count), rmap(tl.tile_count);
    for (uint32_t i = 0; i < tl.tile_count; ++i) {
        qmap[i] = (uint8_t)(10 + (i * 5) % 45);
        rmap[i] = v.res_pattern == 1 ? (uint8_t)(i % 3) : 2;
    }

    std::vector<uint8_t> fbuf((size_t)v.w * v.h * 8 + (1u << 20));
    for (int f = 0; f < v.frames; ++f) {
        TestImage im = make_image(v.w, v.h, v.c444 != 0, v.kind,
                                  (uint32_t)(1000 + f * 37 + v.kind));
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = im.w; img.stride[1] = im.cw;
        img.stride[2] = im.cw; img.stride[3] = im.w;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(
            e, &img, v.qp_pattern ? qmap.data() : nullptr,
            v.res_pattern ? rmap.data() : nullptr, fbuf.data(), fbuf.size(), &ol);
        if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
        r.stream.insert(r.stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);
    if (v.raw) {
        Result base;
        base.stream = r.stream;
        base.ok = true;
        Result rr = build_raw(v, base);
        if (!rr.ok) { r.err = rr.err; return r; }
        r.stream = rr.stream;
    }
    r.stream_md5 = md5_hex(r.stream.data(), r.stream.size());

    // Decode it back and hash the output planes.
    nxvc_decoder *d = nxvc_decoder_create(&st);
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, r.stream.data(), r.stream.size(),
                                          &consumed);
    if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
    off = consumed;
    nxvc_stream_info si;
    nxvc_decoder_stream_info(d, &si);
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
        V((size_t)cw * ch), A((size_t)yw * yh, 255);
    MD5 md;
    int frames = 0;
    while (off < r.stream.size()) {
        nxvc_image oi{};
        oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
        oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
        oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
        oi.plane[3] = A.data(); oi.stride[3] = (int)yw;
        st = nxvc_decoder_decode_frame(d, r.stream.data() + off,
                                       r.stream.size() - off, &oi, &consumed);
        if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
        md.update(Y.data(), Y.size());
        md.update(U.data(), U.size());
        md.update(V.data(), V.size());
        if (si.alpha) md.update(A.data(), A.size());
        off += consumed;
        ++frames;
    }
    nxvc_decoder_destroy(d);
    if (frames != v.frames) { r.err = "frame count mismatch"; return r; }
    r.decoded_md5 = md.hex();
    r.ok = true;
    return r;
}


// ------------------------------------------------------- Phase 2 vectors
// The twelve inter vectors spec/annex-d-inter-decisions.md D-21 asks for.
// D-21 names them by what they must fix, not by how they must be produced;
// each row below says which entry it is.  Where D-21's entry is a *property*
// rather than a stream -- "STATIC_MV must not read the matrix", "an integer
// vector under identity equals a shifted tile" -- the property is asserted in
// tests/ref/test_inter.cpp and warp/'s own suite, and the vector here pins the
// bitstream and the reconstruction that the property holds for.
struct InterSpec {
    const char *name;
    const char *fixes;      // the D-21 entry
    int eye_w, h;
    int eyes;
    int c444;
    int qp;
    int frames;
    int stereo;
    int iperiod;            // rolling intra refresh period
    int ref_sel;
    double yaw;             // degrees per frame in the pose log
    double pan;             // picture shift per frame, samples
    int obj;                // moving-disc speed
    int disparity;          // per-eye horizontal offset
    int salt;               // per-frame content reseed (new content everywhere)
};

static const InterSpec kInterVectors[] = {
    // name                     fixes                        w    h  ey 444 qp fr st per rs   yaw   pan obj disp salt
    {"v45_inter_identity",      "inter/identity",           128, 128, 1, 1, 24, 4, 0, 999, 0,  0.0,  0.0, 0,  0, 0},
    {"v46_inter_warp_mv",       "inter/integer_mv",         128, 128, 1, 1, 26, 5, 0, 999, 0,  0.7,  2.0, 3,  0, 0},
    {"v47_inter_static_mv",     "inter/static_mv",          128, 128, 1, 1, 26, 4, 0, 999, 0, 12.0,  0.0, 0,  0, 0},
    {"v48_inter_warp_sweep",    "inter/warp_sweep",         128, 128, 1, 1, 28, 6, 0, 999, 0,  4.5,  6.0, 2,  0, 0},
    {"v49_inter_warp_border",   "inter/warp_border",        128,  64, 1, 1, 28, 5, 0, 999, 0,  9.0, 14.0, 5,  0, 0},
    {"v50_inter_skip_state",    "inter/skip",               128, 128, 1, 1, 22, 4, 0, 999, 0,  0.2,  0.5, 1,  0, 0},
    {"v51_inter_ref_sel1",      "inter/ref_sel",            128, 128, 1, 1, 26, 6, 0, 999, 1,  0.5,  1.0, 2,  0, 0},
    {"v52_inter_ref_sel2",      "inter/ref_sel",            128, 128, 1, 1, 26, 7, 0, 999, 2,  0.5,  1.0, 2,  0, 0},
    {"v53_inter_stereo",        "inter/stereo",             128, 128, 2, 1, 24, 4, 1, 999, 0,  0.0,  0.0, 0, 11, 1},
    {"v54_inter_stereo_static", "inter/stereo_static_equiv",128, 128, 2, 1, 24, 4, 0, 999, 0,  0.0,  0.0, 0, 11, 1},
    {"v55_inter_420",           "inter/warp_sweep (4:2:0)", 128, 128, 1, 0, 26, 5, 0, 999, 0,  1.5,  3.0, 3,  0, 0},
    {"v56_inter_refresh",       "inter/skip (refresh)",     128, 128, 1, 1, 26, 8, 0,   4, 0,  0.4,  1.0, 2,  0, 0},
};
static const int kNumInterVectors =
    (int)(sizeof(kInterVectors) / sizeof(kInterVectors[0]));

// The material: a textured plane sampled through a per-frame translation, a
// disc moving independently of it, and a per-eye horizontal offset.  It is
// generated here rather than shared with test_inter.cpp so that changing a
// test can never move a committed conformance digest.
static int vec_tex(int x, int y) {
    double v = 128 + 55 * std::sin(x * 0.031) * std::cos(y * 0.027) +
               30 * std::sin((x * 3 + y * 5) * 0.11) +
               18 * std::sin((double)(x * x + y * y) * 0.00042);
    v += ((x / 13 + y / 11) % 2) ? 12 : -12;
    return v < 0 ? 0 : (v > 255 ? 255 : (int)v);
}

struct InterFrame {
    int w = 0, h = 0, cw = 0, ch = 0;
    std::vector<uint8_t> Y, U, V;
};

static InterFrame make_inter_frame(const InterSpec &v, int f) {
    InterFrame s;
    s.w = v.eye_w * v.eyes;
    s.h = v.h;
    s.cw = v.c444 ? s.w : (s.w + 1) / 2;
    s.ch = v.c444 ? s.h : (s.h + 1) / 2;
    s.Y.assign((size_t)s.w * s.h, 0);
    s.U.assign((size_t)s.cw * s.ch, 128);
    s.V.assign((size_t)s.cw * s.ch, 128);
    const int obj_x = 20 + v.obj * f, obj_y = 40 + (f % 3);
    for (int e = 0; e < v.eyes; ++e) {
        const int ex = e * v.disparity;
        for (int y = 0; y < v.h; ++y)
            for (int x = 0; x < v.eye_w; ++x) {
                int sx = (int)std::lround(x + v.pan * f) + ex + v.salt * f * 37;
                int sy = (int)std::lround(y) + v.salt * f * 11;
                int val = vec_tex(sx, sy);
                const double dx = x - obj_x, dy = y - obj_y;
                if (v.obj && dx * dx + dy * dy < 17.0 * 17.0)
                    val = 235 - (int)(dx * dx) % 90;
                s.Y[(size_t)y * s.w + e * v.eye_w + x] = (uint8_t)val;
            }
        const int cew = v.c444 ? v.eye_w : v.eye_w / 2;
        const int cf = v.c444 ? 1 : 2;
        for (int y = 0; y < s.ch; ++y)
            for (int x = 0; x < cew; ++x) {
                s.U[(size_t)y * s.cw + e * cew + x] =
                    (uint8_t)(110 + (vec_tex(x * cf + ex, y * cf) >> 3));
                s.V[(size_t)y * s.cw + e * cew + x] =
                    (uint8_t)(140 - (vec_tex(y * cf, x * cf + ex) >> 3));
            }
    }
    return s;
}

static nxvc_view vec_view_yaw(double deg) {
    nxvc_view v{};
    const double a = deg * 3.14159265358979323846 / 360.0;
    v.qy = std::sin(a);
    v.qw = std::cos(a);
    const double fh = 95.0 * 3.14159265358979323846 / 360.0;
    v.fov_left = -fh;
    v.fov_right = fh;
    v.fov_up = fh;
    v.fov_down = -fh;
    return v;
}

static Result build_inter(const InterSpec &v) {
    Result r;
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)v.eye_w;
    cfg.height = (uint32_t)v.h;
    cfg.eyes = (uint32_t)v.eyes;
    cfg.chroma = v.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)v.qp;
    cfg.inter = 1;
    cfg.stereo = (uint32_t)v.stereo;
    cfg.intra_period = (uint32_t)v.iperiod;
    cfg.ref_sel = (uint32_t)v.ref_sel;
    cfg.custom_tables = 0;

    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { r.err = nxvc_status_string(st); return r; }
    uint8_t pose[26];
    for (int i = 0; i < 26; ++i) pose[i] = (uint8_t)(0x10 + i * 7);
    nxvc_encoder_set_pose(e, pose);

    std::vector<uint8_t> buf(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, buf.data(), buf.size(), &hl);
    if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
    r.stream.assign(buf.begin(), buf.begin() + hl);

    const size_t px = (size_t)v.eye_w * v.eyes * v.h;
    std::vector<uint8_t> fbuf(px * 8 + (1u << 20));
    for (int f = 0; f < v.frames; ++f) {
        InterFrame s = make_inter_frame(v, f);
        nxvc_image img{};
        img.plane[0] = s.Y.data(); img.stride[0] = s.w;
        img.plane[1] = s.U.data(); img.stride[1] = s.cw;
        img.plane[2] = s.V.data(); img.stride[2] = s.cw;
        nxvc_view views[2];
        for (int k = 0; k < v.eyes; ++k) views[k] = vec_view_yaw(v.yaw * f);
        nxvc_encoder_set_views(e, views, (uint32_t)v.eyes);
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fbuf.data(),
                                       fbuf.size(), &ol);
        if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
        r.stream.insert(r.stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);
    r.stream_md5 = md5_hex(r.stream.data(), r.stream.size());

    nxvc_decoder *d = nxvc_decoder_create(&st);
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, r.stream.data(), r.stream.size(),
                                          &consumed);
    if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
    off = consumed;
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch), V((size_t)cw * ch);
    MD5 md;
    int frames = 0;
    while (off < r.stream.size()) {
        nxvc_image oi{};
        oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
        oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
        oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
        st = nxvc_decoder_decode_frame(d, r.stream.data() + off,
                                       r.stream.size() - off, &oi, &consumed);
        if (st != NXVC_OK) { r.err = nxvc_status_string(st); return r; }
        md.update(Y.data(), Y.size());
        md.update(U.data(), U.size());
        md.update(V.data(), V.size());
        off += consumed;
        ++frames;
    }
    nxvc_decoder_destroy(d);
    if (frames != v.frames) { r.err = "frame count mismatch"; return r; }
    r.decoded_md5 = md.hex();
    r.ok = true;
    return r;
}


// ------------------------------------------- Phase 2 rejection vectors
// D-21 again: "the rejection vectors matter more than the positive ones".
// Each is a legal inter stream with exactly one field corrupted, and the
// offsets are found by walking the frame rather than hard-coded, so a change
// in the encoder's mode decision cannot silently make a vector patch the
// wrong byte.
struct FrameWalk {
    size_t frame_off = 0;      // start of the frame unit
    size_t warp_off = 0;       // warp_ext(), 0 if absent
    size_t row0_off = 0;       // first tile-row header
    int eyes = 1, cols = 1, rows = 1;
    bool warp_present = false;
};

static uint32_t rd_u32(const std::vector<uint8_t> &b, size_t o) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)b[o + i] << (8 * i);
    return v;
}
static uint64_t rd_u64(const std::vector<uint8_t> &b, size_t o) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)b[o + i] << (8 * i);
    return v;
}

// Offset of frame `n` (0-based) in a stream whose header is `hdr_len` bytes.
static bool frame_offset(const std::vector<uint8_t> &b, size_t hdr_len, int n,
                         size_t *out) {
    size_t off = hdr_len;
    for (int i = 0; i < n; ++i) {
        if (off + 40 > b.size()) return false;
        off += rd_u32(b, off + 36);
    }
    if (off + 40 > b.size()) return false;
    *out = off;
    return true;
}

static bool walk_frame(const std::vector<uint8_t> &b, size_t hdr_len, int n,
                       int eyes, int cols, int rows, int nctx, FrameWalk *w) {
    if (!frame_offset(b, hdr_len, n, &w->frame_off)) return false;
    const size_t f = w->frame_off;
    w->eyes = eyes;
    w->cols = cols;
    w->rows = rows;
    const uint32_t flags = b[f + 34];
    w->warp_present = (flags >> 3) & 1;
    size_t off = f + 40;
    if (w->warp_present) {
        w->warp_off = off;
        off += (size_t)36 * eyes;
    }
    if (b[f + 31] == 255) off += 128;
    const uint32_t tp = b[f + 32];
    for (int k = 0; k < 8; ++k)
        if (tp & (1u << k)) off += (size_t)nctx * 16 * 5 / 8;
    w->row0_off = off;
    return true;
}

// Offset of the header of the first tile matching (mode, eye); -1 for "any".
// Returns the tile header offset, and through `opt` the offset of its optional
// area (the mv / disparity bytes).
static bool find_tile(const std::vector<uint8_t> &b, const FrameWalk &w,
                      int want_mode, int want_eye, size_t *hdr, size_t *opt) {
    size_t off = w.row0_off;
    for (int row = 0; row < w.rows; ++row) {
        for (int eye = 0; eye < w.eyes; ++eye) {
            if (off + 12 > b.size()) return false;
            const uint64_t skip = rd_u64(b, off + 4);
            off += 12;
            for (int col = 0; col < w.cols; ++col) {
                if ((skip >> col) & 1ull) continue;
                if (off + 8 > b.size()) return false;
                const uint32_t w0 = rd_u32(b, off), w1 = rd_u32(b, off + 4);
                const int mode = (int)(w1 & 7);
                const int mv_present = (int)((w1 >> 20) & 1);
                const int alpha_mode = (int)((w1 >> 6) & 3);
                const size_t optoff = off + 8;
                const size_t plen = (w0 >> 16) & 0xffff;
                const size_t adv = 8 + (mv_present ? 2u : 0u) +
                                   (alpha_mode == 1 ? 1u : 0u) + plen;
                if ((want_mode < 0 || mode == want_mode) &&
                    (want_eye < 0 || eye == want_eye)) {
                    *hdr = off;
                    *opt = optoff;
                    return true;
                }
                off += adv;
            }
        }
    }
    return false;
}

struct InterReject {
    const char *name;
    const char *why;
    int expect;
    int base;      // 0 = the mono inter base, 1 = the stereo one
};

static const InterReject kInterRejects[] = {
    {"r18_h22_not_one",        "warp_ext h22 != 2^29",                     NXVC_ERR_BITSTREAM, 0},
    {"r19_entry_range",        "a warp_ext entry beyond kEntryMax",        NXVC_ERR_BITSTREAM, 0},
    {"r20_den_range",          "den leaves [2^28, 2^30) at a picture corner", NXVC_ERR_BITSTREAM, 0},
    {"r21_warp_no_flag",       "a WARP_MV tile with warp_present == 0",    NXVC_ERR_BITSTREAM, 0},
    {"r22_mode_reserved",      "mode 5 is reserved",                       NXVC_ERR_BITSTREAM, 0},
    {"r23_intra_ref_sel",      "ref_sel != 0 on an INTRA tile",            NXVC_ERR_BITSTREAM, 0},
    {"r24_ref_sel_three",      "ref_sel == 3 is reserved",                 NXVC_ERR_BITSTREAM, 0},
    {"r25_ref_slots_wrong",    "ref_slots != 1 << (frame_number mod 4)",   NXVC_ERR_BITSTREAM, 0},
    {"r26_tool_catmullrom",    "tool bit 23 FILTER_CATMULL_ROM in v1",     NXVC_ERR_VERSION,   0},
    {"r27_warp_without_inter", "the WARP tool bit without INTER",          NXVC_ERR_BITSTREAM, 0},
    {"r28_stereo_left_eye",    "mode STEREO on the left eye",              NXVC_ERR_BITSTREAM, 1},
    {"r29_disparity_reserved", "disparity bits 15:12 are not zero",        NXVC_ERR_BITSTREAM, 1},
};
static const int kNumInterRejects =
    (int)(sizeof(kInterRejects) / sizeof(kInterRejects[0]));

// The two base streams the patches are applied to.  Both are ordinary encoder
// output; only the fields named above are touched.
static const InterSpec kRejectBaseMono = {
    "reject_base_mono", "", 128, 128, 1, 1, 26, 3, 0, 999, 0, 0.7, 2.0, 3, 0, 0};
static const InterSpec kRejectBaseStereo = {
    "reject_base_stereo", "", 128, 128, 2, 1, 24, 3, 1, 999, 0, 0.0, 0.0, 0, 11, 1};

static bool make_inter_reject(int idx, const std::vector<uint8_t> &base,
                              const InterSpec &spec, std::vector<uint8_t> *out,
                              std::string *why) {
    std::vector<uint8_t> b = base;
    const int cols = (spec.eye_w + 63) / 64, rows = (spec.h + 63) / 64;
    FrameWalk f0{}, f1{};
    if (!walk_frame(b, 64, 0, spec.eyes, cols, rows, 16, &f0) ||
        !walk_frame(b, 64, 1, spec.eyes, cols, rows, 16, &f1)) {
        *why = "frame walk failed";
        return false;
    }
    size_t hdr = 0, opt = 0;
    auto patch_w1 = [&](size_t o, uint32_t clear, uint32_t set) {
        uint32_t w1 = rd_u32(b, o + 4);
        w1 = (w1 & ~clear) | set;
        for (int i = 0; i < 4; ++i) b[o + 4 + i] = (uint8_t)(w1 >> (8 * i));
    };
    auto put32 = [&](size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[o + i] = (uint8_t)(v >> (8 * i));
    };
    switch (idx) {
        case 0:
            if (!f1.warp_present) { *why = "frame 1 has no warp_ext"; return false; }
            put32(f1.warp_off + 32, 0x20000001u);
            break;
        case 1:
            if (!f1.warp_present) { *why = "frame 1 has no warp_ext"; return false; }
            put32(f1.warp_off + 0, 0x40000001u);   // kEntryMax + 1
            break;
        case 2:
            if (!f1.warp_present) { *why = "frame 1 has no warp_ext"; return false; }
            put32(f1.warp_off + 24, 1u << 24);     // h20: den runs past 2^30
            break;
        case 3:
            // Frame 0 has no warp_ext at all, so a warped mode there is the
            // "warp_present == 0" case with no offsets to shift.
            if (!find_tile(b, f0, NXVC_MODE_INTRA, -1, &hdr, &opt)) {
                *why = "no INTRA tile in frame 0"; return false;
            }
            patch_w1(hdr, 7u, (uint32_t)NXVC_MODE_WARP_MV);
            break;
        case 4:
            if (!find_tile(b, f0, NXVC_MODE_INTRA, -1, &hdr, &opt)) {
                *why = "no INTRA tile in frame 0"; return false;
            }
            patch_w1(hdr, 7u, 5u);
            break;
        case 5:
            if (!find_tile(b, f0, NXVC_MODE_INTRA, -1, &hdr, &opt)) {
                *why = "no INTRA tile in frame 0"; return false;
            }
            patch_w1(hdr, 3u << 21, 1u << 21);
            break;
        case 6:
            if (!find_tile(b, f1, NXVC_MODE_WARP_MV, -1, &hdr, &opt) &&
                !find_tile(b, f1, NXVC_MODE_STATIC_MV, -1, &hdr, &opt)) {
                *why = "no coded inter tile in frame 1"; return false;
            }
            patch_w1(hdr, 3u << 21, 3u << 21);
            break;
        case 7:
            b[f1.frame_off + 33] ^= 0xff;
            break;
        case 8:
            b[32 + 2] |= 0x80;   // tools bit 23
            break;
        case 9:
            b[32 + 1] &= (uint8_t)~0x04;   // clear tools bit 10 (INTER)
            break;
        case 10:
            if (!find_tile(b, f1, -1, 0, &hdr, &opt)) {
                *why = "no left-eye tile in frame 1"; return false;
            }
            patch_w1(hdr, 7u, (uint32_t)NXVC_MODE_STEREO);
            break;
        case 11: {
            FrameWalk fw = f1;
            size_t h2 = 0, o2 = 0;
            bool found = false;
            for (int fr = 1; fr < spec.frames && !found; ++fr) {
                if (!walk_frame(b, 64, fr, spec.eyes, cols, rows, 16, &fw)) break;
                found = find_tile(b, fw, NXVC_MODE_STEREO, 1, &h2, &o2);
            }
            if (!found) { *why = "no STEREO tile to corrupt"; return false; }
            b[o2 + 1] |= 0x10;   // disparity bit 12
            break;
        }
        default: break;
    }
    *out = b;
    return true;
}

static std::string manifest_path(const std::string &dir) {
    return dir + "/vectors.md5";
}
static std::string reject_manifest_path(const std::string &dir) {
    return dir + "/rejects.md5";
}

// ------------------------------------------------------- rejection vectors
// A conforming decoder never produces output from a stream it must reject, and
// the *named* status matters: NXVC_ERR_UNSUPPORTED means "legal v1 syntax this
// profile does not implement" (a Phase 2 stream, 10-bit) and must not be
// confused with NXVC_ERR_BITSTREAM, "this cannot be a legal stream at all".
// Spec annex C-19.  Each vector is the v01 stream with one field corrupted;
// the byte offsets are fixed because that stream carries no custom matrices
// and no transmitted tables:
//   [0,64) stream header   [64,104) frame header   [104,116) row header
//   [116,124) tile 0 header
struct RejectSpec {
    const char *name;
    const char *why;
    int expect;          // nxvc_status the decoder must return
    int stage;           // 0 = stream header, 1 = frame decode
};

enum {
    kOffFrame = 64, kOffRow = 104, kOffTile0 = 116
};

static const RejectSpec kRejects[] = {
    {"r01_bad_magic",         "magic is not 'NXV1'",                     NXVC_ERR_VERSION,     0},
    {"r02_unknown_tool",      "a mandatory tool bit the decoder lacks",   NXVC_ERR_VERSION,     0},
    {"r03_bit_depth_10",      "bit_depth 10 is reserved in v1",          NXVC_ERR_UNSUPPORTED, 0},
    {"r04_tile_size_32",      "32x32 tiles are a reserved profile",      NXVC_ERR_UNSUPPORTED, 0},
    {"r05_payload_past_row",  "payload_len runs past the frame",         NXVC_ERR_TRUNCATED,   1},
    {"r06_res_level3",        "res_level 3 is reserved",                 NXVC_ERR_BITSTREAM,   1},
    {"r07_truncated_rans",    "the tile payload is cut short",           NXVC_ERR_TRUNCATED,   1},
    {"r08_skip_bitmap",       "a skip bit for a column past the picture",NXVC_ERR_BITSTREAM,   1},
    {"r09_reserved_tile_bit", "tile word1 bit 28 is reserved",           NXVC_ERR_BITSTREAM,   1},
    {"r10_mode_inter",        "an INTER tile in a Phase 1 stream",       NXVC_ERR_UNSUPPORTED, 1},
    {"r11_wm_id_no_tool",     "wm_id != 0 without the WM_ID tool bit",   NXVC_ERR_BITSTREAM,   1},
    {"r12_row_index_wrong",   "row_index does not match its position",   NXVC_ERR_BITSTREAM,   1},
    {"r13_frame_bytes_short", "frame_bytes below the header size",       NXVC_ERR_TRUNCATED,   1},
    {"r14_dir_layer_no_tool", "frame flags bit 2 without INTRA_DIR",      NXVC_ERR_BITSTREAM,   1},
    {"r15_ycocgr_420",        "YCoCg-R declared with 4:2:0 chroma",       NXVC_ERR_BITSTREAM,   0},
    {"r16_ctx_v2_short_table", "CTX_V2 table set overruns the tile rows",  NXVC_ERR_BITSTREAM,   1},
    {"r17_lossless_sign_hide", "LOSSLESS and SIGN_HIDE together",          NXVC_ERR_BITSTREAM,   0},
    // syntax v1.5: the three illegal ways to say `xform` (SYNTAX.md 6.7).
    {"r30_xform_reserved",    "xform 3 is reserved",                      NXVC_ERR_BITSTREAM,   1},
    {"r31_xform_no_tool",     "xform != 0 without the XFORM_LARGE bit",   NXVC_ERR_BITSTREAM,   1},
    {"r32_xform_tskip",       "xform != 0 together with tskip",           NXVC_ERR_BITSTREAM,   1},
};
static const int kNumRejects = (int)(sizeof(kRejects) / sizeof(kRejects[0]));

// The manifest is whitespace-separated, so the status travels as a token.
static const char *status_token(int st) {
    switch ((nxvc_status)st) {
        case NXVC_OK: return "OK";
        case NXVC_ERR_ARG: return "ARG";
        case NXVC_ERR_UNSUPPORTED: return "UNSUPPORTED";
        case NXVC_ERR_BITSTREAM: return "BITSTREAM";
        case NXVC_ERR_TRUNCATED: return "TRUNCATED";
        case NXVC_ERR_NOMEM: return "NOMEM";
        case NXVC_ERR_VERSION: return "VERSION";
    }
    return "UNKNOWN";
}

static void put_u32(std::vector<uint8_t> &b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get_u32(const std::vector<uint8_t> &b, size_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)b[off + i] << (8 * i);
    return v;
}

static std::vector<uint8_t> make_reject(int idx, const std::vector<uint8_t> &base) {
    std::vector<uint8_t> b = base;
    uint32_t w0 = get_u32(b, kOffTile0), w1 = get_u32(b, kOffTile0 + 4);
    switch (idx) {
        case 0: b[0] ^= 0x01; break;
        case 1: b[32 + 7] |= 0x80; break;              // tools bit 63
        case 2: b[13] = 10; break;                     // bit_depth
        case 3: b[7] = 1; break;                       // tile_size 32x32
        case 4: put_u32(b, kOffTile0, (w0 & 0xffffu) | (0xf000u << 16)); break;
        case 5: put_u32(b, kOffTile0 + 4, w1 | (3u << 3)); break;
        case 6: b.resize(b.size() - 16); break;
        case 7: b[kOffRow + 4 + 7] = 0x80; break;      // skip bitmap bit 63
        case 8: put_u32(b, kOffTile0 + 4, w1 | (1u << 28)); break;
        case 9: put_u32(b, kOffTile0 + 4, (w1 & ~7u) | 2u); break;  // WARP_MV
        case 10: put_u32(b, kOffTile0 + 4, w1 | (1u << 26)); break; // wm_id 1
        case 11: b[kOffRow + 2] = 7; break;   // row_index
        case 12: put_u32(b, kOffFrame + 36, 8); break;
        // frame header byte 34 is `flags`; bit 2 selects the layered form of
        // directional intra and is meaningless without tool bit 17.
        case 13: b[kOffFrame + 34] |= 0x04; break;
        // SYNTAX.md 2: color_transform 1 requires 4:4:4.  v01 is 4:2:0, so
        // declaring YCoCg-R (and the RGB colour space and tool bit 9 that
        // must accompany it) makes the header self-contradictory.
        case 14: b[41] = 1; b[42] = 3; b[32 + 1] |= 0x02; break;
        // A CTX_V2 stream's transmitted table sets are 160 bytes, not 120.
        // Declaring the tool and set 0 without lengthening the frame makes the
        // table run past `frame_bytes`.
        case 15:
            b[32 + 2] |= 0x20;              // tools bit 21 (CTX_V2)
            b[kOffFrame + 32] |= 0x01;      // tables_present bit 0
            break;
        // Sign data hiding is lossy by construction; the two tool bits are
        // mutually exclusive.
        case 16: b[32 + 0] |= 0x20; b[32 + 2] |= 0x40; break;
        // Tile word1 bits 28-29 are `xform`.  Value 3 is reserved; any
        // nonzero value needs tool bit 24; and no value but 0 may accompany
        // transform skip.  The third needs both tool bits (1 TRANSFORM_SKIP
        // and 24 XFORM_LARGE) declared, or an earlier check fires instead.
        case 17: put_u32(b, kOffTile0 + 4, w1 | (3u << 28)); break;
        case 18: put_u32(b, kOffTile0 + 4, w1 | (2u << 28)); break;
        case 19:
            b[32 + 0] |= 0x02;             // tools bit 1  (TRANSFORM_SKIP)
            b[32 + 3] |= 0x01;             // tools bit 24 (XFORM_LARGE)
            put_u32(b, kOffTile0 + 4, w1 | (1u << 23) | (1u << 28));
            break;
        default: break;
    }
    return b;
}

// Decode `data` and return the first non-OK status the decoder gives.  Every
// frame is walked, not just the first: an inter stream's malformed field is
// usually in frame 1 or later, since frame 0 has no reference and no
// warp_ext().  Checking that no output was produced before the failure is the
// caller's job (the planes go into a scratch buffer that is discarded).
static int reject_status(const std::vector<uint8_t> &data) {
    nxvc_status st;
    nxvc_decoder *d = nxvc_decoder_create(&st);
    size_t consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, data.data(), data.size(), &consumed);
    if (st != NXVC_OK) { nxvc_decoder_destroy(d); return (int)st; }
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
        V((size_t)cw * ch), A((size_t)yw * yh, 255);
    nxvc_image oi{};
    oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
    oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
    oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
    oi.plane[3] = A.data(); oi.stride[3] = (int)yw;
    size_t off = consumed;
    while (off < data.size()) {
        st = nxvc_decoder_decode_frame(d, data.data() + off, data.size() - off,
                                       &oi, &consumed);
        if (st != NXVC_OK) break;
        off += consumed;
    }
    nxvc_decoder_destroy(d);
    return (int)st;
}

int main(int argc, char **argv) {
    std::string dir;
    bool generate = false, check = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--generate" && i + 1 < argc) { generate = true; dir = argv[++i]; }
        else if (a == "--check" && i + 1 < argc) { check = true; dir = argv[++i]; }
        else {
            std::fprintf(stderr, "usage: nxv-vectors --generate|--check DIR\n");
            return 2;
        }
    }
    if (!generate && !check) {
        std::fprintf(stderr, "usage: nxv-vectors --generate|--check DIR\n");
        return 2;
    }

    if (generate) {
        std::FILE *m = std::fopen(manifest_path(dir).c_str(), "wb");
        if (!m) { std::perror("open manifest"); return 1; }
        std::fprintf(m,
            "# NX Warp v1 conformance vectors.  Generated by tests/ref/vectors.cpp.\n"
            "# name  stream_md5  decoded_md5  width height pix alpha frames\n");
        for (int i = 0; i < kNumVectors; ++i) {
            const VecSpec &v = kVectors[i];
            Result r = build(v);
            if (!r.ok) {
                std::fprintf(stderr, "%s: %s\n", v.name, r.err.c_str());
                return 1;
            }
            std::string path = dir + "/" + v.name + ".nxv";
            std::FILE *f = std::fopen(path.c_str(), "wb");
            if (!f) { std::perror(path.c_str()); return 1; }
            std::fwrite(r.stream.data(), 1, r.stream.size(), f);
            std::fclose(f);
            std::fprintf(m, "%s %s %s %d %d %s %d %d\n", v.name,
                         r.stream_md5.c_str(), r.decoded_md5.c_str(), v.w, v.h,
                         v.c444 ? "yuv444p" : "yuv420p", v.alpha, v.frames);
            std::printf("%-26s %7zu B  %s\n", v.name, r.stream.size(),
                        r.decoded_md5.c_str());
        }
        // --- Phase 2 (Annex D D-21)
        std::fprintf(m, "# --- Phase 2 inter vectors (Annex D D-21)\n");
        for (int i = 0; i < kNumInterVectors; ++i) {
            const InterSpec &v = kInterVectors[i];
            Result r = build_inter(v);
            if (!r.ok) {
                std::fprintf(stderr, "%s: %s\n", v.name, r.err.c_str());
                return 1;
            }
            std::string path = dir + "/" + v.name + ".nxv";
            std::FILE *f = std::fopen(path.c_str(), "wb");
            if (!f) { std::perror(path.c_str()); return 1; }
            std::fwrite(r.stream.data(), 1, r.stream.size(), f);
            std::fclose(f);
            std::fprintf(m, "%s %s %s %d %d %s %d %d\n", v.name,
                         r.stream_md5.c_str(), r.decoded_md5.c_str(),
                         v.eye_w * v.eyes, v.h, v.c444 ? "yuv444p" : "yuv420p",
                         0, v.frames);
            std::printf("%-26s %7zu B  %s   [%s]\n", v.name, r.stream.size(),
                        r.decoded_md5.c_str(), v.fixes);
        }
        std::fclose(m);
        std::printf("%d vectors written to %s\n",
                    kNumVectors + kNumInterVectors, dir.c_str());

        // Rejection vectors: the v01 stream with one field corrupted each.
        Result base = build(kVectors[0]);
        if (!base.ok) {
            std::fprintf(stderr, "reject base: %s\n", base.err.c_str());
            return 1;
        }
        std::FILE *rm = std::fopen(reject_manifest_path(dir).c_str(), "wb");
        if (!rm) { std::perror("open reject manifest"); return 1; }
        std::fprintf(rm,
            "# NX Warp v1 rejection vectors.  Generated by tests/ref/vectors.cpp.\n"
            "# Each stream MUST be refused with exactly the named status, with no\n"
            "# decoded output.  See docs/SYNTAX.md 12 and Appendix A item 30.\n"
            "# name  md5  expected_status  why\n");
        int bad = 0;
        for (int i = 0; i < kNumRejects; ++i) {
            std::vector<uint8_t> data = make_reject(i, base.stream);
            int got = reject_status(data);
            if (got != kRejects[i].expect) {
                std::fprintf(stderr, "%s: decoder returned %s, expected %s\n",
                             kRejects[i].name, status_token(got),
                             status_token(kRejects[i].expect));
                ++bad;
                continue;
            }
            std::string path = dir + "/" + kRejects[i].name + ".nxv";
            std::FILE *f = std::fopen(path.c_str(), "wb");
            if (!f) { std::perror(path.c_str()); return 1; }
            std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
            std::fprintf(rm, "%s %s %s %s\n", kRejects[i].name,
                         md5_hex(data.data(), data.size()).c_str(),
                         status_token(kRejects[i].expect), kRejects[i].why);
            std::printf("%-26s %7zu B  rejected as %-12s %s\n", kRejects[i].name,
                        data.size(), status_token(kRejects[i].expect),
                        kRejects[i].why);
        }
        // --- Phase 2 rejection vectors
        Result mono = build_inter(kRejectBaseMono);
        Result ster = build_inter(kRejectBaseStereo);
        if (!mono.ok || !ster.ok) {
            std::fprintf(stderr, "inter reject base: %s %s\n",
                         mono.err.c_str(), ster.err.c_str());
            return 1;
        }
        std::fprintf(rm, "# --- Phase 2 (Annex D D-21)\n");
        for (int i = 0; i < kNumInterRejects; ++i) {
            const InterReject &rj = kInterRejects[i];
            const Result &bs = rj.base ? ster : mono;
            const InterSpec &sp = rj.base ? kRejectBaseStereo : kRejectBaseMono;
            std::vector<uint8_t> data;
            std::string why;
            if (!make_inter_reject(i, bs.stream, sp, &data, &why)) {
                std::fprintf(stderr, "%s: %s\n", rj.name, why.c_str());
                ++bad;
                continue;
            }
            int got = reject_status(data);
            if (got != rj.expect) {
                std::fprintf(stderr, "%s: decoder returned %s, expected %s\n",
                             rj.name, status_token(got), status_token(rj.expect));
                ++bad;
                continue;
            }
            std::string path = dir + "/" + rj.name + ".nxv";
            std::FILE *f = std::fopen(path.c_str(), "wb");
            if (!f) { std::perror(path.c_str()); return 1; }
            std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
            std::fprintf(rm, "%s %s %s %s\n", rj.name,
                         md5_hex(data.data(), data.size()).c_str(),
                         status_token(rj.expect), rj.why);
            std::printf("%-26s %7zu B  rejected as %-12s %s\n", rj.name,
                        data.size(), status_token(rj.expect), rj.why);
        }
        std::fclose(rm);
        if (bad) return 1;
        std::printf("%d rejection vectors written to %s\n",
                    kNumRejects + kNumInterRejects, dir.c_str());
        return 0;
    }

    // --check: the committed bitstreams must decode to the committed MD5s, and
    // the encoder must still reproduce the same bitstreams.
    std::FILE *m = std::fopen(manifest_path(dir).c_str(), "rb");
    if (!m) {
        std::fprintf(stderr, "missing %s\n", manifest_path(dir).c_str());
        return 1;
    }
    char line[512];
    int checked = 0;
    while (std::fgets(line, sizeof(line), m)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[128], smd5[64], dmd5[64], pix[32];
        int w, h, alpha, frames;
        if (std::sscanf(line, "%127s %63s %63s %d %d %31s %d %d", name, smd5,
                        dmd5, &w, &h, pix, &alpha, &frames) != 8)
            continue;
        const VecSpec *spec = nullptr;
        const InterSpec *ispec = nullptr;
        for (int i = 0; i < kNumVectors; ++i)
            if (std::strcmp(kVectors[i].name, name) == 0) spec = &kVectors[i];
        for (int i = 0; i < kNumInterVectors; ++i)
            if (std::strcmp(kInterVectors[i].name, name) == 0)
                ispec = &kInterVectors[i];
        CHECK(spec != nullptr || ispec != nullptr,
              "vector %s is in the manifest but not the table", name);
        if (!spec && !ispec) continue;

        // (a) the committed file decodes to the committed plane MD5
        std::string path = dir + "/" + name + ".nxv";
        std::FILE *f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr, "missing vector file %s", path.c_str());
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long fsz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> data((size_t)(fsz > 0 ? fsz : 0));
        size_t rd = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        CHECK(rd == data.size(), "short read %s", name);
        CHECK(md5_hex(data.data(), data.size()) == smd5,
              "%s: bitstream md5 changed", name);

        nxvc_status st;
        nxvc_decoder *d = nxvc_decoder_create(&st);
        size_t off = 0, consumed = 0;
        st = nxvc_decoder_parse_stream_header(d, data.data(), data.size(), &consumed);
        CHECK(st == NXVC_OK, "%s: header %s", name, nxvc_status_string(st));
        if (st != NXVC_OK) { nxvc_decoder_destroy(d); continue; }
        off = consumed;
        uint32_t yw, yh, cw, ch;
        nxvc_decoder_plane_size(d, 0, &yw, &yh);
        nxvc_decoder_plane_size(d, 1, &cw, &ch);
        std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
            V((size_t)cw * ch), A((size_t)yw * yh, 255);
        MD5 md;
        int nf = 0;
        bool bad = false;
        while (off < data.size()) {
            nxvc_image oi{};
            oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
            oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
            oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
            oi.plane[3] = A.data(); oi.stride[3] = (int)yw;
            st = nxvc_decoder_decode_frame(d, data.data() + off,
                                           data.size() - off, &oi, &consumed);
            if (st != NXVC_OK) {
                CHECK(false, "%s: frame %d %s", name, nf, nxvc_status_string(st));
                bad = true;
                break;
            }
            md.update(Y.data(), Y.size());
            md.update(U.data(), U.size());
            md.update(V.data(), V.size());
            if (alpha) md.update(A.data(), A.size());
            off += consumed;
            ++nf;
        }
        nxvc_decoder_destroy(d);
        if (bad) continue;
        CHECK(nf == frames, "%s: %d frames, expected %d", name, nf, frames);
        CHECK(md.hex() == dmd5, "%s: decoded md5 %s != %s", name, md.hex().c_str(),
              dmd5);

        // (b) the encoder still reproduces the same bitstream
        Result r = spec ? build(*spec) : build_inter(*ispec);
        CHECK(r.ok, "%s: re-encode failed (%s)", name, r.err.c_str());
        if (r.ok) {
            CHECK(r.stream_md5 == smd5, "%s: encoder output changed", name);
            CHECK(r.decoded_md5 == dmd5, "%s: decoder output changed", name);
        }
        ++checked;
    }
    std::fclose(m);
    CHECK(checked == kNumVectors + kNumInterVectors,
          "checked %d of %d vectors", checked,
          kNumVectors + kNumInterVectors);

    // Rejection vectors.
    std::FILE *rm = std::fopen(reject_manifest_path(dir).c_str(), "rb");
    CHECK(rm != nullptr, "missing %s", reject_manifest_path(dir).c_str());
    int rchecked = 0;
    while (rm && std::fgets(line, sizeof(line), rm)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[128], md5s[64], status[64];
        if (std::sscanf(line, "%127s %63s %63s", name, md5s, status) != 3) continue;
        std::string path = dir + "/" + name + ".nxv";
        std::FILE *f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr, "missing rejection vector %s", path.c_str());
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long fsz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> data((size_t)(fsz > 0 ? fsz : 0));
        size_t rd = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        CHECK(rd == data.size(), "short read %s", name);
        CHECK(md5_hex(data.data(), data.size()) == md5s, "%s: md5 changed", name);
        int got = reject_status(data);
        CHECK(std::strcmp(status_token(got), status) == 0,
              "%s: decoder returned %s, expected %s", name, status_token(got),
              status);
        ++rchecked;
    }
    if (rm) std::fclose(rm);
    CHECK(rchecked == kNumRejects + kNumInterRejects,
          "checked %d of %d rejection vectors", rchecked,
          kNumRejects + kNumInterRejects);
    return test_report("test_vectors");
}
