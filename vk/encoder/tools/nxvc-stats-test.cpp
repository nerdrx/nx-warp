// nxvc-stats-test.cpp -- GPU versus CPU diff harness for the encoder analysis
// kernels E0, E1 and E2.
//
// SPDX-License-Identifier: Apache-2.0
//
// Paper 3.9 makes the GPU-versus-CPU diff the definition of done: "the CPU
// reference is the specification; SPIR-V is validated against it, not the other
// way round", and the cross-vendor determinism test is what Phase 1 and Phase 2
// exit on.  This tool is that test for the analysis kernels.  It must report
// zero mismatches on lavapipe (subgroup size 8) and on RADV (subgroup size 32
// or 64 at the driver's discretion) -- running on both is the point, because
// the only way these kernels can disagree with the model is by depending on
// subgroup width, and the two devices differ by 4x to 8x in exactly that.
//
// Exit codes:  0 pass, 1 mismatch or error, 77 skipped (no ICD / no device),
// matching the SKIP_RETURN_CODE the ctest wrapper sets.
//
// Usage:
//   nxvc-stats-test [--list] [--device N] [--seed S] [--validation]
//                   [--bench-w W] [--bench-h H] [--iters N] [--no-bench]

#include "vk_min.h"

extern "C" {
#include "stats_cpu.h"
}

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "E0_convert_rgba8_444.spv.h"
#include "E0_convert_rgba8_420.spv.h"
#include "E0_convert_rgb10a2_444.spv.h"
#include "E0_convert_rgb10a2_420.spv.h"
#include "E1_stats.spv.h"
#include "E2_prefix_p0.spv.h"
#include "E2_prefix_p1.spv.h"
#include "E2_prefix_p2.spv.h"

static_assert(sizeof(nxe_tile_stats) == NXE_TILE_STATS_SIZE,
              "nxe_tile_stats must stay 48 tightly packed bytes so the C layout "
              "and the std430 layout of the GLSL mirror are identical");
static_assert(sizeof(nxe_frame_params) == 32, "nxe_frame_params layout");
static_assert(sizeof(nxe_e0_push) == 36, "nxe_e0_push layout");
static_assert(sizeof(nxe_e1_push) == 48, "nxe_e1_push layout");

// --------------------------------------------------------------------- rng
// xorshift128+, so the test material is identical on every machine and every
// run for a given seed.  A failure is always reproducible from its seed.
struct Rng {
    uint64_t s0, s1;
    explicit Rng(uint64_t seed) : s0(seed * 0x9e3779b97f4a7c15ull + 1), s1(seed ^ 0xdeadbeefcafebabeull) {
        for (int i = 0; i < 8; ++i) next();
    }
    uint64_t next() {
        uint64_t x = s0, y = s1;
        s0 = y;
        x ^= x << 23;
        s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
        return s1 + y;
    }
    uint32_t u32() { return (uint32_t)(next() >> 32); }
    uint32_t below(uint32_t n) { return n ? u32() % n : 0; }
};

