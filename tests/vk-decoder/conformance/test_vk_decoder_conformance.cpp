// vk.decoder.conformance
//
// The exit criterion of docs/PAPER.md 3.11: the Vulkan decoder must reproduce
// the CPU reference's output with ZERO mismatching samples, on every device
// we ship on.
//
// Two bodies of evidence, both run on whatever ICD the environment selects:
//
//   1. tests/vectors/*.nxv, the frozen conformance vectors.  Each one is
//      decoded on the GPU and checked twice: against the `decoded_md5` pinned
//      in tests/vectors/vectors.md5 (the normative answer, produced by
//      tests/ref/vectors.cpp and independent of whatever ref/ compiles to
//      today), and pixel-for-pixel against an in-process nxvc_ref decode so a
//      failure names the first differing sample rather than just a hash.
//
//   2. A synthetic sweep encoded here and now with nxvc_encoder over QP,
//      res_level pattern, chroma format, colour transform, transform skip,
//      lane count, custom tables, alpha and picture size.  This is what
//      catches a combination nobody thought to freeze a vector for.
//
// Exit codes: 0 conformant, 1 a mismatch, 77 no usable Vulkan ICD (which is
// how ctest reports the test as a skip on a machine without one).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "nxvc/nxvc_vk.h"
#include "test_util.h"  // tests/ref: MD5, make_image, Rng

namespace {

const char *g_vectors_dir = nullptr;
// Device selection for the per-ICD ctest entries.  Unset means "first device
// that can run the decoder", which is what a developer run wants.
const char *device_filter() { return std::getenv("NXVC_VKD_DEVICE"); }
int g_fail = 0;
int g_checked = 0;
int g_skipped = 0;
bool g_verbose = false;

// ------------------------------------------------------------------ planes
struct Planes {
    uint32_t yw = 0, yh = 0, cw = 0, ch = 0;
    bool alpha = false;
    std::vector<uint8_t> p[4];
    void size_for(uint32_t w, uint32_t h, uint32_t c_w, uint32_t c_h, bool a) {
        yw = w; yh = h; cw = c_w; ch = c_h; alpha = a;
        p[0].assign((size_t)w * h, 0);
        p[1].assign((size_t)c_w * c_h, 0);
        p[2].assign((size_t)c_w * c_h, 0);
        p[3].assign((size_t)w * h, 255);
    }
    void hash_into(MD5 &md) const {
        md.update(p[0].data(), p[0].size());
        md.update(p[1].data(), p[1].size());
        md.update(p[2].data(), p[2].size());
        if (alpha) md.update(p[3].data(), p[3].size());
    }
};

size_t compare(const char *what, int frame, const Planes &a, const Planes &b) {
    static const char *kPlane[4] = {"Y/R", "Co/G", "Cg/B", "A"};
    size_t bad = 0;
    const int np = a.alpha ? 4 : 3;
    for (int pl = 0; pl < np; ++pl) {
        const uint32_t w = (pl == 1 || pl == 2) ? a.cw : a.yw;
        const uint32_t h = (pl == 1 || pl == 2) ? a.ch : a.yh;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                size_t i = (size_t)y * w + x;
                if (a.p[pl][i] == b.p[pl][i]) continue;
                if (!bad)
                    std::printf(
                        "FAIL %s frame %d: plane %s (%u,%u) ref %u gpu %u\n",
                        what, frame, kPlane[pl], x, y, a.p[pl][i], b.p[pl][i]);
                ++bad;
            }
    }
    return bad;
}

// --------------------------------------------------------------- decoders
bool ref_decode(const std::vector<uint8_t> &stream, std::vector<Planes> &out,
                std::string &err) {
    nxvc_status st;
    nxvc_decoder *d = nxvc_decoder_create(&st);
    if (!d) { err = "nxvc_decoder_create"; return false; }
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, stream.data(), stream.size(),
                                          &consumed);
    if (st != NXVC_OK) {
        err = std::string("stream header: ") + nxvc_status_string(st);
        nxvc_decoder_destroy(d);
        return false;
    }
    off = consumed;
    nxvc_stream_info si;
    nxvc_decoder_stream_info(d, &si);
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw, &ch);
    while (off < stream.size()) {
        Planes pl;
        pl.size_for(yw, yh, cw, ch, si.alpha != 0);
        nxvc_image img{};
        img.plane[0] = pl.p[0].data(); img.stride[0] = (int)yw;
        img.plane[1] = pl.p[1].data(); img.stride[1] = (int)cw;
        img.plane[2] = pl.p[2].data(); img.stride[2] = (int)cw;
        img.plane[3] = pl.p[3].data(); img.stride[3] = (int)yw;
        st = nxvc_decoder_decode_frame(d, stream.data() + off,
                                       stream.size() - off, &img, &consumed);
        if (st != NXVC_OK) {
            err = std::string("frame ") + std::to_string(out.size()) + ": " +
                  nxvc_status_string(st);
            nxvc_decoder_destroy(d);
            return false;
        }
        out.push_back(std::move(pl));
        off += consumed;
    }
    nxvc_decoder_destroy(d);
    return true;
}

