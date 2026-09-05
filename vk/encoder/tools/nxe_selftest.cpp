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

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
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
    /* Tool bits 6 and 26.  `tab` is 1 or 2 and is meaningless without
     * `custom_tables`, exactly as the syntax has it: TAB_V2 requires
     * CUSTOM_TABLES (SYNTAX.md 9.4.1). */
    bool custom_tables;
    int tab;
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
    /* name              w    h  ey 444  qp  mx wm ns ts cq ctx ct tab sdh dir lay seed  fr digest */
    {"420-qp24",       256, 192, 1, 0, 24, 1, 0, 3, 0, 0, 2, 0, 1, 1, 0, 0, 0, 2, 0xfbb920bd4efe23a5ull},
    {"420-qp0",        256, 192, 1, 0, 0, 1, 0, 3, 0, 0, 2, 0, 1, 1, 0, 0, 0, 1, 0x7922a0f47daf5431ull},
    {"420-qp48-tskip", 256, 192, 1, 0, 48, 1, 0, 3, 1, 0, 2, 0, 1, 1, 0, 0, 0, 1, 0xf5c37a85ff044415ull},
    {"444-qp20",       256, 192, 1, 1, 20, 2, 0, 3, 0, 0, 2, 0, 1, 1, 0, 0, 0, 1, 0xe5cda0fcc1347da4ull},
    {"pad-200x150",    200, 150, 1, 0, 30, 1, 0, 3, 0, 0, 2, 0, 1, 1, 0, 0, 0, 2, 0xf1eab9bd6df424a7ull},
    {"stereo-512x128", 512, 128, 2, 0, 26, 1, 0, 3, 0, 0, 2, 0, 1, 1, 0, 0, 0, 2, 0x43870940e7c66cebull},
    {"v1-nosdh-wm2",   256, 192, 1, 0, 30, 1, 2, 1, 0, -4, 1, 0, 1, 0, 0, 0, 0, 1, 0xca44792ca56fc0bbull},
    {"v3-420-qp24",    256, 192, 1, 0, 24, 1, 0, 3, 0, 0, 3, 0, 1, 1, 0, 0, 0, 2, 0x2ec73852e46d482aull},
    {"v3-420-qp0",     256, 192, 1, 0, 0, 1, 0, 3, 0, 0, 3, 0, 1, 1, 0, 0, 0, 1, 0x4f3e6ac793b5b7ddull},
    {"v3-444-qp20",    256, 192, 1, 1, 20, 2, 0, 3, 0, 0, 3, 0, 1, 1, 0, 0, 0, 1, 0xa0bb4193c6769674ull},
    {"v3-nsub1",       256, 192, 1, 0, 30, 1, 2, 1, 0, -4, 3, 0, 1, 0, 0, 0, 0, 1, 0x1204ac612ee6ac60ull},
    /* Custom tables (6) and the compact table set (26).  `ct-multi` is the
     * one that matters most: three Lloyd iterations only differ from zero once
     * a frame has enough tiles to reassign, and a second frame is what catches
     * a trained table leaking from one frame into the next frame's training
     * pass -- which is not a hypothetical, it is the bug this pipeline had. */
    {"ct-420-qp24",    256, 192, 1, 0,  24, 1, 0, 3, 0,  0, 2, 1, 1, 1, 0, 0, 0, 2,  0x7970fa4e06fd6f63ull},
    {"ct2-420-qp24",   256, 192, 1, 0,  24, 1, 0, 3, 0,  0, 2, 1, 2, 1, 0, 0, 0, 2,  0xd52de71889c67b7eull},
    {"ct2-v3-qp24",    256, 192, 1, 0,  24, 1, 0, 3, 0,  0, 3, 1, 2, 1, 0, 0, 0, 2,  0x47368ad6d086ef8cull},
    {"ct2-v3-qp0",     256, 192, 1, 0,   0, 1, 0, 3, 0,  0, 3, 1, 2, 1, 0, 0, 0, 1,  0x9c1cc30b3668793bull},
    {"ct2-v3-444",     256, 192, 1, 1,  20, 2, 0, 3, 0,  0, 3, 1, 2, 1, 0, 0, 0, 1,  0x9cb12ade654cbd3full},
    {"ct2-v3-nsub1",   256, 192, 1, 0,  30, 1, 2, 1, 0, -4, 3, 1, 2, 0, 0, 0, 0, 1,  0x0198f79597fe01c1ull},
    {"ct-multi",       512, 320, 2, 0,  36, 1, 0, 3, 0,  0, 3, 1, 2, 1, 0, 0, 0, 3,  0x55f600b26572c9f2ull},
    {"dir-replace",    256, 192, 1, 0, 24, 1, 0, 3, 0, 0, 2, 0, 1, 1, 1, 0, 12345, 2, 0x33bc9e051b089775ull},
    {"dir-layer",      256, 192, 1, 0, 24, 1, 0, 3, 0, 0, 2, 0, 1, 1, 1, 1, 12345, 2, 0x67c9b1dc640b104bull},
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
    cfg.custom_tables = c.custom_tables;
    cfg.tab_v2 = c.tab >= 2;
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
        std::printf("%s %d %d %d %s %d %d %d %d %d %d %d %d %d %d %u %d %d %d\n",
                    path, c.w, c.h, c.eyes,
                    c.chroma444 ? "yuv444p" : "yuv420p", c.qp, c.matrix,
                    c.wm_id, c.nsub_log2, c.tskip, c.chroma_qp_off, c.ctx,
                    c.sign_hide, c.intra_dir, c.dir_layer, c.dir_mode_seed,
                    c.frames, c.custom_tables, c.tab);
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