// ------------------------------------------------------------ test material
//
// Pure noise would exercise the arithmetic but not the statistics: a noise tile
// has no coherent gradient, so the structure tensor would be near-isotropic
// everywhere and a bug in the Jxy cross term could hide.  The generator lays
// down gradients, hard edges at arbitrary angles, glyph-like strips and flat
// patches, then adds noise, so every tile class of paper 4.6.1 appears
// somewhere in the frame.
static void make_source(std::vector<uint32_t> &rgba, uint32_t w, uint32_t h,
                        uint32_t maxval, Rng &rng)
{
    rgba.assign((size_t)w * h * 4, 0);
    // A handful of random half-plane edges and boxes over a base gradient.
    struct Edge { int32_t a, b, c, amp; };
    std::vector<Edge> edges;
    for (int i = 0; i < 24; ++i) {
        Edge e;
        e.a = (int32_t)rng.below(2001) - 1000;
        e.b = (int32_t)rng.below(2001) - 1000;
        e.c = (int32_t)rng.below((uint32_t)(w + h) * 1000u);
        e.amp = (int32_t)rng.below(maxval + 1);
        edges.push_back(e);
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            int32_t base = (int32_t)(((x * 3 + y * 5) % (maxval + 1)));
            int32_t v = base;
            for (const auto &e : edges) {
                int64_t s = (int64_t)e.a * (int64_t)x + (int64_t)e.b * (int64_t)y;
                if (s > (int64_t)e.c) v += e.amp / 4;
            }
            // Glyph-like vertical strips in a band: high coherence, high contrast.
            if ((y / 64) % 5 == 2 && ((x / 3) % 2) == 0) v = (int32_t)maxval;
            // Flat patches.
            if ((x / 64) % 7 == 3 && (y / 64) % 7 == 3) v = (int32_t)(maxval / 2);
            uint32_t *px = &rgba[((size_t)y * w + x) * 4];
            int32_t n = (int32_t)rng.below(9) - 4;
            int32_t r = v + n;
            int32_t g = v - n + (int32_t)rng.below(5);
            int32_t b = (v * 2) / 3 + (int32_t)rng.below(17);
            px[0] = (uint32_t)std::clamp<int32_t>(r, 0, (int32_t)maxval);
            px[1] = (uint32_t)std::clamp<int32_t>(g, 0, (int32_t)maxval);
            px[2] = (uint32_t)std::clamp<int32_t>(b, 0, (int32_t)maxval);
            px[3] = maxval;
        }
    }
}

// Pack the expanded component array into the wire format the storage image
// wants.  RGBA8 is 4 bytes; RGB10A2 is A2B10G10R10_UINT_PACK32 (R in bits 0-9).
static void pack_image(const std::vector<uint32_t> &rgba, uint32_t w, uint32_t h,
                       bool depth10, std::vector<uint8_t> &out)
{
    if (depth10) {
        out.resize((size_t)w * h * 4);
        uint32_t *d = (uint32_t *)out.data();
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint32_t r = rgba[i * 4 + 0] & 1023u;
            uint32_t g = rgba[i * 4 + 1] & 1023u;
            uint32_t b = rgba[i * 4 + 2] & 1023u;
            uint32_t a = rgba[i * 4 + 3] & 3u;
            d[i] = r | (g << 10) | (b << 20) | (a << 30);
        }
    } else {
        out.resize((size_t)w * h * 4);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            out[i * 4 + 0] = (uint8_t)rgba[i * 4 + 0];
            out[i * 4 + 1] = (uint8_t)rgba[i * 4 + 1];
            out[i * 4 + 2] = (uint8_t)rgba[i * 4 + 2];
            out[i * 4 + 3] = (uint8_t)rgba[i * 4 + 3];
        }
    }
}

// ------------------------------------------------------------------ helpers
struct Ctx {
    vkmin::Device dev;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    vkmin::Pipeline e0[4];         // [depth10][chroma420]
    vkmin::Pipeline e1;
    vkmin::Pipeline e2[3];
    bool quiet = false;
};

static void image_barrier(VkCommandBuffer cb, VkImage img,
                          VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src, VkAccessFlags dst,
                          VkPipelineStageFlags sstage, VkPipelineStageFlags dstage)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, sstage, dstage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void write_set(VkDevice dev, VkDescriptorSet set,
                      const std::vector<VkDescriptorType> &types,
                      const std::vector<VkBuffer> &bufs,
                      VkImageView view)
{
    std::vector<VkDescriptorBufferInfo> bi(types.size());
    VkDescriptorImageInfo ii{};
    std::vector<VkWriteDescriptorSet> w;
    size_t bidx = 0;
    for (size_t i = 0; i < types.size(); ++i) {
        VkWriteDescriptorSet ws{};
        ws.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws.dstSet = set;
        ws.dstBinding = (uint32_t)i;
        ws.descriptorCount = 1;
        ws.descriptorType = types[i];
        if (types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            ii.imageView = view;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ws.pImageInfo = &ii;
        } else {
            bi[i] = { bufs[bidx++], 0, VK_WHOLE_SIZE };
            ws.pBufferInfo = &bi[i];
        }
        w.push_back(ws);
    }
    vkUpdateDescriptorSets(dev, (uint32_t)w.size(), w.data(), 0, nullptr);
}

// ---------------------------------------------------------------- E0 + E1 test
struct Failures {
    uint64_t e0 = 0, e1 = 0, e2 = 0;
    uint64_t total() const { return e0 + e1 + e2; }
};