bool gpu_decode(const std::vector<uint8_t> &stream, uint32_t out_format,
                std::vector<Planes> &out, std::string &err,
                bool *unsupported) {
    *unsupported = false;
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = (uint32_t)NXVC_VKD_FLAG_READBACK;
    ci.output_format = out_format;
    ci.device_name = device_filter();
    nxvc_vk_decoder *dec = nullptr;
    nxvc_vkd_status st = nxvc_vk_decoder_create(&ci, &dec);
    if (st != NXVC_VKD_OK) {
        err = dec ? nxvc_vk_decoder_last_error(dec) : "create failed";
        *unsupported = (st == NXVC_VKD_ERR_NO_DEVICE);
        nxvc_vk_decoder_destroy(dec);
        return false;
    }
    size_t off = 0, consumed = 0;
    st = nxvc_vk_decoder_parse_stream_header(dec, stream.data(), stream.size(),
                                             &consumed);
    if (st != NXVC_VKD_OK) {
        err = nxvc_vk_decoder_last_error(dec);
        *unsupported = (st == NXVC_VKD_ERR_UNSUPPORTED);
        nxvc_vk_decoder_destroy(dec);
        return false;
    }
    off = consumed;
    nxvc_vkd_stream_info si;
    nxvc_vk_decoder_stream_info(dec, &si);
    while (off < stream.size()) {
        st = nxvc_vk_decode_frame(dec, stream.data() + off, stream.size() - off,
                                  &consumed);
        if (st != NXVC_VKD_OK) {
            err = nxvc_vk_decoder_last_error(dec);
            // "shared memory over the device limit" is a device-capability
            // statement, not a conformance failure.
            *unsupported = (st == NXVC_VKD_ERR_UNSUPPORTED);
            nxvc_vk_decoder_destroy(dec);
            return false;
        }
        Planes pl;
        pl.size_for(si.width, si.height, si.chroma_width, si.chroma_height,
                    si.alpha != 0);
        uint8_t *planes[4] = {pl.p[0].data(), pl.p[1].data(), pl.p[2].data(),
                              pl.p[3].data()};
        int32_t strides[4] = {(int32_t)si.width, (int32_t)si.chroma_width,
                              (int32_t)si.chroma_width, (int32_t)si.width};
        st = nxvc_vk_decoder_read_planes(dec, planes, strides);
        if (st != NXVC_VKD_OK) {
            err = nxvc_vk_decoder_last_error(dec);
            nxvc_vk_decoder_destroy(dec);
            return false;
        }
        out.push_back(std::move(pl));
        off += consumed;
    }
    nxvc_vk_decoder_destroy(dec);
    return true;
}

// One stream, checked every way.  `pinned_md5` is empty for synthetic
// streams, which have no manifest entry.
void check_stream(const char *what, const std::vector<uint8_t> &stream,
                  const std::string &pinned_md5, uint32_t out_format) {
    std::string err;
    std::vector<Planes> ref, gpu;
    if (!ref_decode(stream, ref, err)) {
        std::printf("FAIL %s: reference decode failed (%s)\n", what,
                    err.c_str());
        ++g_fail;
        return;
    }
    bool unsupported = false;
    if (!gpu_decode(stream, out_format, gpu, err, &unsupported)) {
        if (unsupported) {
            std::printf("SKIP %s: %s\n", what, err.c_str());
            ++g_skipped;
            return;
        }
        std::printf("FAIL %s: GPU decode failed (%s)\n", what, err.c_str());
        ++g_fail;
        return;
    }
    ++g_checked;
    if (ref.size() != gpu.size()) {
        std::printf("FAIL %s: %zu reference frames, %zu GPU frames\n", what,
                    ref.size(), gpu.size());
        ++g_fail;
        return;
    }
    size_t bad = 0;
    for (size_t f = 0; f < ref.size(); ++f)
        bad += compare(what, (int)f, ref[f], gpu[f]);
    if (bad) {
        std::printf("FAIL %s: %zu mismatching samples\n", what, bad);
        ++g_fail;
        return;
    }
    if (!pinned_md5.empty()) {
        MD5 md;
        for (const Planes &p : gpu) p.hash_into(md);
        std::string got = md.hex();
        if (got != pinned_md5) {
            std::printf("FAIL %s: decoded md5 %s, manifest says %s\n", what,
                        got.c_str(), pinned_md5.c_str());
            ++g_fail;
            return;
        }
    }
    if (g_verbose) std::printf("ok   %s\n", what);
}

// ---------------------------------------------------------------- vectors
struct ManifestRow {
    std::string name, stream_md5, decoded_md5;
};

bool read_manifest(std::vector<ManifestRow> &rows) {
    std::string path = std::string(g_vectors_dir) + "/vectors.md5";
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) {
        std::printf("cannot open %s\n", path.c_str());
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[128], sm[64], dm[64];
        if (std::sscanf(line, "%127s %63s %63s", name, sm, dm) != 3) continue;
        rows.push_back({name, sm, dm});
    }
    std::fclose(f);
    return !rows.empty();
}

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    out.resize((size_t)n);
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

const char *vkd_status_token(nxvc_vkd_status st);

// The tool bits this Phase 1 decoder implements.  docs/SYNTAX.md 12: a
// Phase 1 decoder "rejects ... any tool bit outside the supported set" with a
// VERSION status, and tests/vectors now also holds the Phase 2 inter vectors,
// which every bit of that set refuses on purpose.  Those are counted as skips
// rather than failures -- but only when the stream's own tool mask says so and
// only when the decoder refuses it with exactly VERSION, so a regression that
// starts refusing a Phase 1 vector still fails.
constexpr uint64_t kPhase1Tools =
    (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 4) |
    (1ull << 5) | (1ull << 6) | (1ull << 7) | (1ull << 8) | (1ull << 9) |
    (1ull << 17) | (1ull << 20) | (1ull << 21) | (1ull << 22);

// docs/SYNTAX.md 11: `tools` is a u64 at byte 32 of the 64-byte stream header.
bool stream_needs_phase2(const std::vector<uint8_t> &s, uint64_t &tools) {
    tools = 0;
    if (s.size() < 40) return false;
    for (int i = 0; i < 8; ++i) tools |= (uint64_t)s[32 + i] << (8 * i);
    return (tools & ~kPhase1Tools) != 0;
}

