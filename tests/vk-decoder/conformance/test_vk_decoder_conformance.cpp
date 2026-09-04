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
        ++n;
        ++g_checked;
        if (std::strcmp(vkd_status_token(st), want) != 0) {
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

// Pass A's cost scales with the symbol rate, so the budget is quoted over a
// bitrate range rather than at one operating point.
int run_bench(int iters) {
    for (int qp : {12, 24, 36}) {
        int rc = run_bench_qp(iters, qp);
        if (rc) return rc;
    }
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