static bool run_e0_e1(Ctx &c, uint32_t w, uint32_t h, bool depth10, bool c420,
                      uint64_t seed, Failures &fail, bool bench,
                      uint32_t iters, double *ms_e0, double *ms_e1)
{
    std::string err;
    auto &dev = c.dev;
    const uint32_t maxval = depth10 ? 1023u : 255u;
    // Every case is submitted and waited on before the next, so the pool can
    // simply be recycled rather than sized for the whole run.
    vkResetDescriptorPool(dev.handle(), c.dpool, 0);

    nxe_frame_params fp{};
    nxe_frame_params_init(&fp, w, h, c420 ? 1 : 0, depth10 ? 1 : 0);
    const uint32_t num_tiles = fp.tiles_x * fp.tiles_y;
    const uint32_t plane_words = nxe_plane_total_words(num_tiles, c420 ? 1 : 0);

    if (num_tiles > NXE_MAX_TILES) {
        fprintf(stderr, "  %ux%u exceeds NXE_MAX_TILES\n", w, h);
        return false;
    }

    Rng rng(seed);
    std::vector<uint32_t> src_rgba, ref_rgba;
    make_source(src_rgba, w, h, maxval, rng);
    make_source(ref_rgba, w, h, maxval, rng);

    // ---- CPU models first; they are the specification.
    std::vector<uint32_t> cpu_planes(plane_words, 0), cpu_ref_planes(plane_words, 0);
    nxe_e0_convert_cpu(src_rgba.data(), w, h, &fp, c420 ? 1 : 0,
                       cpu_planes.data(), plane_words);
    nxe_e0_convert_cpu(ref_rgba.data(), w, h, &fp, c420 ? 1 : 0,
                       cpu_ref_planes.data(), plane_words);

    // Per-tile quarter-pel offsets: a mix of zero (the identity hook the warp
    // will replace), small residual motion, and deliberately huge values that
    // push the reference fetch past the frame edge so the clamp is exercised.
    std::vector<int32_t> mv(num_tiles * 2, 0);
    for (uint32_t t = 0; t < num_tiles; ++t) {
        uint32_t k = rng.below(4);
        if (k == 0) { mv[t * 2] = 0; mv[t * 2 + 1] = 0; }
        else if (k == 3) {
            mv[t * 2] = (int32_t)rng.below(4001) - 2000;
            mv[t * 2 + 1] = (int32_t)rng.below(4001) - 2000;
        } else {
            mv[t * 2] = (int32_t)rng.below(129) - 64;
            mv[t * 2 + 1] = (int32_t)rng.below(129) - 64;
        }
    }

    std::vector<nxe_tile_stats> cpu_stats(num_tiles);
    nxe_e1_stats_cpu(cpu_planes.data(), plane_words, &fp, cpu_ref_planes.data(),
                     mv.data(), 1, cpu_stats.data(), num_tiles);

    // ---- GPU resources.
    vkmin::Image img;
    VkFormat ifmt = depth10 ? VK_FORMAT_A2B10G10R10_UINT_PACK32 : VK_FORMAT_R8G8B8A8_UINT;
    if (!dev.create_storage_image(w, h, ifmt, img, err)) {
        fprintf(stderr, "  image: %s\n", err.c_str());
        return false;
    }

    std::vector<uint8_t> wire;
    pack_image(src_rgba, w, h, depth10, wire);

    vkmin::Buffer staging{}, planes{}, refplanes{}, mvbuf{}, stats{}, readback{};
    const VkDeviceSize plane_bytes = (VkDeviceSize)plane_words * 4;
    const VkDeviceSize stats_bytes = (VkDeviceSize)num_tiles * NXE_TILE_STATS_SIZE;
    VkDeviceSize stage_bytes = std::max<VkDeviceSize>(
        { (VkDeviceSize)wire.size(), plane_bytes, stats_bytes, mv.size() * 4 });

    bool ok = dev.create_buffer(stage_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                true, staging, err)
           && dev.create_buffer(plane_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false, planes, err)
           && dev.create_buffer(plane_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false, refplanes, err)
           && dev.create_buffer(mv.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false, mvbuf, err)
           && dev.create_buffer(stats_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                false, stats, err);
    if (!ok) { fprintf(stderr, "  buffers: %s\n", err.c_str()); return false; }

    auto upload = [&](const void *data, size_t bytes, vkmin::Buffer &dst) {
        memcpy(staging.map, data, bytes);
        VkCommandBuffer cb = dev.begin();
        VkBufferCopy bc{ 0, 0, bytes };
        vkCmdCopyBuffer(cb, staging.buf, dst.buf, 1, &bc);
        std::string e;
        return dev.submit_and_wait(cb, e);
    };

    if (!upload(cpu_ref_planes.data(), plane_bytes, refplanes)) return false;
    if (!upload(mv.data(), mv.size() * 4, mvbuf)) return false;

    // Upload the source image.
    {
        memcpy(staging.map, wire.data(), wire.size());
        VkCommandBuffer cb = dev.begin();
        image_barrier(cb, img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cb, staging.buf, img.img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        image_barrier(cb, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (!dev.submit_and_wait(cb, err)) { fprintf(stderr, "  upload: %s\n", err.c_str()); return false; }
    }

    // ---- descriptors.
    const std::vector<VkDescriptorType> e0_types = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    const std::vector<VkDescriptorType> e1_types(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    vkmin::Pipeline &p0 = c.e0[(depth10 ? 2 : 0) + (c420 ? 1 : 0)];
    VkDescriptorSet s0 = dev.allocate_set(c.dpool, p0.dsl);
    VkDescriptorSet s1 = dev.allocate_set(c.dpool, c.e1.dsl);
    if (s0 == VK_NULL_HANDLE || s1 == VK_NULL_HANDLE) {
        fprintf(stderr, "  descriptor set allocation failed\n");
        return false;
    }
    write_set(dev.handle(), s0, e0_types, { planes.buf }, img.view);
    write_set(dev.handle(), s1, e1_types,
              { planes.buf, refplanes.buf, mvbuf.buf, stats.buf }, VK_NULL_HANDLE);

    nxe_e0_push pc0{};
    pc0.f = fp;
    pc0.plane_words = plane_words;
    nxe_e1_push pc1{};
    pc1.f = fp;
    pc1.plane_words = plane_words;
    pc1.num_tiles = num_tiles;
    pc1.has_ref = 1;
    pc1.pad_ = 0;

    VkQueryPool qp = dev.timestamps_valid() ? dev.create_timestamp_pool(4) : VK_NULL_HANDLE;

    auto record = [&](VkCommandBuffer cb, bool ts) {
        if (ts && qp) vkCmdResetQueryPool(cb, qp, 0, 4);
        if (ts && qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p0.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p0.layout, 0, 1, &s0, 0, nullptr);
        vkCmdPushConstants(cb, p0.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc0), &pc0);
        vkCmdDispatch(cb, fp.tiles_x, fp.tiles_y, 1);
        if (ts && qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        dev.barrier_compute_to_compute(cb);
        if (ts && qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 2);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.e1.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.e1.layout, 0, 1, &s1, 0, nullptr);
        vkCmdPushConstants(cb, c.e1.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc1), &pc1);
        vkCmdDispatch(cb, num_tiles, 1, 1);
        if (ts && qp) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 3);
        dev.barrier_compute_to_host(cb);
    };

    {
        VkCommandBuffer cb = dev.begin();
        record(cb, false);
        if (!dev.submit_and_wait(cb, err)) { fprintf(stderr, "  dispatch: %s\n", err.c_str()); return false; }
    }

    // ---- read back and diff.
    auto download = [&](vkmin::Buffer &srcb, size_t bytes, void *dst) {
        VkCommandBuffer cb = dev.begin();
        VkBufferCopy bc{ 0, 0, bytes };
        vkCmdCopyBuffer(cb, srcb.buf, staging.buf, 1, &bc);
        std::string e;
        if (!dev.submit_and_wait(cb, e)) return false;
        memcpy(dst, staging.map, bytes);
        return true;
    };

    std::vector<uint32_t> gpu_planes(plane_words);
    std::vector<nxe_tile_stats> gpu_stats(num_tiles);
    if (!download(planes, plane_bytes, gpu_planes.data())) return false;
    if (!download(stats, stats_bytes, gpu_stats.data())) return false;

    uint64_t e0bad = 0;
    for (uint32_t i = 0; i < plane_words; ++i) {
        if (gpu_planes[i] != cpu_planes[i]) {
            if (e0bad < 4 && !c.quiet)
                fprintf(stderr, "    E0 word %u: gpu %08x cpu %08x\n",
                        i, gpu_planes[i], cpu_planes[i]);
            ++e0bad;
        }
    }

    uint64_t e1bad = 0;
    for (uint32_t t = 0; t < num_tiles; ++t) {
        const nxe_tile_stats &a = gpu_stats[t], &b = cpu_stats[t];
        if (memcmp(&a, &b, sizeof(a)) == 0) continue;
        if (e1bad < 4 && !c.quiet) {
            fprintf(stderr, "    E1 tile %u mismatch:\n", t);
#define F(name, fmt) if (a.name != b.name) fprintf(stderr, "      " #name " gpu " fmt " cpu " fmt "\n", a.name, b.name)
            F(sum_luma, "%u"); F(sum_sq_luma, "%u"); F(mean_luma_q8, "%u");
            F(sum_dev_sq, "%u"); F(j_xx, "%d"); F(j_xy, "%d"); F(j_yy, "%d");
            F(sad, "%u"); F(mv_qx, "%d"); F(mv_qy, "%d"); F(mv_mag_q4, "%u");
            F(flags, "%u");
#undef F
        }
        ++e1bad;
    }
    fail.e0 += e0bad;
    fail.e1 += e1bad;

    if (!c.quiet)
        printf("  E0/E1 %ux%u %s %s : %u tiles, E0 %s (%" PRIu64 " bad words), "
               "E1 %s (%" PRIu64 " bad tiles)\n",
               w, h, depth10 ? "RGB10A2" : "RGBA8  ", c420 ? "4:2:0" : "4:4:4",
               num_tiles, e0bad ? "FAIL" : "ok", e0bad, e1bad ? "FAIL" : "ok", e1bad);

    // ---- optional timing.
    if (bench && qp) {
        std::vector<double> t0, t1;
        for (uint32_t it = 0; it < iters; ++it) {
            VkCommandBuffer cb = dev.begin();
            record(cb, true);
            if (!dev.submit_and_wait(cb, err)) break;
            std::vector<uint64_t> ts;
            if (!dev.read_timestamps(qp, 4, ts)) break;
            double per = (double)dev.timestamp_period() * 1e-6; // ns -> ms
            t0.push_back((double)(ts[1] - ts[0]) * per);
            t1.push_back((double)(ts[3] - ts[2]) * per);
        }
        if (!t0.empty()) {
            std::sort(t0.begin(), t0.end());
            std::sort(t1.begin(), t1.end());
            if (ms_e0) *ms_e0 = t0[t0.size() / 2];
            if (ms_e1) *ms_e1 = t1[t1.size() / 2];
        }
    }

    dev.destroy_buffer(staging);
    dev.destroy_buffer(planes);
    dev.destroy_buffer(refplanes);
    dev.destroy_buffer(mvbuf);
    dev.destroy_buffer(stats);
    dev.destroy_image(img);
    return true;
}