void run_vectors() {
    std::vector<ManifestRow> rows;
    if (!read_manifest(rows)) {
        std::printf("FAIL: no conformance manifest\n");
        ++g_fail;
        return;
    }
    std::printf("-- %zu conformance vectors from %s\n", rows.size(),
                g_vectors_dir);
    for (const ManifestRow &r : rows) {
        std::string path = std::string(g_vectors_dir) + "/" + r.name + ".nxv";
        std::vector<uint8_t> stream;
        if (!read_file(path, stream)) {
            std::printf("FAIL %s: cannot read %s\n", r.name.c_str(),
                        path.c_str());
            ++g_fail;
            continue;
        }
        // The manifest pins the bitstream too, so a corrupted vector file is
        // reported as such rather than as a decoder bug.
        std::string sm = md5_hex(stream.data(), stream.size());
        if (sm != r.stream_md5) {
            std::printf("FAIL %s: vector file md5 %s, manifest says %s\n",
                        r.name.c_str(), sm.c_str(), r.stream_md5.c_str());
            ++g_fail;
            continue;
        }
        uint64_t tools = 0;
        if (stream_needs_phase2(stream, tools)) {
            // Must be refused, and refused with VERSION: "the tools mask is
            // not something this decoder speaks".
            nxvc_vkd_create_info ci;
            nxvc_vk_decoder_create_info_default(&ci);
            ci.device_name = device_filter();
            nxvc_vk_decoder *dec = nullptr;
            nxvc_vkd_status cst = nxvc_vk_decoder_create(&ci, &dec);
            if (cst != NXVC_VKD_OK) {
                nxvc_vk_decoder_destroy(dec);
                ++g_skipped;
                continue;
            }
            size_t consumed = 0;
            nxvc_vkd_status st = nxvc_vk_decoder_parse_stream_header(
                dec, stream.data(), stream.size(), &consumed);
            nxvc_vk_decoder_destroy(dec);
            // VERSION for the tool mask, or UNSUPPORTED when a Phase 1
            // check fires first (a stereo vector carries eyes == 2, which
            // docs/SYNTAX.md 12 says to refuse as UNSUPPORTED).  Anything
            // else, including a successful parse, is a bug.
            if (st != NXVC_VKD_ERR_VERSION && st != NXVC_VKD_ERR_UNSUPPORTED) {
                std::printf("FAIL %s: Phase 2 tools 0x%llx must be refused, "
                            "got %s\n",
                            r.name.c_str(), (unsigned long long)tools,
                            vkd_status_token(st));
                ++g_fail;
                continue;
            }
            if (g_verbose)
                std::printf("skip %s: Phase 2 tools 0x%llx, correctly "
                            "refused\n",
                            r.name.c_str(), (unsigned long long)tools);
            ++g_skipped;
            continue;
        }
        check_stream(r.name.c_str(), stream, r.decoded_md5, NXVC_VKD_OUT_AUTO);
    }
}

// ------------------------------------------------------------- rejections
// docs/SYNTAX.md 12: "a conforming decoder never produces output from a
// stream it must reject", and the *named* status matters -- UNSUPPORTED is
// "legal v1 syntax this profile does not implement", BITSTREAM is "this
// cannot be a legal stream at all".  tests/vectors/rejects.md5 pins both.
const char *vkd_status_token(nxvc_vkd_status st) {
    switch (st) {
        case NXVC_VKD_OK: return "OK";
        case NXVC_VKD_ERR_ARG: return "ARG";
        case NXVC_VKD_ERR_UNSUPPORTED: return "UNSUPPORTED";
        case NXVC_VKD_ERR_BITSTREAM: return "BITSTREAM";
        case NXVC_VKD_ERR_TRUNCATED: return "TRUNCATED";
        case NXVC_VKD_ERR_NOMEM: return "NOMEM";
        case NXVC_VKD_ERR_VERSION: return "VERSION";
        default: return "OTHER";
    }
}

void run_rejects() {
    std::string path = std::string(g_vectors_dir) + "/rejects.md5";
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) {
        std::printf("-- no rejects.md5, rejection sweep skipped\n");
        return;
    }
    int n = 0;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[128], md5[64], want[32];
        if (std::sscanf(line, "%127s %63s %31s", name, md5, want) != 3) continue;
        std::vector<uint8_t> stream;
        std::string vp = std::string(g_vectors_dir) + "/" + name + ".nxv";
        if (!read_file(vp, stream)) {
            std::printf("FAIL %s: cannot read %s\n", name, vp.c_str());
            ++g_fail;
            continue;
        }
        if (md5_hex(stream.data(), stream.size()) != md5) {
            std::printf("FAIL %s: rejection vector file md5 differs from the "
                        "manifest\n", name);
            ++g_fail;
            continue;
        }
        // A Phase 2 rejection vector may be malformed for a reason inside
        // the inter syntax, which a Phase 1 decoder never gets far enough to
        // see: it refuses the stream at the tool mask, earlier and with a
        // different but equally correct status.  Such a vector still has to
        // be REFUSED -- that is checked below either way -- but the exact
        // named status is not this decoder's to reproduce until the inter
        // path lands.
        uint64_t tools = 0;
        const bool phase2 = stream_needs_phase2(stream, tools);
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.device_name = device_filter();
        nxvc_vk_decoder *dec = nullptr;
        if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
            nxvc_vk_decoder_destroy(dec);
            std::printf("SKIP %s: no decoder\n", name);
            ++g_skipped;
            continue;
        }
        size_t consumed = 0;
        nxvc_vkd_status st = nxvc_vk_decoder_parse_stream_header(
            dec, stream.data(), stream.size(), &consumed);
        if (st == NXVC_VKD_OK)
            st = nxvc_vk_decode_frame(dec, stream.data() + consumed,
                                      stream.size() - consumed, &consumed);
        nxvc_vk_decoder_destroy(dec);
        const bool match = std::strcmp(vkd_status_token(st), want) == 0;
        if (!match && phase2 &&
            (st == NXVC_VKD_ERR_VERSION || st == NXVC_VKD_ERR_UNSUPPORTED)) {
            // Refused, just earlier than the manifest describes.
            if (g_verbose)
                std::printf("skip %s: Phase 2 tools 0x%llx, refused with %s "
                            "at the tool mask before the manifest's %s\n",
                            name, (unsigned long long)tools,
                            vkd_status_token(st), want);
            ++g_skipped;
            continue;
        }
        ++n;
        ++g_checked;
        if (!match) {
            std::printf("FAIL %s: refused with %s, manifest says %s\n", name,
                        vkd_status_token(st), want);
            ++g_fail;
        } else if (g_verbose) {
            std::printf("ok   %s (%s)\n", name, want);
        }
    }
    std::fclose(f);
    std::printf("-- %d rejection vector(s)\n", n);
}

