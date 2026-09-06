/* nxe_vk.cpp -- see nxe_vk.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_vk.h"

#include "nxe_inter.h"
#include "E1c_decide.spv.h"
#include "warp_pred.spv.h"
#include "reconstruct_v1_x8.spv.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "nxe_tables.h"
}
#include "nxe_e0.h"
#include "vk_min.h"

#include "E2_prefix_p0.spv.h"
#include "E2_prefix_p1.spv.h"
#include "E2_prefix_p2.spv.h"
#include "E3_forward.spv.h"
#include "E4_lite_encode.spv.h"
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

    vkmin::Buffer b_params, b_jobs, b_src, b_coef, b_modes, b_tabs, b_tabbytes;
    vkmin::Buffer b_slots, b_sizes, b_prefix, b_blocks, b_total;
    vkmin::Buffer b_ops, b_slotops, b_out, b_pose, b_warpext;
    /* [planar] One raw body per tile, NXE_PLANAR_BODY_UINTS words each, filled
     * on the host by the shared integer fit and copied out verbatim by E5.
     * Allocated always -- an unbound descriptor is illegal and 104 B a tile is
     * cheaper than a branch in create(). */
    vkmin::Buffer b_planar;
    /* The inter path: the four-slot reference ring, the parameter buffer
     * Pass W reads, and the predictor it writes.  Allocated even on an
     * intra-only stream, at four bytes each -- an unbound descriptor is
     * illegal and a branch in create() is worse than 12 bytes. */
    vkmin::Buffer b_ring, b_warp, b_wpred;
    /* Pass B's consumer buffers.  The COEFFICIENTS are not among them: Pass B
     * strides tiles by its `coefStrideI16` push constant, and E3's per-tile
     * layout is already the one it wants -- DC levels then blocks of 64, and
     * plane p at the sum of nb*nb*65 over earlier planes, which is exactly
     * nxvw_plane_coef_count.  So b_coef is bound straight through with the
     * stride set to NXE_TILE_COEFS_MAX, and there is no repack pass. */
    vkmin::Buffer b_tilerecs, b_weights, b_order, b_dummy;
    std::vector<vkmin::Image> ph;   /* 1x1 placeholders, Pass B's 7 images */
    vkmin::Buffer b_stage_src, b_stage_coef, b_stage_small;

    vkmin::Pipeline p_e3, p_e4, p_e5, p_e5z, p_e2[3];
    /* E4-lite (ENTROPY_LITE, 30) and the E5 variant that reads its payload
     * layout.  Both are created only when the stream carries the tool: they
     * share E4's and E5's descriptor set layouts, so what a Lite frame
     * changes is which pipeline is bound and nothing else. */
    vkmin::Pipeline p_e4l, p_e5l;
    bool entropy_lite = false;
    /* Pass W is the DECODER's warp_pred.comp; E1c is the mode decision. */
    vkmin::Pipeline p_w, p_dec, p_b;
    E0 e0;
    /* Plane views of the caller's images, one entry per (image, layer) the
     * caller has presented.  A compositor rotates over a handful of images
     * for the life of a stream, so this never grows: creating the two views
     * per frame instead would be two vkCreateImageView calls on the encode
     * path for handles that never change. */
    struct SrcViews {
        VkImage image = VK_NULL_HANDLE;
        uint32_t layer = 0;
        /* Layers the view spans: 1, or 2 when the eyes are separate layers.
         * Part of the key because the same image and base layer can be asked
         * for both ways and the views are not interchangeable. */
        uint32_t layers = 1;
        VkImageView y = VK_NULL_HANDLE, c = VK_NULL_HANDLE;
    };
    std::vector<SrcViews> src_views;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet s_e3{}, s_e4{}, s_e5{}, s_e5z{}, s_e2[3]{};
    VkDescriptorSet s_w{}, s_dec{}, s_b{};
    VkQueryPool qpool = VK_NULL_HANDLE;
    bool e0_timed = false;
    double e0_ms = 0;

    size_t src_bytes = 0, coef_bytes = 0, out_bytes = 0;
    uint32_t e4_groups = 1;

    /* Inter state.  `inter` is the stream's; the rest is per frame. */
    bool inter = false;
    RingLayout ring{};
    RingState ringst{};
    /* What the HEADSET is believed to hold, which is not what the encoder
     * produced the moment a frame is dropped on the client.  nxe_inter.h. */
    HeldState heldst{};
    /* The distance this frame ended up asking for, and the frame number it
     * predicted from (-1 when every tile came out INTRA).  Both are decided
     * before the dispatches and consumed after them. */
    int cur_ref_sel = 0;
    /* Snap-to-identity, per frame, for the harness to report: the worst tile
     * corner displacement in Q.6 (-1 when the tool is off or there was no
     * reference) and whether the matrix was replaced.  A stream cannot be
     * asked afterwards -- an identity warp_ext looks the same whether it was
     * derived or snapped -- so the encoder is the only place that knows. */
    int32_t snap_worst_q6 = -1;
    bool snap_applied = false;
    uint32_t snap_frames = 0, snap_frames_applied = 0;
    uint64_t identity_tiles = 0, identity_tiles_total = 0;
    /* The distribution of the thing the threshold is compared against, so a
     * run can say "this clip moves N/16 of a sample a frame" instead of
     * leaving the operator to bisect for it. */
    int32_t off_min_q6 = -1, off_max_q6 = -1;
    double off_sum_q6 = 0;
    uint32_t off_n = 0;
    int64_t cur_pred_fn = -1;
    WarpParams warp{};
    int wpred_stride = 0;
    nxvw::NxvwWarpPush wpush{};
    int32_t decide_push[12] = {};
    nxvw::NxvwPassBPush bpush{};
    ViewState views{};
    /* Tiles the client is known NOT to hold.  Set by set_received_tiles(),
     * consumed by the next frame's eligibility pass and then cleared: a tile
     * coded INTRA is a tile the client can hold again. */
    std::vector<uint8_t> force_intra;
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

/* Pass B's set is not all buffers: seven of its sixteen bindings are storage
 * images it may write the decoded picture to.  The encoder writes none of them
 * -- it runs Pass B with kOutFormat == kOutNone, for the reference ring alone
 * -- but a descriptor still has to be there, so each is a 1x1 placeholder of
 * the format its binding declares.  A storage image's format is a layout
 * qualifier in GLSL and cannot follow a specialization constant, which is why
 * there are seven of them and not one.
 *
 * `slots` names each binding as buffer-or-image in binding order; a null
 * VkBuffer means "take the next image instead". */