// ----------------------------------------------------------------- E2 test
static bool run_e2(Ctx &c, uint32_t n, uint64_t seed, Failures &fail, bool sparse)
{
    std::string err;
    auto &dev = c.dev;
    vkResetDescriptorPool(dev.handle(), c.dpool, 0);
    Rng rng(seed ^ (0x51ull * n));

    std::vector<uint32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (sparse) in[i] = (rng.below(4) == 0) ? rng.below(1373) : 0;
        else        in[i] = rng.below(1373);   // paper 4.1 caps a tile at 1372 B
    }
    std::vector<uint32_t> cpu_out(n);
    uint32_t cpu_total = nxe_e2_prefix_cpu(in.data(), cpu_out.data(), n);

    const uint32_t num_blocks = (n + NXE_E2_BLOCK - 1) / NXE_E2_BLOCK;

    vkmin::Buffer staging{}, ib{}, ob{}, bb{}, tb{};
    VkDeviceSize nbytes = (VkDeviceSize)n * 4;
    bool ok = dev.create_buffer(std::max<VkDeviceSize>(nbytes, 64),
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                true, staging, err)
           && dev.create_buffer(nbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, ib, err)
           && dev.create_buffer(nbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, ob, err)
           && dev.create_buffer(NXE_E2_MAX_BLOCKS * 4 + 64,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, bb, err)
           && dev.create_buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, tb, err);
    if (!ok) { fprintf(stderr, "  E2 buffers: %s\n", err.c_str()); return false; }

    memcpy(staging.map, in.data(), nbytes);
    {
        VkCommandBuffer cb = dev.begin();
        VkBufferCopy bc{ 0, 0, nbytes };
        vkCmdCopyBuffer(cb, staging.buf, ib.buf, 1, &bc);
        if (!dev.submit_and_wait(cb, err)) return false;
    }

    const std::vector<VkDescriptorType> types(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    VkDescriptorSet sets[3];
    for (int p = 0; p < 3; ++p) {
        sets[p] = dev.allocate_set(c.dpool, c.e2[p].dsl);
        if (sets[p] == VK_NULL_HANDLE) {
            fprintf(stderr, "  E2 descriptor set allocation failed\n");
            return false;
        }
        write_set(dev.handle(), sets[p], types, { ib.buf, ob.buf, bb.buf, tb.buf }, VK_NULL_HANDLE);
    }

    nxe_e2_push pc{ n, num_blocks };
    {
        VkCommandBuffer cb = dev.begin();
        const uint32_t groups[3] = { num_blocks, 1, num_blocks };
        for (int p = 0; p < 3; ++p) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.e2[p].pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c.e2[p].layout,
                                    0, 1, &sets[p], 0, nullptr);
            vkCmdPushConstants(cb, c.e2[p].layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDispatch(cb, groups[p], 1, 1);
            dev.barrier_compute_to_compute(cb);
        }
        dev.barrier_compute_to_host(cb);
        if (!dev.submit_and_wait(cb, err)) { fprintf(stderr, "  E2 dispatch: %s\n", err.c_str()); return false; }
    }

    std::vector<uint32_t> gpu_out(n);
    uint32_t gpu_total = 0;
    {
        VkCommandBuffer cb = dev.begin();
        VkBufferCopy bc{ 0, 0, nbytes };
        vkCmdCopyBuffer(cb, ob.buf, staging.buf, 1, &bc);
        if (!dev.submit_and_wait(cb, err)) return false;
        memcpy(gpu_out.data(), staging.map, nbytes);
        cb = dev.begin();
        VkBufferCopy bc2{ 0, 0, 4 };
        vkCmdCopyBuffer(cb, tb.buf, staging.buf, 1, &bc2);
        if (!dev.submit_and_wait(cb, err)) return false;
        memcpy(&gpu_total, staging.map, 4);
    }

    uint64_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (gpu_out[i] != cpu_out[i]) {
            if (bad < 4 && !c.quiet)
                fprintf(stderr, "    E2 n=%u index %u: gpu %u cpu %u\n", n, i, gpu_out[i], cpu_out[i]);
            ++bad;
        }
    }
    if (gpu_total != cpu_total) {
        if (!c.quiet)
            fprintf(stderr, "    E2 n=%u total: gpu %u cpu %u\n", n, gpu_total, cpu_total);
        ++bad;
    }
    fail.e2 += bad;
    if (!c.quiet)
        printf("  E2 n=%-6u %s: %s (%" PRIu64 " bad)\n", n, sparse ? "sparse" : "dense",
               bad ? "FAIL" : "ok", bad);

    dev.destroy_buffer(staging);
    dev.destroy_buffer(ib);
    dev.destroy_buffer(ob);
    dev.destroy_buffer(bb);
    dev.destroy_buffer(tb);
    return true;
}