// -------------------------------------------------------------- synthetic
struct Case {
    std::string name;
    int w, h;
    int c444, kind, qp, lossless, alpha, tskip, nsub, tables, t420, ct, matrix;
    int res_pattern;  // 0 none, 1 cycling 0/1/2, 2 all level 2
    int qp_pattern;
    int frames;
    int wm_id = 0;    // per-tile weighting-matrix override, 0 = frame's
    // [v3] the three v2 intra tools.  -1 means "whatever nxvc_config_default
    // chose", which is all three on; 0 and 1 pin them, so a case can walk the
    // combinations and prove the tools are additive.
    int intra_dir = -1;
    int dir_layer = -1;
    int ctx_v2 = -1;
    int sign_hide = -1;
};

bool encode_case(const Case &c, std::vector<uint8_t> &stream,
                 std::string &err) {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)c.w;
    cfg.height = (uint32_t)c.h;
    cfg.chroma = c.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)c.qp;
    cfg.lossless = (uint32_t)c.lossless;
    cfg.alpha = (uint32_t)c.alpha;
    cfg.transform_skip = (uint32_t)c.tskip;
    cfg.nsub_log2 = (uint32_t)c.nsub;
    cfg.custom_tables = (uint32_t)c.tables;
    cfg.tile_chroma420 = (uint32_t)c.t420;
    cfg.color_transform = (uint32_t)c.ct;
    cfg.color_space =
        c.ct ? (uint32_t)NXVC_CS_RGB : (uint32_t)NXVC_CS_YCBCR_709_LIMITED;
    cfg.quant_matrix = (uint32_t)c.matrix;
    cfg.wm_id = (uint32_t)c.wm_id;
    if (c.intra_dir >= 0) cfg.intra_dir = (uint32_t)c.intra_dir;
    if (c.dir_layer >= 0) cfg.intra_dir_layer = (uint32_t)c.dir_layer;
    if (c.ctx_v2 >= 0) cfg.ctx_v2 = (uint32_t)c.ctx_v2;
    if (c.sign_hide >= 0) cfg.sign_hide = (uint32_t)c.sign_hide;

    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { err = nxvc_status_string(st); return false; }
    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) {
        err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return false;
    }
    stream.assign(hdr.begin(), hdr.begin() + hl);

    nxvc_tile_layout tl;
    nxvc_tile_layout_get(cfg.width, cfg.height, &tl);
    std::vector<uint8_t> qmap(tl.tile_count), rmap(tl.tile_count);
    for (uint32_t i = 0; i < tl.tile_count; ++i) {
        qmap[i] = (uint8_t)(8 + (i * 7) % 50);
        rmap[i] = c.res_pattern == 1 ? (uint8_t)(i % 3) : 2;
    }
    std::vector<uint8_t> fbuf((size_t)c.w * c.h * 8 + (1u << 20));
    for (int f = 0; f < c.frames; ++f) {
        TestImage im = make_image(c.w, c.h, c.c444 != 0, c.kind,
                                  (uint32_t)(7000 + f * 41 + c.kind * 3));
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = (uint8_t *)im.p[p].data();
        img.stride[0] = im.w;
        img.stride[1] = im.cw;
        img.stride[2] = im.cw;
        img.stride[3] = im.w;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img,
                                       c.qp_pattern ? qmap.data() : nullptr,
                                       c.res_pattern ? rmap.data() : nullptr,
                                       fbuf.data(), fbuf.size(), &ol);
        if (st != NXVC_OK) {
            err = nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return false;
        }
        stream.insert(stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);
    return true;
}

