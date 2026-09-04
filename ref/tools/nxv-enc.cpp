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
        "  --matrix 0..3        frame weighting matrix (default 1)\n"
        "  --wm 0..3|auto       per-tile weighting matrix id (default 0)\n"
        "  --no-rdo             plain dead-zone quantizer (default: RD trellis)\n"
        "  --rdo-lambda F       RD lambda scale (default 0.30)\n"
        "  --qp-search N        try per-tile qp_delta in [-N, +N] (default 0)\n"
        "  --intra-dir on|off|layer  directional intra (tool 17); `layer`\n"
        "                       predicts the DC-plane residual instead\n"
        "  --intra-dir-cand N   modes RD-checked per block (default 2)\n"
        "  --ctx v1|v2          12 or 16 entropy contexts (tool 21)\n"
        "  --no-sign-hide       code every sign (default: hide one per unit)\n"
        "  --chroma-qp-off N    chroma QP offset\n"
        "  --custom-tables      derive and transmit probability tables\n"
        "  --tile-420           code 4:2:0 tiles inside a 4:4:4 stream\n"
        "  --rgb                input planes are R,G,B; apply YCoCg-R\n"
        "  --color-space S      unspecified|yuv709l|yuv709f (YCbCr passthrough)\n"
        "  --stats              print where the bits went\n"
        "  --quiet\n");
}

static bool read_exact(std::FILE *f, void *p, size_t n) {
    return std::fread(p, 1, n, f) == n;
}

int main(int argc, char **argv) {
    std::string in, out, pix = "yuv420p", resmap_path, qpmap_path;
    int W = 0, H = 0, qp = 24, frames = -1, matrix = 1, chroma_qp_off = 0;
    int lossless = 0, tile420 = 0, custom_tables = 1, rgb = 0, quiet = 0;
    int tskip = 0, nsub = 255, stats = 0;  // nsub 255 = auto lane count
    int color_space = 0;
    int rdo = 1, rdo_lambda_q8 = 0, qp_search = 0, wm = 0;
    // These mirror nxvc_config_default(): the v2 intra tools are on.
    int intra_dir = 1, intra_dir_layer = 0, ctx_v2 = 1, dir_cand = 0;
    int sign_hide = 1;

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
        else if (a == "--no-custom-tables") custom_tables = 0;
        else if (a == "--rgb") rgb = 1;
        else if (a == "--color-space") {
            std::string v = val();
            if (v == "unspecified") color_space = 0;
            else if (v == "yuv709l") color_space = 1;
            else if (v == "yuv709f") color_space = 2;
            else { std::fprintf(stderr, "--color-space: unspecified|yuv709l|yuv709f\n"); return 2; }
        }
        else if (a == "--quiet") quiet = 1;
        else if (a == "--stats") stats = 1;
        else if (a == "--matrix") matrix = std::atoi(val());
        else if (a == "--rdo") rdo = 1;
        else if (a == "--no-rdo") rdo = 0;
        else if (a == "--rdo-lambda") rdo_lambda_q8 = (int)(std::atof(val()) * 256.0 + 0.5);
        else if (a == "--qp-search") qp_search = std::atoi(val());
        else if (a == "--intra-dir") {
            std::string v = val();
            if (v == "on") { intra_dir = 1; intra_dir_layer = 0; }
            else if (v == "layer") { intra_dir = 1; intra_dir_layer = 1; }
            else if (v == "off") intra_dir = 0;
            else { std::fprintf(stderr, "--intra-dir: on|off|layer\n"); return 2; }
        }
        else if (a == "--intra-dir-cand") dir_cand = std::atoi(val());
        else if (a == "--sign-hide") sign_hide = 1;
        else if (a == "--no-sign-hide") sign_hide = 0;
        else if (a == "--ctx") {
            std::string v = val();
            if (v == "v2") ctx_v2 = 1;
            else if (v == "v1") ctx_v2 = 0;
            else { std::fprintf(stderr, "--ctx: v1|v2\n"); return 2; }
        }
        else if (a == "--wm") { std::string v = val(); wm = v == "auto" ? 255 : std::atoi(v.c_str()); }
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
    cfg.rdo = (uint32_t)rdo;
    cfg.rdo_lambda_q8 = (uint32_t)rdo_lambda_q8;
    cfg.qp_search = (uint32_t)qp_search;
    cfg.wm_id = (uint32_t)wm;
    cfg.intra_dir = (uint32_t)intra_dir;
    cfg.intra_dir_layer = (uint32_t)intra_dir_layer;
    cfg.intra_dir_cand = (uint32_t)dir_cand;
    cfg.ctx_v2 = (uint32_t)ctx_v2;
    cfg.sign_hide = (uint32_t)sign_hide;
    cfg.chroma_qp_off = chroma_qp_off;
    cfg.lossless = (uint32_t)lossless;
    cfg.transform_skip = (uint32_t)tskip;
    cfg.nsub_log2 = (uint32_t)nsub;
    cfg.tile_chroma420 = (uint32_t)tile420;
    cfg.custom_tables = (uint32_t)custom_tables;
    cfg.color_transform = rgb ? NXVC_CT_YCOCGR : NXVC_CT_NONE;
    cfg.collect_stats = (uint32_t)stats;
    cfg.color_space = rgb ? (uint32_t)NXVC_CS_RGB : (uint32_t)color_space;

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
        if (stats) {
            nxvc_encode_stats st2;
            nxvc_encoder_stats(enc, &st2);
            double px = (double)W * H;
            auto row = [&](const char *name, double bytes) {
                std::printf("  %-18s %9.0f B  %6.2f%%  %.5f bpp\n", name, bytes,
                            100.0 * bytes / (double)st2.bytes_total,
                            bytes * 8.0 / px);
            };
            std::printf("bit breakdown, frame %d (%llu tiles, %.2f lanes/tile, "
                        "%llu transform-skip)\n", n,
                        (unsigned long long)st2.tiles,
                        st2.tiles ? (double)st2.lanes_total / (double)st2.tiles : 0.0,
                        (unsigned long long)st2.tiles_tskip);
            row("frame header", (double)st2.bytes_frame_header);
            row("prob tables", (double)st2.bytes_tables);
            row("tile-row headers", (double)st2.bytes_row_headers);
            row("tile headers", (double)st2.bytes_tile_headers);
            row("rANS init/flush", (double)st2.bytes_rans_init);
            row("  DC planes", st2.bits_dc_plane / 8.0);
            row("  luma blocks", st2.bits_luma_blocks / 8.0);
            row("  chroma blocks", st2.bits_chroma_blocks / 8.0);
            if (st2.bits_alpha_blocks)
                row("  alpha blocks", st2.bits_alpha_blocks / 8.0);
            row("payload total", (double)st2.bytes_payload);
            std::printf("  res levels 0/1/2: %llu / %llu / %llu\n",
                        (unsigned long long)st2.tiles_res[0],
                        (unsigned long long)st2.tiles_res[1],
                        (unsigned long long)st2.tiles_res[2]);
        }
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
