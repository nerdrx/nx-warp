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
        "  --dir-sched 0..3       INTRA_DIR wavefront schedule (0 = normative;\n"
        "                         1 drops the above-right reference, 2 adds\n"
        "                         32x32 sub-tiles, 3 both).  Measurement only:\n"
        "                         a value other than 0 decodes a conformant\n"
        "                         stream to different pixels.\n"
        "  --tile-sort            group Pass B's workgroups by tile shape\n"
        "  --unorm 0|1            write the 8-bit output through UNORM images\n"
        "                         instead of integer ones.  Bit-identical\n"
        "                         either way (tests/vk-decoder/unorm proves\n"
        "                         it per driver); default on for Android,\n"
        "                         where an integer storage image costs about\n"
        "                         3x a normalised one\n"
        "  --stats                per-frame timing to stderr\n"
        "  --dense                the pre-ADR-0026 dense coefficient layout\n"
        "  --repeat N             decode the first frame N times and report\n"
        "                         the best per-pass device time.  A timing\n"
        "                         loop that needs no re-encode: push one\n"
        "                         .nxv and measure the kernels over it\n"
        "  --no-out               skip the readback and the output file.\n"
        "                         With --repeat this leaves the two\n"
        "                         dispatches and nothing else in the submit\n"
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
    // [v3] measurement knobs, see nxvc_vk_decoder_set_dir_sched /
    // _set_tile_sort in <nxvc/nxvc_vk.h>.  --dir-sched is a BITSTREAM
    // property: anything but 0 decodes a normal stream to different pixels.
    int dir_sched = 0, tile_sort = 0;
    // A timing loop over one frame.  The device timestamps are per pass and
    // already exclude the readback, but the readback shares the submission,
    // so --no-out is what makes the measured submit contain the two
    // dispatches alone.
    int repeat = 0, no_out = 0, dense = 0;
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
        else if (a == "--dir-sched") dir_sched = std::atoi(val());
        else if (a == "--tile-sort") tile_sort = 1;
        else if (a == "--repeat") repeat = std::atoi(val());
        else if (a == "--no-out") no_out = 1;
        else if (a == "--dense") dense = 1;
        // The decoder reads this at create time.  It is an environment
        // variable rather than a create_info field because the store format
        // is a device-performance decision, not part of the C ABI's contract.
        else if (a == "--unorm") ::setenv("NXVC_VKD_UNORM", val(), 1);
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (in.empty() || (out.empty() && !no_out)) { usage(); return 2; }
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
    ci.flags = (no_out ? 0u : (uint32_t)NXVC_VKD_FLAG_READBACK) |
               (lds ? (uint32_t)NXVC_VKD_FLAG_LDS_FALLBACK : 0u) |
               (dense ? (uint32_t)NXVC_VKD_FLAG_DENSE_COEF : 0u);
    ci.device_name = device.empty() ? nullptr : device.c_str();
    ci.output_format = format == "rgba8"      ? NXVC_VKD_OUT_RGBA8
                       : format == "rgb10a2"  ? NXVC_VKD_OUT_RGB10A2
                       : format == "ycbcr420" ? NXVC_VKD_OUT_YCBCR420
                                              : NXVC_VKD_OUT_AUTO;

    nxvc_vk_decoder *dec = nullptr;
    nxvc_vkd_status st = nxvc_vk_decoder_create(&ci, &dec);
    if (dec) {
        if (nxvc_vk_decoder_set_dir_sched(dec, (uint32_t)dir_sched) !=
            NXVC_VKD_OK) {
            std::fprintf(stderr, "--dir-sched must be 0..3\n");
            nxvc_vk_decoder_destroy(dec);
            return 2;
        }
        nxvc_vk_decoder_set_tile_sort(dec, (uint32_t)tile_sort);
    }
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

    std::FILE *fo = nullptr;
    if (!no_out) {
        fo = std::fopen(out.c_str(), "wb");
        if (!fo) {
            std::perror("open output");
            nxvc_vk_decoder_destroy(dec);
            return 1;
        }
    }

    // --repeat: one frame, decoded N times, best per-pass device time.  The
    // timestamps are the decoder's own, so this measures exactly what the
    // bench in the conformance harness measures without paying for an encode.
    if (repeat > 0) {
        double bestA = 1e9, bestB = 1e9, bestG = 1e9, bestT = 1e9;
        // The host's own share of the frame, which on a phone is not small:
        // parse builds every descriptor and record, submit records the
        // command buffer and stages the bitstream.
        double bestP = 1e9, bestS = 1e9;
        nxvc_vkd_stats s{};
        for (int i = 0; i < repeat; ++i) {
            st = nxvc_vk_decode_frame(dec, data.data() + off,
                                      data.size() - off, &consumed);
            if (st != NXVC_VKD_OK) {
                std::fprintf(stderr, "repeat %d: %s\n", i,
                             nxvc_vk_decoder_last_error(dec));
                if (fo) std::fclose(fo);
                nxvc_vk_decoder_destroy(dec);
                return st == NXVC_VKD_ERR_UNSUPPORTED ? 77 : 1;
            }
            nxvc_vk_decoder_stats(dec, &s);
            if (s.pass_a_ms < bestA) bestA = s.pass_a_ms;
            if (s.pass_b_ms < bestB) bestB = s.pass_b_ms;
            if (s.gpu_ms < bestG) bestG = s.gpu_ms;
            if (s.total_ms < bestT) bestT = s.total_ms;
            if (s.parse_ms < bestP) bestP = s.parse_ms;
            if (s.submit_ms < bestS) bestS = s.submit_ms;
        }
        std::printf("repeat %d on %s: %u tiles, %llu B frame, %u dispatch(es)\n"
                    "  best  passA %.3f  passB %.3f  gpu %.3f  wall %.3f ms"
                    "  (host parse %.3f submit %.3f)\n",
                    repeat, nxvc_vk_decoder_device_name(dec), s.tiles,
                    (unsigned long long)s.frame_bytes, s.dispatches, bestA,
                    bestB, bestG, bestT, bestP, bestS);
        if (fo) std::fclose(fo);
        nxvc_vk_decoder_destroy(dec);
        return 0;
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
        if (fo) {
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
        }
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
    if (fo) std::fclose(fo);
    if (!quiet && rc == 0)
        std::printf("%d frame(s), %ux%u %s%s on %s\n", n, yw, yh, want,
                    si.alpha ? " +alpha" : "",
                    nxvc_vk_decoder_device_name(dec));
    nxvc_vk_decoder_destroy(dec);
    if (rc) return rc;
    return n > 0 ? 0 : 1;
}