std::vector<Case> synthetic_cases(bool quick) {
    std::vector<Case> v;
    auto nm = [](const char *fmt, auto... a) {
        char b[80];
        std::snprintf(b, sizeof b, fmt, a...);
        return std::string(b);
    };
    // QP ladder x chroma format x colour transform.  YCoCg-R requires 4:4:4
    // (docs/SYNTAX.md 2), so this is not a full cross product.
    static const int kQps[] = {0, 8, 16, 24, 32, 40, 51, 63};
    for (int qi = 0; qi < 8; ++qi) {
        if (quick && (qi % 3)) continue;
        int qp = kQps[qi];
        for (int c444 = 0; c444 <= 1; ++c444)
            for (int ct = 0; ct <= c444; ++ct)
                v.push_back({nm("syn_qp%02d_%s_%s", qp, c444 ? "444" : "420",
                                ct ? "ycocgr" : "pass"),
                             192, 128, c444, 1, qp, 0, 0, 0, 3, 0, 0, ct, 1, 0,
                             0, 1});
    }
    if (quick) return v;

    for (int c444 = 0; c444 <= 1; ++c444)
        for (int rp = 1; rp <= 2; ++rp)
            v.push_back({nm("syn_res%d_%s", rp, c444 ? "444" : "420"), 192, 128,
                         c444, 1, 28, 0, 0, 0, 3, 0, 0, 0, 1, rp, 0, 1});
    // Lane counts 1, 2, 32 and the encoder's own per-tile choice.
    for (int ns : {0, 1, 5, 255})
        v.push_back({nm("syn_nsub%d", ns), 192, 128, 0, 1, 28, 0, 0, 0, ns, 0,
                     0, 0, 1, 0, 0, 1});
    v.push_back({"syn_tskip420", 192, 128, 0, 2, 16, 0, 0, 1, 3, 0, 0, 0, 0, 0,
                 0, 1});
    v.push_back({"syn_tables420", 192, 128, 0, 1, 28, 0, 0, 0, 3, 1, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_tile420_in444", 192, 128, 1, 1, 26, 0, 0, 0, 3, 0, 1, 0,
                 1, 0, 0, 1});
    v.push_back({"syn_alpha420", 192, 128, 0, 1, 24, 0, 1, 0, 3, 0, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_alpha444", 192, 128, 1, 2, 24, 0, 1, 0, 3, 0, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_lossless420", 192, 128, 0, 1, 0, 1, 0, 1, 3, 0, 0, 0, 0,
                 0, 0, 1});
    v.push_back({"syn_lossless_ycocgr", 192, 128, 1, 1, 0, 1, 0, 1, 3, 0, 0, 1,
                 0, 0, 0, 1});
    v.push_back({"syn_qpmap_resmap", 192, 128, 0, 1, 30, 0, 0, 0, 3, 0, 0, 0, 2,
                 1, 1, 1});
    for (int m = 0; m < 4; ++m)
        v.push_back({nm("syn_matrix%d", m), 192, 128, 0, 1, 28, 0, 0, 0, 3, 0,
                     0, 0, m, 0, 0, 1});
    for (int wm = 1; wm <= 3; ++wm)
        v.push_back({nm("syn_wm_id%d", wm), 192, 128, 0, 1, 26, 0, 0, 0, 3, 0,
                     0, 0, 1, 0, 0, 1, wm});
    v.push_back({"syn_wm_id2_444", 192, 128, 1, 1, 24, 0, 0, 0, 3, 0, 0, 0, 2,
                 0, 0, 1, 2});
    v.push_back({"syn_odd_200x140", 200, 140, 0, 1, 28, 0, 0, 0, 3, 0, 0, 0, 1,
                 0, 0, 1});
    v.push_back({"syn_tiny_64x64", 64, 64, 0, 4, 28, 0, 0, 0, 3, 0, 0, 0, 1, 0,
                 0, 1});
    v.push_back({"syn_tall_64x320", 64, 320, 0, 3, 22, 0, 0, 0, 3, 0, 0, 0, 1,
                 0, 0, 1});
    v.push_back({"syn_multiframe", 128, 128, 0, 1, 30, 0, 0, 0, 3, 0, 0, 0, 1,
                 0, 0, 3});

    // [v3] The three v2 intra tools are on by default, so every case above
    // already exercises them.  These walk the combinations the other way --
    // each tool alone and all three off -- so the "additive, and off unless
    // the bit is set" claim of docs/SYNTAX.md 12 is checked on synthetic
    // content as well as on the committed vectors.
    auto v3 = [&](const char *name, int c444, int qp, int dir, int layer,
                  int ctx, int sdh, int res_pat = 0, int tsk = 0) {
        Case c{name, 192, 128, c444, 1, qp, 0, 0, tsk, 3, 0, 0, 0, res_pat,
               0,    0,   1};
        c.intra_dir = dir;
        c.dir_layer = layer;
        c.ctx_v2 = ctx;
        c.sign_hide = sdh;
        v.push_back(c);
    };
    v3("syn_v1tools_420", 0, 24, 0, 0, 0, 0);
    v3("syn_v1tools_444", 1, 24, 0, 0, 0, 0);
    v3("syn_dir_only_420", 0, 24, 1, 0, 0, 0);
    v3("syn_dir_only_444", 1, 16, 1, 0, 0, 0);
    v3("syn_ctxv2_only_420", 0, 24, 0, 0, 1, 0);
    v3("syn_sdh_only_420", 0, 24, 0, 0, 0, 1);
    v3("syn_dir_layer_420", 0, 24, 1, 1, 0, 0);
    v3("syn_dir_layer_ctxv2_444", 1, 24, 1, 1, 1, 1);
    if (!quick) {
        // The wavefront meeting the other per-tile shape knobs: cycling
        // res_level (4x4 and 2x2 block planes as well as 8x8) and transform
        // skip, whose residual path skips the transform but not the
        // prediction.
        v3("syn_dir_res_cycle_420", 0, 20, 1, 0, 1, 1, 1, 0);
        v3("syn_dir_res_cycle_444", 1, 20, 1, 0, 1, 1, 1, 0);
        v3("syn_dir_tskip_420", 0, 16, 1, 0, 1, 0, 0, 1);
        v3("syn_dir_layer_tskip_444", 1, 16, 1, 1, 1, 0, 0, 1);
    }
    return v;
}

