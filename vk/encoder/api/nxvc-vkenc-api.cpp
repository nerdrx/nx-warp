/* nxvc-vkenc-api.cpp -- drive the encoder through its C ABI and nothing else.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `nxvc-vkenc` drives nxe::VkEncoder directly, so it proves the kernels but
 * not the library: the ABI's own configuration mapping -- which tools it turns
 * off, which quantiser matrix it picks, what it puts in the stream header --
 * is code the harness never executes.  This tool executes only that.  It
 * writes the same .nxv `nxvc-vkenc` would, so the acid test's comparison
 * against `nxv-enc` applies to it unchanged.
 *
 * It also prints the per-frame timing the ABI reports, which is where the
 * encode-time numbers in the integration notes come from.
 *
 * Exit 77 when no usable Vulkan device is present, so ctest reports a skip.
 */

#include <nxvc/nxvc_vk_enc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    std::string in, out;
    uint32_t w = 0, h = 0, qp = 26, frames = 8, matrix = 1;
    bool timing = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--in") in = next();
        else if (a == "--out") out = next();
        else if (a == "--w") w = (uint32_t)std::atoi(next());
        else if (a == "--h") h = (uint32_t)std::atoi(next());
        else if (a == "--qp") qp = (uint32_t)std::atoi(next());
        else if (a == "--frames") frames = (uint32_t)std::atoi(next());
        else if (a == "--matrix") matrix = (uint32_t)std::atoi(next());
        else if (a == "--timing") timing = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (in.empty() || out.empty() || !w || !h) {
        std::fprintf(stderr,
                     "usage: nxvc-vkenc-api --in f.yuv --w W --h H --out f.nxv\n"
                     "                      [--qp N] [--frames N] [--matrix N] [--timing]\n");
        return 2;
    }

    nxvc_vke_create_info ci;
    nxvc_vk_encoder_create_info_default(&ci);
    ci.width = w;
    ci.height = h;
    ci.base_qp = qp;
    ci.quant_matrix = matrix;

    nxvc_vk_encoder *enc = nullptr;
    nxvc_vke_status st = nxvc_vk_encoder_create(&ci, &enc);
    if (st != NXVC_VKE_OK) {
        std::fprintf(stderr, "nxvc_vk_encoder_create: %s\n",
                     nxvc_vk_encoder_status_string(st));
        /* No device is a skip, not a failure: this runs on CI boxes with no
         * ICD at all. */
        return (st == NXVC_VKE_ERR_NO_DEVICE || st == NXVC_VKE_ERR_VULKAN) ? 77 : 1;
    }

    std::FILE *fi = std::fopen(in.c_str(), "rb");
    if (!fi) { std::perror("open input"); nxvc_vk_encoder_destroy(enc); return 1; }
    std::FILE *fo = std::fopen(out.c_str(), "wb");
    if (!fo) { std::perror("open output"); std::fclose(fi);
               nxvc_vk_encoder_destroy(enc); return 1; }

    size_t hlen = 0;
    nxvc_vk_encoder_stream_header(enc, nullptr, 0, &hlen);
    std::vector<uint8_t> hdr(hlen);
    if (nxvc_vk_encoder_stream_header(enc, hdr.data(), hdr.size(), &hlen) != NXVC_VKE_OK) {
        std::fprintf(stderr, "stream header failed\n");
        return 1;
    }
    std::fwrite(hdr.data(), 1, hdr.size(), fo);

    const size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<uint8_t> Y((size_t)w * h), U(cw * ch), V(cw * ch);

    uint32_t n = 0;
    double sum_ms = 0, max_ms = 0, sum_up = 0;
    size_t total_bytes = 0;
    int rc = 0;
    while (n < frames) {
        if (std::fread(Y.data(), 1, Y.size(), fi) != Y.size()) break;
        if (std::fread(U.data(), 1, U.size(), fi) != U.size()) break;
        if (std::fread(V.data(), 1, V.size(), fi) != V.size()) break;

        const uint8_t *bytes = nullptr;
        size_t len = 0;
        st = nxvc_vk_encoder_encode_planes(enc, Y.data(), w, U.data(), V.data(),
                                          cw, &bytes, &len);
        if (st != NXVC_VKE_OK) {
            std::fprintf(stderr, "encode frame %u: %s (%s)\n", n,
                         nxvc_vk_encoder_status_string(st),
                         nxvc_vk_encoder_last_error(enc));
            rc = 1;
            break;
        }
        std::fwrite(bytes, 1, len, fo);
        total_bytes += len;

        const double ms = nxvc_vk_encoder_last_encode_ms(enc);
        sum_ms += ms;
        sum_up += nxvc_vk_encoder_last_upload_ms(enc);
        if (ms > max_ms) max_ms = ms;

        /* The per-tile spans must tile the frame exactly: every tile's bytes
         * inside the frame, and no two overlapping.  It is the claim the
         * transport would rely on, so check it here rather than trust it. */
        uint32_t tc = 0;
        const nxvc_vke_tile *tiles = nxvc_vk_encoder_tiles(enc, &tc);
        for (uint32_t t = 0; t < tc; ++t) {
            if (size_t(tiles[t].offset) + tiles[t].length > len) {
                std::fprintf(stderr,
                             "tile %u span [%u,+%u) runs past the %zu-byte frame\n",
                             t, tiles[t].offset, tiles[t].length, len);
                rc = 1;
            }
        }
        ++n;
    }
    std::fclose(fo);
    std::fclose(fi);

    if (timing && n) {
        std::printf("%u frames, %ux%u QP %u: encode mean %.3f ms, max %.3f ms, "
                    "repack mean %.3f ms, %zu bytes/frame\n",
                    n, w, h, qp, sum_ms / n, max_ms, sum_up / n,
                    total_bytes / n);
    }
    nxvc_vk_encoder_destroy(enc);
    if (rc) return rc;
    return n > 0 ? 0 : 1;
}
