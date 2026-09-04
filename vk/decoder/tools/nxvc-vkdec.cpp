// nxvc-vkdec: decode an .nxv stream to raw planar 8-bit YUV on the GPU.
//
// Deliberately option-for-option compatible with ref/tools/nxv-dec so the
// quality harness (tools/quality/README.md) can point --codec-cmd at either
// binary and diff the results.  What it adds over nxv-dec is --icd, --device
// and --stats.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc_vk.h"

namespace {

void usage() {
    std::fprintf(
        stderr,
        "usage: nxvc-vkdec --in file.nxv --out out.yuv [options]\n"
        "  --pix yuv444p|yuv420p  assert the stream's pixel format\n"
        "  --frames N             decode at most N frames\n"
        "  --nv12                 write Y then interleaved UV (4:2:0 only)\n"
        "  --icd PATH             VK_DRIVER_FILES for a specific ICD\n"
        "  --device SUBSTR        pick a device by name substring\n"
        "  --format auto|rgba8|rgb10a2|ycbcr420\n"
        "  --lds                  force Pass A's LDS read-pointer fallback\n"
        "  --stats                per-frame timing to stderr\n"
        "  --quiet\n"
        "exit 0 decoded, 1 error, 2 usage, 77 no usable Vulkan ICD\n");
}

int fail_no_icd(const char *why) {
    std::fprintf(stderr, "no usable Vulkan ICD: %s\n", why);
    return 77;
}

}  // namespace