// --------------------------------------------------------------------- main
int main(int argc, char **argv)
{
    uint32_t device_index = 0;
    uint64_t seed = 12345;
    bool list = false, validation = false, bench = true, quiet = false;
    uint32_t bench_w = 2048, bench_h = 4096, iters = 50;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--list") list = true;
        else if (a == "--device") device_index = (uint32_t)strtoul(next(), nullptr, 10);
        else if (a == "--seed") seed = strtoull(next(), nullptr, 10);
        else if (a == "--validation") validation = true;
        else if (a == "--no-bench") bench = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--bench-w") bench_w = (uint32_t)strtoul(next(), nullptr, 10);
        else if (a == "--bench-h") bench_h = (uint32_t)strtoul(next(), nullptr, 10);
        else if (a == "--iters") iters = (uint32_t)strtoul(next(), nullptr, 10);
        else if (a == "--help" || a == "-h") {
            printf("usage: nxvc-stats-test [--list] [--device N] [--seed S] "
                   "[--validation] [--no-bench] [--quiet]\n"
                   "                       [--bench-w W] [--bench-h H] [--iters N]\n");
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    int st = nxe_stats_selftest();
    if (st != 0) {
        fprintf(stderr, "CPU model selftest failed (%d)\n", st);
        return 1;
    }

    std::string err;
    if (list) {
        std::vector<vkmin::DeviceInfo> devs;
        if (!vkmin::Device::enumerate(devs, err)) {
            fprintf(stderr, "no Vulkan device: %s\n", err.c_str());
            return 77;
        }
        for (size_t i = 0; i < devs.size(); ++i)
            printf("%zu: %s (subgroup %u, vendor 0x%04x)\n", i, devs[i].name.c_str(),
                   devs[i].subgroup_size, devs[i].vendor_id);
        return 0;
    }

    Ctx c;
    c.quiet = quiet;
    {
        std::vector<vkmin::DeviceInfo> devs;
        if (!vkmin::Device::enumerate(devs, err)) {
            fprintf(stderr, "SKIP: no Vulkan device (%s)\n", err.c_str());
            return 77;
        }
        if (device_index >= devs.size()) {
            fprintf(stderr, "SKIP: device %u not present (%zu available)\n",
                    device_index, devs.size());
            return 77;
        }
    }
    if (!c.dev.create(device_index, validation, err)) {
        fprintf(stderr, "SKIP: %s\n", err.c_str());
        return 77;
    }

    const auto &di = c.dev.info();
    printf("device %u: %s  (subgroup %u, api %u.%u.%u, maxWGInvocations %u, "
           "sharedMem %u B, timestamps %s)\n",
           device_index, di.name.c_str(), di.subgroup_size,
           VK_VERSION_MAJOR(di.api_version), VK_VERSION_MINOR(di.api_version),
           VK_VERSION_PATCH(di.api_version),
           c.dev.max_workgroup_invocations(), c.dev.max_shared_memory(),
           c.dev.timestamps_valid() ? "yes" : "no");

    if (c.dev.max_workgroup_invocations() < NXE_WG_SIZE) {
        fprintf(stderr, "SKIP: device supports only %u workgroup invocations, "
                        "the kernels need %u\n",
                c.dev.max_workgroup_invocations(), NXE_WG_SIZE);
        c.dev.destroy();
        return 77;
    }
    if (di.subgroup_size < NXE_MIN_SUBGROUP) {
        fprintf(stderr, "SKIP: subgroup size %u below the supported minimum %u "
                        "(paper 3.2.6)\n", di.subgroup_size, NXE_MIN_SUBGROUP);
        c.dev.destroy();
        return 77;
    }

    // ---- pipelines.
    const std::vector<VkDescriptorType> e0_types = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    const std::vector<VkDescriptorType> buf4(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    struct { const uint32_t *spv; size_t bytes; } e0src[4] = {
        { E0_convert_rgba8_444_spv,   sizeof(E0_convert_rgba8_444_spv) },
        { E0_convert_rgba8_420_spv,   sizeof(E0_convert_rgba8_420_spv) },
        { E0_convert_rgb10a2_444_spv, sizeof(E0_convert_rgb10a2_444_spv) },
        { E0_convert_rgb10a2_420_spv, sizeof(E0_convert_rgb10a2_420_spv) },
    };
    bool pok = true;
    for (int i = 0; i < 4 && pok; ++i)
        pok = c.dev.create_pipeline(e0src[i].spv, e0src[i].bytes, e0_types,
                                    sizeof(nxe_e0_push), c.e0[i], err);
    pok = pok && c.dev.create_pipeline(E1_stats_spv, sizeof(E1_stats_spv), buf4,
                                       sizeof(nxe_e1_push), c.e1, err);
    const uint32_t *e2src[3] = { E2_prefix_p0_spv, E2_prefix_p1_spv, E2_prefix_p2_spv };
    const size_t e2sz[3] = { sizeof(E2_prefix_p0_spv), sizeof(E2_prefix_p1_spv),
                             sizeof(E2_prefix_p2_spv) };
    for (int i = 0; i < 3 && pok; ++i)
        pok = c.dev.create_pipeline(e2src[i], e2sz[i], buf4, sizeof(nxe_e2_push),
                                    c.e2[i], err);
    if (!pok) {
        fprintf(stderr, "pipeline creation failed: %s\n", err.c_str());
        c.dev.destroy();
        return 1;
    }

    c.dpool = c.dev.create_descriptor_pool(64, 128, 32);

    Failures fail;
    bool hard_error = false;

    // ---- correctness: every format x chroma combination, and sizes that
    // exercise the tile grid, the padding path and a single tile.
    struct Case { uint32_t w, h; };
    const bool can10 = di.extended_storage_formats &&
                       c.dev.supports_storage_format(VK_FORMAT_A2B10G10R10_UINT_PACK32);
    if (!can10)
        printf("note: RGB10A2 storage images unsupported here; "
               "the 10-bit E0 variants are not exercised on this device\n");

    const Case cases[] = { { 64, 64 }, { 300, 180 }, { 1024, 1024 }, { 640, 129 } };
    for (const auto &cs : cases) {
        for (int d10 = 0; d10 < 2; ++d10) {
            if (d10 && !can10) continue;
            for (int c42 = 0; c42 < 2; ++c42) {
                if (!run_e0_e1(c, cs.w, cs.h, d10 != 0, c42 != 0,
                               seed + cs.w * 7 + cs.h + d10 * 3 + c42, fail,
                               false, 0, nullptr, nullptr)) {
                    hard_error = true;
                }
            }
        }
    }

    // ---- E2: block boundaries, the 8192-tile cap, and sparse input (many
    // skip tiles is the realistic case at low bitrate, paper 4.6).
    const uint32_t ns[] = { 1, 2, 255, 256, 257, 1023, 1024, 1025, 2048, 2312, 4096, 8192 };
    for (uint32_t n : ns) {
        if (!run_e2(c, n, seed, fail, false)) hard_error = true;
        if (!run_e2(c, n, seed + 1, fail, true)) hard_error = true;
    }

    // ---- timing (informational).  Paper 3.6 targets the whole encoder under
    // 4 ms on an RX 580 and under 1 ms on a 7900 XTX, so these two analysis
    // passes have to be a small fraction of that.
    if (bench) {
        double ms0 = 0, ms1 = 0;
        printf("\ntiming: %ux%u (%u tiles), RGBA8 4:2:0, %u iterations, median\n",
               bench_w, bench_h, (bench_w / 64) * (bench_h / 64), iters);
        if (!run_e0_e1(c, bench_w, bench_h, false, true, seed + 99, fail, true,
                       iters, &ms0, &ms1))
            hard_error = true;
        if (ms0 > 0 || ms1 > 0)
            printf("  E0_convert %.3f ms   E1_stats %.3f ms   sum %.3f ms\n",
                   ms0, ms1, ms0 + ms1);
        else
            printf("  (no timestamp support on this queue)\n");
    }

    printf("\ntotals: E0 %" PRIu64 " mismatching words, E1 %" PRIu64
           " mismatching tiles, E2 %" PRIu64 " mismatching entries\n",
           fail.e0, fail.e1, fail.e2);

    if (c.dpool) vkDestroyDescriptorPool(c.dev.handle(), c.dpool, nullptr);
    for (auto &p : c.e0) c.dev.destroy_pipeline(p);
    c.dev.destroy_pipeline(c.e1);
    for (auto &p : c.e2) c.dev.destroy_pipeline(p);
    c.dev.destroy();

    if (hard_error) { printf("RESULT: ERROR\n"); return 1; }
    if (fail.total()) { printf("RESULT: FAIL\n"); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
