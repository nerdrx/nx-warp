// nxv-enc: encode raw planar 8-bit YUV frames to an .nxv stream.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"

static void usage() {
    std::fprintf(stderr,
        "usage: nxv-enc --in file.yuv --w W --h H --pix yuv444p|yuv420p\n"
        "               --qp N [--res-map file] --out out.nxv\n"
        "optional:\n"
        "  --qp-map FILE        per-tile QP bytes, tile_count per frame\n"
        "  --frames N           encode at most N frames\n"
        "  --lossless           QP 0 + transform skip (bit exact)\n"
        "  --tskip off|on|auto  transform-skip decision (default off)\n"
        "  --nsub 0..5|auto     rANS lane count log2 (default 3 = 8 lanes)\n"
        "  --matrix 0..3        weighting matrix (default 1)\n"
        "  --chroma-qp-off N    chroma QP offset\n"
        "  --custom-tables      derive and transmit probability tables\n"
        "  --tile-420           code 4:2:0 tiles inside a 4:4:4 stream\n"
        "  --rgb                input planes are R,G,B; apply YCoCg-R\n"
        "  --quiet\n");
}

static bool read_exact(std::FILE *f, void *p, size_t n) {
    return std::fread(p, 1, n, f) == n;
}

int main(int argc, char **argv) {
    std::string in, out, pix = "yuv420p", resmap_path, qpmap_path;
    int W = 0, H = 0, qp = 24, frames = -1, matrix = 1, chroma_qp_off = 0;
    int lossless = 0, tile420 = 0, custom_tables = 0, rgb = 0, quiet = 0;
    int tskip = 0, nsub = 3;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--in") in = val();
        else if (a == "--out") out = val();
        else if (a == "--w") W = std::atoi(val());
        else if (a == "--h") H = std::atoi(val());
        else if (a == "--pix") pix = val();
        else if (a == "--qp") qp = std::atoi(val());
        else if (a == "--res-map") resmap_path = val();
        else if (a == "--qp-map") qpmap_path = val();
        else if (a == "--frames") frames = std::atoi(val());
        else if (a == "--lossless") lossless = 1;
        else if (a == "--tile-420") tile420 = 1;
        else if (a == "--custom-tables") custom_tables = 1;
        else if (a == "--rgb") rgb = 1;
        else if (a == "--quiet") quiet = 1;
        else if (a == "--matrix") matrix = std::atoi(val());
        else if (a == "--chroma-qp-off") chroma_qp_off = std::atoi(val());
        else if (a == "--tskip") {
            std::string v = val();
            tskip = v == "on" ? 1 : (v == "auto" ? 2 : 0);
        } else if (a == "--nsub") {
            std::string v = val();
            nsub = v == "auto" ? 255 : std::atoi(v.c_str());
        } else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (in.empty() || out.empty() || W <= 0 || H <= 0) { usage(); return 2; }
    if (pix != "yuv420p" && pix != "yuv444p") {
        std::fprintf(stderr, "--pix must be yuv420p or yuv444p\n");
        return 2;
    }

    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)W;
    cfg.height = (uint32_t)H;
    cfg.chroma = pix == "yuv444p" ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)(qp < 0 ? 0 : (qp > 63 ? 63 : qp));
    cfg.quant_matrix = (uint32_t)matrix;
    cfg.chroma_qp_off = chroma_qp_off;
    cfg.lossless = (uint32_t)lossless;
    cfg.transform_skip = (uint32_t)tskip;
    cfg.nsub_log2 = (uint32_t)nsub;
    cfg.tile_chroma420 = (uint32_t)tile420;
    cfg.custom_tables = (uint32_t)custom_tables;
    cfg.color_transform = rgb ? NXVC_CT_YCOCGR : NXVC_CT_NONE;

    nxvc_status st;
    nxvc_encoder *enc = nxvc_encoder_create(&cfg, &st);
    if (!enc) {
        std::fprintf(stderr, "encoder create failed: %s\n", nxvc_status_string(st));
        return 1;
    }

    nxvc_tile_layout tl;
    nxvc_tile_layout_get(cfg.width, cfg.height, &tl);
    const size_t cw = cfg.chroma == NXVC_CHROMA_444 ? (size_t)W : (size_t)((W + 1) / 2);
    const size_t chh = cfg.chroma == NXVC_CHROMA_444 ? (size_t)H : (size_t)((H + 1) / 2);
    const size_t ysz = (size_t)W * H, csz = cw * chh;

    std::FILE *fi = std::fopen(in.c_str(), "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::FILE *fo = std::fopen(out.c_str(), "wb");
    if (!fo) { std::perror("open output"); return 1; }
    std::FILE *fr = nullptr, *fq = nullptr;
    if (!resmap_path.empty()) {
        fr = std::fopen(resmap_path.c_str(), "rb");
        if (!fr) { std::perror("open res map"); return 1; }
    }
    if (!qpmap_path.empty()) {
        fq = std::fopen(qpmap_path.c_str(), "rb");
        if (!fq) { std::perror("open qp map"); return 1; }
    }

    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(enc, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) { std::fprintf(stderr, "header: %s\n", nxvc_status_string(st)); return 1; }
    std::fwrite(hdr.data(), 1, hl, fo);

    std::vector<uint8_t> Y(ysz), U(csz), V(csz);
    std::vector<uint8_t> rmap(tl.tile_count), qmap(tl.tile_count);
    std::vector<uint8_t> outbuf(ysz * 4 + csz * 8 + (1u << 20));
    size_t total = hl;
    int n = 0;
    while (frames < 0 || n < frames) {
        if (!read_exact(fi, Y.data(), ysz)) break;
        if (!read_exact(fi, U.data(), csz) || !read_exact(fi, V.data(), csz)) {
            std::fprintf(stderr, "short frame %d\n", n);
            break;
        }
        const uint8_t *rm = nullptr, *qm = nullptr;
        if (fr && read_exact(fr, rmap.data(), rmap.size())) rm = rmap.data();
        if (fq && read_exact(fq, qmap.data(), qmap.size())) qm = qmap.data();
        nxvc_image img{};
        img.plane[0] = Y.data(); img.stride[0] = W;
        img.plane[1] = U.data(); img.stride[1] = (int)cw;
        img.plane[2] = V.data(); img.stride[2] = (int)cw;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(enc, &img, qm, rm, outbuf.data(),
                                       outbuf.size(), &ol);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "encode frame %d: %s\n", n, nxvc_status_string(st));
            return 1;
        }
        std::fwrite(outbuf.data(), 1, ol, fo);
        total += ol;
        if (!quiet)
            std::printf("frame %d: %zu bytes  %.4f bpp\n", n, ol,
                        ol * 8.0 / ((double)W * H));
        ++n;
    }
    std::fclose(fo);
    std::fclose(fi);
    if (fr) std::fclose(fr);
    if (fq) std::fclose(fq);
    if (!quiet)
        std::printf("%d frame(s), %zu bytes total, %.4f bpp mean\n", n, total,
                    n ? total * 8.0 / ((double)W * H * n) : 0.0);
    nxvc_encoder_destroy(enc);
    return n > 0 ? 0 : 1;
}