/* ------------------------------------------------------- the inter fixture
 *
 * One moving sequence and its pose track, written to disk so that the inter
 * acid test can drive `nxv-enc` and `nxvc-vkenc` from ONE description and the
 * two cannot drift.  It is here rather than in the cmake test because a
 * picture written by CMake would be a third description of the same thing.
 *
 * The content is deliberately a MIX and not a best case: a static, structured
 * background that the warp predicts exactly, plus a disc that moves fast
 * enough that no warp predicts it.  A fixture where everything skips would
 * pass with the residual path unexercised, and one where nothing skips would
 * pass with the ring unexercised; this one puts both kinds of tile, and the
 * boundary between them, in every frame.
 *
 * The pose track is a slow yaw.  It matters that it is not zero: the warp
 * matrix then has real off-diagonal terms, warp_ext() carries something a
 * reader can be wrong about, and the conjugated chroma matrix is exercised.
 */
int inter_fixture_dump(const char *prefix) {
    const int W = 256, H = 192, F = 8;
    const double kFovDeg = 95.0;
    std::string base(prefix);
    const std::string yuv = base + "inter.yuv";
    const std::string js = base + "inter.poses.json";

    std::FILE *fy = std::fopen(yuv.c_str(), "wb");
    if (!fy) { std::perror("dump inter yuv"); return 1; }
    std::vector<uint8_t> Y((size_t)W * H), U((size_t)(W / 2) * (H / 2)),
        V((size_t)(W / 2) * (H / 2));
    for (int n = 0; n < F; ++n) {
        /* The disc moves; everything else is a function of position alone. */
        const double cx = 60.0 + 9.0 * n, cy = 96.0 + 4.0 * n;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int v = 128 + ((x * 3 + y * 5) & 63) - 32;
                if (((x >> 4) + (y >> 4)) % 3 == 0) v += 40;
                const double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy < 22.0 * 22.0) v = 235;
                Y[(size_t)y * W + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        for (int y = 0; y < H / 2; ++y)
            for (int x = 0; x < W / 2; ++x) {
                U[(size_t)y * (W / 2) + x] = (uint8_t)(112 + ((x * 5 + y * 3) % 32));
                V[(size_t)y * (W / 2) + x] = (uint8_t)(144 - ((x * 3 + y * 7) % 32));
            }
        std::fwrite(Y.data(), 1, Y.size(), fy);
        std::fwrite(U.data(), 1, U.size(), fy);
        std::fwrite(V.data(), 1, V.size(), fy);
    }
    std::fclose(fy);

    std::FILE *fj = std::fopen(js.c_str(), "wb");
    if (!fj) { std::perror("dump inter poses"); return 1; }
    std::fprintf(fj,
                 "{\n \"version\": 2,\n"
                 " \"convention\": { \"id\": \"nxv-openxr-1\" },\n"
                 " \"fov_deg\": { \"h\": %.1f, \"v\": %.1f },\n"
                 " \"frames\": [\n",
                 kFovDeg, kFovDeg);
    for (int n = 0; n < F; ++n) {
        /* A yaw of 0.35 degrees a frame: a quaternion about Y. */
        const double ang = 0.35 * 3.14159265358979323846 / 180.0 * n;
        std::fprintf(fj,
                     "  { \"orientation_xyzw\": [0, %.17g, 0, %.17g] }%s\n",
                     std::sin(ang / 2), std::cos(ang / 2), n + 1 < F ? "," : "");
    }
    std::fprintf(fj, " ]\n}\n");
    std::fclose(fj);

    std::printf("%s %s %d %d %d\n", yuv.c_str(), js.c_str(), W, H, F);
    return 0;
}


