/* nxe_selftest.cpp -- `nxvc-vkenc --selftest`.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two things are checked over a table of configurations, on frames the tool
 * synthesizes itself so the suite needs nothing on disk:
 *
 *  1. The GPU pipeline against the CPU models, coefficient by coefficient and
 *     then byte by byte.  That is the test paper 3.9 calls the definition of
 *     done for a kernel, and it is what runs on both RADV and lavapipe --
 *     eight times apart in subgroup width, which is the only axis along which
 *     these kernels could disagree.
 *
 *  2. The CPU models against a table of expected stream digests.  The models
 *     are the specification, and the specification is `nxv-enc --no-rdo
 *     --intra-dir off --no-custom-tables`: `tests/vk-encoder` runs that
 *     comparison directly when the reference tools are in the build, but this
 *     digest check runs even in the standalone encoder build, where they are
 *     not, and it is what fails loudly if an edit changes the bitstream.
 *
 * A digest is FNV-1a over the whole stream, header included.  It is not a
 * conformance vector and does not pretend to be one; it is a tripwire.
 */

#include <cstdio>
#include <cstring>

#include "nxe_host.h"
#include "nxe_vk.h"

namespace nxe {

struct Case {
    const char *name;
    int w, h, eyes;
    bool chroma444;
    int qp, matrix, wm_id, nsub_log2, tskip, chroma_qp_off;
    /* 1, 2 or 3: the entropy context model, tools 21 and 25.  A level rather
     * than two bools, because v3 implies v2 and the two can never disagree. */
    int ctx;
    bool sign_hide, intra_dir, dir_layer;
    uint32_t dir_mode_seed;
    int frames;
    uint64_t digest;
};

/* The digests were produced by this tool and cross-checked, configuration by
 * configuration, against `nxv-enc --no-rdo --intra-dir off --no-custom-tables
 * --split4x4 off --cfl off --tab v1 --xform 8 --entropy rans` on the same
 * input for every case that the reference encoder can express -- which is
 * every case here but the two directional ones, where the reference chooses
 * its own per-block modes and this pipeline takes them as input.
 *
 * The `v3-` cases add `--ctx v3`.  `v3-nsub1` is the one that matters most of
 * the four: the neighbour class is carried along a *lane*, so halving the
 * lane count from eight to two changes which unit is "the previous one" for
 * every unit in the tile.  A chain that were merely walking the unit list
 * would pass the other three and fail this. */
static const Case kCases[] = {
    /* name              w    h  ey 444  qp  mx wm ns ts cq ctx sdh dir lay seed  fr digest */
    {"420-qp24",       256, 192, 1, 0,  24, 1, 0, 3, 0,  0, 2, 1, 0, 0, 0, 2, 0xfbb920bd4efe23a5ull},
    {"420-qp0",        256, 192, 1, 0,   0, 1, 0, 3, 0,  0, 2, 1, 0, 0, 0, 1, 0x7922a0f47daf5431ull},
    {"420-qp48-tskip", 256, 192, 1, 0,  48, 1, 0, 3, 1,  0, 2, 1, 0, 0, 0, 1, 0xf5c37a85ff044415ull},
    {"444-qp20",       256, 192, 1, 1,  20, 2, 0, 3, 0,  0, 2, 1, 0, 0, 0, 1, 0xe5cda0fcc1347da4ull},
    {"pad-200x150",    200, 150, 1, 0,  30, 1, 0, 3, 0,  0, 2, 1, 0, 0, 0, 2, 0xf1eab9bd6df424a7ull},
    {"stereo-512x128", 512, 128, 2, 0,  26, 1, 0, 3, 0,  0, 2, 1, 0, 0, 0, 2, 0x43870940e7c66cebull},
    {"v1-nosdh-wm2",   256, 192, 1, 0,  30, 1, 2, 1, 0, -4, 1, 0, 0, 0, 0, 1, 0xca44792ca56fc0bbull},
    {"v3-420-qp24",    256, 192, 1, 0,  24, 1, 0, 3, 0,  0, 3, 1, 0, 0, 0, 2, 0x2ec73852e46d482aull},
    {"v3-420-qp0",     256, 192, 1, 0,   0, 1, 0, 3, 0,  0, 3, 1, 0, 0, 0, 1, 0x4f3e6ac793b5b7ddull},
    {"v3-444-qp20",    256, 192, 1, 1,  20, 2, 0, 3, 0,  0, 3, 1, 0, 0, 0, 1, 0xa0bb4193c6769674ull},
    {"v3-nsub1",       256, 192, 1, 0,  30, 1, 2, 1, 0, -4, 3, 0, 0, 0, 0, 1, 0x1204ac612ee6ac60ull},
    {"dir-replace",    256, 192, 1, 0,  24, 1, 0, 3, 0,  0, 2, 1, 1, 0, 12345, 2, 0x33bc9e051b089775ull},
    {"dir-layer",      256, 192, 1, 0,  24, 1, 0, 3, 0,  0, 2, 1, 1, 1, 12345, 2, 0x67c9b1dc640b104bull},
};

static uint64_t fnv1a(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static Config config_of(const Case &c, int device, bool cpu_only) {
    Config cfg;
    cfg.w = c.w;
    cfg.h = c.h;
    cfg.eyes = c.eyes;
    cfg.chroma444 = c.chroma444 != 0;
    cfg.qp = c.qp;
    cfg.matrix = c.matrix;
    cfg.wm_id = c.wm_id;
    cfg.nsub_log2 = c.nsub_log2;
    cfg.tskip = c.tskip;
    cfg.chroma_qp_off = c.chroma_qp_off;
    cfg.ctx_v2 = c.ctx >= 2;
    cfg.ctx_v3 = c.ctx >= 3;
    cfg.sign_hide = c.sign_hide;
    cfg.intra_dir = c.intra_dir;
    cfg.dir_layer = c.dir_layer;
    cfg.dir_mode_seed = c.dir_mode_seed;
    cfg.frames = c.frames;
    cfg.device = device;
    cfg.cpu_only = cpu_only;
    cfg.quiet = true;
    return cfg;
}

/* Write the synthesized frames back out as planar YUV, so the digests above
 * can be cross-checked against `nxv-enc` on exactly the same picture.  The
 * inverse of the tile-major layout: a sample at (gx, gy) of plane p belongs to
 * the tile of its eye, column and row. */
static void dump_yuv(const Config &cfg, Frame &f, std::FILE *fo) {
    const nxe_frame_params &fp = f.fp;
    for (int p = 0; p < NXE_MAX_PLANES; ++p) {
        const int sub = (p && fp.chroma420) ? 2 : 1;
        const int pw = (int)fp.width / sub, ph = (int)fp.height / sub;
        const int size = f.plane_size[p];
        for (int gy = 0; gy < ph; ++gy)
            for (uint32_t eye = 0; eye < fp.eyes; ++eye)
                for (int gx = 0; gx < pw; ++gx) {
                    uint32_t col = (uint32_t)(gx / size), row = (uint32_t)(gy / size);
                    uint32_t t = row * fp.eyes * fp.tiles_x + eye * fp.tiles_x + col;
                    int lx = gx % size, ly = gy % size;
                    uint8_t v = (uint8_t)f.src[p][(size_t)t * size * size +
                                                  (size_t)ly * size + lx];
                    std::fwrite(&v, 1, 1, fo);
                }
    }
}

int selftest_dump(const char *prefix) {
    for (const Case &c : kCases) {
        Config cfg = config_of(c, 0, true);
        Frame f;
        setup(cfg, f);
        char path[512];
        std::snprintf(path, sizeof path, "%s%s.yuv", prefix, c.name);
        std::FILE *fo = std::fopen(path, "wb");
        if (!fo) return 1;
        for (int n = 0; n < c.frames; ++n) {
            gen_frame(cfg, f, (uint32_t)n);
            dump_yuv(cfg, f, fo);
        }
        std::fclose(fo);
        std::printf("%s %d %d %d %s %d %d %d %d %d %d %d %d %d %d %u %d\n", path,
                    c.w, c.h, c.eyes, c.chroma444 ? "yuv444p" : "yuv420p", c.qp,
                    c.matrix, c.wm_id, c.nsub_log2, c.tskip, c.chroma_qp_off,
                    c.ctx, c.sign_hide, c.intra_dir, c.dir_layer,
                    c.dir_mode_seed, c.frames);
    }
    return 0;
}

/* Returns 0 pass, 1 fail, 77 no usable device. */
int selftest(int device, bool cpu_only, bool print_digests, bool quiet) {
    int bad = 0, no_dev = 0;
    for (const Case &c : kCases) {
        Config cfg = config_of(c, device, cpu_only);
        Frame f;
        setup(cfg, f);
        build_tables(cfg, f);
        std::vector<uint8_t> hdr = stream_header(cfg, f);
        uint64_t h = fnv1a(hdr.data(), hdr.size(), 14695981039346656037ull);

        VkEncoder gpu;
        bool use_gpu = !cpu_only;
        if (use_gpu) {
            std::string err;
            if (!gpu.create(cfg, f, err)) {
                if (!quiet)
                    std::printf("  %-16s no device: %s\n", c.name, err.c_str());
                no_dev = 1;
                continue;
            }
        }
        bool ok = true;
        for (int n = 0; n < c.frames && ok; ++n) {
            gen_frame(cfg, f, (uint32_t)n);
            fill_modes(cfg, f, (uint32_t)n);
            if (use_gpu) {
                /* `check` runs the CPU models alongside and fails on any
                 * difference, so this one call is both halves of the test. */
                if (!gpu.encode_frame(f, (uint32_t)n, true, true)) ok = false;
            } else {
                encode_frame_cpu(f, (uint32_t)n);
            }
            if (ok) h = fnv1a(f.out.data(), f.out.size(), h);
        }
        if (!ok) {
            std::printf("  %-16s FAIL (GPU differs from the CPU models)\n", c.name);
            bad = 1;
            continue;
        }
        if (print_digests) {
            std::printf("    {\"%s\", ... 0x%016llxull},\n", c.name,
                        (unsigned long long)h);
            continue;
        }
        if (c.digest != 0 && h != c.digest) {
            std::printf("  %-16s FAIL digest 0x%016llx, expected 0x%016llx\n",
                        c.name, (unsigned long long)h,
                        (unsigned long long)c.digest);
            bad = 1;
        } else if (!quiet) {
            std::printf("  %-16s ok  0x%016llx%s\n", c.name,
                        (unsigned long long)h,
                        c.digest == 0 ? "  (digest not pinned)" : "");
        }
    }
    if (bad) return 1;
    if (no_dev && !cpu_only) return 77;
    return 0;
}

}  // namespace nxe
