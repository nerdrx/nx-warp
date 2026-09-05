/* nxe_vk.cpp -- see nxe_vk.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_vk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "vk_min.h"

#include "E2_prefix_p0.spv.h"
#include "E2_prefix_p1.spv.h"
#include "E2_prefix_p2.spv.h"
#include "E3_forward.spv.h"
#include "E4_rans_encode.spv.h"
#include "E5_packetize.spv.h"
#include "E5_zero.spv.h"

namespace nxe {

/* Persistent workgroups for E4.  The operation scratch is indexed by workgroup
 * slot rather than by tile, so its size is set by how many tiles are in flight
 * rather than by how many exist -- but E4 is the serial pass of the pipeline
 * and it wants every tile resident, so the cap is set by memory (46 KB of
 * scratch per tile in flight) rather than by taste. */
static const uint32_t kE4GroupsMax = 512;
/* E2_prefix scans 1024 elements per workgroup (vk/encoder/stats). */
static const uint32_t kE2Block = 1024;

struct VkEncoder::Impl {
    vkmin::Device dev;
    Config cfg;
    uint32_t ntiles = 0;
    bool ok = false;

    vkmin::Buffer b_params, b_jobs, b_src, b_coef, b_modes, b_tabs;
    vkmin::Buffer b_slots, b_sizes, b_prefix, b_blocks, b_total;
    vkmin::Buffer b_ops, b_slotops, b_out, b_pose;
    vkmin::Buffer b_stage_src, b_stage_coef, b_stage_small;

    vkmin::Pipeline p_e3, p_e4, p_e5, p_e5z, p_e2[3];
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet s_e3{}, s_e4{}, s_e5{}, s_e5z{}, s_e2[3]{};
    VkQueryPool qpool = VK_NULL_HANDLE;

    size_t src_bytes = 0, coef_bytes = 0, out_bytes = 0;
    uint32_t e4_groups = 1;
};

int vk_list_devices() {
    std::vector<vkmin::DeviceInfo> devs;
    std::string err;
    if (!vkmin::Device::enumerate(devs, err)) {
        std::fprintf(stderr, "no Vulkan: %s\n", err.c_str());
        return 77;
    }
    for (size_t i = 0; i < devs.size(); ++i)
        std::printf("%zu: %s (%s, subgroup %u)\n", i, devs[i].name.c_str(),
                    devs[i].driver.c_str(), devs[i].subgroup_size);
    return devs.empty() ? 77 : 0;
}

