// roundtrip_psnr.cpp -- encode, decode, measure, in one process.
//
//   Reads a raw planar YUV sequence, runs every frame through the encoder and
//   straight back through the decoder in memory, and prints per-plane PSNR and
//   the JVET-weighted (6Y + Cb + Cr) / 8 figure, per frame and overall.
//
// Run:
//   nxvc-example-roundtrip --in in.yuv --w 1024 --h 512 --pix yuv444p
//       --qp 24 [--frames N] [--lossless] [--csv out.csv]
//
// Why this example exists.  A codec bug that survives the unit tests usually
// shows up first as a PSNR that is *too good* (an accidental passthrough) or a
// PSNR that collapses on one frame (a state bug that only bites the second
// frame).  Running both halves in one process, with no files in between, makes
// that a two-second check instead of a scripted one.
//
// The numbers here are NOT the quality harness.  tools/quality/compare.py is:
// it drives the real CLIs, runs the x264/x265 anchors, does BD-rate, and
// evaluates the paper's gates.  This is the ten-line version you reach for when
// you have just changed a quantiser and want to know if it got better.
//
// Two properties worth asserting by hand while you are in here:
//   * --lossless must give "inf dB" on every plane.  Anything else is a bug in
//     the transform-skip path, not a rounding artefact.
//   * PSNR must fall monotonically as --qp rises.  ref.codec tests that; if it
//     ever fails here first, the test's material is not representative.

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <nxvc/nxvc.h>

namespace {

struct PlaneGeom {
    uint32_t w = 0, h = 0;
    size_t bytes() const { return size_t(w) * h; }
};

// Sum of squared error between two tightly packed 8-bit planes.
double sse(const uint8_t* a, const uint8_t* b, size_t n) {
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double d = double(int(a[i]) - int(b[i]));
        acc += d * d;
    }
    return acc;
}

