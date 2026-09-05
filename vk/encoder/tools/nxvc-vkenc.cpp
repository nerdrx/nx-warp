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

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nxe_host.h"
#include "nxe_vk.h"

namespace nxe {
int selftest(int device, bool cpu_only, bool print_digests, bool quiet);
int selftest_dump(const char *prefix);
int inter_fixture_dump(const char *prefix);
int ring_check(const char *prefix, const char *decoded, int w, int h, int frames);
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
        "  --custom-tables      train the eight probability table sets on\n"
        "                       the frame and transmit those that pay (6)\n"
        "  --tab v1|v2          transmitted-table coding: flat 5-bit rows\n"
        "                       or the compact per-row-flag form (26);\n"
        "                       v2 needs --custom-tables\n"
        "  --table-iters N      Lloyd iterations refining the trained\n"
        "                       sets (default 3, 0 = the v1.4 encoder)\n"
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
    const char *dump_inter = nullptr;
    const char *ring_prefix = nullptr, *ring_decoded = nullptr;
    int ring_frames = 0;
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
        else if (a == "--inter") cfg.inter = true;
        else if (a == "--poses") cfg.poses = val();
        else if (a == "--coded-vectors") {
            /* Refused, not ignored.  Everything a STATIC_MV tile needs to be
             * DECIDED is implemented and correct -- E1c's search, the vector
             * in the tile header, the slot layout that makes room for it --
             * but E3 still codes every tile it does not skip against the
             * DC-plane INTRA predictor, so a STATIC_MV tile comes out with a
             * full intra-sized residual (526 bytes against the reference's
             * 40) and a stream that decodes to the wrong picture.
             *
             * The missing piece is E3's inter residual path: a sixth binding
             * carrying Pass W's predictor and a branch that subtracts it
             * instead of pred_at().  Until that exists this flag would
             * produce a legal, larger, WRONG stream, which is the failure
             * mode this encoder refuses on principle. */
            std::fprintf(stderr,
                         "--coded-vectors: STATIC_MV is decided but not yet "
                         "coded -- E3 has no inter residual path, so the tile "
                         "would be coded against the intra predictor.  See "
                         "vk/encoder/README.md \"What STATIC_MV still "
                         "needs\".\n");
            return 2;
        }
        else if (a == "--intra-period") cfg.intra_period = std::atoi(val());
        else if (a == "--skip-thresh")
            cfg.skip_thresh = (int)(std::atof(val()) * 256.0 + 0.5);
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
        else if (a == "--custom-tables") cfg.custom_tables = true;
        else if (a == "--no-custom-tables") cfg.custom_tables = false;
        else if (a == "--tab" && i + 1 < argc) {
            const char *v = argv[++i];
            cfg.tab_v2 = std::strcmp(v, "v2") == 0;
        } else if (a == "--table-iters" && i + 1 < argc)
            cfg.table_iters = std::atoi(argv[++i]);
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
        else if (a == "--dump-inter") dump_inter = val();
        else if (a == "--check-ring") ring_prefix = val();
        else if (a == "--check-ring-decoded") ring_decoded = val();
        else if (a == "--check-ring-frames") ring_frames = std::atoi(val());
        else if (a == "--quiet") cfg.quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }

    if (list) return nxe::vk_list_devices();
    if (dump) return nxe::selftest_dump(dump);
    if (dump_inter) return nxe::inter_fixture_dump(dump_inter);
    if (ring_prefix && ring_decoded)
        return nxe::ring_check(ring_prefix, ring_decoded, cfg.w, cfg.h,
                               ring_frames);
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

    /* The pose sidecar, scraped rather than parsed -- exactly as nxv-enc does
     * it, and deliberately so: the two encoders have to read one file the same
     * way or a byte-identity test compares two different warps.  The only keys
     * that matter are `orientation_xyzw` and the FOV, and a version 2 sidecar
     * that names a convention this encoder does not implement is REFUSED
     * rather than guessed at, because a wrong convention does not crash and
     * does not make an illegal stream: it makes a worse picture, which looks
     * exactly like a codec that is merely bad. */
    std::vector<std::array<double, 4>> poses;
    double fov_h = 95.0, fov_v = 95.0;
    if (!cfg.poses.empty()) {
        std::FILE *pf = std::fopen(cfg.poses.c_str(), "rb");
        if (!pf) { std::perror("open poses"); return 1; }
        std::string txt;
        char chunk[4096];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof chunk, pf)) > 0)
            txt.append(chunk, got);
        std::fclose(pf);
        const std::string key = "\"orientation_xyzw\"";
        size_t pos = 0;
        while ((pos = txt.find(key, pos)) != std::string::npos) {
            size_t lb = txt.find('[', pos);
            size_t rb = txt.find(']', lb == std::string::npos ? pos : lb);
            if (lb == std::string::npos || rb == std::string::npos) break;
            std::array<double, 4> q{0, 0, 0, 1};
            const char *p2 = txt.c_str() + lb + 1;
            char *end = nullptr;
            for (int k = 0; k < 4; ++k) {
                q[k] = std::strtod(p2, &end);
                if (end == p2) break;
                p2 = end;
                while (*p2 == ',' || *p2 == ' ' || *p2 == '\n') ++p2;
            }
            poses.push_back(q);
            pos = rb;
        }
        if (poses.empty()) {
            std::fprintf(stderr, "%s: no orientation_xyzw entries\n",
                         cfg.poses.c_str());
            return 1;
        }
        const size_t cid = txt.find("\"id\"");
        if (cid != std::string::npos) {
            const size_t q0 = txt.find('"', txt.find(':', cid));
            const size_t q1 = txt.find('"', q0 + 1);
            const std::string id = txt.substr(q0 + 1, q1 - q0 - 1);
            if (id != "nxv-openxr-1") {
                std::fprintf(stderr,
                             "%s: pose convention \"%s\" is not implemented "
                             "(this encoder implements \"nxv-openxr-1\", "
                             "docs/WARP.md 2.1)\n",
                             cfg.poses.c_str(), id.c_str());
                return 1;
            }
        }
        const size_t fd = txt.find("\"fov_deg\"");
        if (fd != std::string::npos) {
            const size_t hh = txt.find("\"h\"", fd), vv = txt.find("\"v\"", fd);
            if (hh != std::string::npos)
                fov_h = std::strtod(txt.c_str() + txt.find(':', hh) + 1, nullptr);
            if (vv != std::string::npos)
                fov_v = std::strtod(txt.c_str() + txt.find(':', vv) + 1, nullptr);
        }
        if (!cfg.quiet)
            std::printf("poses: %zu orientations, fov %.4g,%.4g deg\n",
                        poses.size(), fov_h, fov_v);
    }

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
        if (!poses.empty()) {
            /* Both eyes get the same orientation: the sidecar is a head pose,
             * and the per-eye difference is a translation the homography does
             * not carry (docs/WARP.md 2.1). */
            const std::array<double, 4> &q =
                poses[(size_t)n < poses.size() ? (size_t)n : poses.size() - 1];
            const double hr = fov_h * 3.14159265358979323846 / 360.0;
            const double vr = fov_v * 3.14159265358979323846 / 360.0;
            nxe::View v[2];
            for (int e = 0; e < 2; ++e) {
                v[e].qx = q[0]; v[e].qy = q[1]; v[e].qz = q[2]; v[e].qw = q[3];
                v[e].fov_left = -hr; v[e].fov_right = hr;
                v[e].fov_up = vr; v[e].fov_down = -vr;
            }
            gpu.set_views(v, cfg.eyes, (uint32_t)n);
        }
        if (cfg.cpu_only) {
            nxe::encode_frame_cpu(f, (uint32_t)n);
        } else if (!gpu.encode_frame(f, (uint32_t)n, check, cfg.quiet)) {
            std::fprintf(stderr, "nxvc-vkenc: GPU encode failed on frame %d\n", n);
            rc = 1;
            break;
        }
        /* NXE_DUMP_RING=<path> writes the ring slot this frame just wrote,
         * luma only, as raw uint16.  It is what the ring-vs-decoder test
         * compares; the encoder is otherwise the only thing that can see it. */
        if (const char *rp = std::getenv("NXE_DUMP_RING")) {
            /* `nsamp`, not `n`: `n` is the frame counter of the loop this
             * sits in, and shadowing it wrote every frame to one file named
             * after the sample count and read slot (count & 3). */
            const size_t nsamp = (size_t)((cfg.w + 1) & ~1) * (size_t)cfg.h;
            std::vector<uint16_t> ring(nsamp, 0);
            if (gpu.read_ring_luma((uint32_t)(n & 3), ring.data(), nsamp)) {
                char path[512];
                std::snprintf(path, sizeof path, "%s.%d", rp, n);
                if (std::FILE *rf = std::fopen(path, "wb")) {
                    std::fwrite(ring.data(), 2, nsamp, rf);
                    std::fclose(rf);
                }
            }
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