int main(int argc, char **argv) {
    std::string in, out, pix, icd, device, format = "auto";
    int frames = -1, quiet = 0, nv12 = 0, stats = 0, lds = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--in") in = val();
        else if (a == "--out") out = val();
        else if (a == "--pix") pix = val();
        else if (a == "--icd") icd = val();
        else if (a == "--device") device = val();
        else if (a == "--format") format = val();
        else if (a == "--frames") frames = std::atoi(val());
        else if (a == "--quiet") quiet = 1;
        else if (a == "--nv12") nv12 = 1;
        else if (a == "--stats") stats = 1;
        else if (a == "--lds") lds = 1;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (in.empty() || out.empty()) { usage(); return 2; }
    if (!icd.empty()) {
#ifdef _WIN32
        _putenv_s("VK_DRIVER_FILES", icd.c_str());
#else
        setenv("VK_DRIVER_FILES", icd.c_str(), 1);
        setenv("VK_ICD_FILENAMES", icd.c_str(), 1);  // pre-1.3.207 loaders
#endif
    }

    std::FILE *fi = std::fopen(in.c_str(), "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::fseek(fi, 0, SEEK_END);
    long fsz = std::ftell(fi);
    std::fseek(fi, 0, SEEK_SET);
    if (fsz <= 0) { std::fprintf(stderr, "empty input\n"); std::fclose(fi); return 1; }
    std::vector<uint8_t> data((size_t)fsz);
    if (std::fread(data.data(), 1, data.size(), fi) != data.size()) {
        std::fprintf(stderr, "short read\n");
        std::fclose(fi);
        return 1;
    }
    std::fclose(fi);

    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = (uint32_t)NXVC_VKD_FLAG_READBACK |
               (lds ? (uint32_t)NXVC_VKD_FLAG_LDS_FALLBACK : 0u);
    ci.device_name = device.empty() ? nullptr : device.c_str();
    ci.output_format = format == "rgba8"      ? NXVC_VKD_OUT_RGBA8
                       : format == "rgb10a2"  ? NXVC_VKD_OUT_RGB10A2
                       : format == "ycbcr420" ? NXVC_VKD_OUT_YCBCR420
                                              : NXVC_VKD_OUT_AUTO;

    nxvc_vk_decoder *dec = nullptr;
    nxvc_vkd_status st = nxvc_vk_decoder_create(&ci, &dec);
    if (st != NXVC_VKD_OK) {
        const char *why = dec ? nxvc_vk_decoder_last_error(dec) : "no decoder";
        int rc = (st == NXVC_VKD_ERR_NO_DEVICE || st == NXVC_VKD_ERR_UNSUPPORTED)
                     ? fail_no_icd(why)
                     : (std::fprintf(stderr, "decoder: %s\n", why), 1);
        nxvc_vk_decoder_destroy(dec);
        return rc;
    }

    size_t off = 0, consumed = 0;
    st = nxvc_vk_decoder_parse_stream_header(dec, data.data(), data.size(),
                                             &consumed);
    if (st != NXVC_VKD_OK) {
        std::fprintf(stderr, "stream header: %s\n",
                     nxvc_vk_decoder_last_error(dec));
        int rc = st == NXVC_VKD_ERR_UNSUPPORTED ? 77 : 1;
        nxvc_vk_decoder_destroy(dec);
        return rc;
    }
    off += consumed;

    nxvc_vkd_stream_info si;
    nxvc_vk_decoder_stream_info(dec, &si);
    if (nv12 && si.chroma != 0) {
        std::fprintf(stderr, "--nv12 requires a 4:2:0 stream\n");
        nxvc_vk_decoder_destroy(dec);
        return 1;
    }
    const char *want = si.chroma == 1 ? "yuv444p" : "yuv420p";
    if (!pix.empty() && pix != want) {
        std::fprintf(stderr, "stream is %s, --pix says %s\n", want, pix.c_str());
        nxvc_vk_decoder_destroy(dec);
        return 1;
    }

    uint32_t yw, yh, cw, ch;
    nxvc_vk_decoder_plane_size(dec, 0, &yw, &yh);
    nxvc_vk_decoder_plane_size(dec, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
        V((size_t)cw * ch), A((size_t)yw * yh, 255);

    std::FILE *fo = std::fopen(out.c_str(), "wb");
    if (!fo) {
        std::perror("open output");
        nxvc_vk_decoder_destroy(dec);
        return 1;
    }
    int n = 0, rc = 0;
    while (off < data.size() && (frames < 0 || n < frames)) {
        st = nxvc_vk_decode_frame(dec, data.data() + off, data.size() - off,
                                  &consumed);
        if (st != NXVC_VKD_OK) {
            std::fprintf(stderr, "frame %d: %s\n", n,
                         nxvc_vk_decoder_last_error(dec));
            rc = st == NXVC_VKD_ERR_UNSUPPORTED ? 77 : 1;
            break;
        }
        uint8_t *planes[4] = {Y.data(), U.data(), V.data(), A.data()};
        int32_t strides[4] = {(int32_t)yw, (int32_t)cw, (int32_t)cw,
                              (int32_t)yw};
        st = nxvc_vk_decoder_read_planes(dec, planes, strides);
        if (st != NXVC_VKD_OK) {
            std::fprintf(stderr, "readback: %s\n",
                         nxvc_vk_decoder_last_error(dec));
            rc = 1;
            break;
        }
        std::fwrite(Y.data(), 1, Y.size(), fo);
        if (nv12) {
            std::vector<uint8_t> uv(U.size() * 2);
            for (size_t i = 0; i < U.size(); ++i) {
                uv[i * 2] = U[i];
                uv[i * 2 + 1] = V[i];
            }
            std::fwrite(uv.data(), 1, uv.size(), fo);
        } else {
            std::fwrite(U.data(), 1, U.size(), fo);
            std::fwrite(V.data(), 1, V.size(), fo);
        }
        if (si.alpha) std::fwrite(A.data(), 1, A.size(), fo);
        if (stats) {
            nxvc_vkd_stats s;
            nxvc_vk_decoder_stats(dec, &s);
            std::fprintf(stderr,
                         "frame %d: %llu B, %u tiles (%u tskip, %u lane "
                         "group(s), %u dispatches)  parse %.3f  submit %.3f  "
                         "passA %.3f  passB %.3f  gpu %.3f  total %.3f ms\n",
                         n, (unsigned long long)s.frame_bytes, s.tiles,
                         s.tiles_tskip, s.lane_groups, s.dispatches, s.parse_ms,
                         s.submit_ms, s.pass_a_ms, s.pass_b_ms, s.gpu_ms,
                         s.total_ms);
        }
        off += consumed;
        ++n;
    }
    std::fclose(fo);
    if (!quiet && rc == 0)
        std::printf("%d frame(s), %ux%u %s%s on %s\n", n, yw, yh, want,
                    si.alpha ? " +alpha" : "",
                    nxvc_vk_decoder_device_name(dec));
    nxvc_vk_decoder_destroy(dec);
    if (rc) return rc;
    return n > 0 ? 0 : 1;
}
