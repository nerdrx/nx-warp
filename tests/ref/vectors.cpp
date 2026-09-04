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
};

static const VecSpec kVectors[] = {
    // name                     w    h  444 kind qp  ll  a  ts nsub tab t420 ct mat res qpp fr
    {"v01_intra420_qp12",      192, 128, 0,  1, 12,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v02_intra420_qp24",      192, 128, 0,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v03_intra420_qp36",      192, 128, 0,  1, 36,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v04_intra420_qp51",      192, 128, 0,  1, 51,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v05_intra444_qp24",      192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v06_gradient420_qp20",   192, 128, 0,  0, 20,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v07_checker420_qp28",    192, 128, 0,  2, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v08_noise420_qp28",      192, 128, 0,  3, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v09_flat420_qp28",       192, 128, 0,  4, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v10_lossless420",        192, 128, 0,  1,  0,  1, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1},
    {"v11_lossless444",        192, 128, 1,  1,  0,  1, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1},
    {"v12_lossless444_alpha",  192, 128, 1,  2,  0,  1, 1,  1,  3,  0,  0, 0,  0,  0,  0, 1},
    {"v13_tskip420_qp16",      192, 128, 0,  2, 16,  0, 0,  1,  3,  0,  0, 0,  0,  0,  0, 1},
    {"v14_alpha420_qp24",      192, 128, 0,  1, 24,  0, 1,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v15_res_cycle420",       192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  1,  0, 1},
    {"v16_res_level2_420",     192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  2,  0, 1},
    {"v17_res_cycle444",       192, 128, 1,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  1,  0, 1},
    {"v18_qpmap420",           192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  1, 1},
    {"v19_qp_res_map420",      192, 128, 0,  1, 30,  0, 0,  0,  3,  0,  0, 0,  2,  1,  1, 1},
    {"v20_tile420_in444",      192, 128, 1,  1, 26,  0, 0,  0,  3,  0,  1, 0,  1,  0,  0, 1},
    {"v21_ycocgr444_qp24",     192, 128, 1,  1, 24,  0, 0,  0,  3,  0,  0, 1,  1,  0,  0, 1},
    {"v22_ycocgr_lossless",    192, 128, 1,  1,  0,  1, 0,  1,  3,  0,  0, 1,  0,  0,  0, 1},
    {"v23_custom_tables420",   192, 128, 0,  1, 28,  0, 0,  0,  3,  1,  0, 0,  1,  0,  0, 1},
    {"v24_nsub0_420",          192, 128, 0,  1, 28,  0, 0,  0,  0,  0,  0, 0,  1,  0,  0, 1},
    {"v25_nsub5_420",          192, 128, 0,  1, 28,  0, 0,  0,  5,  0,  0, 0,  1,  0,  0, 1},
    {"v26_nsub_auto_420",      192, 128, 0,  1, 28,  0, 0,  0,255,  0,  0, 0,  1,  0,  0, 1},
    {"v27_matrix0_420",        192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  0,  0,  0, 1},
    {"v28_matrix3_420",        192, 128, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  3,  0,  0, 1},
    {"v29_odd_size_200x140",   200, 140, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v30_tiny_64x64",          64,  64, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v31_wide_320x64",        320,  64, 0,  1, 28,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 1},
    {"v32_multiframe420",      128, 128, 0,  1, 30,  0, 0,  0,  3,  0,  0, 0,  1,  0,  0, 3},
};
static const int kNumVectors = (int)(sizeof(kVectors) / sizeof(kVectors[0]));

struct Result {
    std::vector<uint8_t> stream;
    std::string stream_md5, decoded_md5;
    bool ok = false;
    std::string err;
};

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
    cfg.quant_matrix = (uint32_t)v.matrix;

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
    return test_report("test_vectors");
}
