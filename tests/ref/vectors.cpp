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
};

static const VecSpec kVectors[] = {
    // name                     w    h  444 kind qp  ll  a  ts nsub tab t420 ct mat res qpp fr
    {"v01_intra420_qp12",      192, 128, 0,  1, 12,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v02_intra420_qp24",      192, 128, 0,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v03_intra420_qp36",      192, 128, 0,  1, 36,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v04_intra420_qp51",      192, 128, 0,  1, 51,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v05_intra444_qp24",      192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v06_gradient420_qp20",   192, 128, 0,  0, 20,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v07_checker420_qp28",    192, 128, 0,  2, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v08_noise420_qp28",      192, 128, 0,  3, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v09_flat420_qp28",       192, 128, 0,  4, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v10_lossless420",        192, 128, 0,  1,  0,  1, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0},
    {"v11_lossless444",        192, 128, 1,  1,  0,  1, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0},
    {"v12_lossless444_alpha",  192, 128, 1,  2,  0,  1, 1,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0},
    {"v13_tskip420_qp16",      192, 128, 0,  2, 16,  0, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0},
    {"v14_alpha420_qp24",      192, 128, 0,  1, 24,  0, 1,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v15_res_cycle420",       192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  1,  0, 1, 0, 0},
    {"v16_res_level2_420",     192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  2,  0, 1, 0, 0},
    {"v17_res_cycle444",       192, 128, 1,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  1,  0, 1, 0, 0},
    {"v18_qpmap420",           192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  1, 1, 0, 0},
    {"v19_qp_res_map420",      192, 128, 0,  1, 30,  0, 0,  0,  3,  0,  0, 0,  2,  1,  1, 1, 0, 0},
    {"v20_tile420_in444",      192, 128, 1,  1, 26,  0, 0,  0,  3,  0,  1, 0,  1,  0,  0, 1, 0, 0},
    {"v21_ycocgr444_qp24",     192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 1,  1,  0,  0, 1, 0, 0},
    {"v22_ycocgr_lossless",    192, 128, 1,  1,  0,  1, 0,  1,  3,  0,  0, 1,  0,  0,  0, 1, 0, 0},
    {"v23_custom_tables420",   192, 128, 0,  1, 28,  0, 0,  0,  3,  1,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v24_nsub0_420",          192, 128, 0,  1, 28,  0, 0,  0,  0,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v25_nsub5_420",          192, 128, 0,  1, 28,  0, 0,  0,  5,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v26_nsub_auto_420",      192, 128, 0,  1, 28,  0, 0,  0,255,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v27_matrix0_420",        192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  0,  0,  0, 1, 0, 0},
    {"v28_matrix3_420",        192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  3,  0,  0, 1, 0, 0},
    {"v29_odd_size_200x140",   200, 140, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v30_tiny_64x64",          64,  64, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v31_wide_320x64",        320,  64, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 0, 0},
    {"v32_multiframe420",      128, 128, 0,  1, 30,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 3, 0, 0},
    // v1.2 additions.
    {"v33_wm_id444",           192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1, 2, 0},
    {"v34_wm_id420_tables",    192, 128, 0,  1, 30,  0, 0,  0,  3,  1,  0, 0,  2,  0,  0, 1, 3, 0},
    // Hand-built: every dequantized coefficient saturates the int16 clamp, so
    // the IDCT runs at its documented worst case (SYNTAX.md 6.3).  This is the
    // vector that pins the odd-part rotation's range; the reference decoder
    // must run it clean under -fsanitize=undefined (ctest ref.saturate).
    {"v35_saturate420",         64,  64, 0,  4, 63,  0, 0,  0,  3,  0,  0, 0,  2,  0,  0, 1, 0, 1},
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

// raw == 1: the saturating vector.  Everything but the tile payload comes from
// a real encode of the same geometry, so the headers stay exactly conformant.
static Result build_raw(const VecSpec &v, Result base) {
    Result r;
    if (!base.ok && base.err.empty()) base.err = "base build failed";
    if (!base.err.empty()) { r.err = base.err; return r; }
    std::vector<uint8_t> payload;
    if (!build_saturating_payload(payload)) { r.err = "payload build"; return r; }
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
    w1 &= ~((7u << 14) | (7u << 17));
    w1 |= (3u << 17);
    for (int i = 0; i < 4; ++i) out[kSH + kFH + kRH + 4 + i] = (uint8_t)(w1 >> (8 * i));
    out.insert(out.end(), payload.begin(), payload.end());
    // frame header frame_bytes at offset 36 of the frame header.
    uint32_t fb = (uint32_t)(out.size() - kSH);
    for (int i = 0; i < 4; ++i) out[kSH + 36 + i] = (uint8_t)(fb >> (8 * i));
    r.stream = out;
    (void)v;
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
    {"r08_skip_bitmap",       "a skip bit for a column past the picture",NXVC_ERR_UNSUPPORTED, 1},
    {"r09_reserved_tile_bit", "tile word1 bit 28 is reserved",           NXVC_ERR_BITSTREAM,   1},
    {"r10_mode_inter",        "an INTER tile in a Phase 1 stream",       NXVC_ERR_UNSUPPORTED, 1},
    {"r11_wm_id_no_tool",     "wm_id != 0 without the WM_ID tool bit",   NXVC_ERR_BITSTREAM,   1},
    {"r12_row_index_wrong",   "row_index does not match its position",   NXVC_ERR_BITSTREAM,   1},
    {"r13_frame_bytes_short", "frame_bytes below the header size",       NXVC_ERR_TRUNCATED,   1},
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
        default: break;
    }
    return b;
}

// Decode `data` and return the status the decoder gives, checking that it
// produced no output before failing is the caller's job (the planes are
// written into a scratch buffer that is discarded).
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
    st = nxvc_decoder_decode_frame(d, data.data() + consumed,
                                   data.size() - consumed, &oi, &consumed);
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
        std::fclose(m);
        std::printf("%d vectors written to %s\n", kNumVectors, dir.c_str());

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
        std::fclose(rm);
        if (bad) return 1;
        std::printf("%d rejection vectors written to %s\n", kNumRejects,
                    dir.c_str());
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
        for (int i = 0; i < kNumVectors; ++i)
            if (std::strcmp(kVectors[i].name, name) == 0) spec = &kVectors[i];
        CHECK(spec != nullptr, "vector %s is in the manifest but not the table",
              name);
        if (!spec) continue;

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
        Result r = build(*spec);
        CHECK(r.ok, "%s: re-encode failed (%s)", name, r.err.c_str());
        if (r.ok) {
            CHECK(r.stream_md5 == smd5, "%s: encoder output changed", name);
            CHECK(r.decoded_md5 == dmd5, "%s: decoder output changed", name);
        }
        ++checked;
    }
    std::fclose(m);
    CHECK(checked == kNumVectors, "checked %d of %d vectors", checked,
          kNumVectors);

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
    CHECK(rchecked == kNumRejects, "checked %d of %d rejection vectors",
          rchecked, kNumRejects);
    return test_report("test_vectors");
}
