// nxv-dec: decode an .nxv stream to raw planar 8-bit YUV frames.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"

static void usage() {
    std::fprintf(stderr,
        "usage: nxv-dec --in out.nxv --out out.yuv [--pix yuv444p|yuv420p]\n"
        "  --frames N   decode at most N frames\n"
        "  --quiet\n");
}

int main(int argc, char **argv) {
    std::string in, out, pix;
    int frames = -1, quiet = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--in") in = val();
        else if (a == "--out") out = val();
        else if (a == "--pix") pix = val();
        else if (a == "--frames") frames = std::atoi(val());
        else if (a == "--quiet") quiet = 1;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (in.empty() || out.empty()) { usage(); return 2; }

    std::FILE *fi = std::fopen(in.c_str(), "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::fseek(fi, 0, SEEK_END);
    long fsz = std::ftell(fi);
    std::fseek(fi, 0, SEEK_SET);
    if (fsz <= 0) { std::fprintf(stderr, "empty input\n"); return 1; }
    std::vector<uint8_t> data((size_t)fsz);
    if (std::fread(data.data(), 1, data.size(), fi) != data.size()) {
        std::fprintf(stderr, "short read\n");
        return 1;
    }
    std::fclose(fi);

    nxvc_status st;
    nxvc_decoder *dec = nxvc_decoder_create(&st);
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(dec, data.data(), data.size(), &consumed);
    if (st != NXVC_OK) {
        std::fprintf(stderr, "stream header: %s\n", nxvc_status_string(st));
        return 1;
    }
    off += consumed;
    nxvc_stream_info si;
    nxvc_decoder_stream_info(dec, &si);
    const char *want = si.chroma == NXVC_CHROMA_444 ? "yuv444p" : "yuv420p";
    if (!pix.empty() && pix != want) {
        std::fprintf(stderr, "stream is %s, --pix says %s\n", want, pix.c_str());
        return 1;
    }
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(dec, 0, &yw, &yh);
    nxvc_decoder_plane_size(dec, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch), V((size_t)cw * ch),
        A((size_t)yw * yh, 255);

    std::FILE *fo = std::fopen(out.c_str(), "wb");
    if (!fo) { std::perror("open output"); return 1; }
    int n = 0;
    while (off < data.size() && (frames < 0 || n < frames)) {
        nxvc_image img{};
        img.plane[0] = Y.data(); img.stride[0] = (int)yw;
        img.plane[1] = U.data(); img.stride[1] = (int)cw;
        img.plane[2] = V.data(); img.stride[2] = (int)cw;
        img.plane[3] = A.data(); img.stride[3] = (int)yw;
        st = nxvc_decoder_decode_frame(dec, data.data() + off, data.size() - off,
                                       &img, &consumed);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "frame %d: %s\n", n, nxvc_status_string(st));
            return 1;
        }
        std::fwrite(Y.data(), 1, Y.size(), fo);
        std::fwrite(U.data(), 1, U.size(), fo);
        std::fwrite(V.data(), 1, V.size(), fo);
        if (si.alpha) std::fwrite(A.data(), 1, A.size(), fo);
        off += consumed;
        ++n;
    }
    std::fclose(fo);
    if (!quiet)
        std::printf("%d frame(s), %ux%u %s%s\n", n, yw, yh, want,
                    si.alpha ? " +alpha" : "");
    nxvc_decoder_destroy(dec);
    return n > 0 ? 0 : 1;
}