void run_synthetic(bool quick) {
    std::vector<Case> cases = synthetic_cases(quick);
    std::printf("-- %zu synthetic streams from nxvc_encoder\n", cases.size());
    for (const Case &c : cases) {
        std::vector<uint8_t> s;
        std::string err;
        if (!encode_case(c, s, err)) {
            std::printf("FAIL %s: encode failed (%s)\n", c.name.c_str(),
                        err.c_str());
            ++g_fail;
            continue;
        }
        check_stream(c.name.c_str(), s, "", NXVC_VKD_OUT_AUTO);
        // A 4:4:4 stream also goes through the RGB10A2 store: its 8-bit
        // samples are replicated into 10 bits and read back by taking the top
        // 8, which is a lossless round trip, so the same comparison holds.
        if (!quick && c.c444 && !c.alpha)
            check_stream((c.name + "_rgb10a2").c_str(), s, "",
                         NXVC_VKD_OUT_RGB10A2);
    }
}

// ------------------------------------------------------------------ bench
// PAPER 3.4's decode budget, measured on the shape the headset actually
// streams: two 2048x2048 eyes at 4:2:0, which is 2048 tiles in one frame.
// Informational -- it never fails the test.
int run_bench_qp(int iters, int qp) {
    Case c{"bench_2x2048sq_420", 2048, 4096, 0, 1, qp, 0, 0, 0, 3, 0, 0, 0,
           1,   0,    0, 1};
    std::vector<uint8_t> stream;
    std::string err;
    std::printf("-- encoding %dx%d 4:2:0 at QP %d (%d tiles)\n", c.w, c.h, qp,
                (c.w / 64) * (c.h / 64));
    if (!encode_case(c, stream, err)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.device_name = device_filter();
    nxvc_vk_decoder *dec = nullptr;
    if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
        std::printf("SKIP: %s\n",
                    dec ? nxvc_vk_decoder_last_error(dec) : "no decoder");
        nxvc_vk_decoder_destroy(dec);
        return 77;
    }
    size_t consumed = 0;
    if (nxvc_vk_decoder_parse_stream_header(dec, stream.data(), stream.size(),
                                            &consumed) != NXVC_VKD_OK) {
        std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
        nxvc_vk_decoder_destroy(dec);
        return 1;
    }
    const uint8_t *frame = stream.data() + consumed;
    const size_t flen = stream.size() - consumed;
    double bestA = 1e9, bestB = 1e9, bestG = 1e9, bestT = 1e9;
    nxvc_vkd_stats st{};
    for (int i = 0; i < iters; ++i) {
        size_t c2 = 0;
        if (nxvc_vk_decode_frame(dec, frame, flen, &c2) != NXVC_VKD_OK) {
            std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
            nxvc_vk_decoder_destroy(dec);
            return 1;
        }
        nxvc_vk_decoder_stats(dec, &st);
        if (st.pass_a_ms < bestA) bestA = st.pass_a_ms;
        if (st.pass_b_ms < bestB) bestB = st.pass_b_ms;
        if (st.gpu_ms < bestG) bestG = st.gpu_ms;
        if (st.total_ms < bestT) bestT = st.total_ms;
    }
    std::printf(
        "%s, QP %d: %u tiles, %llu B frame (%llu B payload)\n"
        "  best of %d: Pass A %.3f ms, Pass B %.3f ms, GPU total %.3f ms, "
        "wall %.3f ms\n"
        "  host parse %.3f ms, record+submit %.3f ms\n"
        "  coefficient SSBO %.1f MB written by Pass A and read by Pass B\n",
        nxvc_vk_decoder_device_name(dec), qp, st.tiles,
        (unsigned long long)st.frame_bytes,
        (unsigned long long)st.payload_bytes, iters, bestA, bestB, bestG, bestT,
        st.parse_ms, st.submit_ms, (double)st.coef_bytes / 1e6);
    nxvc_vk_decoder_destroy(dec);
    return 0;
}

// ------------------------------------------------ [v3] bench helpers
// One decoder over one stream, best-of-N per-pass timings.
struct BenchRun {
    double passA = 0, passB = 0, gpu = 0, wall = 0;
    uint64_t frameBytes = 0, payloadBytes = 0, coefBytes = 0;
    uint32_t tiles = 0;
    bool ok = false;
    bool skipped = false;
};

BenchRun time_stream(const std::vector<uint8_t> &stream, int iters,
                     uint32_t dirSched, uint32_t tileSort) {
    BenchRun r;
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.device_name = device_filter();
    nxvc_vk_decoder *dec = nullptr;
    if (nxvc_vk_decoder_create(&ci, &dec) != NXVC_VKD_OK) {
        nxvc_vk_decoder_destroy(dec);
        r.skipped = true;
        return r;
    }
    nxvc_vk_decoder_set_dir_sched(dec, dirSched);
    nxvc_vk_decoder_set_tile_sort(dec, tileSort);
    size_t consumed = 0;
    if (nxvc_vk_decoder_parse_stream_header(dec, stream.data(), stream.size(),
                                            &consumed) != NXVC_VKD_OK) {
        std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
        nxvc_vk_decoder_destroy(dec);
        return r;
    }
    const uint8_t *frame = stream.data() + consumed;
    const size_t flen = stream.size() - consumed;
    double bA = 1e9, bB = 1e9, bG = 1e9, bT = 1e9;
    nxvc_vkd_stats st{};
    for (int i = 0; i < iters; ++i) {
        size_t c2 = 0;
        if (nxvc_vk_decode_frame(dec, frame, flen, &c2) != NXVC_VKD_OK) {
            std::printf("bench: %s\n", nxvc_vk_decoder_last_error(dec));
            nxvc_vk_decoder_destroy(dec);
            return r;
        }
        nxvc_vk_decoder_stats(dec, &st);
        bA = std::min(bA, st.pass_a_ms);
        bB = std::min(bB, st.pass_b_ms);
        bG = std::min(bG, st.gpu_ms);
        bT = std::min(bT, st.total_ms);
    }
    r.passA = bA; r.passB = bB; r.gpu = bG; r.wall = bT;
    r.frameBytes = st.frame_bytes;
    r.payloadBytes = st.payload_bytes;
    r.coefBytes = st.coef_bytes;
    r.tiles = st.tiles;
    r.ok = true;
    nxvc_vk_decoder_destroy(dec);
    return r;
}

