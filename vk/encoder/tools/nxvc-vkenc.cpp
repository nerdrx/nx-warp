/* nxvc-vkenc -- the NX Warp GPU encoder's host harness.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mirrors the flags of `ref/tools/nxv-enc` that this pipeline implements, so
 * the two can be pointed at the same input and their output compared byte for
 * byte.  That comparison is the acid test of docs/PAPER.md 3.9: a frame the
 * GPU encoded must be the frame the reference encoder would have produced for
 * the same decisions, and must therefore decode with `nxv-dec` to exactly the
 * same pixels.
 *
 *   nxvc-vkenc --in f.yuv --w W --h H --pix yuv420p --qp N --out f.nxv
 *
 * The pipeline it drives is intra-only: E3 `forward.comp`, E4
 * `rans_encode.comp`, E5 `packetize.comp`, with E0's colour conversion stood
 * in for on the host because the input is a file rather than a compositor
 * image.  Inter tools, res levels, alpha and custom probability tables are out
 * of scope and are refused rather than silently ignored.
 *
 *   --cpu          run the CPU models instead of the GPU (always available)
 *   --device N     physical device index
 *   --check        also run the CPU models and diff every intermediate
 *   --bench N      time the passes over N iterations
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nxe_host.h"
#include "nxe_vk.h"

namespace nxe {
int selftest(int device, bool cpu_only, bool print_digests, bool quiet);
int selftest_dump(const char *prefix);
}

static void usage() {
    std::fprintf(stderr,
        "usage: nxvc-vkenc --in file.yuv --w W --h H --pix yuv444p|yuv420p\n"
        "                  --qp N --out out.nxv\n"
        "optional:\n"
        "  --frames N           encode at most N frames\n"
        "  --eyes 1|2           2 = side-by-side stereo, --w is the full width\n"
        "  --matrix 0..3        frame weighting matrix (default 1)\n"
        "  --wm 0..3            per-tile weighting matrix id (default 0)\n"
        "  --nsub 0..5          rANS lane count log2 (default 3 = 8 lanes)\n"
        "  --tskip off|on       transform skip (default off)\n"
        "  --ctx v1|v2|v3       12, 16 or 27 entropy contexts (tools 21,\n"
        "                       25); default v2, and v3 implies v2\n"
        "  --no-sign-hide       code every sign (default: hide one per unit)\n"
        "  --intra-dir on|off|layer   directional intra, modes from the host\n"
        "  --dir-mode-seed N    fill the per-block modes from a PRNG (test aid)\n"
        "  --chroma-qp-off N    chroma QP offset\n"
        "  --device N           Vulkan physical device index (default 0)\n"
        "  --cpu                run the CPU models, no Vulkan\n"
        "  --check              run both and diff every intermediate\n"
        "  --bench N            time each pass over N iterations\n"
        "  --list               list Vulkan devices and exit\n"
        "  --selftest           run the built-in configuration table:\n"
        "                       GPU against the CPU models, and the CPU\n"
        "                       models against pinned stream digests\n"
        "  --print-digests      with --selftest, print them instead\n"
        "  --quiet\n"
        "\n"
        "Exit code 77 means \"no usable Vulkan device\"; ctest reports it as a\n"
        "skip.  --cpu never returns it.\n");
}

int main(int argc, char **argv) {
    nxe::Config cfg;
    bool list = false, check = false, self = false, digests = false;
    const char *dump = nullptr;
    std::string pix = "yuv420p";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--in") cfg.in = val();
        else if (a == "--out") cfg.out = val();
        else if (a == "--w") cfg.w = std::atoi(val());
        else if (a == "--h") cfg.h = std::atoi(val());
        else if (a == "--pix") pix = val();
        else if (a == "--qp") cfg.qp = std::atoi(val());
        else if (a == "--frames") cfg.frames = std::atoi(val());
        else if (a == "--eyes") cfg.eyes = std::atoi(val());
        else if (a == "--matrix") cfg.matrix = std::atoi(val());
        else if (a == "--wm") cfg.wm_id = std::atoi(val());
        else if (a == "--nsub") cfg.nsub_log2 = std::atoi(val());
        else if (a == "--chroma-qp-off") cfg.chroma_qp_off = std::atoi(val());
        else if (a == "--tskip") cfg.tskip = std::strcmp(val(), "on") == 0 ? 1 : 0;
        else if (a == "--ctx") {
            const char *v = val();
            /* v3 is a refinement of v2 and the stream header refuses bit 25
             * without bit 21, so v3 sets both. */
            cfg.ctx_v3 = std::strcmp(v, "v3") == 0;
            cfg.ctx_v2 = cfg.ctx_v3 || std::strcmp(v, "v2") == 0;
        }
        else if (a == "--no-sign-hide") cfg.sign_hide = false;
        else if (a == "--sign-hide") cfg.sign_hide = true;
        else if (a == "--intra-dir") {
            std::string v = val();
            if (v == "on") { cfg.intra_dir = true; cfg.dir_layer = false; }
            else if (v == "layer") { cfg.intra_dir = true; cfg.dir_layer = true; }
            else cfg.intra_dir = false;
        }
        else if (a == "--dir-mode-seed") cfg.dir_mode_seed = (uint32_t)std::strtoul(val(), nullptr, 0);
        else if (a == "--device") cfg.device = std::atoi(val());
        else if (a == "--cpu") cfg.cpu_only = true;
        else if (a == "--check") check = true;
        else if (a == "--bench") { cfg.bench = true; cfg.bench_iters = std::atoi(val()); }
        else if (a == "--list") list = true;
        else if (a == "--selftest") self = true;
        else if (a == "--print-digests") digests = true;
        else if (a == "--dump-selftest-yuv") dump = val();
        else if (a == "--quiet") cfg.quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }

    if (list) return nxe::vk_list_devices();
    if (dump) return nxe::selftest_dump(dump);
    if (self)
        return nxe::selftest(cfg.device, cfg.cpu_only, digests, cfg.quiet);

    if (cfg.in.empty() || cfg.out.empty() || cfg.w <= 0 || cfg.h <= 0) {
        usage();
        return 2;
    }
    if (pix != "yuv420p" && pix != "yuv444p") {
        std::fprintf(stderr, "--pix must be yuv420p or yuv444p\n");
        return 2;
    }
    cfg.chroma444 = (pix == "yuv444p");
    if (cfg.eyes != 1 && cfg.eyes != 2) {
        std::fprintf(stderr, "--eyes must be 1 or 2\n");
        return 2;
    }
    if (cfg.qp < 0 || cfg.qp > 63) { std::fprintf(stderr, "--qp 0..63\n"); return 2; }
    if (cfg.nsub_log2 < 0 || cfg.nsub_log2 > 5) {
        std::fprintf(stderr, "--nsub 0..5\n");
        return 2;
    }

    nxe::Frame f;
    nxe::setup(cfg, f);
    nxe::build_tables(cfg, f);

    std::FILE *fi = std::fopen(cfg.in.c_str(), "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::FILE *fo = std::fopen(cfg.out.c_str(), "wb");
    if (!fo) { std::perror("open output"); return 1; }

    std::vector<uint8_t> hdr = nxe::stream_header(cfg, f);
    std::fwrite(hdr.data(), 1, hdr.size(), fo);

    nxe::VkEncoder gpu;
    if (!cfg.cpu_only) {
        std::string err;
        if (!gpu.create(cfg, f, err)) {
            std::fprintf(stderr, "nxvc-vkenc: %s\n", err.c_str());
            std::fclose(fi);
            std::fclose(fo);
            std::remove(cfg.out.c_str());
            return 77;
        }
    }

    size_t total = hdr.size();
    int n = 0;
    int rc = 0;
    while (cfg.frames < 0 || n < cfg.frames) {
        if (!nxe::read_frame(fi, cfg, f)) break;
        nxe::fill_modes(cfg, f, (uint32_t)n);
        if (cfg.cpu_only) {
            nxe::encode_frame_cpu(f, (uint32_t)n);
        } else if (!gpu.encode_frame(f, (uint32_t)n, check, cfg.quiet)) {
            std::fprintf(stderr, "nxvc-vkenc: GPU encode failed on frame %d\n", n);
            rc = 1;
            break;
        }
        std::fwrite(f.out.data(), 1, f.out.size(), fo);
        total += f.out.size();
        if (!cfg.quiet)
            std::printf("frame %d: %zu bytes  %.4f bpp\n", n, f.out.size(),
                        f.out.size() * 8.0 / ((double)cfg.w * cfg.h));
        ++n;
    }
    std::fclose(fo);
    std::fclose(fi);

    if (cfg.bench && !cfg.cpu_only && n > 0) gpu.bench(f, cfg.bench_iters);

    if (!cfg.quiet)
        std::printf("%d frame(s), %zu bytes total, %.4f bpp mean\n", n, total,
                    n ? total * 8.0 / ((double)cfg.w * cfg.h * n) : 0.0);
    if (rc) return rc;
    return n > 0 ? 0 : 1;
}