/* ------------------------------------------------- the ring-vs-decoder check
 *
 * The proof that the encoder's reference picture IS the decoder's.
 *
 * The encoder's ring is written by the decoder's own Pass B, so the claim is
 * meant to be structural -- but "meant to be" is what this directory has been
 * wrong about three times, so it is measured.  `NXE_DUMP_RING=<prefix>` makes
 * the encoder write the ring slot each frame lands in; this compares those
 * dumps against the LUMA the reference decoder produces from the very same
 * stream.  For a 4:2:0 stream with no colour transform the coded luma plane
 * and the display luma plane are the same samples, so the comparison is
 * direct and exact -- not a tolerance.
 *
 * A single differing sample is a failure.  It would mean the encoder is
 * predicting from a picture the decoder does not have, which is the one bug
 * class that does not show up as a broken frame: it shows up as drift, three
 * seconds later, on content nobody was looking at.
 */
int ring_check(const char *prefix, const char *decoded, int w, int h,
               int frames) {
    std::FILE *fd = std::fopen(decoded, "rb");
    if (!fd) { std::perror("ring_check: decoded"); return 1; }
    const size_t luma = (size_t)w * (size_t)h;
    const size_t fsz = luma + luma / 2;   /* 4:2:0 */
    /* The ring's row stride is padded to an even number of samples. */
    const size_t stride = (size_t)((w + 1) & ~1);
    std::vector<uint8_t> dec(fsz);
    std::vector<uint16_t> ring(stride * (size_t)h);
    int bad_frames = 0;
    for (int n = 0; n < frames; ++n) {
        if (std::fread(dec.data(), 1, fsz, fd) != fsz) break;
        char path[512];
        std::snprintf(path, sizeof path, "%s.%d", prefix, n);
        std::FILE *fr = std::fopen(path, "rb");
        if (!fr) {
            std::fprintf(stderr, "ring_check: no ring dump for frame %d\n", n);
            ++bad_frames;
            continue;
        }
        const size_t got = std::fread(ring.data(), 2, ring.size(), fr);
        std::fclose(fr);
        if (got < ring.size()) {
            std::fprintf(stderr, "ring_check: frame %d dump is short\n", n);
            ++bad_frames;
            continue;
        }
        size_t ndiff = 0;
        int worst = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int a = (int)ring[(size_t)y * stride + (size_t)x];
                const int b = (int)dec[(size_t)y * (size_t)w + (size_t)x];
                if (a != b) {
                    ++ndiff;
                    const int e = a > b ? a - b : b - a;
                    if (e > worst) worst = e;
                }
            }
        if (ndiff) {
            std::fprintf(stderr,
                         "ring_check: frame %d: %zu of %zu luma samples differ "
                         "(worst %d).  The encoder's reference is not the "
                         "decoder's.\n",
                         n, ndiff, luma, worst);
            ++bad_frames;
        }
    }
    std::fclose(fd);
    if (bad_frames) return 1;
    std::printf("ring_check: %d frames, encoder ring == decoder output\n",
                frames);
    return 0;
}

}  // namespace nxe