// The 2048-tile shape the headset streams, as one 4:2:0 frame.
Case bench_case(int qp, int intra_dir, int res_pattern, int tskip) {
    Case c{"bench", 2048, 4096, 0, 1, qp, 0, 0, tskip, 3, 0, 0, 0,
           res_pattern, 0, 0, 1};
    c.intra_dir = intra_dir;
    return c;
}

// docs/SYNTAX.md 7.6: with the above-right dependency the independent set is
// 2*by + bx, so an nb x nb block plane takes 3*nb - 2 steps; without it, the
// anti-diagonal's 2*nb - 1.  The sub-tile restriction caps nb at 4.  One
// barrier per step, plus the 3 the DC-plane path already pays per plane.
int wavefront_steps(int sched, int nb) {
    int n = (sched & 2) ? std::min(nb, 4) : nb;
    return (sched & 1) ? 2 * n - 1 : 3 * n - 2;
}
// Mean occupancy during the prediction step: nb*nb blocks spread over S
// steps at 4 threads each, against the workgroup's 256.  SYNTAX.md 7.6's
// 4.5 % is this at nb == 8, S == 22.
double wavefront_occupancy_pct(int sched, int nb) {
    return 100.0 * (double)(nb * nb) /
           (64.0 * (double)wavefront_steps(sched, nb));
}

// Barriers reconstruct.comp actually executes for one tile, counted off the
// kernel rather than off the idealized schedule.  Per plane: one after the DC
// dequantize, two more for the second-level IDCT when nb == 8, one after the
// block means, two for the transform's row/column passes, and then either one
// prediction barrier (v1) or one per wavefront step (plus one for the layered
// form's recon -> samples pass).
int barriers_per_plane(int nb, int dirSteps, bool layer) {
    int b = 4 + (nb == 8 ? 2 : 0) + 2;
    b += dirSteps > 0 ? dirSteps + (layer ? 1 : 0) : 1;
    return b;
}
int barriers_per_tile(int sched, bool c444, bool dir, bool layer) {
    const int lumaNb = 8, chromaNb = c444 ? 8 : 4;
    const int ls = dir ? wavefront_steps(sched, lumaNb) : 0;
    const int cs = dir ? wavefront_steps(sched, chromaNb) : 0;
    return barriers_per_plane(lumaNb, ls, layer) +
           2 * barriers_per_plane(chromaNb, cs, layer);
}

// -------------------------------------------------- [v3] wavefront variants
// SYNTAX.md 7.6 prices three schedules in rate; this prices them in decode
// time on the same 2048-tile all-INTRA_DIR frame.  Only schedule 0 decodes
// this stream correctly -- the other two are what a stream encoded under the
// matching restriction would cost -- so the pixels are not compared here,
// only the time.
int run_bench_dir(int iters) {
    struct Variant { uint32_t sched; const char *what; const char *rate; };
    static const Variant kVars[] = {
        {0, "as written (left, above, above-right)", "--"},
        {1, "no above-right reference", "+0.24 %"},
        {3, "no above-right + 32x32 sub-tiles", "+1.8 %"},
    };
    std::vector<uint8_t> stream, base;
    std::string err;
    if (!encode_case(bench_case(24, 1, 0, 0), stream, err) ||
        !encode_case(bench_case(24, 0, 0, 0), base, err)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    BenchRun b0 = time_stream(base, iters, 0, 0);
    if (b0.skipped) { std::printf("SKIP: no usable Vulkan device\n"); return 77; }
    if (!b0.ok) return 1;
    std::printf(
        "\n-- Pass B wavefront variants, 2048 tiles 4:2:0 QP 24, best of %d\n"
        "   INTRA_DIR off (v1 planar predictor, no wavefront): Pass B %.3f ms,"
        " %d barriers/tile\n",
        iters, b0.passB, barriers_per_tile(0, false, false, false));
    for (const Variant &v : kVars) {
        BenchRun r = time_stream(stream, iters, v.sched, 0);
        if (!r.ok) return 1;
        std::printf(
            "   sched %u  %-38s  Pass B %.3f ms  (%.2fx v1)  "
            "%2d steps  %3d barriers/tile  occupancy %.1f %%  rate %s\n",
            v.sched, v.what, r.passB, r.passB / b0.passB,
            wavefront_steps((int)v.sched, 8),
            barriers_per_tile((int)v.sched, false, true, false),
            wavefront_occupancy_pct((int)v.sched, 8), v.rate);
    }
    return 0;
}

// ------------------------------------------- [v3] fixed vs per-byte cost
// The GDeflate question: how much of a frame's decode time is per-tile
// overhead that 64x64 tiling buys us, and how much is per-byte work?  A
// straight least-squares fit of pass time against payload size over the QP
// ladder separates the two.  Note that COEFFICIENT traffic is the same at
// every QP -- the layout between the passes is dense -- so the slope is per
// megabyte of entropy-coded payload, and the intercept is what a frame costs
// with no payload at all.
int run_bench_overhead(int iters) {
    static const int kQps[] = {63, 51, 36, 24, 12};
    double x[5] = {}, ya[5] = {}, yb[5] = {};
    int n = 0;
    uint32_t tiles = 0;
    double coefMB = 0;
    std::printf("\n-- fixed vs per-byte decode cost, 2048 tiles 4:2:0, "
                "best of %d\n", iters);
    for (int qp : kQps) {
        std::vector<uint8_t> stream;
        std::string err;
        if (!encode_case(bench_case(qp, -1, 0, 0), stream, err)) {
            std::printf("bench: encode failed (%s)\n", err.c_str());
            return 1;
        }
        BenchRun r = time_stream(stream, iters, 0, 0);
        if (r.skipped) { std::printf("SKIP: no usable Vulkan device\n"); return 77; }
        if (!r.ok) return 1;
        x[n] = (double)r.payloadBytes / 1e6;
        ya[n] = r.passA;
        yb[n] = r.passB;
        tiles = r.tiles;
        coefMB = (double)r.coefBytes / 1e6;
        std::printf("   QP %2d: payload %6.3f MB   Pass A %7.3f ms   "
                    "Pass B %6.3f ms\n", qp, x[n], ya[n], yb[n]);
        ++n;
    }
    auto fit = [&](const double *y, double &a, double &b) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
        double den = n * sxx - sx * sx;
        b = den != 0 ? (n * sxy - sx * sy) / den : 0;   // slope, ms per MB
        a = (sy - b * sx) / n;                          // intercept, ms
    };
    double ia, sa, ib, sb;
    fit(ya, ia, sa);
    fit(yb, ib, sb);
    std::printf(
        "   Pass A: %.3f ms fixed + %.3f ms per MB of payload\n"
        "   Pass B: %.3f ms fixed + %.3f ms per MB of payload\n"
        "   per tile at zero payload: Pass A %.1f ns, Pass B %.1f ns "
        "(%u tiles)\n"
        "   coefficient traffic is %.1f MB at every QP: the layout between the "
        "passes is dense\n",
        ia, sa, ib, sb, ia * 1e6 / tiles, ib * 1e6 / tiles, tiles, coefMB);
    return 0;
}