// PSNR for 8-bit content.  Infinite MSE-of-zero is reported as +inf, not as
// some large sentinel: a caller that averages sentinels gets a wrong answer,
// and a caller that averages infinities notices.
double psnr_from_mse(double mse) {
    if (mse <= 0.0) return INFINITY;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

void usage() {
    std::fprintf(stderr,
                 "usage: nxvc-example-roundtrip --in FILE --w W --h H "
                 "[--pix yuv420p|yuv444p] [--qp 0..63] [--frames N] "
                 "[--lossless] [--csv FILE]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string in_path, csv_path, pix = "yuv420p";
    uint32_t w = 0, h = 0, qp = 28, want_frames = 0;
    bool lossless = false;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto take = [&](const char* name) { return a == name && v && ++i; };
        if (take("--in")) in_path = v;
        else if (take("--csv")) csv_path = v;
        else if (take("--pix")) pix = v;
        else if (take("--w")) w = uint32_t(std::strtoul(v, nullptr, 10));
        else if (take("--h")) h = uint32_t(std::strtoul(v, nullptr, 10));
        else if (take("--qp")) qp = uint32_t(std::strtoul(v, nullptr, 10));
        else if (take("--frames")) want_frames = uint32_t(std::strtoul(v, nullptr, 10));
        else if (a == "--lossless") lossless = true;
        else { usage(); return 2; }
    }
    if (in_path.empty() || !w || !h) { usage(); return 2; }

    const bool c420 = (pix == "yuv420p");
    if (!c420 && pix != "yuv444p") {
        std::fprintf(stderr, "--pix must be yuv420p or yuv444p\n");
        return 2;
    }
    // Lossless in v1 implies 4:4:4 (a 4:2:0 chroma decimation is not
    // reversible), so say so rather than letting the encoder refuse the config.
    if (lossless && c420) {
        std::fprintf(stderr, "--lossless requires --pix yuv444p (4:2:0 discards chroma)\n");
        return 2;
    }

    // ---------------------------------------------------------------- encoder
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = w;
    cfg.height = h;
    cfg.chroma = c420 ? NXVC_CHROMA_420 : NXVC_CHROMA_444;
    cfg.base_qp = qp;
    cfg.lossless = lossless ? 1u : 0u;

    nxvc_status st{};
    nxvc_encoder* enc = nxvc_encoder_create(&cfg, &st);
    if (!enc) {
        std::fprintf(stderr, "encoder_create: %s\n", nxvc_status_string(st));
        return 1;
    }

    uint8_t hdr[512];
    size_t hdr_len = 0;
    st = nxvc_encoder_stream_header(enc, hdr, sizeof hdr, &hdr_len);
    if (st != NXVC_OK) {
        std::fprintf(stderr, "stream_header: %s\n", nxvc_status_string(st));
        nxvc_encoder_destroy(enc);
        return 1;
    }

    // ---------------------------------------------------------------- decoder
    // The decoder is fed the very header the encoder just produced.  That is
    // the point: the two halves agree on geometry through the bitstream alone.
    nxvc_decoder* dec = nxvc_decoder_create(&st);
    if (!dec) {
        std::fprintf(stderr, "decoder_create: %s\n", nxvc_status_string(st));
        nxvc_encoder_destroy(enc);
        return 1;
    }
    size_t consumed = 0;
    st = nxvc_decoder_parse_stream_header(dec, hdr, hdr_len, &consumed);
    if (st != NXVC_OK) {
        std::fprintf(stderr, "parse_stream_header: %s\n", nxvc_status_string(st));
        nxvc_encoder_destroy(enc);
        nxvc_decoder_destroy(dec);
        return 1;
    }

    nxvc_stream_info si;
    nxvc_decoder_stream_info(dec, &si);
    const int nplanes = si.alpha ? 4 : 3;

    PlaneGeom g[4];
    for (int p = 0; p < nplanes; p++) {
        if (nxvc_decoder_plane_size(dec, p, &g[p].w, &g[p].h) != NXVC_OK) {
            std::fprintf(stderr, "plane_size(%d) failed\n", p);
            return 1;
        }
    }

    std::vector<std::vector<uint8_t>> src(nplanes), out(nplanes);
    nxvc_image in_img{}, out_img{};
    size_t frame_bytes = 0;
    for (int p = 0; p < nplanes; p++) {
        src[p].resize(g[p].bytes());
        out[p].resize(g[p].bytes());
        in_img.plane[p] = src[p].data();
        in_img.stride[p] = int32_t(g[p].w);
        out_img.plane[p] = out[p].data();
        out_img.stride[p] = int32_t(g[p].w);
        frame_bytes += g[p].bytes();
    }

    std::FILE* fin = std::fopen(in_path.c_str(), "rb");
    if (!fin) { std::perror(in_path.c_str()); return 1; }
    std::FILE* fcsv = nullptr;
    if (!csv_path.empty()) {
        fcsv = std::fopen(csv_path.c_str(), "w");
        if (!fcsv) { std::perror(csv_path.c_str()); return 1; }
        std::fprintf(fcsv, "frame,bytes,psnr_y,psnr_u,psnr_v,psnr_ycbcr\n");
    }

    std::vector<uint8_t> bs(frame_bytes + frame_bytes / 2 + (1u << 16));

    std::printf("%-6s %10s %8s %8s %8s %9s\n", "frame", "bytes", "psnr_y",
                "psnr_u", "psnr_v", "weighted");

    // Running totals are accumulated in the SSE domain, never as an average of
    // per-frame dB.  Averaging decibels is the single most common way to report
    // a quality number that is quietly wrong.
    double tot_sse[4] = {0, 0, 0, 0};
    size_t tot_px[4] = {0, 0, 0, 0};
    uint64_t tot_bytes = 0;
    uint32_t n = 0;
    bool ok = true;

    while (!want_frames || n < want_frames) {
        bool eof = false;
        for (int p = 0; p < nplanes && !eof; p++) {
            const size_t got = std::fread(src[p].data(), 1, g[p].bytes(), fin);
            if (got != g[p].bytes()) {
                if (p == 0 && got == 0) { eof = true; break; }
                std::fprintf(stderr, "truncated frame %u (plane %d)\n", n, p);
                ok = false;
                eof = true;
            }
        }
        if (eof) break;

        size_t len = 0;
        st = nxvc_encoder_encode_frame(enc, &in_img, nullptr, nullptr, bs.data(),
                                       bs.size(), &len);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "encode frame %u: %s\n", n, nxvc_status_string(st));
            ok = false;
            break;
        }

        st = nxvc_decoder_decode_frame(dec, bs.data(), len, &out_img, &consumed);
        if (st != NXVC_OK) {
            std::fprintf(stderr, "decode frame %u: %s\n", n, nxvc_status_string(st));
            ok = false;
            break;
        }
        // The decoder must consume exactly what the encoder produced.  If it
        // does not, one of them is desynchronised and every PSNR after this
        // point is meaningless -- so stop rather than print more numbers.
        if (consumed != len) {
            std::fprintf(stderr,
                         "frame %u: encoder wrote %zu bytes, decoder consumed %zu\n",
                         n, len, consumed);
            ok = false;
            break;
        }

        double e[4] = {0, 0, 0, 0}, p_db[4] = {0, 0, 0, 0};
        for (int p = 0; p < nplanes; p++) {
            e[p] = sse(src[p].data(), out[p].data(), g[p].bytes());
            p_db[p] = psnr_from_mse(e[p] / double(g[p].bytes()));
            tot_sse[p] += e[p];
            tot_px[p] += g[p].bytes();
        }
        // JVET weighting is formed in the MSE domain, matching nxq/metrics.py.
        const double wmse = (6.0 * (e[0] / double(g[0].bytes())) +
                             (e[1] / double(g[1].bytes())) +
                             (e[2] / double(g[2].bytes()))) / 8.0;
        const double wdb = psnr_from_mse(wmse);

        std::printf("%-6u %10zu %8.3f %8.3f %8.3f %9.3f\n", n, len, p_db[0],
                    p_db[1], p_db[2], wdb);
        if (fcsv)
            std::fprintf(fcsv, "%u,%zu,%.4f,%.4f,%.4f,%.4f\n", n, len, p_db[0],
                         p_db[1], p_db[2], wdb);

        tot_bytes += len;
        n++;
    }

    std::fclose(fin);
    if (fcsv) std::fclose(fcsv);
    nxvc_encoder_destroy(enc);
    nxvc_decoder_destroy(dec);

    if (n == 0) {
        std::fprintf(stderr, "no complete frames in %s\n", in_path.c_str());
        return 1;
    }

    double avg[4];
    for (int p = 0; p < nplanes; p++)
        avg[p] = psnr_from_mse(tot_sse[p] / double(tot_px[p]));
    const double wmse = (6.0 * (tot_sse[0] / double(tot_px[0])) +
                         (tot_sse[1] / double(tot_px[1])) +
                         (tot_sse[2] / double(tot_px[2]))) / 8.0;

    std::printf("---\n");
    std::printf("%u frames, qp %u%s, %ux%u %s\n", n, qp,
                lossless ? " (lossless)" : "", w, h, pix.c_str());
    std::printf("  mean PSNR   Y %.3f  U %.3f  V %.3f  weighted %.3f dB\n",
                avg[0], avg[1], avg[2], psnr_from_mse(wmse));
    std::printf("  rate        %.3f bits/pixel, %.1f kB/frame\n",
                double(tot_bytes * 8) / (double(n) * w * h),
                double(tot_bytes) / n / 1000.0);
    // Rate at 90 Hz is the number the paper's gates are stated in, so print it
    // rather than leaving every reader to multiply.
    std::printf("  at 90 Hz    %.1f Mbit/s\n",
                double(tot_bytes) / n * 8.0 * 90.0 / 1e6);

    if (lossless && std::isfinite(avg[0])) {
        std::fprintf(stderr, "FAIL: --lossless did not reproduce the input exactly\n");
        return 1;
    }
    return ok ? 0 : 1;
}