static void write_set_mixed(VkDevice dev, VkDescriptorSet set,
                            const std::vector<VkBuffer> &bufs,
                            const std::vector<VkImageView> &imgs) {
    const size_t n = bufs.size();
    std::vector<VkDescriptorBufferInfo> bi(n);
    std::vector<VkDescriptorImageInfo> ii(n);
    std::vector<VkWriteDescriptorSet> w;
    size_t next_img = 0;
    for (size_t i = 0; i < n; ++i) {
        VkWriteDescriptorSet ws{};
        ws.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws.dstSet = set;
        ws.dstBinding = (uint32_t)i;
        ws.descriptorCount = 1;
        if (bufs[i] != VK_NULL_HANDLE) {
            bi[i] = {bufs[i], 0, VK_WHOLE_SIZE};
            ws.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ws.pBufferInfo = &bi[i];
        } else {
            ii[i] = {VK_NULL_HANDLE, imgs[next_img++],
                     VK_IMAGE_LAYOUT_GENERAL};
            ws.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            ws.pImageInfo = &ii[i];
        }
        w.push_back(ws);
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
        for (auto &s : p_->src_views) {
            vkDestroyImageView(p_->dev.handle(), s.y, nullptr);
            vkDestroyImageView(p_->dev.handle(), s.c, nullptr);
        }
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
    d.out_bytes = (size_t)NXE_FRAME_HEADER_BYTES + NXE_TABLE_AREA_MAX +
                  (size_t)NXE_ROW_HEADER_BYTES * f.fp.tiles_y * f.fp.eyes +
                  (size_t)d.ntiles * NXE_TILE_SLOT_BYTES;

    /* Inter geometry.  On an intra-only stream every one of these is the
     * minimum legal size and nothing ever reads them. */
    d.inter = cfg.inter;
    /* A caller that says its client confirms gets confirmations REQUIRED from
     * frame 0, which is what removes the startup window in which the encoder
     * would otherwise still be guessing. */
    d.heldst.require_confirmed = cfg.inter && cfg.ref_confirm;
    if (d.inter) {
        const int cw = cfg.chroma444 ? cfg.w / cfg.eyes : (cfg.w / cfg.eyes + 1) / 2;
        const int ch = cfg.chroma444 ? cfg.h : (cfg.h + 1) / 2;
        ring_layout(cfg.w / cfg.eyes, cfg.h, cw, ch, cfg.eyes, 3, d.ring);
        d.wpred_stride = wpred_stride_i16(cfg.chroma444 ? 0 : 1, 0);
    }
    const size_t ring_bytes = d.inter ? d.ring.bytes() : 4u;
    const size_t wpred_b =
        d.inter ? wpred_bytes(d.ntiles, cfg.chroma444 ? 0 : 1, 0) : 4u;
    const size_t warp_b =
        d.inter ? ((size_t)NXVW_WARP_HDR_UINTS +
                   (size_t)d.ntiles * NXVW_WARP_TILE_UINTS) * 4u
                : 4u;

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
        {&d.b_tabbytes, NXE_TABLE_AREA_MAX, false},
        /* warp_ext(): nine int32 per eye.  Sized for two eyes whatever the
         * stream is, because it is 72 bytes. */
        {&d.b_warpext, 9 * 4 * 2, false},
        /* [planar] One body a tile.  Host-visible: the fit runs on the CPU
         * (it is the shared exact-integer one) and E5 only reads it. */
        {&d.b_planar,
         (size_t)std::max(d.ntiles, 1u) * NXE_PLANAR_BODY_UINTS * 4, true},
        {&d.b_ring,    ring_bytes, false},
        {&d.b_warp,    warp_b,     false},
        {&d.b_wpred,   wpred_b,    false},
        {&d.b_tilerecs, (size_t)std::max(d.ntiles, 1u) * 16, false},
        {&d.b_weights,  512 * 4,    false},
        {&d.b_order,    (size_t)std::max(d.ntiles, 1u) * 4, false},
        {&d.b_dummy,    4096,       false},
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
    /* [planar] E5 gained binding 9, the planar body buffer.  E4 keeps nine. */
    const std::vector<VkDescriptorType> sb10(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const std::vector<VkDescriptorType> sb4(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    const std::vector<VkDescriptorType> sb8(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    /* One module for both intra paths: the running reconstruction's shared
     * array is sized by the specialization constant, so a pipeline built with
     * NXE_SC_INTRA_DIR = 0 allocates none of it. */
    const std::vector<VkDescriptorType> sb6e(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    if (!d.dev.create_pipeline(E3_forward_spv, sizeof E3_forward_spv, sb6e, 0,
                               d.p_e3, err, &si))
        return false;
    if (!d.dev.create_pipeline(E4_rans_encode_spv, sizeof E4_rans_encode_spv,
                               sb9, 0, d.p_e4, err, &si))
        return false;
    if (!d.dev.create_pipeline(E5_packetize_spv, sizeof E5_packetize_spv, sb10, 0,
                               d.p_e5, err, &si))
        return false;
    if (!d.dev.create_pipeline(E5_zero_spv, sizeof E5_zero_spv, sb10, 0, d.p_e5z,
                               err, &si))
        return false;
    /* ENTROPY_LITE.  E4-lite is its own module; E5 is the same module with
     * NXE_SC_ENTROPY_LITE set, because the only thing the tool changes there
     * is where a tile's payload bytes live inside the slot. */
    d.entropy_lite = f.entropy_lite != 0;
    if (d.entropy_lite) {
        const uint32_t lite_vals[3] = {cfg.intra_dir ? 1u : 0u, 3u, 1u};
        VkSpecializationMapEntry lents[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
        VkSpecializationInfo lsi{3, lents, sizeof lite_vals, lite_vals};
        if (!d.dev.create_pipeline(E4_lite_encode_spv,
                                   sizeof E4_lite_encode_spv, sb9, 0, d.p_e4l,
                                   err, &lsi))
            return false;
        if (!d.dev.create_pipeline(E5_packetize_spv, sizeof E5_packetize_spv,
                                   sb10, 0, d.p_e5l, err, &lsi))
            return false;
    }
    const uint32_t *e2[3] = {E2_prefix_p0_spv, E2_prefix_p1_spv, E2_prefix_p2_spv};
    const size_t e2n[3] = {sizeof E2_prefix_p0_spv, sizeof E2_prefix_p1_spv,
                           sizeof E2_prefix_p2_spv};
    for (int i = 0; i < 3; ++i)
        if (!d.dev.create_pipeline(e2[i], e2n[i], sb4, 8, d.p_e2[i], err))
            return false;

    /* Pass W's FOUR buffers and E1c's five, plus two more sets.  The fourth
     * is the tile order: warp_pred.comp takes its tile from it rather than
     * from gl_WorkGroupID.x, so that one dispatch can cover a contiguous
     * range of the decoder's partition.  This encoder dispatches every tile,
     * and b_order is the identity here (aux[512 + t] = t below), so binding
     * it changes nothing -- but the binding must exist or the kernel reads a
     * descriptor that is not there. */
    const std::vector<VkDescriptorType> sb3(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    if (!d.dev.create_pipeline(warp_pred_spv, sizeof warp_pred_spv, sb3,
                               (uint32_t)sizeof(nxvw::NxvwWarpPush), d.p_w, err))
        return false;
    const std::vector<VkDescriptorType> sb8d(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    if (!d.dev.create_pipeline(E1c_decide_spv, sizeof E1c_decide_spv, sb8d,
                               (uint32_t)sizeof d.decide_push, d.p_dec, err))
        return false;

    /* Pass B: the decoder's module, in the variant matching this stream --
     * kOutFormat kOutNone (it writes no picture here, only the ring),
     * kSparse 0 (E3's dense per-tile layout), inter_pred and ring_store on.
     * The nine spec constants and their order are the decoder's own; see
     * nxvc_vkdec.cpp's Pass B pipeline cache. */
    {
        const int store_words =
            cfg.chroma444 ? 3 * (64 * 64 / 2) : (64 * 64 / 2) + 2 * (32 * 32 / 2);
        const int32_t bspec[9] = {
            -1 /* kOutFormat  = kOutNone */,
            (int32_t)store_words,
            0 /* kDirSched */,
            -1 /* kOutSecond  = kOutNone */,
            0 /* kSparse: dense, so unitLen() is 64 and UnitLens is unread */,
            0 /* kUnormStore */,
            0 /* kSplitTool */,
            1 /* inter_pred */,
            1 /* kRefRingStore */};
        VkSpecializationMapEntry bme[9];
        for (int i = 0; i < 9; ++i)
            bme[i] = {(uint32_t)i, (uint32_t)(i * 4), 4};
        VkSpecializationInfo bsi{9, bme, sizeof bspec, bspec};
        std::vector<VkDescriptorType> bb(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        for (int i : {3, 4, 5, 6, 10, 11, 12})
            bb[(size_t)i] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (!d.dev.create_pipeline(reconstruct_v1_x8_spv,
                                   sizeof reconstruct_v1_x8_spv, bb,
                                   (uint32_t)sizeof(nxvw::NxvwPassBPush), d.p_b,
                                   err, &bsi))
            return false;
    }

    d.pool = d.dev.create_descriptor_pool(13, 100, 10);
    d.s_e3 = d.dev.allocate_set(d.pool, d.p_e3.dsl);
    d.s_e4 = d.dev.allocate_set(d.pool, d.p_e4.dsl);
    d.s_e5 = d.dev.allocate_set(d.pool, d.p_e5.dsl);
    d.s_e5z = d.dev.allocate_set(d.pool, d.p_e5z.dsl);
    for (int i = 0; i < 3; ++i)
        d.s_e2[i] = d.dev.allocate_set(d.pool, d.p_e2[i].dsl);

    VkDevice h = d.dev.handle();
    write_set(h, d.s_e3, {d.b_params.buf, d.b_jobs.buf, d.b_src.buf,
                          d.b_coef.buf, d.b_modes.buf, d.b_wpred.buf});
    write_set(h, d.s_e4, {d.b_params.buf, d.b_jobs.buf, d.b_coef.buf,
                          d.b_modes.buf, d.b_tabs.buf, d.b_slots.buf,
                          d.b_sizes.buf, d.b_ops.buf, d.b_slotops.buf});
    for (int i = 0; i < 3; ++i)
        write_set(h, d.s_e2[i], {d.b_sizes.buf, d.b_prefix.buf, d.b_blocks.buf,
                                 d.b_total.buf});
    const std::vector<VkBuffer> e5bufs = {d.b_params.buf, d.b_jobs.buf,
                                         d.b_slots.buf,  d.b_prefix.buf,
                                         d.b_total.buf,  d.b_out.buf,
                                         d.b_pose.buf,   d.b_tabbytes.buf,
                                         d.b_warpext.buf, d.b_planar.buf};
    write_set(h, d.s_e5, e5bufs);
    write_set(h, d.s_e5z, e5bufs);

    /* Pass W takes {ring, warp, wpred} in that order; E1c takes
     * {params, jobs, src, wpred, warp}.  The two orders differ, and so do
     * Pass B's -- the decoder's host keeps three separate arrays for exactly
     * this reason and getting it wrong binds a readonly buffer where a
     * writeonly one belongs. */
    d.s_w = d.dev.allocate_set(d.pool, d.p_w.dsl);
    d.s_dec = d.dev.allocate_set(d.pool, d.p_dec.dsl);
    write_set(h, d.s_w,
              {d.b_ring.buf, d.b_warp.buf, d.b_wpred.buf, d.b_order.buf});
    write_set(h, d.s_dec, {d.b_params.buf, d.b_jobs.buf, d.b_src.buf,
                           d.b_wpred.buf, d.b_warp.buf, d.b_tilerecs.buf,
                           d.b_ring.buf, d.b_warp.buf});

    /* Pass B's sixteen bindings.  The seven image ones get 1x1 placeholders of
     * exactly the format each declares; nothing is ever written to them. */
    {
        const VkFormat pf[7] = {
            VK_FORMAT_R8G8B8A8_UINT,           /* 3  uOutRgba8    */
            VK_FORMAT_A2B10G10R10_UINT_PACK32, /* 4  uOutRgb10a2  */
            VK_FORMAT_R8_UINT,                 /* 5  uOutLuma     */
            VK_FORMAT_R8G8_UINT,               /* 6  uOutCbCr     */
            VK_FORMAT_R8G8B8A8_UNORM,          /* 10 uOutRgba8N   */
            VK_FORMAT_R8_UNORM,                /* 11 uOutLumaN    */
            VK_FORMAT_R8G8_UNORM};             /* 12 uOutCbCrN    */
        d.ph.resize(7);
        std::vector<VkImageView> views;
        for (int i = 0; i < 7; ++i) {
            if (!d.dev.create_storage_image(1, 1, pf[i], d.ph[(size_t)i], err))
                return false;
            views.push_back(d.ph[(size_t)i].view);
        }

        /* Into GENERAL, once, here.  The descriptor written just below says
         * VK_IMAGE_LAYOUT_GENERAL for all seven, and a descriptor's declared
         * layout is a promise about the image at the moment the command that
         * reads it executes -- not about whether the shader touches it.  These
         * are created VK_IMAGE_LAYOUT_UNDEFINED and Pass B never writes them
         * (it runs with kOutFormat == kOutNone), so nothing here ever moved
         * them and every submit that bound this set was submitting seven
         * images in the wrong layout: VUID-vkCmdDraw-None-09600, ten of them
         * on a twelve-frame encode.
         *
         * One command buffer for all seven, at create time, so the encode path
         * is untouched.  TOP_OF_PIPE with no access mask on either side is the
         * correct pair for a layout transition out of UNDEFINED that discards
         * whatever the contents were: there is nothing to make visible, and
         * these images have no contents anybody wants. */
        {
            VkCommandBuffer cb = d.dev.begin();
            VkImageMemoryBarrier bs[7]{};
            for (int i = 0; i < 7; ++i) {
                bs[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                bs[i].srcAccessMask = 0;
                bs[i].dstAccessMask = 0;
                bs[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                bs[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                bs[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bs[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bs[i].image = d.ph[(size_t)i].img;
                bs[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0,
                                 nullptr, 0, nullptr, 7, bs);
            if (!d.dev.submit_and_wait(cb, err)) return false;
        }
        d.s_b = d.dev.allocate_set(d.pool, d.p_b.dsl);
        const VkBuffer N = VK_NULL_HANDLE;
        write_set_mixed(h, d.s_b,
                        {d.b_coef.buf, d.b_tilerecs.buf, d.b_weights.buf,
                         N, N, N, N,
                         d.b_dummy.buf, d.b_order.buf, d.b_dummy.buf,
                         N, N, N,
                         d.b_wpred.buf, d.b_ring.buf, d.b_warp.buf},
                        views);
    }

    /* E0 is created whether or not an image is ever presented: it is two
     * dozen kilobytes of SPIR-V and one descriptor set, and a create-time
     * failure is worth having at create time rather than on the first frame
     * a compositor hands over an image. */
    if (!d.e0.create(d.dev, d.pool, err)) return false;

    d.qpool = d.dev.create_timestamp_pool(16);
    return true;
}

/* Plane views of a caller's two-plane image, cached per (image, layer).
 *
 * The views are UINT (R8_UINT over the R8_UNORM plane, R8G8_UINT over the
 * R8G8_UNORM one) because E0 reads stored codes and never a filtered sample;
 * the image must have been created with MUTABLE_FORMAT and a
 * VkImageFormatListCreateInfo naming both, or the driver rejects them.
 * VkImageViewUsageCreateInfo narrows the view to STORAGE, which is what lets
 * a storage view exist over an image whose planar format has no storage
 * feature of its own (the EXTENDED_USAGE rule of maintenance2). */
static bool src_views_for(VkEncoder::Impl &d, VkImage image, uint32_t layer,
                          uint32_t layers, VkImageView &vy, VkImageView &vc,
                          std::string &err) {
    for (const auto &s : d.src_views)
        if (s.image == image && s.layer == layer && s.layers == layers) {
            vy = s.y;
            vc = s.c;
            return true;
        }

    VkImageViewUsageCreateInfo usage{};
    usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
    usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;

    auto make = [&](VkImageAspectFlagBits aspect, VkFormat fmt,
                    VkImageView &out) {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.pNext = &usage;
        ci.image = image;
        /* Always an ARRAY view, even for the one layer a mono picture uses:
         * E0 samples through a uimage2DArray so that one binding can carry
         * both eyes when they arrive as separate layers, and a 2D view is not
         * a legal array view.  layerCount is `layers`, which is 1 unless the
         * caller asked for the layered stereo shape. */
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        ci.format = fmt;
        ci.subresourceRange.aspectMask = VkImageAspectFlags(aspect);
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.baseArrayLayer = layer;
        ci.subresourceRange.layerCount = layers;
        VkResult r = vkCreateImageView(d.dev.handle(), &ci, nullptr, &out);
        if (r != VK_SUCCESS) {
            err = std::string("vkCreateImageView for the source plane: ") +
                  vkmin::result_str(r);
            return false;
        }
        return true;
    };

    VkEncoder::Impl::SrcViews s{};
    s.image = image;
    s.layer = layer;
    if (!make(VK_IMAGE_ASPECT_PLANE_0_BIT, VK_FORMAT_R8_UINT, s.y))
        return false;
    if (!make(VK_IMAGE_ASPECT_PLANE_1_BIT, VK_FORMAT_R8G8_UINT, s.c)) {
        vkDestroyImageView(d.dev.handle(), s.y, nullptr);
        return false;
    }
    s.layers = layers;
    d.src_views.push_back(s);
    vy = s.y;
    vc = s.c;
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

    /* E4, or E4-lite.  The two take the same descriptor set and write the
     * same three job fields and the same `sizes` entry, so everything after
     * this point is unchanged: only the workgroup shape differs, one tile per
     * group against eight, because Lite has no serial chain to keep resident
     * and wants a whole workgroup per tile instead. */
    const vkmin::Pipeline &pe4 = d.entropy_lite ? d.p_e4l : d.p_e4;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pe4.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pe4.layout,
                            0, 1, &d.s_e4, 0, nullptr);
    vkCmdDispatch(cb, d.entropy_lite ? std::max(d.ntiles, 1u) : d.e4_groups,
                  1, 1);
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
    const vkmin::Pipeline &pe5 = d.entropy_lite ? d.p_e5l : d.p_e5;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pe5.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pe5.layout,
                            0, 1, &d.s_e5, 0, nullptr);
    vkCmdDispatch(cb, std::max(d.ntiles, 1u), 1, 1);
    d.dev.barrier_compute_to_host(cb);
    ts(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

bool VkEncoder::encode_frame(Frame &f, uint32_t frame_number, bool check,
                             bool quiet) {
    std::string err;
    return encode_frame_common(f, frame_number, check, quiet, nullptr, 0, 1, err);
}

bool VkEncoder::encode_frame_image(Frame &f, uint32_t frame_number,
                                   VkImage image, uint32_t array_layer,
                                   uint32_t layers, std::string &err) {
    return encode_frame_common(f, frame_number, false, true, &image,
                               array_layer, layers, err);
}

bool VkEncoder::encode_frame_common(Frame &f, uint32_t frame_number, bool check,
                                    bool quiet, const VkImage *image,
                                    uint32_t array_layer, uint32_t src_layers,
                                    std::string &err) {
    Impl &d = *p_;
    /* The plane views before anything is recorded: a failure here is a
     * bring-up failure and there is nothing to unwind. */
    VkImageView src_y = VK_NULL_HANDLE, src_c = VK_NULL_HANDLE;
    if (image && !src_views_for(d, *image, array_layer, src_layers, src_y, src_c, err))
        return false;
    /* NXE_TIME=1 prints where a frame's milliseconds went, in the four pieces
     * that can move independently: the GPU passes up to E3, the table-set
     * choice, the E4/E5 submit, and the two host reads.  It is off unless the
     * environment asks, and it is the measurement the image entry point was
     * written against. */
    static const bool nxe_time = std::getenv("NXE_TIME") != nullptr;
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    nxe_frame_params fp = f.fp;
    fp.frame_number = frame_number;

    /* ---- the inter frame's own parameters, decided on the host before a
     * single dispatch: which ring slot this frame predicts from, whether
     * there is one at all, and which tiles the rolling refresh forces INTRA.
     *
     * The mode decision proper is E1c's, on the device.  What cannot go there
     * is the part that depends on state the GPU does not hold -- the ring's
     * validity record -- and the part that must be known BEFORE Pass W runs,
     * because Pass W only predicts a tile whose record has the inter bit set.
     * So the host marks every eligible tile WARP_SKIP, Pass W predicts them
     * all, and E1c then keeps or overturns that per tile.  A tile the
     * decision turns back to INTRA has simply had a predictor computed that
     * nothing reads, which costs a little of Pass W and no correctness.
     */
    int ref_slot = -1;
    if (d.inter) {
        /* Which reference, not merely whether there is one.  `ref_sel` walks
         * outwards from the configured distance to the nearest slot the
         * headset still holds, so a client that dropped frame N-1 is sent a
         * frame predicting from N-2 instead of a full INTRA resync.  The
         * fallback is unchanged: nothing held within three frames is still an
         * all-INTRA frame with the tile-map reset flag. */
        d.cur_ref_sel = 0;
        if (!select_reference(d.ringst, d.heldst, frame_number, d.cfg.ref_sel,
                              &d.cur_ref_sel, &ref_slot))
            ref_slot = -1;
        d.cur_pred_fn =
            ref_slot >= 0
                ? (int64_t)frame_number - 1 - (int64_t)d.cur_ref_sel
                : -1;
        WarpBuildInfo bi;
        bi.width = (int)f.fp.width;
        bi.height = (int)f.fp.height;
        bi.cw = d.cfg.chroma444 ? bi.width : (bi.width + 1) / 2;
        bi.ch = d.cfg.chroma444 ? bi.height : (bi.height + 1) / 2;
        bi.eyes = (int)f.fp.eyes;
        bi.cols_per_eye = (int)f.fp.tiles_x;
        bi.rows = (int)f.fp.tiles_y;
        bi.chroma420 = d.cfg.chroma444 ? 0 : 1;
        bi.nplanes = 3;
        bi.frame_number = frame_number;
        bi.ref_slot = ref_slot;
        /* warp_ext(), from the view that went with the reference slot and the
         * view of this frame.  With no pose input the view history is all
         * identity and so is the matrix, which predicts a still picture
         * correctly and a turning head badly -- and the mode decision then
         * declines to skip, which is the right failure. */
        WarpMatrix wm[2];
        for (int e = 0; e < 2; ++e) {
            wm[e] = derive_warp(d.views, ref_slot, e, (int)f.fp.width,
                                (int)f.fp.height);
            for (int i = 0; i < 9; ++i) f.warp[e][i] = wm[e].h[i];
        }
        /* Snap a nearly-still warp to the identity, so every skipped tile is a
         * copy on the decoder rather than an integer warp of a picture that
         * has not moved (nxe_host.h `snap_identity`).  Both eyes are decided
         * TOGETHER on the worse of the two: they are one picture to the
         * decoder's skip module, and snapping one eye while the other warps
         * would buy half the saving for all of the error.
         *
         * The measure is the largest tile-corner displacement in the picture,
         * so a matrix that is identity where it matters and not at the edges
         * does not qualify -- the fast path is per tile, but the choice is per
         * frame, and half a frame of copies is not what this is for. */
        if (d.cfg.snap_identity > 0 && ref_slot >= 0) {
            const int32_t thr = d.cfg.snap_identity * 4;   /* 1/16 -> Q.6 */
            int32_t worst = 0;
            for (int e = 0; e < (int)f.fp.eyes; ++e) {
                const int32_t o = nxe::warp_max_corner_offset(
                    wm[e], (int)f.fp.width, (int)f.fp.height, 1);
                if (o > worst) worst = o;
            }
            d.snap_worst_q6 = worst;
            d.snap_applied = worst < thr;
            if (d.off_min_q6 < 0 || worst < d.off_min_q6) d.off_min_q6 = worst;
            if (worst > d.off_max_q6) d.off_max_q6 = worst;
            d.off_sum_q6 += (double)worst;
            ++d.off_n;
            if (d.snap_applied)
                for (int e = 0; e < 2; ++e) {
                    wm[e] = WarpMatrix{};   /* the identity */
                    for (int i = 0; i < 9; ++i) f.warp[e][i] = wm[e].h[i];
                }
        } else {
            d.snap_worst_q6 = -1;
            d.snap_applied = false;
        }
        if (d.cfg.snap_identity > 0 && ref_slot >= 0) {
            ++d.snap_frames;
            if (d.snap_applied) ++d.snap_frames_applied;
        }
        /* The identity predicate, COUNTED on the matrix this frame will carry,
         * whether or not the snap is on.  It is the tile count the decoder's
         * fast path will take, and the only place it can be measured: the
         * stream cannot be asked, because a matrix does not say how it was
         * arrived at. */
        if (ref_slot >= 0) {
            int total = 0;
            const int id = nxe::warp_identity_tiles(wm[0], (int)f.fp.width,
                                                    (int)f.fp.height,
                                                    (int)f.fp.eyes, &total);
            d.identity_tiles += (uint64_t)id;
            d.identity_tiles_total += (uint64_t)total;
            /* NXE_IDENTITY_MAP=<path> appends one byte per tile per inter
             * frame -- 1 where the warp is the identity and the decoder will
             * copy, 0 where it will interpolate.  It is the encoder's own
             * predicate rather than a second implementation of it, which is
             * the only way a picture of this can be trusted. */
            if (const char *mp = std::getenv("NXE_IDENTITY_MAP")) {
                if (std::FILE *mf = std::fopen(mp, "ab")) {
                    std::vector<uint8_t> row;
                    nxe::warp_identity_tile_map(wm[0], (int)f.fp.width,
                                                (int)f.fp.height,
                                                (int)f.fp.eyes, row);
                    std::fwrite(row.data(), 1, row.size(), mf);
                    std::fclose(mf);
                }
            }
        }
        bi.warp = wm;
        build_warp_params(bi, d.ring, d.warp);
        d.wpush = warp_push(bi, d.ring);

        const uint32_t period =
            d.cfg.intra_period > 0 ? (uint32_t)d.cfg.intra_period : 180u;
        for (uint32_t t = 0; t < d.ntiles; ++t) {
            const bool missing =
                t < d.force_intra.size() && d.force_intra[t] != 0;
            const bool eligible = ref_slot >= 0 && !missing &&
                                  !refresh_due(t, frame_number, period);
            if (eligible)
                set_tile_mode(d.warp, t, nxvw::kModeWarpSkip, 0, 0);
            /* The job's mode is what E3/E4/E5 read.  It starts INTRA and E1c
             * writes it; setting it here as well would make the CPU model and
             * the GPU disagree about who owns the field. */
            f.jobs[t].mode = (uint32_t)nxvw::kModeIntra;
        }

        /* warp_ext() travels only when there is a reference to warp. */
        fp.warp_bytes = ref_slot >= 0 ? (uint32_t)(36 * f.fp.eyes) : 0u;
        fp.ref_slots = 1u << (frame_number & 3u);
        /* Frame-uniform: the warp matrix is derived from ONE reference view,
         * so every inter tile of the frame predicts from the same slot and
         * carries the same `ref_sel`.  E4 writes it into word1 bits 21-22 of
         * a tile whose mode is not INTRA. */
        fp.ref_sel = ref_slot >= 0 ? (uint32_t)d.cur_ref_sel : 0u;
        /* Frame flag bit 0 is the tile-map reset -- set exactly when there is
         * no usable reference -- and bit 3 says warp_ext() is present. */
        fp.frame_flags = (fp.frame_flags & ~9u) | (ref_slot >= 0 ? 8u : 1u);

        /* The QP of THIS frame, from the frame parameter record -- not from
         * the create-time config.  nxvc_vk_encoder_set_qp() rewrites fp and
         * the job list between frames, so a decision reading d.cfg.qp would
         * gate every skip at the quantiser the stream STARTED at. */
        const int qpc = (int)fp.base_qp > 63 ? 63 : (int)fp.base_qp;
        d.decide_push[0] = (int32_t)nxe_qstep[qpc];
        d.decide_push[1] =
            d.cfg.skip_thresh > 0 ? (int32_t)d.cfg.skip_thresh : 256;
        d.decide_push[2] = d.wpred_stride;
        /* The INTRA fallback threshold, Q8.  nxvc_config::int_intra_mad_q8's
         * default is 2304 (a MAD of 9) and the harness has no knob for it
         * yet, so it is the default here too. */
        d.decide_push[3] = d.cfg.int_intra_mad_q8 > 0
                               ? (int32_t)d.cfg.int_intra_mad_q8
                               : 2304;
        /* The SAD-domain lambda, Q8: int_lambda_q8 per unit of quantiser step,
         * and qstep is Q4, so lam = int_lambda_q8 * qstep / 16. */
        const int32_t lam0 =
            d.cfg.int_lambda_q8 > 0 ? (int32_t)d.cfg.int_lambda_q8 : 45;
        d.decide_push[4] = (int32_t)(((int64_t)lam0 * nxe_qstep[qpc]) / 16);
        d.decide_push[5] = d.cfg.mv_range > 0 ? d.cfg.mv_range : 16;
        d.decide_push[6] = d.cfg.int_coded_vectors ? 1 : 0;
        d.decide_push[7] = d.ring.stride[0];
        /* ring_w is the luma plane's extent over the eye PAIR; eye_w is one
         * eye's, which is what the search clamps at -- the reference's warp
         * source is one eye's sub-picture (`ref_image()`), so an eye never
         * samples across the seam.  They coincide at eyes == 1. */
        d.decide_push[8] = d.ring.planeW[0] * (int)f.fp.eyes;
        d.decide_push[9] = (int)f.fp.height;
        d.decide_push[10] = d.ring.planeW[0];
        d.decide_push[11] = 0;

        /* Pass B's push block.  `coefStrideI16` is the lever that lets it read
         * E3's coefficient buffer with no repack: the within-tile layout is
         * already the one Pass B addresses, only the per-tile stride differs,
         * and it is a push constant. */
        d.bpush = nxvw::NxvwPassBPush{};
        d.bpush.imageW = (int)(f.fp.width * f.fp.eyes);
        d.bpush.imageH = (int)f.fp.height;
        d.bpush.tilesX = (int)(f.fp.tiles_x * f.fp.eyes);
        d.bpush.baseQp = (int)f.fp.base_qp;
        d.bpush.chromaQpOff = f.fp.chroma_qp_off;
        d.bpush.alphaQpOff = 0;
        d.bpush.coefStrideI16 = NXE_TILE_COEFS_MAX;
        d.bpush.colorTransform = (int)f.fp.ycocgr;
        d.bpush.chroma420 = d.cfg.chroma444 ? 0 : 1;
        d.bpush.alphaPresent = 0;
        d.bpush.planeWords0 = 64 * 64 / 2;
        d.bpush.planeWords1 = d.cfg.chroma444 ? 64 * 64 / 2 : 32 * 32 / 2;
        d.bpush.planeWords2 = d.bpush.planeWords1;
        d.bpush.planeWords3 = 0;
        d.bpush.intraDir = 0;
        d.bpush.dirLayer = 0;
        d.bpush.sparse = 0;

        /* Write the three frame-layout fields back into the caller's record.
         * `fp` here is a LOCAL copy -- the upload wants one and the QP may have
         * moved -- but nxe_e5_tile_offset() and the API's per-tile spans read
         * `f.fp`, so a warp_bytes left only in the copy makes every tile offset
         * 36 bytes short on an inter frame and the last tile appear to end
         * before the frame does. */
        fp.wpred_stride = (uint32_t)d.wpred_stride;
        f.fp.wpred_stride = fp.wpred_stride;
        f.fp.warp_bytes = fp.warp_bytes;
        f.fp.ref_slots = fp.ref_slots;
        f.fp.frame_flags = fp.frame_flags;
    }

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
        if (!image) {
            std::memcpy(d.b_stage_src.map, f.src_packed.data(), d.src_bytes);
            VkBufferCopy cs{0, 0, d.src_bytes};
            vkCmdCopyBuffer(cb, d.b_stage_src.buf, d.b_src.buf, 1, &cs);
        }
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
        if (d.inter) {
            /* The parameter buffer and warp_ext(), through the small staging
             * area.  The parameter buffer is 64 + 12 * ntiles uints -- 14 KB
             * for a 1088x1088 eye -- so it fits the staging window with room
             * to spare. */
            const size_t wb = d.warp.bytes();
            std::memcpy((uint8_t *)d.b_stage_small.map + (1 << 17),
                        d.warp.w.data(), wb);
            VkBufferCopy cw{1 << 17, 0, wb};
            vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_warp.buf, 1, &cw);

            uint32_t we[18];
            for (uint32_t e = 0; e < f.fp.eyes && e < 2; ++e)
                for (int i = 0; i < 9; ++i)
                    we[e * 9 + (uint32_t)i] = (uint32_t)f.warp[e][i];
            const size_t web = (size_t)9 * 4 * (f.fp.eyes < 2 ? 1 : 2);
            std::memcpy((uint8_t *)d.b_stage_small.map + (1 << 16), we, web);
            VkBufferCopy cwe{1 << 16, 0, web};
            vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_warpext.buf, 1, &cwe);

            /* Pass B's tile records are NOT built here.  They carry the
             * tile's mode, and the mode is E1c's decision, which happens on
             * the device later in this same command buffer -- so the host
             * cannot know it yet.  E1c writes the records itself, for every
             * tile including the ones it leaves INTRA.
             *
             * What the host does own is constant for the stream: the
             * weighting matrices and the workgroup-to-tile order. */
            std::vector<uint32_t> aux(512 + (size_t)d.ntiles, 0u);
            for (int set = 0; set < 4; ++set)
                for (int i = 0; i < 64; ++i) {
                    /* wm_id is 0 on every tile this pipeline codes, so sets
                     * 1..3 are never read; they are filled with the frame's
                     * matrices rather than left zero so that a stray read is a
                     * wrong picture and not a division by a zero step. */
                    aux[(size_t)set * 128 + (size_t)i] = f.fp.wm_luma[i];
                    aux[(size_t)set * 128 + 64 + (size_t)i] = f.fp.wm_chroma[i];
                }
            for (uint32_t t = 0; t < d.ntiles; ++t) aux[512 + t] = t;
            const size_t ab = aux.size() * 4;
            std::memcpy((uint8_t *)d.b_stage_small.map + (1 << 15), aux.data(),
                        ab);
            VkBufferCopy cwg{1 << 15, 0, 512 * 4};
            vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_weights.buf, 1, &cwg);
            VkBufferCopy cor{(1 << 15) + 512 * 4, 0, (size_t)d.ntiles * 4};
            vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_order.buf, 1, &cor);
        }
        d.dev.barrier_transfer_to_compute(cb);
        if (image) {
            /* E0 fills b_src from the caller's image, in the same command
             * buffer and one barrier ahead of E3.  No plane ever touches host
             * memory: this is the whole reason the entry point exists. */
            d.e0.bind(d.dev, src_y, src_c, d.b_src.buf);
            E0Geometry g{};
            /* Over the eye PAIR.  `fp.width` and `fp.tiles_x` are per eye
             * ([SYN] 3.3), but the source image is the side-by-side pair and
             * E0's `tile = ty * tiles_x + tx` is already the pair-wide linear
             * index of 3.3 -- `row * cols + eye * cols_per_eye + index` is
             * just `row * cols + col` once `tx` runs over the pair.  Passing
             * the per-eye numbers converted eye 0 only and left eye 1's tiles
             * holding whatever b_src had, which is a shorter and wrong stream.
             *
             * The clamp at `width - 1` is then the pair's right edge rather
             * than each eye's, which is safe only because a stereo picture's
             * per-eye width is a multiple of 64: no tile is partial, so no
             * fetch ever reaches the seam.  create() enforces that. */
            /* `width` is the picture in ONE LAYER: the pair's when both eyes
             * share a layer side by side, one eye's when each has its own.
             * `tiles_x` is pair-wide either way -- it indexes the output. */
            const bool layered = src_layers > 1;
            g.width = layered ? f.fp.width : f.fp.width * f.fp.eyes;
            g.height = f.fp.height;
            g.tiles_x = f.fp.tiles_x * f.fp.eyes;
            g.tiles_y = f.fp.tiles_y;
            g.eye_cols = layered ? f.fp.tiles_x : 0u;
            g.plane_y_off = (uint32_t)f.plane_base[0];
            g.plane_co_off = (uint32_t)f.plane_base[1];
            g.plane_cg_off = (uint32_t)f.plane_base[2];
            g.plane_words = (uint32_t)(f.src_packed.size() / 2);
            /* E0's own cost, on the device.  It is the only pass whose work
             * changes with the source shape -- side by side in one layer, or
             * one layer per eye -- so it is the one that has to be measured
             * before the layered path can be said to cost nothing.  Slots 8
             * and 9 of a 16-slot pool that record_passes uses 0..4 of. */
            /* Reset our own two slots: record_passes resets 0..16 but only
             * when it is asked for timestamps, which is `bench` and not a
             * normal encode, so these would otherwise be read back stale.
             *
             * Only when someone is asking, and only where the queue can
             * answer: a queue family with timestampValidBits == 0 may not be
             * written to at all, and an encode nobody is timing should record
             * exactly the commands it recorded before this pass was measured.
             */
            const bool time_e0 = nxe_time && d.qpool != VK_NULL_HANDLE &&
                                 d.dev.timestamps_valid();
            if (time_e0) {
                vkCmdResetQueryPool(cb, d.qpool, 8, 2);
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    d.qpool, 8);
            }
            d.e0.record(cb, g);
            if (time_e0) {
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    d.qpool, 9);
                d.e0_timed = true;
            }
            d.dev.barrier_compute_to_compute(cb);
        }
        if (d.inter) {
            /* Pass W, then the decision, then E3.  Two barriers: the
             * predictor has to be complete before it is measured, and the
             * modes have to be written before E3 reads them to decide whether
             * to code the tile at all. */
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_w.pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d.p_w.layout, 0, 1, &d.s_w, 0, nullptr);
            vkCmdPushConstants(cb, d.p_w.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               (uint32_t)sizeof d.wpush, &d.wpush);
            vkCmdDispatch(cb, d.ntiles, 1, 1);
            d.dev.barrier_compute_to_compute(cb);

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_dec.pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d.p_dec.layout, 0, 1, &d.s_dec, 0, nullptr);
            vkCmdPushConstants(cb, d.p_dec.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, (uint32_t)sizeof d.decide_push,
                               d.decide_push);
            vkCmdDispatch(cb, d.ntiles, 1, 1);
            d.dev.barrier_compute_to_compute(cb);

            /* Pass W again, now that the decision has published each tile's
             * mode and vector into its warp record.  The first dispatch
             * predicted every eligible tile at the skip vector so the decision
             * had something to measure; this one produces the predictor the
             * tile will actually be CODED against, which is what E3 subtracts
             * and what Pass B adds back.  A coded-vector tile predicted at the
             * skip vector would reconstruct to something the decoder does not
             * agree with. */
            if (d.cfg.int_coded_vectors) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  d.p_w.pipe);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        d.p_w.layout, 0, 1, &d.s_w, 0, nullptr);
                vkCmdPushConstants(cb, d.p_w.layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   (uint32_t)sizeof d.wpush, &d.wpush);
                vkCmdDispatch(cb, d.ntiles, 1, 1);
                d.dev.barrier_compute_to_compute(cb);
            }
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e3.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d.p_e3.layout, 0, 1, &d.s_e3, 0, nullptr);
        vkCmdDispatch(cb, d.ntiles, 1, 1);
        if (d.inter) {
            /* E3b: the reference store, and it is the DECODER'S Pass B --
             * byte-identical SPIR-V, kOutFormat kOutNone so it writes no
             * picture, kRefRingStore on so it writes the ring.  It runs over
             * EVERY tile: an intra tile is reconstructed from E3's
             * coefficients, and a skipped tile is an inter tile whose residual
             * is the zeros E3 just wrote, which is reconstruct_skip.  One pass
             * covers both, which is why there is no separate skip-store
             * kernel.
             *
             * It reads b_coef directly.  E3's per-tile layout is already the
             * one Pass B addresses -- DC levels then blocks of 64, plane p at
             * the sum of nb*nb*65 over earlier planes -- and the only
             * difference, the per-tile stride, is a push constant. */
            d.dev.barrier_compute_to_compute(cb);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_b.pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d.p_b.layout, 0, 1, &d.s_b, 0, nullptr);
            vkCmdPushConstants(cb, d.p_b.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               (uint32_t)sizeof d.bpush, &d.bpush);
            vkCmdDispatch(cb, d.ntiles, 1, 1);
        }
        d.dev.barrier_compute_to_host(cb);
        if (d.inter) {
            /* The modes back to the host.  E1c decided them on the device, and
             * the host is about to choose table sets and re-upload the job
             * array for E4/E5 -- which would put its own stale INTRA back over
             * every decision.  That is not a hypothetical: it cost a whole
             * debugging pass, because the encoder still SHRANK (a skipped
             * tile's coefficients are zeroed) while E5 wrote a row header
             * saying nothing was skipped, so the decoder faithfully rebuilt
             * intra tiles out of zero residual and the picture fell to 14 dB.
             * The host also needs the modes for its own per-tile reporting. */
            /* 0xC0000: past the job upload at 1<<19 and clear of the pose
             * blob at the top of the 1 MB staging window. */
            VkBufferCopy cjb{0, 0xC0000u,
                             (size_t)d.ntiles * sizeof(nxe_tile_job)};
            vkCmdCopyBuffer(cb, d.b_jobs.buf, d.b_stage_small.buf, 1, &cjb);
        }
        VkBufferCopy cc{0, 0, d.coef_bytes};
        vkCmdCopyBuffer(cb, d.b_coef.buf, d.b_stage_coef.buf, 1, &cc);
        if (!d.dev.submit_and_wait(cb, err)) {
            std::fprintf(stderr, "E0/E3 submit: %s\n", err.c_str());
            return false;
        }
    }
    /* The frame's reconstruction is in its ring slot now, so the slot becomes
     * a reference for the frames that follow.  Published AFTER the submit that
     * ran Pass B, never before: `resolve` is what decides whether the next
     * frame may predict at all, and a slot announced before it was written
     * would have the encoder predict from a picture that does not exist yet --
     * which the decoder, doing the same bookkeeping from the bitstream, would
     * not. */
    if (d.inter) {
        d.ringst.publish(frame_number);
        d.views.publish(frame_number);
        /* One frame's worth: the tiles just coded INTRA are tiles the client
         * can hold again.  A caller whose client is still missing them says
         * so again. */
        d.force_intra.clear();
        const nxe_tile_job *back =
            (const nxe_tile_job *)((uint8_t *)d.b_stage_small.map + 0xC0000u);
        // The mode AND the vector.  Both are E1c's output, and the host is
        // about to re-upload the job array to carry the table-set choice --
        // which puts its own copy back over anything the device decided.  The
        // mode was read back from the start; leaving the vector behind cost a
        // debugging pass in which E1c demonstrably chose mv (-1,0) and the
        // bitstream carried (0,0), because the readback was one field short.
        bool any_inter = false;
        for (uint32_t t = 0; t < d.ntiles; ++t) {
            f.jobs[t].mode = back[t].mode;
            f.jobs[t].mv = back[t].mv;
            if (back[t].mode != (uint32_t)nxvw::kModeIntra) any_inter = true;
        }
        /* What the CLIENT will hold, once this frame reaches it.  A frame
         * every tile of which came out INTRA reconstructs from nothing, so it
         * is held whatever happened to its nominal reference -- which is
         * exactly the resync frame, and getting this wrong would keep the
         * encoder in INTRA for ever after one drop.  Any other frame is held
         * only if the frame it predicted from is (nxe_inter.h, rule 2). */
        d.heldst.publish(frame_number, any_inter ? d.cur_pred_fn : -1);
    }


    auto t1 = clk::now();
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
    }

    auto t2 = clk::now();
    /* The table-set choice reads the coefficients where E3 left them: the
     * staging buffer is host-cached, and copying seven megabytes into f.coef
     * first -- which nothing else on this path reads -- cost more than the
     * choice it fed.  The `check` path above has already copied them, because
     * it compares them against the CPU model. */
    /* ENTROPY_LITE has no probability tables: the tile header's `table_set`
     * field names the VARIANT, which setup() already put on every job, and
     * running the choice here would overwrite it with a table index. */
    if (!d.entropy_lite)
        choose_table_sets(f, check ? f.coef.data()
                                   : (const int16_t *)d.b_stage_coef.map);
    /* Custom tables (tool bit 6) are trained on the histogram the choice above
     * has just built, so they cost no second walk of the coefficients.  They
     * rewrite f.tabs, f.fp.tables_present and f.fp.table_bytes, which is why
     * the parameter record and the tables are uploaded again below rather than
     * only before E3. */
    if (!d.entropy_lite) train_table_sets(f);
    fp.tables_present = f.fp.tables_present;
    fp.table_bytes = f.fp.table_bytes;
    auto t3 = clk::now();

    /* ---- the jobs go back with their table sets, then E4, E2 and E5. */
    {
        VkCommandBuffer cb = d.dev.begin();
        std::memcpy(d.b_stage_small.map, f.jobs.data(),
                    f.jobs.size() * sizeof(nxe_tile_job));
        VkBufferCopy cj{0, 0, f.jobs.size() * sizeof(nxe_tile_job)};
        vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_jobs.buf, 1, &cj);
        if (f.custom_tables) {
            copy_up(cb, d.b_stage_small, d.b_params, &fp, sizeof fp, 1 << 18);
            copy_up(cb, d.b_stage_small, d.b_tabs, &f.tabs, sizeof f.tabs,
                    (1 << 18) + 4096);
            /* The serialized table area E5 lays down between the frame header
             * and the first row header.  Whole buffer every frame: it is at
             * most NXE_TABLE_AREA_MAX bytes and a partial copy would leave the
             * previous frame's tail behind a shorter one. */
            std::vector<uint8_t> area(NXE_TABLE_AREA_MAX, 0);
            std::memcpy(area.data(), f.table_area.data(), f.table_area.size());
            copy_up(cb, d.b_stage_small, d.b_tabbytes, area.data(), area.size(),
                    (1 << 20) - 8192);
        }
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

    auto t4 = clk::now();
    uint32_t run = 0;
    for (uint32_t t = 0; t < d.ntiles; ++t) run += f.tile_bytes[t];
    const uint32_t total = nxe_e5_frame_bytes(&fp, run);
    f.out.assign(total, 0);
    std::memcpy(f.out.data(), d.b_out.map, total);

    auto t5 = clk::now();
    if (d.e0_timed) {
        /* Slots 8 and 9 only.  read_timestamps() starts at query 0 and waits,
         * and 0..7 are written only by `bench`, so asking for ten would block
         * on queries this submit never wrote. */
        uint64_t q[2] = {0, 0};
        if (vkGetQueryPoolResults(d.dev.handle(), d.qpool, 8, 2, sizeof q, q,
                                  sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT |
                                          VK_QUERY_RESULT_WAIT_BIT) ==
                    VK_SUCCESS &&
            q[1] >= q[0])
            d.e0_ms = double(q[1] - q[0]) * d.dev.timestamp_period() * 1e-6;
        d.e0_timed = false;
    }
    if (nxe_time)
        std::fprintf(stderr,
                     "nxe: E0 %.3f  passes to E3 %.2f  coef read %.2f  "
                     "table sets %.2f  E4/E5 %.2f  frame out %.2f  total %.2f ms\n",
                     d.e0_ms, ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4),
                     ms(t4, t5), ms(t0, t5));
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

bool VkEncoder::read_ring_luma(uint32_t slot, uint16_t *out, size_t count) {
    Impl &d = *p_;
    if (!d.inter || !d.ok) return false;
    const size_t need = (size_t)d.ring.stride[0] * (size_t)d.bpush.imageH;
    if (count > need) return false;
    const VkDeviceSize off =
        (VkDeviceSize)(slot & 3u) * (VkDeviceSize)d.ring.slot_u16 * 2u;
    std::string err;
    VkCommandBuffer cb = d.dev.begin();
    VkBufferCopy c{off, 0, (VkDeviceSize)count * 2u};
    vkCmdCopyBuffer(cb, d.b_ring.buf, d.b_stage_coef.buf, 1, &c);
    if (!d.dev.submit_and_wait(cb, err)) return false;
    std::memcpy(out, d.b_stage_coef.map, count * 2);
    return true;
}

void VkEncoder::set_views(const View *v, int eyes, uint32_t frame_number) {
    p_->views.set(v, eyes, frame_number);
}

void VkEncoder::set_received_tiles(const uint8_t *received, uint32_t count) {
    Impl &d = *p_;
    d.force_intra.assign(count, 0);
    for (uint32_t t = 0; t < count; ++t)
        d.force_intra[t] = received[t] ? 0u : 1u;
}

void VkEncoder::set_frame_held(uint32_t frame_number, bool held) {
    Impl &d = *p_;
    if (!d.inter) return;
    if (held)
        d.heldst.confirm(frame_number);
    else
        d.heldst.not_held(frame_number);
}

void VkEncoder::snap_stats(uint32_t &frames, uint32_t &applied) const {
    frames = p_->snap_frames;
    applied = p_->snap_frames_applied;
}

void VkEncoder::identity_stats(uint64_t &tiles, uint64_t &total) const {
    tiles = p_->identity_tiles;
    total = p_->identity_tiles_total;
}

void VkEncoder::warp_offset_stats(double &min16, double &mean16,
                                  double &max16) const {
    /* Q.6 -> sixteenths, which is the unit the threshold is written in. */
    min16 = p_->off_min_q6 < 0 ? 0.0 : (double)p_->off_min_q6 / 4.0;
    max16 = p_->off_max_q6 < 0 ? 0.0 : (double)p_->off_max_q6 / 4.0;
    mean16 = p_->off_n ? p_->off_sum_q6 / (double)p_->off_n / 4.0 : 0.0;
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