// ------------------------------------- [v3] host-side tile grouping for Pass B
// The divergence question: Pass A already groups its dispatches by lane count;
// does grouping Pass B's workgroups by tile shape pay?  Measured on a frame
// whose tiles deliberately differ -- cycling res_level and the encoder's own
// per-tile transform-skip decision -- so neighbouring workgroups take
// different branches unless they are sorted.
int run_bench_sort(int iters) {
    std::vector<uint8_t> stream;
    std::string err;
    if (!encode_case(bench_case(24, -1, 1, 2), stream, err)) {
        std::printf("bench: encode failed (%s)\n", err.c_str());
        return 1;
    }
    BenchRun off = time_stream(stream, iters, 0, 0);
    if (off.skipped) { std::printf("SKIP: no usable Vulkan device\n"); return 77; }
    BenchRun on = time_stream(stream, iters, 0, 1);
    if (!off.ok || !on.ok) return 1;
    std::printf(
        "\n-- Pass B workgroup ordering, 2048 tiles 4:2:0 QP 24, mixed "
        "res_level and transform skip, best of %d\n"
        "   raster order      : Pass B %.3f ms\n"
        "   sorted by shape   : Pass B %.3f ms  (%+.1f %%)\n",
        iters, off.passB, on.passB,
        100.0 * (on.passB - off.passB) / off.passB);
    return 0;
}

// Pass A's cost scales with the symbol rate, so the budget is quoted over a
// bitrate range rather than at one operating point.
int run_bench(int iters) {
    for (int qp : {12, 24, 36}) {
        int rc = run_bench_qp(iters, qp);
        if (rc) return rc;
    }
    int rc = run_bench_dir(iters);
    if (rc) return rc;
    if ((rc = run_bench_overhead(iters))) return rc;
    if ((rc = run_bench_sort(iters))) return rc;
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    bool quick = false, do_vectors = true, do_synth = true;
    int bench = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--vectors" && i + 1 < argc) g_vectors_dir = argv[++i];
        else if (a == "--quick") quick = true;
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--only-vectors") do_synth = false;
        else if (a == "--only-synthetic") do_vectors = false;
        else if (a == "--bench") bench = i + 1 < argc && argv[i + 1][0] != '-'
                                             ? std::atoi(argv[++i])
                                             : 20;
        else {
            std::fprintf(stderr,
                         "usage: %s [--vectors DIR] [--quick] [--verbose]\n",
                         argv[0]);
            return 2;
        }
    }
    if (!g_vectors_dir) g_vectors_dir = NXVC_VECTORS_DIR;
    if (bench) return run_bench(bench);

    // Probe once, so "no ICD" is one skip rather than 32 identical failures.
    {
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.device_name = device_filter();
        nxvc_vk_decoder *dec = nullptr;
        nxvc_vkd_status st = nxvc_vk_decoder_create(&ci, &dec);
        if (st != NXVC_VKD_OK) {
            std::printf("SKIP: %s\n",
                        dec ? nxvc_vk_decoder_last_error(dec) : "no decoder");
            nxvc_vk_decoder_destroy(dec);
            return 77;
        }
        std::printf("-- device: %s\n", nxvc_vk_decoder_device_name(dec));
        nxvc_vk_decoder_destroy(dec);
    }

    if (do_vectors) {
        run_vectors();
        run_rejects();
    }
    if (do_synth) run_synthetic(quick);

    std::printf("-- %d stream(s) checked, %d skipped, %d failure(s)\n",
                g_checked, g_skipped, g_fail);
    if (g_checked == 0) {
        std::printf("SKIP: nothing was checked\n");
        return 77;
    }
    return g_fail ? 1 : 0;
}