/* --------------------------------------------------------------- helpers */
static void write_set(VkDevice dev, VkDescriptorSet set,
                      const std::vector<VkBuffer> &bufs) {
    std::vector<VkDescriptorBufferInfo> bi(bufs.size());
    std::vector<VkWriteDescriptorSet> w(bufs.size());
    for (size_t i = 0; i < bufs.size(); ++i) {
        bi[i] = {bufs[i], 0, VK_WHOLE_SIZE};
        w[i] = {};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(dev, (uint32_t)w.size(), w.data(), 0, nullptr);
}

static const VkBufferUsageFlags kDevUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

VkEncoder::VkEncoder() : p_(new Impl) {}
VkEncoder::~VkEncoder() {
    if (p_ && p_->ok) {
        vkDeviceWaitIdle(p_->dev.handle());
        p_->dev.destroy();
    }
    delete p_;
}

bool VkEncoder::create(const Config &cfg, const Frame &f, std::string &err,
                       const Adopt *adopt) {
    Impl &d = *p_;
    d.cfg = cfg;
    d.ntiles = f.fp.ntiles;

    if (cfg.nsub_log2 > 3) {
        err = "the GPU entropy kernel is built for up to eight rANS lanes "
              "(paper 6.3); --nsub 4 and 5 need --cpu";
        return false;
    }

    if (adopt) {
        if (!d.dev.adopt(adopt->instance, adopt->physical_device, adopt->device,
                         adopt->queue, adopt->queue_family, err))
            return false;
    } else {
        std::vector<vkmin::DeviceInfo> devs;
        if (!vkmin::Device::enumerate(devs, err)) return false;
        if ((size_t)cfg.device >= devs.size()) {
            err = "no such device index";
            return false;
        }
        if (!d.dev.create((uint32_t)cfg.device, false, err)) return false;
    }
    if (d.dev.max_workgroup_invocations() < 256) {
        err = "device cannot run 256-lane workgroups";
        d.dev.destroy();
        return false;
    }
    d.ok = true;

    d.src_bytes = f.src_packed.size() * sizeof(uint16_t);
    d.coef_bytes = (size_t)d.ntiles * NXE_TILE_COEF_WORDS * 4;
    /* The frame can never exceed its tile slots plus its headers. */
    d.out_bytes = (size_t)NXE_FRAME_HEADER_BYTES +
                  (size_t)NXE_ROW_HEADER_BYTES * f.fp.tiles_y * f.fp.eyes +
                  (size_t)d.ntiles * NXE_TILE_SLOT_BYTES;

    d.e4_groups = std::min(kE4GroupsMax,
                           (d.ntiles + NXE_E4_TILES_PER_WG - 1) /
                               NXE_E4_TILES_PER_WG);
    if (d.e4_groups == 0) d.e4_groups = 1;
    const size_t slots = (size_t)d.e4_groups * NXE_E4_TILES_PER_WG;
    struct { vkmin::Buffer *b; size_t size; bool host; } mk[] = {
        {&d.b_params,  sizeof(nxe_frame_params), false},
        {&d.b_jobs,    (size_t)d.ntiles * sizeof(nxe_tile_job), false},
        {&d.b_src,     d.src_bytes, false},
        {&d.b_coef,    d.coef_bytes, false},
        {&d.b_modes,   (size_t)d.ntiles * 3 * 64, false},
        {&d.b_tabs,    sizeof(nxe_tables), false},
        {&d.b_slots,   (size_t)d.ntiles * NXE_TILE_SLOT_BYTES, false},
        {&d.b_sizes,   (size_t)d.ntiles * 4, false},
        {&d.b_prefix,  (size_t)d.ntiles * 4, false},
        {&d.b_blocks,  4096, false},
        {&d.b_total,   64, false},
        {&d.b_ops,     slots * 8 * NXE_LANE_OPS_CAP * 4, false},
        {&d.b_slotops, slots * 8 * NXE_TILE_UNIT_SLOTS * 4, false},
        {&d.b_pose,    32, false},
        {&d.b_out,     d.out_bytes, true},
        {&d.b_stage_src, d.src_bytes, true},
        {&d.b_stage_coef, d.coef_bytes, true},
        {&d.b_stage_small, 1 << 20, true},
    };
    for (auto &m : mk)
        if (!d.dev.create_buffer(m.size, kDevUsage, m.host, *m.b, err))
            return false;

    /* Specialization: the directional-intra switch and the transform edge.
     * E3 has two pipelines only because the LDS footprint of the running
     * reconstruction is sized by the constant; the behaviour switch is the
     * constant itself. */
    const uint32_t spec_vals[2] = {cfg.intra_dir ? 1u : 0u, 3u};
    VkSpecializationMapEntry ents[2] = {{0, 0, 4}, {1, 4, 4}};
    VkSpecializationInfo si{2, ents, sizeof spec_vals, spec_vals};

    const std::vector<VkDescriptorType> sb5(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const std::vector<VkDescriptorType> sb9(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const std::vector<VkDescriptorType> sb4(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const std::vector<VkDescriptorType> sb7(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    /* One module for both intra paths: the running reconstruction's shared
     * array is sized by the specialization constant, so a pipeline built with
     * NXE_SC_INTRA_DIR = 0 allocates none of it. */
    if (!d.dev.create_pipeline(E3_forward_spv, sizeof E3_forward_spv, sb5, 0,
                               d.p_e3, err, &si))
        return false;
    if (!d.dev.create_pipeline(E4_rans_encode_spv, sizeof E4_rans_encode_spv,
                               sb9, 0, d.p_e4, err, &si))
        return false;
    if (!d.dev.create_pipeline(E5_packetize_spv, sizeof E5_packetize_spv, sb7, 0,
                               d.p_e5, err, &si))
        return false;
    if (!d.dev.create_pipeline(E5_zero_spv, sizeof E5_zero_spv, sb7, 0, d.p_e5z,
                               err, &si))
        return false;
    const uint32_t *e2[3] = {E2_prefix_p0_spv, E2_prefix_p1_spv, E2_prefix_p2_spv};
    const size_t e2n[3] = {sizeof E2_prefix_p0_spv, sizeof E2_prefix_p1_spv,
                           sizeof E2_prefix_p2_spv};
    for (int i = 0; i < 3; ++i)
        if (!d.dev.create_pipeline(e2[i], e2n[i], sb4, 8, d.p_e2[i], err))
            return false;

    d.pool = d.dev.create_descriptor_pool(8, 64, 0);
    d.s_e3 = d.dev.allocate_set(d.pool, d.p_e3.dsl);
    d.s_e4 = d.dev.allocate_set(d.pool, d.p_e4.dsl);
    d.s_e5 = d.dev.allocate_set(d.pool, d.p_e5.dsl);
    d.s_e5z = d.dev.allocate_set(d.pool, d.p_e5z.dsl);
    for (int i = 0; i < 3; ++i)
        d.s_e2[i] = d.dev.allocate_set(d.pool, d.p_e2[i].dsl);

    VkDevice h = d.dev.handle();
    write_set(h, d.s_e3, {d.b_params.buf, d.b_jobs.buf, d.b_src.buf,
                          d.b_coef.buf, d.b_modes.buf});
    write_set(h, d.s_e4, {d.b_params.buf, d.b_jobs.buf, d.b_coef.buf,
                          d.b_modes.buf, d.b_tabs.buf, d.b_slots.buf,
                          d.b_sizes.buf, d.b_ops.buf, d.b_slotops.buf});
    for (int i = 0; i < 3; ++i)
        write_set(h, d.s_e2[i], {d.b_sizes.buf, d.b_prefix.buf, d.b_blocks.buf,
                                 d.b_total.buf});
    const std::vector<VkBuffer> e5bufs = {d.b_params.buf, d.b_jobs.buf,
                                         d.b_slots.buf,  d.b_prefix.buf,
                                         d.b_total.buf,  d.b_out.buf,
                                         d.b_pose.buf};
    write_set(h, d.s_e5, e5bufs);
    write_set(h, d.s_e5z, e5bufs);

    d.qpool = d.dev.create_timestamp_pool(16);
    return true;
}

/* ------------------------------------------------------------- recording */
static void copy_up(VkCommandBuffer cb, vkmin::Buffer &stage, vkmin::Buffer &dst,
                    const void *src, size_t n, size_t stage_off = 0) {
    std::memcpy((uint8_t *)stage.map + stage_off, src, n);
    VkBufferCopy c{stage_off, 0, n};
    vkCmdCopyBuffer(cb, stage.buf, dst.buf, 1, &c);
}

/* The five dispatches, with no host work between them: this is the shape
 * paper 3.6 specifies and the shape `bench` times. */
static void record_passes(VkEncoder::Impl &d, VkCommandBuffer cb, bool e3,
                          bool timestamps) {
    uint32_t q = 0;
    auto ts = [&](VkPipelineStageFlagBits s) {
        if (timestamps) vkCmdWriteTimestamp(cb, s, d.qpool, q++);
    };
    if (timestamps) vkCmdResetQueryPool(cb, d.qpool, 0, 16);

    if (e3) {
        ts(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e3.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d.p_e3.layout, 0, 1, &d.s_e3, 0, nullptr);
        vkCmdDispatch(cb, d.ntiles, 1, 1);
        d.dev.barrier_compute_to_compute(cb);
        ts(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e4.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e4.layout,
                            0, 1, &d.s_e4, 0, nullptr);
    vkCmdDispatch(cb, d.e4_groups, 1, 1);
    d.dev.barrier_compute_to_compute(cb);
    ts(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    const uint32_t nblocks = (d.ntiles + kE2Block - 1) / kE2Block;
    const uint32_t pc[2] = {d.ntiles, nblocks};
    const uint32_t groups[3] = {nblocks, 1, nblocks};
    for (int i = 0; i < 3; ++i) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e2[i].pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d.p_e2[i].layout, 0, 1, &d.s_e2[i], 0, nullptr);
        vkCmdPushConstants(cb, d.p_e2[i].layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           8, pc);
        vkCmdDispatch(cb, groups[i], 1, 1);
        d.dev.barrier_compute_to_compute(cb);
    }
    ts(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    /* E5 writes with atomicOr where a tile shares a word with its neighbour,
     * so the destination starts zeroed -- but only over the bytes this frame
     * actually occupies, which the zero pass reads from E2's total.  The
     * buffer is dimensioned for every tile at its bounded slot size, forty
     * times a real frame, and filling all of it cost more than the whole of
     * E5. */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e5z.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e5z.layout,
                            0, 1, &d.s_e5z, 0, nullptr);
    vkCmdDispatch(cb, 256, 1, 1);
    d.dev.barrier_compute_to_compute(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e5.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e5.layout,
                            0, 1, &d.s_e5, 0, nullptr);
    vkCmdDispatch(cb, std::max(d.ntiles, 1u), 1, 1);
    d.dev.barrier_compute_to_host(cb);
    ts(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

bool VkEncoder::encode_frame(Frame &f, uint32_t frame_number, bool check,
                             bool quiet) {
    Impl &d = *p_;
    std::string err;
    nxe_frame_params fp = f.fp;
    fp.frame_number = frame_number;

    /* ---- upload, then E3 alone.
     *
     * The table-set choice sits between E3 and E4 and is host work: it
     * minimises a sum of log2s over eight candidate tables, which is neither
     * normative nor integer, and paper 3.6 has E1 and the rate controller
     * settling the per-tile parameters anyway.  Reading the coefficients back
     * to make it is a harness cost, not a pipeline cost, which is why `bench`
     * times the five dispatches with the jobs already populated. */
    {
        VkCommandBuffer cb = d.dev.begin();
        copy_up(cb, d.b_stage_small, d.b_params, &fp, sizeof fp, 0);
        copy_up(cb, d.b_stage_small, d.b_tabs, &f.tabs, sizeof f.tabs,
                1 << 18);
        std::memcpy((uint8_t *)d.b_stage_small.map + (1 << 19), f.jobs.data(),
                    f.jobs.size() * sizeof(nxe_tile_job));
        VkBufferCopy cj{1 << 19, 0, f.jobs.size() * sizeof(nxe_tile_job)};
        vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_jobs.buf, 1, &cj);
        std::memcpy(d.b_stage_src.map, f.src_packed.data(), d.src_bytes);
        VkBufferCopy cs{0, 0, d.src_bytes};
        vkCmdCopyBuffer(cb, d.b_stage_src.buf, d.b_src.buf, 1, &cs);
        uint8_t pose[28] = {0};   /* the compositor's blob; zero here */
        std::memcpy((uint8_t *)d.b_stage_small.map + (1 << 20) - 64, pose, 28);
        VkBufferCopy cp{(1 << 20) - 64, 0, 28};
        vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_pose.buf, 1, &cp);
        {
            /* modes: four per byte-addressed word, 3*64 per tile */
            VkBufferCopy cm{0, 0, f.modes.size()};
            std::memcpy((uint8_t *)d.b_stage_coef.map, f.modes.data(),
                        f.modes.size());
            vkCmdCopyBuffer(cb, d.b_stage_coef.buf, d.b_modes.buf, 1, &cm);
        }
        d.dev.barrier_transfer_to_compute(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e3.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d.p_e3.layout, 0, 1, &d.s_e3, 0, nullptr);
        vkCmdDispatch(cb, d.ntiles, 1, 1);
        d.dev.barrier_compute_to_host(cb);
        VkBufferCopy cc{0, 0, d.coef_bytes};
        vkCmdCopyBuffer(cb, d.b_coef.buf, d.b_stage_coef.buf, 1, &cc);
        if (!d.dev.submit_and_wait(cb, err)) {
            std::fprintf(stderr, "E3 submit: %s\n", err.c_str());
            return false;
        }
    }

    if (check) {
        std::vector<int16_t> gpu(f.coef.size());
        std::memcpy(gpu.data(), d.b_stage_coef.map, d.coef_bytes);
        for (uint32_t t = 0; t < d.ntiles; ++t) {
            const int32_t *src[NXE_MAX_PLANES];
            for (int p = 0; p < NXE_MAX_PLANES; ++p)
                src[p] = &f.src[p][(size_t)t * f.plane_size[p] * f.plane_size[p]];
            nxe_e3_tile(&fp, &f.jobs[t], src, &f.modes[(size_t)t * 3 * 64],
                        &f.coef[(size_t)t * NXE_TILE_COEFS_MAX]);
        }
        /* Only the coded prefix of each tile's slot is compared: the slot is
         * dimensioned for 4:4:4 and a 4:2:0 tile leaves its tail untouched, so
         * on the GPU that tail is whatever the allocation came with. */
        size_t coded = 0;
        for (int p = 0; p < NXE_MAX_PLANES; ++p) {
            int nb = nxe_plane_size(&fp, &f.jobs[0], p) / 8;
            coded += (size_t)nb * nb + (size_t)nb * nb * 64;
        }
        size_t bad = 0;
        for (uint32_t t = 0; t < d.ntiles; ++t) {
            size_t b = (size_t)t * NXE_TILE_COEFS_MAX;
            for (size_t k = 0; k < coded; ++k)
                if (f.coef[b + k] != gpu[b + k] && ++bad <= 8)
                    std::fprintf(stderr,
                                 "E3 mismatch: tile %u level %zu cpu %d gpu %d\n",
                                 t, k, (int)f.coef[b + k], (int)gpu[b + k]);
        }
        if (bad) {
            std::fprintf(stderr, "E3: %zu coefficient mismatches\n", bad);
            return false;
        }
        if (!quiet)
            std::printf("  E3: bit-exact (%u tiles x %zu levels)\n", d.ntiles,
                        coded);
    } else {
        std::memcpy(f.coef.data(), d.b_stage_coef.map, d.coef_bytes);
    }

    choose_table_sets(f);

    /* ---- the jobs go back with their table sets, then E4, E2 and E5. */
    {
        VkCommandBuffer cb = d.dev.begin();
        std::memcpy(d.b_stage_small.map, f.jobs.data(),
                    f.jobs.size() * sizeof(nxe_tile_job));
        VkBufferCopy cj{0, 0, f.jobs.size() * sizeof(nxe_tile_job)};
        vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_jobs.buf, 1, &cj);
        d.dev.barrier_transfer_to_compute(cb);
        record_passes(d, cb, false, false);
        VkBufferCopy cz{0, 1 << 19, (size_t)d.ntiles * 4};
        vkCmdCopyBuffer(cb, d.b_sizes.buf, d.b_stage_small.buf, 1, &cz);
        if (!d.dev.submit_and_wait(cb, err)) {
            std::fprintf(stderr, "E4/E5 submit: %s\n", err.c_str());
            return false;
        }
        std::memcpy(f.tile_bytes.data(),
                    (uint8_t *)d.b_stage_small.map + (1 << 19), d.ntiles * 4);
    }

    uint32_t run = 0;
    for (uint32_t t = 0; t < d.ntiles; ++t) run += f.tile_bytes[t];
    const uint32_t total = nxe_e5_frame_bytes(&fp, run);
    f.out.assign(total, 0);
    std::memcpy(f.out.data(), d.b_out.map, total);

    if (check) {
        std::vector<uint8_t> gpu = f.out;
        encode_frame_cpu(f, frame_number);
        if (gpu.size() != f.out.size() ||
            std::memcmp(gpu.data(), f.out.data(), gpu.size()) != 0) {
            size_t i = 0;
            while (i < std::min(gpu.size(), f.out.size()) && gpu[i] == f.out[i])
                ++i;
            std::fprintf(stderr,
                         "E4/E5 mismatch: gpu %zu bytes, cpu %zu bytes, first "
                         "difference at %zu\n",
                         gpu.size(), f.out.size(), i);
            return false;
        }
        if (!quiet) std::printf("  E4/E5: byte-identical (%zu bytes)\n", gpu.size());
    }
    return true;
}

void VkEncoder::bench(Frame &f, int iters) {
    Impl &d = *p_;
    std::string err;
    std::vector<double> ms[4];
    for (int it = 0; it < iters; ++it) {
        VkCommandBuffer cb = d.dev.begin();
        record_passes(d, cb, true, true);
        if (!d.dev.submit_and_wait(cb, err)) return;
        std::vector<uint64_t> q;
        if (!d.dev.read_timestamps(d.qpool, 5, q)) return;
        const double per = d.dev.timestamp_period() * 1e-6;
        for (int i = 0; i < 4; ++i)
            ms[i].push_back((double)(q[i + 1] - q[i]) * per);
    }
    const char *names[4] = {"E3 forward", "E4 rans_encode", "E2 prefix",
                            "E5 packetize"};
    double sum = 0;
    std::printf("\n%s, %u tiles, median of %d iterations:\n",
                d.dev.info().name.c_str(), d.ntiles, iters);
    for (int i = 0; i < 4; ++i) {
        std::sort(ms[i].begin(), ms[i].end());
        double m = ms[i][ms[i].size() / 2];
        sum += m;
        std::printf("  %-16s %8.3f ms\n", names[i], m);
    }
    std::printf("  %-16s %8.3f ms\n", "total", sum);
}

}  // namespace nxe
