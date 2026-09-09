/* nxe_vk.cpp -- see nxe_vk.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_vk.h"
#include "nxe_planar_host.h"

#include "nxe_atlas.h"
#include "nxe_inter.h"
#include "E1c_decide.spv.h"
#include "warp_pred.spv.h"
#include "reconstruct_v1_x8.spv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
#include "Planar_fit.spv.h"
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
    /* Pass B consumes the parser's padded 26-word representation, while E5
     * consumes the byte-packed body above.  Keep these contracts separate. */
    vkmin::Buffer b_planar_recon, b_planar_cadence;
    bool planar_cadence = false;
    bool planar_cadence_started = false;
    bool planar_wide_ring = false;
    /* The inter path: the four-slot reference ring, the parameter buffer
     * Pass W reads, and the predictor it writes.  Allocated even on an
     * intra-only stream, at four bytes each -- an unbound descriptor is
     * illegal and a branch in create() is worse than 12 bytes. */
    vkmin::Buffer b_ring, b_warp, b_wpred;
    /* Effort 2 only.  `b_rate` is the trellis's Q10 rate model, uploaded per
     * pass; `b_trelf`/`b_trelm` are its per-tile forward-pass scratch -- nine
     * units (the DC plane and eight block entries) of 64 positions of four
     * states, which is 24 kB a tile and belongs in a buffer rather than in the
     * shared memory the directional predictor already fills. */
    vkmin::Buffer b_rate, b_trelf, b_trelm;
    bool trellis = false;
    /* Pass B's consumer buffers.  The COEFFICIENTS are not among them: Pass B
     * strides tiles by its `coefStrideI16` push constant, and E3's per-tile
     * layout is already the one it wants -- DC levels then blocks of 64, and
     * plane p at the sum of nb*nb*65 over earlier planes, which is exactly
     * nxvw_plane_coef_count.  So b_coef is bound straight through with the
     * stride set to NXE_TILE_COEFS_MAX, and there is no repack pass. */
    vkmin::Buffer b_tilerecs, b_weights, b_order, b_dummy;
    /* [ATLAS] the compacted coded-tile order E1c builds, and the
     * indirect dispatch that walks it. */
    vkmin::Buffer b_order_coded, b_indirect;
    std::vector<vkmin::Image> ph;   /* 1x1 placeholders, Pass B's 7 images */
    vkmin::Buffer b_stage_src, b_stage_coef, b_stage_small;

    vkmin::Pipeline p_e3, p_planar_fit, p_e4, p_e5, p_e5z, p_e2[3];
    VkDescriptorSet s_planar_fit{};
    /* E4-lite (ENTROPY_LITE, 30) and the E5 variant that reads its payload
     * layout.  Both are created only when the stream carries the tool: they
     * share E4's and E5's descriptor set layouts, so what a Lite frame
     * changes is which pipeline is bound and nothing else. */
    vkmin::Pipeline p_e4l, p_e5l;
    bool entropy_lite = false;
    bool gpu_planar = false;
    bool gpu_planar_centre = false;
    /* Pass W is the DECODER's warp_pred.comp; E1c is the mode decision. */
    vkmin::Pipeline p_w, p_dec, p_b;
    VkDescriptorSet s_b_full = VK_NULL_HANDLE;   /* Pass B over EVERY tile */
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
    /* GPU-timestamp accounting, per frame, gated on NXE_TIME.  Wall clock
     * around a submit is not the encoder's cost: it carries the submit itself,
     * the fence wait, every host copy on either side and whatever else is
     * running on the box.  These three are the device's own execution of the
     * command buffers, read from the queue's timestamps. */
    double gpu_ms_1 = 0.0, gpu_ms_2 = 0.0, gpu_ms_asm = 0.0;
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
    /* Opt-in diagnosis of the first ATLAS admission gate per eye. */
    bool admission_stats = false;
    bool optimistic_atlas = false;
    uint64_t admission[2][8] = {};
    uint64_t optimistic_bypassed[2] = {};
    uint32_t admission_frames = 0;
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

    /* ---------------------------------------------------- ATLAS (bit 31)
     *
     * The encoder's shadow of [SYN] 13.12.  `atlas_tab` is the per-tile table
     * the decoder derives independently from warp_ext() and the skip map, so
     * every rule that touches it is normative even though the object is
     * encoder-side.  The PIXELS are ring slot 0; slot 1 is their one-deep
     * undo, and there are no other slots -- the ring shrinks from four to two,
     * which is a reduction even before the model's own saving.
     *
     * Why a WHOLE-picture copy into slot 1 each frame is the right one-deep
     * undo, rather than a copy of the coded tiles: a tile's atlas pixels
     * change only when the tile is CODED, so a full copy taken before every
     * Pass B leaves each tile position holding exactly the pixels it had
     * before its own most recent coded write-back.  That is one-deep per TILE,
     * which is what ADR-0029 section 7 asks for, and it costs one buffer copy
     * on the encoder's PC rather than a device-side compaction of a list the
     * host does not learn until after the submit. */
    bool atlas = false;
    AtlasGeom atlas_geom{};
    AtlasTable atlas_tab{};
    AtlasUndo atlas_undo{};
    /* Which tiles each of the last kDepth frames CODED, so a negative receipt
     * naming a frame can name its tiles.  The decoder needs no such record --
     * it simply does not update what it did not receive -- and neither would a
     * transport that reported tiles rather than frames; this exists because
     * nxvc_vk_encoder_set_frame_held() reports a FRAME. */
    struct CodedFrame {
        uint32_t frame = 0;
        uint8_t used = 0;
        std::vector<uint32_t> tiles;
    };
    CodedFrame atlas_coded[AtlasUndo::kDepth];
    uint32_t atlas_now = 0;         /* the newest frame the shadow has seen */
    /* Tile positions a receipt has invalidated or restored since the last
     * frame, and whether their PIXELS need restoring from slot 1.  Applied at
     * the head of the next encode, because a receipt arrives between frames
     * and the restore is a device copy. */
    std::vector<uint32_t> atlas_restore;
    /* [SYN] 13.12.11: is THIS frame a PICTURE frame?  Decided once per frame,
     * before the advance, and read by every site that behaves differently --
     * the advance itself, Pass B's dispatch shape, the undo snapshot and the
     * table write-back.  A per-frame bool rather than a parameter because
     * those sites are spread across encode_frame_common() and all of them
     * must agree. */
    bool picture_frame = false;
    /* The ring slot the assembled reference goes in.  Under ATLAS slot 0 IS
     * the atlas and slot 1 holds the one-deep pixel undo snapshot, so 2 is the
     * first free one.  The reconstruction still lands in slot 0, because on a
     * PICTURE frame the reconstruction BECOMES the atlas. */
    static const uint32_t kAssembleSlot = 2u;
    /* The geometry the assembly rebuilds its own parameters from, saved when
     * the mode is decided because the decision and the assembly happen at
     * different points of the frame: the decision must precede the advance,
     * and the assembly must follow Pass B's push block being built. */
    WarpBuildInfo picture_bi{};
    std::vector<uint8_t> picture_modes;
    /* Frames since each position last coded an INTRA, for the AGE form of the
     * hard cap (`drift_refresh`).  A position that has never coded one starts
     * at 0 -- frame 0 is all-INTRA, which is exactly that. */
    std::vector<uint32_t> age_since_intra;
    /* The last frame's report (nxvc_vk_encoder_frame_report).  Reporting only:
     * every field is read off state the encode already produced. */
    nxvc_vke_frame_report last_report{};
    /* How many tiles 13.12.6 forced into the skip range over the clip, because
     * a coded tile there would have been dropped as superseded. */
    uint64_t atlas_superseded = 0;
    /* Base-sourced patches queued by atlas_write_tiles(), applied at the top of
     * the next encode -- in the SAME command buffer as the rollback restore
     * and one barrier ahead of the E-stages, which is the ordering the caller
     * needs and cannot arrange itself: the write must land after the previous
     * frame's Pass B and before this frame's Pass W reads the atlas.
     *
     * Queued rather than submitted on the spot because a submit per call would
     * serialise the caller against the GPU once per patch run, and the whole
     * point of the entry point is that a patch costs 1.9 us. */
    struct BaseWrite {
        VkBuffer src = VK_NULL_HANDLE;
        VkDeviceSize src_offset = 0;
        uint32_t first_tile = 0, count = 0, src_frame = 0;
    };
    std::vector<BaseWrite> atlas_base_writes;
    /* Cheat 3's scratch: which tiles the staggered rule offered this frame,
     * and which of them the cap kept.  Members rather than locals so the
     * allocation does not recur per frame. */
    std::vector<uint8_t> refresh_cand, refresh_pick;
    /* How many skips the displacement bound refused, over the clip.  Reporting
     * only -- it is the price of the rule, and a rule whose price is not
     * measured is a rule nobody can decide about. */
    uint64_t disp_forced = 0;
    /* The head's angular speed between this frame's view and the previous
     * one, in Q8 radians, for the motion-scaled skip threshold of Cheats 5.
     * Derived from the pose stream the encoder already receives; zero when
     * there is no pose input, which is what makes the cheat inert on every
     * fixture that does not drive one. */
    int atlas_motion_q8 = 0;
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

/* A memory barrier between a compute write and a transfer read, and back.
 * vk_min has the three the coding passes needed and this is the fourth: the
 * ATLAS undo snapshot is a buffer copy sitting between two dispatches. */
static void barrier_compute_transfer(VkCommandBuffer cb, bool compute_first) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = compute_first ? VK_ACCESS_SHADER_WRITE_BIT
                                     : VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = compute_first ? VK_ACCESS_TRANSFER_READ_BIT
                                     : (VK_ACCESS_SHADER_READ_BIT |
                                        VK_ACCESS_SHADER_WRITE_BIT);
    const VkPipelineStageFlags src = compute_first
                                         ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                         : VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags dst = compute_first
                                         ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                         : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

/* The byte regions one tile position occupies in a ring slot, over every coded
 * plane -- the rect a rollback restores.  A tile is `full` samples square in
 * its plane (64 luma, 32 chroma of a 4:2:0 stream) and its rows are strided,
 * so it is one region per row rather than one per tile; the edge tiles of a
 * picture that is not a multiple of 64 are clipped to the plane, because the
 * samples past the edge belong to the next eye or to nothing. */
static void atlas_tile_regions(const RingLayout &rl, int eyes, int cols_per_eye,
                               int chroma420, int height, uint32_t tile,
                               uint32_t src_slot, uint32_t dst_slot,
                               std::vector<VkBufferCopy> &out) {
    const int cols = cols_per_eye * eyes;
    const int row = (int)(tile / (uint32_t)cols);
    const int rem = (int)(tile % (uint32_t)cols);
    const int eye = rem / cols_per_eye;
    const int col = rem % cols_per_eye;
    const VkDeviceSize sbase = (VkDeviceSize)src_slot * (VkDeviceSize)rl.slot_u16;
    const VkDeviceSize dbase = (VkDeviceSize)dst_slot * (VkDeviceSize)rl.slot_u16;
    for (int p = 0; p < rl.nplanes; ++p) {
        const int full = nxvw::nxvw_inter_plane_full(p, chroma420);
        const int ph = (p == 1 || p == 2) && chroma420 ? (height + 1) / 2 : height;
        const int x0 = eye * rl.planeW[p] + col * full;
        const int y0 = row * full;
        int w = full, h = full;
        if (col * full + w > rl.planeW[p]) w = rl.planeW[p] - col * full;
        if (y0 + h > ph) h = ph - y0;
        if (w <= 0 || h <= 0) continue;
        for (int y = 0; y < h; ++y) {
            const VkDeviceSize off =
                (VkDeviceSize)rl.off[p] + (VkDeviceSize)(y0 + y) * rl.stride[p] +
                (VkDeviceSize)x0;
            VkBufferCopy c{};
            c.srcOffset = (sbase + off) * 2u;
            c.dstOffset = (dbase + off) * 2u;
            c.size = (VkDeviceSize)w * 2u;
            out.push_back(c);
        }
    }
}

static const VkBufferUsageFlags kDevUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
    /* [ATLAS] Pass B is dispatched INDIRECTLY over the coded-tile count E1c
     * produces, so one buffer in this set is a dispatch argument.  They are
     * all created with one usage mask, and the bit is free on the rest. */
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

VkEncoder::VkEncoder() : p_(new Impl)
{
    p_->admission_stats = std::getenv("NXVC_VKE_ATLAS_ADMISSION_STATS") != nullptr;
    const char *optimistic = std::getenv("NXVC_VKE_ATLAS_OPTIMISTIC");
    p_->optimistic_atlas = optimistic && optimistic[0] == '1';
    if (p_->optimistic_atlas)
        std::fprintf(stderr, "nxvc atlas optimistic admission enabled (after first confirmation)\n");
}
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
    d.gpu_planar = cfg.planar && (cfg.planar_gpu_flat || cfg.planar_gpu_centre ||
        std::getenv("NXVC_ENC_PLANAR_GPU_FLAT") != nullptr);
    d.gpu_planar_centre = d.gpu_planar && cfg.planar_gpu_centre;
    const char *wide_ring = std::getenv("NXVC_PLANAR_WIDE_RING");
    d.planar_wide_ring = wide_ring && std::strcmp(wide_ring, "1") == 0;
    const char *cadence = std::getenv("NXVC_PLANAR_CADENCE");
    d.planar_cadence = cadence && std::strcmp(cadence, "1") == 0;
    if (d.planar_cadence && (!d.gpu_planar_centre || !cfg.planar_graduated || cfg.atlas)) {
        err = "PLANAR cadence requires independent graduated GPU PLANAR centre mode";
        return false;
    }
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
    /* [SYN] 2: ATLAS requires INTER.  Refused rather than downgraded, because
     * an ATLAS stream with no temporal reference is a contradiction and not a
     * degraded configuration. */
    if (cfg.atlas && !cfg.inter) {
        err = "atlas needs inter ([SYN] 2: tool bit 31 requires bit 10)";
        d.dev.destroy();
        return false;
    }
    /* [SYN] 4.1 / 13.12.6: ref_sel SHALL be 0 in every tile header of an
     * ATLAS stream.  The atlas holds ONE generation per tile position, so
     * there is no older reference to select and the walk of select_reference()
     * has nothing to walk.  A configuration asking for both is refused, not
     * resolved: silently coding at ref_sel 0 would produce a stream the caller
     * did not ask for, and silently keeping the ring would produce one the
     * decoder must reject. */
    if (cfg.atlas && cfg.ref_sel != 0) {
        err = "atlas forces ref_sel to 0 ([SYN] 13.12.6); ref_sel is not 0";
        d.dev.destroy();
        return false;
    }
    d.atlas = cfg.atlas;
    /* A caller that says its client confirms gets confirmations REQUIRED from
     * frame 0, which is what removes the startup window in which the encoder
     * would otherwise still be guessing. */
    d.heldst.require_confirmed = cfg.inter && cfg.ref_confirm;
    /* The tile geometry is filled for every inter stream, not only an atlas
     * one: the display helper needs it to re-tile a reconstruction as well as
     * to warp an atlas. */
    if (d.inter) {
        d.atlas_geom.width = cfg.w / cfg.eyes;
        d.atlas_geom.height = cfg.h;
        d.atlas_geom.cols_per_eye = (int)f.fp.tiles_x;
        d.atlas_geom.rows = (int)f.fp.tiles_y;
        d.atlas_geom.eyes = cfg.eyes;
        /* The atlas is one table over BOTH eyes, keyed by the row-major
         * eye-minor tile index of [SYN] 3.3 -- rows interleave the eyes, so
         * eye = (n % cols) / cols_per_eye -- and each eye's tiles compose with
         * their own eye's warp_ext().  That is not the same thing as the
         * STEREO tool, which ATLAS excludes: STEREO predicts one eye from the
         * other WITHIN a frame, and nothing here does.
         *
         * The guard is not decoration.  `fp.tiles_x` and `fp.width` are per
         * eye and `fp.ntiles` is over the pair, and getting that backwards
         * would size the table to one eye and index it with pair-wide indices
         * -- which is a wrong matrix per tile rather than a crash. */
        if (d.atlas_geom.ntiles() != d.ntiles) {
            err = "atlas geometry disagrees with the tile count";
            d.dev.destroy();
            return false;
        }
        if (d.atlas) {
            d.atlas_tab.reset(d.atlas_geom);
            d.atlas_undo.reset(d.atlas_geom);
        }
    }
    if (d.inter) {
        const int cw = cfg.chroma444 ? cfg.w / cfg.eyes : (cfg.w / cfg.eyes + 1) / 2;
        const int ch = cfg.chroma444 ? cfg.h : (cfg.h + 1) / 2;
        ring_layout(cfg.w / cfg.eyes, cfg.h, cw, ch, cfg.eyes, 3, d.ring);
        d.wpred_stride = wpred_stride_i16(cfg.chroma444 ? 0 : 1, 0);
    }
    /* Four slots without ATLAS, TWO with it: the atlas at slot 0 and its
     * one-deep pixel undo at slot 1.  The ADR's memory claim is a reduction
     * and this is where it lands on the encoder as well as the decoder. */
    /* [SYN] 13.12.11 adds a THIRD slot when the per-frame mode is on: slot 2
     * holds the picture a PICTURE frame's step 1 assembles, which the frame
     * then predicts from while writing its reconstruction into the atlas at
     * slot 0.  Two slots is the ATLAS-only figure and staying at it silently
     * put the assembled picture past the end of the buffer -- which reads back
     * as an unusable reference and codes every tile INTRA, with no error
     * anywhere. */
    const size_t atlas_slots = d.cfg.atlas_mode ? 3u : 2u;
    const size_t ring_bytes =
        d.inter ? (d.atlas ? (size_t)d.ring.slot_u16 * 2u * atlas_slots
                           : d.ring.bytes())
                : 4u;
    const size_t wpred_b =
        d.inter ? wpred_bytes(d.ntiles, cfg.chroma444 ? 0 : 1, 0) : 4u;
    /* With ATLAS the buffer carries a matrix PAIR per tile after the tile
     * records, which is where each tile's composed C lives. */
    const size_t warp_b =
        d.inter ? warp_params_uints(d.ntiles, d.atlas ? 1 : 0) * 4u : 4u;

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
        {&d.b_planar_cadence, d.planar_cadence ? size_t(std::max(d.ntiles, 1u)) * 64u * 4u : 4u, true},
        {&d.b_planar_recon,
         d.cfg.planar ? (size_t)std::max(d.ntiles, 1u) * 26u * 4u : 4u, true},
        {&d.b_ring,    ring_bytes, false},
        {&d.b_warp,    warp_b,     false},
        {&d.b_wpred,   wpred_b,    false},
        {&d.b_tilerecs, (size_t)std::max(d.ntiles, 1u) * 16, false},
        {&d.b_weights,  512 * 4,    false},
        {&d.b_order,    (size_t)std::max(d.ntiles, 1u) * 4, false},
        {&d.b_order_coded, (size_t)std::max(d.ntiles, 1u) * 4, false},
        {&d.b_indirect, 16,         false},
        {&d.b_dummy,    4096,       false},
        {&d.b_out,     d.out_bytes, true},
        {&d.b_stage_src, d.src_bytes, true},
        {&d.b_stage_coef, d.coef_bytes, true},
        /* The ordinary uploads use fixed offsets below 1 MiB.  Atlas picture
         * assembly also uploads its per-tile matrix records and can be much
         * larger than the ordinary warp header, so reserve that tail from the
         * actual frame sizes instead of imposing an unrelated 128 KiB cap. */
        {&d.b_stage_small,
         std::max({(size_t)1 << 20,
                   (size_t)0xC0000u +
                       (size_t)d.ntiles * sizeof(nxe_tile_job),
                   ((size_t)1 << 20) + warp_b +
                       (size_t)d.ntiles * 4u * sizeof(uint32_t)}),
         true},
        {&d.b_rate,    (8 * 32 * 16 + 8) * 4, true},
        {&d.b_trelf,   (size_t)d.ntiles * 9 * 64 * 4 * 8, false},
        {&d.b_trelm,   (size_t)d.ntiles * 9 * 64 * 4 * 4, false},
    };
    for (auto &m : mk)
        if (!d.dev.create_buffer(m.size, kDevUsage, m.host, *m.b, err))
            return false;

    if (d.planar_cadence)
        std::memset(d.b_planar_cadence.map, 0, size_t(std::max(d.ntiles, 1u)) * 64u * 4u);

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
    const std::vector<VkDescriptorType> sb6e(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    if (!d.dev.create_pipeline(E3_forward_spv, sizeof E3_forward_spv, sb6e, 0,
                               d.p_e3, err, &si))
        return false;
    if (d.gpu_planar && !d.dev.create_pipeline(Planar_fit_spv, sizeof Planar_fit_spv,
                               std::vector<VkDescriptorType>(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                               24, d.p_planar_fit, err))
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
    d.trellis = cfg.trellis != 0;
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
    /* Ten now: eight, plus the compacted coded-tile order and its indirect
     * dispatch argument ([ATLAS], E1c_decide.comp). */
    const std::vector<VkDescriptorType> sb8d(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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
        /* Pass B's planar body is binding 16 even when the current stream has
         * no PLANAR tiles; the shader interface is fixed by specialization. */
        std::vector<VkDescriptorType> bb(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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
    if (d.gpu_planar) d.s_planar_fit = d.dev.allocate_set(d.pool, d.p_planar_fit.dsl);
    d.s_e4 = d.dev.allocate_set(d.pool, d.p_e4.dsl);
    d.s_e5 = d.dev.allocate_set(d.pool, d.p_e5.dsl);
    d.s_e5z = d.dev.allocate_set(d.pool, d.p_e5z.dsl);
    for (int i = 0; i < 3; ++i)
        d.s_e2[i] = d.dev.allocate_set(d.pool, d.p_e2[i].dsl);

    VkDevice h = d.dev.handle();
    write_set(h, d.s_e3, {d.b_params.buf, d.b_jobs.buf, d.b_src.buf,
                          d.b_coef.buf, d.b_modes.buf, d.b_wpred.buf,
                          d.b_rate.buf, d.b_trelf.buf, d.b_trelm.buf});
    if (d.gpu_planar) write_set(h, d.s_planar_fit, {d.b_src.buf, d.b_planar.buf,
                                                    d.b_jobs.buf, d.b_planar_recon.buf, d.b_planar_cadence.buf});
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
                           d.b_ring.buf, d.b_warp.buf, d.b_order_coded.buf,
                           d.b_indirect.buf});

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
                         d.b_dummy.buf,
                         /* [ATLAS] Pass B walks the CODED tiles only; every
                          * other configuration walks them all.  Pass W keeps
                          * the full order: it must predict every eligible tile
                          * so the decision has something to measure. */
                         d.atlas ? d.b_order_coded.buf : d.b_order.buf,
                         d.b_dummy.buf,
                         N, N, N,
                         d.b_wpred.buf, d.b_ring.buf, d.b_warp.buf,
                         d.b_planar_recon.buf},
                        views);
        /* [SYN] 13.12.11 needs a SECOND Pass B binding: a PICTURE frame
         * reconstructs every tile, and the assembly of step 1 materialises
         * every position, but the set above is bound to the CODED list for
         * the whole life of an ATLAS encoder.  The order buffer is a
         * descriptor, not a push constant, so the choice cannot be made per
         * dispatch -- hence a second set that differs in exactly that one
         * binding.  Allocated only when the mode tool is on, so an ordinary
         * ATLAS encoder is unchanged. */
        if (d.atlas && d.cfg.atlas_mode) {
            d.s_b_full = d.dev.allocate_set(d.pool, d.p_b.dsl);
            write_set_mixed(h, d.s_b_full,
                            {d.b_coef.buf, d.b_tilerecs.buf, d.b_weights.buf,
                             N, N, N, N,
                             d.b_dummy.buf,
                             d.b_order.buf,
                             d.b_dummy.buf,
                             N, N, N,
                             d.b_wpred.buf, d.b_ring.buf, d.b_warp.buf,
                             d.b_planar_recon.buf},
                            views);
        }
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
/* Bracket a command buffer with a pair of timestamps, and read the pair back
 * as milliseconds of DEVICE time.  `first` is the query index of the pair.
 *
 * TOP_OF_PIPE at the open and BOTTOM_OF_PIPE at the close, so the interval is
 * everything the queue executed for this buffer and nothing the host did
 * around it. */
static void gpu_ts_begin(VkEncoder::Impl &d, VkCommandBuffer cb, uint32_t first) {
    if (d.qpool == VK_NULL_HANDLE || !d.dev.timestamps_valid()) return;
    vkCmdResetQueryPool(cb, d.qpool, first, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, d.qpool, first);
}
static void gpu_ts_end(VkEncoder::Impl &d, VkCommandBuffer cb, uint32_t first) {
    if (d.qpool == VK_NULL_HANDLE || !d.dev.timestamps_valid()) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool,
                        first + 1u);
}
static double gpu_ts_read(VkEncoder::Impl &d, uint32_t first) {
    if (d.qpool == VK_NULL_HANDLE || !d.dev.timestamps_valid()) return 0.0;
    uint64_t q[2] = {0, 0};
    if (vkGetQueryPoolResults(d.dev.handle(), d.qpool, first, 2, sizeof q, q,
                              sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return 0.0;
    return (double)(q[1] - q[0]) * (double)d.dev.timestamp_period() / 1e6;
}

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
    bool gpu_planar = d.gpu_planar && d.inter && f.fp.chroma420 && !f.fp.ycocgr &&
                      f.fp.width % 64u == 0 && f.fp.height % 64u == 0;
    for (const auto &job : f.jobs)
        if (job.res_level != 0 || job.chroma444 != 0)
            gpu_planar = false;
    /* GPU-flat emits a complete independent PLANAR frame.  Keep normal inter
     * metadata bookkeeping, but atlas still needs reference reconstruction. */
    const bool independent_gpu_planar = gpu_planar && !d.atlas;
    /* Once GPU-flat is selected, every frame must stay independent PLANAR;
     * falling back to a generic frame would leave the skipped ring stale. */
    if (d.gpu_planar && !gpu_planar) {
        err = "GPU PLANAR requires aligned full-resolution 4:2:0 tiles";
        return false;
    }
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
    /* frame_params is reused between encodes; bit 5 is the per-frame PICTURE
     * declaration and must not survive an ATLAS decision on the next frame. */
    fp.frame_flags &= ~32u;

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
        if (d.atlas) {
            /* [SYN] 13.12.6.  There is no reference SELECTION under ATLAS:
             * `ref_sel` is 0 in every tile header and the atlas holds one
             * generation per tile position.  What replaces the walk is
             * per-tile validity -- a tile whose own atlas entry is invalid
             * must be INTRA, and every other tile predicts -- so the only
             * frame-level question left is whether ANY entry is valid, which
             * is the difference between an ordinary frame and a
             * tile_map_reset one.
             *
             * Slot 0 is the atlas.  It is not `frame_number & 3` and it never
             * moves: the generations the ring used to carry are what the
             * per-tile `src_frame` now carries instead. */
            bool any_valid = false;
            for (uint32_t t = 0; t < d.ntiles && !any_valid; ++t)
                if (d.atlas_tab.valid(t)) any_valid = true;
            ref_slot = any_valid ? 0 : -1;
            /* The frame this one predicts from is always its immediate
             * predecessor, whatever any tile's source frame is: warp_ext() is
             * the step from N to N-1 and the chain back to a tile's own source
             * is the COMPOSITION, not a different matrix.  The held record
             * still wants a predecessor, and this is it. */
            d.cur_pred_fn = ref_slot >= 0 ? (int64_t)frame_number - 1 : -1;
        } else {
            if (!select_reference(d.ringst, d.heldst, frame_number,
                                  d.cfg.ref_sel, &d.cur_ref_sel, &ref_slot))
                ref_slot = -1;
            d.cur_pred_fn =
                ref_slot >= 0
                    ? (int64_t)frame_number - 1 - (int64_t)d.cur_ref_sel
                    : -1;
        }
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
        bi.atlas = d.atlas ? 1 : 0;
        /* warp_ext(), from the view that went with the reference slot and the
         * view of this frame.  With no pose input the view history is all
         * identity and so is the matrix, which predicts a still picture
         * correctly and a turning head badly -- and the mode decision then
         * declines to skip, which is the right failure. */
        /* Which VIEW the matrix is derived against.  Without ATLAS it is the
         * view that went with the ring slot this frame predicts from, because
         * that is the picture being warped.  With ATLAS the picture being
         * warped is the atlas, whose tiles are at assorted poses, and the
         * matrix is the ONE-STEP delta from this frame to its predecessor --
         * so the view is frame N-1's, and the per-tile chain back to each
         * tile's own source pose is built by composition instead.  Deriving
         * it against a tile's source view would be the same mistake in the
         * other direction: a confident prediction of the wrong place. */
        const int warp_view_slot =
            d.atlas ? (ref_slot >= 0 && frame_number > 0
                           ? (int)((frame_number - 1u) & 3u)
                           : -1)
                    : ref_slot;
        WarpMatrix wm[2];
        for (int e = 0; e < 2; ++e) {
            wm[e] = derive_warp(d.views, warp_view_slot, e, (int)f.fp.width,
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

        /* [SYN] 13.12.3 step 1, the advance, and it happens HERE: after the
         * frame's matrix is derived and before a single tile is decided,
         * because step 2's prediction reads the table AFTER step 1.  Getting
         * the order wrong would predict every tile through a matrix one frame
         * stale, which is a wrong picture and not an error. */
        d.picture_frame = false;
        if (d.atlas) {
            int32_t Hm[2][9];
            for (int e = 0; e < 2; ++e)
                for (int i = 0; i < 9; ++i) Hm[e][i] = wm[e].h[i];

            /* [SYN] 13.12.11.1, the MODE decision, and it happens HERE for the
             * reason the clause gives: the metric includes this frame's
             * advance, so `warp_ext()` must already be derived -- it is, just
             * above -- and the advance must not have been applied yet, because
             * the probe applies it to a copy.  A frame with no reference is
             * always an ATLAS frame: there is no atlas to assemble. */
            if (d.cfg.atlas_mode && ref_slot >= 0) {
                const double worst = atlas_worst_disp_after(
                    d.atlas_tab, Hm, bi.width, bi.height);
                d.picture_frame = worst > (double)d.cfg.atlas_picture_d;
                /* The trigger's own number, in sixteenths of a sample, so a
                 * caller can see how close the frame came to switching. */
                d.last_report.worst_disp_q4 =
                    (uint32_t)(worst * 16.0 + 0.5);
                if (std::getenv("NXE_MODE_TRACE"))
                    std::fprintf(stderr,
                                 "[mode] frame %u worst=%.3f D=%d -> %s "
                                 "(ref_slot=%d)\n",
                                 frame_number, worst, d.cfg.atlas_picture_d,
                                 d.picture_frame ? "PICTURE" : "ATLAS",
                                 ref_slot);
            }

            /* The undo log is told about the frame either way, but a PICTURE
             * frame ADVANCES NOTHING (13.12.11 step 1: "the C advance of
             * 13.12.3 step 1 does not run"), so it contributes no step -- and
             * it is a generation boundary, because it overwrites every atlas
             * pixel. */
            d.atlas_undo.note_frame(frame_number, Hm,
                                    ref_slot >= 0 && !d.picture_frame);
            if (ref_slot >= 0 && !d.picture_frame) d.atlas_tab.advance(Hm);
            if (d.picture_frame) d.atlas_undo.note_materialised(frame_number);
            d.atlas_now = frame_number;
            /* Pass B stores into the slot the warp header names, and under
             * ATLAS that is always slot 0 -- the atlas -- rather than
             * `frame_number & 3`.  build_warp_params() wrote the ring rule; it
             * is overridden here rather than parameterised, so the non-atlas
             * path keeps exactly the words it has always had. */
            d.warp.w[(size_t)NXVW_WARP_HDR_RING + 3] = 0u;
            if (d.picture_frame) {
                /* [SYN] 13.12.11 step 2: the frame is coded by the ORDINARY
                 * non-ATLAS process against ONE assembled picture.  So the
                 * frame body wants exactly the parameters a non-atlas frame
                 * has -- the frame-wide matrix, no per-tile `mat_idx`, and a
                 * reference slot that is a picture rather than the atlas --
                 * and NOT the per-tile matrices built below.
                 *
                 * The destination stays slot 0, because step 3 makes this
                 * frame's reconstruction the atlas.  So the frame reads
                 * `kAssembleSlot` and writes the atlas, which is why the two
                 * must be different slots. */
                WarpBuildInfo pbi = bi;
                pbi.atlas = 0;
                pbi.ref_slot = (int)Impl::kAssembleSlot;
                build_warp_params(pbi, d.ring, d.warp);
                d.warp.w[(size_t)NXVW_WARP_HDR_RING + 3] = 0u;
                d.wpush = warp_push(pbi, d.ring);
                /* Frame flags bit 5 ([SYN] 13.12.11): the decoder is told the
                 * mode and needs no policy of its own. */
                fp.frame_flags |= 32u;
                f.fp.frame_flags |= 32u;
                d.picture_bi = bi;
            } else {
                /* Each tile's own composed C, conjugated for both
                 * subsamplings, into the area its `mat_idx` names.  AFTER the
                 * advance: [SYN] 13.12.4 says a coded tile reads the matrix of
                 * its own atlas entry "read after step 1", so building these
                 * before the advance would predict every tile one frame
                 * stale. */
                atlas_build_matrices(d.atlas_tab, bi.width, bi.height, bi.cw,
                                     bi.ch, d.warp);
            }
        }

        const uint32_t period =
            d.cfg.intra_period > 0 ? (uint32_t)d.cfg.intra_period : 180u;

        /* Cheat 3, off unless `atlas_refresh_cap` is set.  The rolling refresh
         * re-codes a staggered fraction of tiles every frame regardless of
         * where the eye is or how stale the tile is; with a cap, the
         * candidates are ordered by fovea distance plus age and only the
         * urgent ones are coded this frame.
         *
         * Built BEFORE the per-tile loop because a cap is a property of the
         * whole frame's candidate set: deciding tile by tile could not know
         * how many better candidates were still to come. */
        const bool cheat3 = d.atlas && d.cfg.atlas_refresh_cap > 0;
        if (cheat3) {
            d.refresh_cand.assign(d.ntiles, 0u);
            d.refresh_pick.assign(d.ntiles, 0u);
            for (uint32_t t = 0; t < d.ntiles; ++t)
                if (refresh_due(t, frame_number, period)) d.refresh_cand[t] = 1u;
            /* Fixed foveation: the eye's centre in tile units.  A headset
             * without eye tracking has exactly this and no more, and wiring a
             * tracker in here would be inventing an input the encoder is not
             * given. */
            atlas_refresh_priority(
                d.atlas_tab, d.refresh_cand.data(), d.ntiles, frame_number,
                (uint32_t)d.cfg.atlas_refresh_cap,
                (double)(d.atlas_geom.cols_per_eye - 1) * 0.5,
                (double)(d.atlas_geom.rows - 1) * 0.5, d.refresh_pick.data());
        }

        for (uint32_t t = 0; t < d.ntiles; ++t) {
            const bool missing =
                t < d.force_intra.size() && d.force_intra[t] != 0;
            /* With Cheat 3 the refresh question is answered by the frame-wide
             * pick above; without it, by the staggered rule alone. */
            /* ENCODER-DECISION.md section 2 step 1.  Two forms of the same
             * cap: the AGE rule that `nxv-enc` defaults to, and the STAGGERED
             * rule this encoder has always used.  Cheat 3's pick overrides
             * both when it is on. */
            const bool due =
                cheat3 ? d.refresh_pick[t] != 0
                       : (d.cfg.drift_refresh
                              ? (t < d.age_since_intra.size() &&
                                 d.age_since_intra[t] >= period)
                              : refresh_due(t, frame_number, period));
            bool eligible = ref_slot >= 0 && !missing && !due;
            const unsigned admission_eye = (d.admission_stats && d.atlas)
                                                ? d.atlas_geom.eye_of(t) : 0;
            auto admission_reject = [&](unsigned gate) {
                if (d.admission_stats && admission_eye < 2) ++d.admission[admission_eye][gate];
            };
            if (d.admission_stats && d.atlas) {
                if (ref_slot < 0) admission_reject(6); /* no reference */
                else if (missing) admission_reject(5); /* forced refresh */
                else if (due) admission_reject(3); /* refresh due */
            }
            /* Under ATLAS eligibility is PER TILE and it is the whole of the
             * loss story ([SYN] 13.12.4: a tile with mode != INTRA whose own
             * atlas entry is invalid is BITSTREAM).  Three things can make an
             * entry unusable, and they are different questions:
             *
             *   * `valid == 0` -- the composition left the envelope, or a
             *     receipt rolled the entry back past the history.  The
             *     envelope check IS the staleness bound; there is no separate
             *     "too old" rule.
             *   * the client has not CONFIRMED the generation.  Positive acks
             *     keep their meaning under ATLAS, per tile rather than per
             *     frame: the question is whether the client holds the frame
             *     that last CODED this position, which is `src_frame`.
             *   * `src_frame` has aged out of the held history.  That is
             *     treated as HELD, and the direction matters: the history is
             *     deeper than the round trip the transport is designed for, so
             *     a negative report for a frame that old would have arrived
             *     long ago.  Treating it as unheld instead would force INTRA
             *     on precisely the long-lived tiles the atlas exists to keep,
             *     which is the model's whole benefit thrown away to re-answer
             *     a question that has already been answered. */
            if (eligible && d.atlas) {
                eligible = d.atlas_tab.valid(t);
                if (!eligible) admission_reject(0); /* invalid */
                if (eligible && d.heldst.confirmation_required()) {
                    const uint32_t src = d.atlas_tab.e[t].src_frame;
                    const bool aged =
                        d.heldst.any &&
                        d.heldst.newest + 1u > (uint32_t)HeldState::kDepth &&
                        src < d.heldst.newest + 1u - (uint32_t)HeldState::kDepth;
                    if (aged) {
                        admission_reject(2); /* aged */
                    } else if (!d.heldst.confirms(src)) {
                        if (d.optimistic_atlas && d.heldst.any_confirmed) {
                            if (d.admission_stats && admission_eye < 2)
                                ++d.optimistic_bypassed[admission_eye];
                        } else {
                            eligible = false;
                            admission_reject(1); /* unconfirmed */
                        }
                    }
                }
                /* ADR-0029's displacement bound, off unless a margin is set.
                 * A skipped tile gathers `d` samples outside its own position,
                 * into neighbouring entries at other poses; bounding `d` at
                 * the corners bounds that contamination directly, where the
                 * error threshold cannot, because the threshold measures the
                 * contaminated predictor.
                 *
                 * Refusing the skip here means the tile is CODED, so the
                 * bound's price is forced refresh and it is paid in bytes. */
                if (eligible && d.cfg.atlas_disp_margin > 0) {
                    const double disp = atlas_corner_disp(
                        d.atlas_tab, t, d.atlas_geom.width,
                        d.atlas_geom.height);
                    if (disp >= (double)d.cfg.atlas_disp_margin) {
                        eligible = false;
                        ++d.disp_forced;
                        admission_reject(7); /* displacement */
                    }
                }
            }
            if (d.admission_stats && d.atlas && eligible && ref_slot >= 0)
                admission_reject(4); /* admitted before E1c may overturn it */
            /* [SYN] 13.12.6, and it is a FORCED skip rather than a refused
             * one.  A position whose entry already holds `src_frame >= N` has
             * been overtaken -- in practice by a base patch carrying a
             * future-dated source, which 13.12.9 makes ordinary rather than
             * exceptional -- and a tile this frame codes there is DROPPED by
             * the decoder.  Coding it anyway would leave the decoder's atlas
             * holding the patch and this encoder's shadow holding the coded
             * write, which is a reference the client does not have.
             *
             * So the tile goes into the SKIP range instead, whatever the
             * refresh rule or a lost-tile report wanted: those ask for the
             * position to be re-coded, and the syntax says a re-code there
             * cannot land.  Overriding `missing` and `due` is the point, not
             * an oversight. */
            if (d.atlas && ref_slot >= 0 &&
                d.atlas_tab.superseded_by(t, frame_number)) {
                eligible = true;
                ++d.atlas_superseded;
            }
            if (eligible)
                set_tile_mode(d.warp, t, nxvw::kModeWarpSkip, 0, 0);
            /* The job's mode is what E3/E4/E5 read.  It starts INTRA and E1c
             * writes it; setting it here as well would make the CPU model and
             * the GPU disagree about who owns the field. */
            f.jobs[t].mode = (uint32_t)nxvw::kModeIntra;
        }

        /* [planar] [SYN] 13.13.  The fit runs HERE, on the host, because it is
         * the shared exact-integer one (nxe_planar_host.h, which the reference
         * includes too) and because its output is a raw body that E3 and E4
         * have nothing to do with: a planar tile has no units, no lanes and no
         * coefficients.  E4 writes its header from `planar_bytes` and E5 copies
         * the body out of b_planar.
         *
         * CONFIGURATION AND GATE.  The reference searches six configurations
         * -- two granularities by three region counts -- and prices them
         * against the transform with the frame's lambda, then applies the
         * level-1 or level-2 rule.  That comparison needs the INTRA cost, which
         * on this pipeline is not known until E3 and E4 have run, so it is not
         * wired yet; what is wired is the fit and the emission, held to the
         * reference by NXVC_PLANAR_CONFIG and NXVC_PLANAR_FORCE -- the same two
         * development hooks ref/ has, pinning one configuration and taking it
         * on every eligible tile.  With them set on both sides the streams are
         * comparable byte for byte and what they compare is the fit.
         */
        if (d.cfg.planar && !gpu_planar) {
            static const int cfg_r = [] {
                const char *v = std::getenv("NXVC_PLANAR_CONFIG");
                return v ? std::atoi(v) : 0;
            }();
            static const int cfg_fine = [] {
                const char *v = std::getenv("NXVC_PLANAR_CONFIG");
                const char *c = v ? std::strchr(v, ',') : nullptr;
                return c ? std::atoi(c + 1) : 0;
            }();
            static const bool force = [] {
                const char *v = std::getenv("NXVC_PLANAR_FORCE");
                return v && v[0] == '1';
            }();
            if (force && cfg_r >= 2) {
                uint32_t *pb = (uint32_t *)d.b_planar.map;
                uint32_t *pr = (uint32_t *)d.b_planar_recon.map;
                const int np = 3;   /* Y, Co, Cg; alpha is never regionised */
                for (uint32_t t = 0; t < d.ntiles; ++t) {
                    if (f.jobs[t].mode != (uint32_t)nxvw::kModeIntra) continue;
                    nxe_planar_plane pp[NXE_PLANAR_PLANES];
                    for (int p = 0; p < np; ++p) {
                        const int sz = nxe_plane_size(&fp, &f.jobs[t], p);
                        pp[p].samples =
                            &f.src[p][(size_t)t * f.plane_size[p] * f.plane_size[p]];
                        pp[p].size = sz;
                        pp[p].dc_off = fp.ycocgr && p ? 256 : 128;
                        pp[p].maxval = fp.ycocgr && p ? 511 : 255;
                        pp[p].dc_step = nxe_planar_dc_step_qp(
                            (int)d.cfg.qp + (int)f.jobs[t].qp_delta);
                    }
                    nxe_planar_rec rec;
                    (void)nxe_planar_fit_tile(pp, np, cfg_r, cfg_fine, &rec);
                    uint8_t body[NXE_PLANAR_BODY_UINTS * 4];
                    const int len = nxe_planar_serialize(&rec, np, body);
                    std::memcpy(&pb[(size_t)t * NXE_PLANAR_BODY_UINTS], body,
                                (size_t)len);
                    /* Pass B has a fixed map capacity (16 words) even when
                     * the serialized map is smaller. */
                    uint32_t *rr = &pr[(size_t)t * 26u];
                    std::memset(rr, 0, 26u * sizeof(*rr));
                    rr[0] = body[0];
                    const int map_bytes = nxe_planar_map_bytes(rec.regions, rec.fine);
                    std::memcpy(&rr[1], body + 1, size_t(map_bytes));
                    std::memcpy(&rr[17], body + 1 + map_bytes,
                                size_t(len - 1 - map_bytes));
                    f.jobs[t].mode = (uint32_t)NXE_MODE_PLANAR;
                    f.jobs[t].planar_bytes = (uint32_t)len;
                }
            }
        }

        /* warp_ext() travels only when there is a reference to warp. */
        fp.warp_bytes = ref_slot >= 0 ? (uint32_t)(36 * f.fp.eyes) : 0u;
        fp.ref_slots = 1u << (frame_number & 3u);
        /* Frame-uniform: the warp matrix is derived from ONE reference view,
         * so every inter tile of the frame predicts from the same slot and
         * carries the same `ref_sel`.  E4 writes it into word1 bits 21-22 of
         * a tile whose mode is not INTRA. */
        /* [SYN] 4.1: 0 in every tile header when ATLAS is set.  create()
         * has already refused a configuration that asked for anything else,
         * so this is the invariant restated where it is emitted rather than a
         * silent correction of a live value. */
        fp.ref_sel =
            (d.atlas || ref_slot < 0) ? 0u : (uint32_t)d.cur_ref_sel;
        /* Frame flag bit 0 is the tile-map reset -- set exactly when there is
         * no usable reference -- and bit 3 says warp_ext() is present. */
        fp.frame_flags = (fp.frame_flags & ~9u) | (ref_slot >= 0 ? 8u : 1u);
        /* The QP of THIS frame, from the frame parameter record -- not from
         * the create-time config.  nxvc_vk_encoder_set_qp() rewrites fp and
         * the job list between frames, so a decision reading d.cfg.qp would
         * gate every skip at the quantiser the stream STARTED at. */
        const int qpc = (int)fp.base_qp > 63 ? 63 : (int)fp.base_qp;
        d.decide_push[0] = (int32_t)nxe_qstep[qpc];
        int32_t skip_thresh =
            d.cfg.skip_thresh > 0 ? (int32_t)d.cfg.skip_thresh : 256;
        /* Cheats 5, the motion-scaled skip threshold.  A tile whose prediction
         * error is under a perceptual threshold IN MOTION may be skipped where
         * it would not be at rest: the artefact is smear during fast rotation,
         * which is where the eye's own contrast sensitivity has collapsed, and
         * it appears at the moment rotation STOPS, for one refresh.
         *
         * Off by default, and the default is what every fixture and every acid
         * test runs, so byte-identity against `nxv-enc` is unaffected by this
         * existing.  Turning it on makes the stream depend on a quantity the
         * reference encoder does not yet derive, so a stream coded with it is
         * NOT byte-comparable until the reference carries the same knob.
         *
         * The angular speed is the frame-to-frame rotation the encoder reads
         * out of the pose stream it already receives; with no pose input it is
         * zero and the cheat is inert. */
        if (d.cfg.motion_skip_gain_q8 > 0) {
            const int64_t add = ((int64_t)d.cfg.motion_skip_gain_q8 *
                                 (int64_t)d.atlas_motion_q8) >> 8;
            const int64_t v = (int64_t)skip_thresh + add;
            skip_thresh = (int32_t)(v > 65535 ? 65535 : v);
        }
        d.decide_push[1] = skip_thresh;
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
        /* 0 none, 1 STATIC_MV, 2 also WARP_MV (section 6). */
        d.decide_push[6] = d.cfg.int_coded_vectors
                               ? (d.cfg.int_warp_mv ? 2 : 1)
                               : 0;
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
        /* Same reason as warp_bytes: row_present() sits before the table area,
         * so a value left only in the local copy would put every per-tile span
         * the API reports `rowpresent_bytes` short. */
    }

    /* [SYN] 3.1.2: flag bit 4 says the bitmap is present, and it is
     * `ceil(tiles_y * eyes / 8)` bytes.  OUTSIDE the inter block, because it
     * is set on every frame of a row_present stream including an all-intra
     * one -- "a frame with no reference has no skipped tiles, so every row
     * structure is present and the bitmap is all ones.  It is still legal, and
     * still five bytes."  Emitting it only on inter frames would make the flag
     * mean something the syntax does not say, and would move every offset
     * after warp_ext() on exactly the frames that did not get it.
     *
     * `f.fp` as well as the local `fp`, for the reason warp_bytes is copied
     * back: the API's per-tile spans read `f.fp`. */
    if (d.cfg.row_present) {
        fp.frame_flags |= 16u;
        f.fp.frame_flags |= 16u;
        fp.rowpresent_bytes = ((f.fp.tiles_y * f.fp.eyes) + 7u) / 8u;
    } else {
        fp.rowpresent_bytes = 0u;
    }
    f.fp.rowpresent_bytes = fp.rowpresent_bytes;

    /* ---- upload, then E3 alone.
     *
     * The table-set choice sits between E3 and E4 and is host work: it
     * minimises a sum of log2s over eight candidate tables, which is neither
     * normative nor integer, and paper 3.6 has E1 and the rate controller
     * settling the per-tile parameters anyway.  Reading the coefficients back
     * to make it is a harness cost, not a pipeline cost, which is why `bench`
     * times the five dispatches with the jobs already populated. */
    /* [SYN] 13.12.11 step 1, and it must be complete before the frame body
     * reads it: the assembled picture IS this frame's reference. */
    if (d.picture_frame &&
        !assemble_atlas_picture(f, d.picture_bi, Impl::kAssembleSlot, err)) {
        std::fprintf(stderr, "atlas assembly: %s\n", err.c_str());
        return false;
    }
    {
        VkCommandBuffer cb = d.dev.begin();
        if (nxe_time) gpu_ts_begin(d, cb, 10u);
        copy_up(cb, d.b_stage_small, d.b_params, &fp, sizeof fp, 0);
        copy_up(cb, d.b_stage_small, d.b_tabs, &f.tabs, sizeof f.tabs,
                1 << 18);
        if (gpu_planar) {
            const uint32_t cols = f.fp.width / 64u;
            const uint32_t rows = f.fp.height / 64u;
            const uint32_t divisor = d.cfg.planar_centre_quarter ? 4u : 2u;
            const uint32_t centre_cols = std::min(
                cols, cols >= 2u ? std::max(2u, (cols / divisor) & ~1u) : cols);
            const uint32_t centre_rows = std::min(
                rows, rows >= 2u ? std::max(2u, (rows / divisor) & ~1u) : rows);
            const uint32_t col0 = (cols - centre_cols) / 2u;
            const uint32_t row0 = (rows - centre_rows) / 2u;
            for (auto &job : f.jobs) {
                const bool centre = d.gpu_planar_centre &&
                                    job.col >= col0 && job.col < col0 + centre_cols &&
                                    job.row >= row0 && job.row < row0 + centre_rows;
                const uint32_t dx = job.col < col0 ? col0 - job.col :
                                    job.col >= col0 + centre_cols ?
                                        job.col - (col0 + centre_cols - 1u) : 0u;
                const uint32_t dy = job.row < row0 ? row0 - job.row :
                                    job.row >= row0 + centre_rows ?
                                        job.row - (row0 + centre_rows - 1u) : 0u;
                const uint32_t dist = std::max(dx, dy);
                const bool use_intra = centre;
                const bool fine = !use_intra && d.cfg.planar_graduated &&
                                  dist <= (d.planar_wide_ring ? 4u : 2u);
                job.flags &= 0x07fffffffu;
                job.res_level = 0;
                if (use_intra) {
                    job.mode = (uint32_t)NXE_MODE_INTRA;
                    job.flags |= 0x80000000u;
                    // Preserve centre detail and clear a stale delta when the
                    // frame QP changes between encodes. The signed six-bit
                    // range bottoms out at -32 for base QP values above 58.
                    job.qp_delta = std::max(-32, std::min(0,
                        26 - int(f.fp.base_qp)));
                } else {
                    job.mode = (uint32_t)NXE_MODE_PLANAR;
                    // E4/E5 use the PLANAR body, and this path skips reference
                    // reconstruction and host table selection. Diagnostic and
                    // trellis paths retain their historical coefficients.
                    if (independent_gpu_planar && !check && !d.trellis)
                        job.flags |= 0x08000000u;
                    if (fine)
                        job.flags |= 0x10000000u; // fine 4x4 Y cells
                    else if (d.cfg.planar_graduated && dist >=
                             (d.planar_wide_ring ? 12u : 8u))
                        job.flags |= 0x20000000u;
                    else if (d.cfg.planar_graduated && dist >=
                             (d.planar_wide_ring ? 8u : 4u))
                        job.flags |= 0x40000000u;
                    // Wide ring leaves distances 5..7 at the normal 8px size.
                }
                // R2/coarse is a 27-byte transmitted body; b_planar is padded
                // to 26 uints only for the fixed storage binding.
                job.planar_bytes = use_intra ? 0u :
                                   (fine ? 51u : 27u);
            }
        }
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
        /* The pixel half of a rollback (ADR-0029 section 7).  A receipt since
         * the last encode named tiles whose atlas pixels still hold the
         * generation the client did not receive; they are restored from the
         * undo slot here, before Pass W reads the atlas, so the predictor and
         * the client's atlas describe the same pixels.  It is a transfer and
         * the barrier below covers it. */
        if (d.atlas && !d.atlas_restore.empty()) {
            std::vector<VkBufferCopy> regs;
            for (uint32_t t : d.atlas_restore)
                atlas_tile_regions(d.ring, (int)f.fp.eyes, (int)f.fp.tiles_x,
                                   d.cfg.chroma444 ? 0 : 1, (int)f.fp.height, t,
                                   1u, 0u, regs);
            /* One command per 4096 regions: vkCmdCopyBuffer takes a count and
             * a very long receipt would otherwise build an unbounded one. */
            for (size_t i = 0; i < regs.size(); i += 4096) {
                const uint32_t n =
                    (uint32_t)std::min<size_t>(4096, regs.size() - i);
                vkCmdCopyBuffer(cb, d.b_ring.buf, d.b_ring.buf, n, &regs[i]);
            }
            d.atlas_restore.clear();
        }
        /* Base-sourced patches, in the same command buffer and under the same
         * barrier.  AFTER the rollback restore, deliberately: a rollback undoes
         * a coded write the client never got, and a patch is a NEW statement
         * about the same position made after it.  Applying them the other way
         * round would let a stale restore overwrite a fresh patch.
         *
         * The source is already in the atlas's own plane layout, so a patch is
         * the same set of strided row regions a rollback is -- one region per
         * tile row per plane, which is what the writes coalesce to. */
        if (d.atlas && !d.atlas_base_writes.empty()) {
            for (const auto &bw : d.atlas_base_writes) {
                std::vector<VkBufferCopy> regs;
                for (uint32_t k = 0; k < bw.count; ++k)
                    atlas_tile_regions(d.ring, (int)f.fp.eyes,
                                       (int)f.fp.tiles_x,
                                       d.cfg.chroma444 ? 0 : 1,
                                       (int)f.fp.height, bw.first_tile + k, 0u,
                                       0u, regs);
                /* The source buffer holds the SAME slot layout, so a region's
                 * source offset is its destination offset plus the caller's
                 * base.  That is what "already in the atlas layout" buys: no
                 * per-tile address arithmetic on the caller's side and none
                 * here either. */
                for (auto &r : regs) r.srcOffset = r.dstOffset + bw.src_offset;
                for (size_t i = 0; i < regs.size(); i += 4096) {
                    const uint32_t n =
                        (uint32_t)std::min<size_t>(4096, regs.size() - i);
                    vkCmdCopyBuffer(cb, bw.src, d.b_ring.buf, n, &regs[i]);
                }
            }
            d.atlas_base_writes.clear();
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
        if (gpu_planar) {
            uint32_t push[6] = {f.fp.width, f.fp.height, f.fp.base_qp,
                                (uint32_t)f.fp.chroma_qp_off,
                                d.planar_cadence ? (d.planar_cadence_started ? 1u : 3u) : 0u, frame_number};
            d.planar_cadence_started = true;
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                              d.p_planar_fit.pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d.p_planar_fit.layout, 0, 1,
                                    &d.s_planar_fit, 0, nullptr);
            vkCmdPushConstants(cb, d.p_planar_fit.layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
            vkCmdDispatch(cb, d.ntiles, 1, 1);
            d.dev.barrier_compute_to_compute(cb);
        }
        if (d.inter) {
            /* Pass W, then the decision, then E3.  Two barriers: the
             * predictor has to be complete before it is measured, and the
             * modes have to be written before E3 reads them to decide whether
             * to code the tile at all. */
            // Complete PLANAR refreshes have no predictor consumer: E1c
            // preserves their mode before reading wpred, and Pass B uses
            // the region body directly. Keep reference reconstruction below.
            if (!gpu_planar) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_w.pipe);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        d.p_w.layout, 0, 1, &d.s_w, 0, nullptr);
                vkCmdPushConstants(cb, d.p_w.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   (uint32_t)sizeof d.wpush, &d.wpush);
                vkCmdDispatch(cb, d.ntiles, 1, 1);
                d.dev.barrier_compute_to_compute(cb);
            }

            /* The indirect dispatch argument E1c counts up: (0, 1, 1).  It is
             * reset every frame, before the decision, because it is a running
             * total and a stale one would dispatch Pass B over last frame's
             * count -- reconstructing tiles this frame did not code and
             * skipping ones it did. */
            {
                const uint32_t init[4] = {0u, 1u, 1u, 0u};
                vkCmdUpdateBuffer(cb, d.b_indirect.buf, 0, sizeof init, init);
                barrier_compute_transfer(cb, false);
            }
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
            if (d.cfg.int_coded_vectors && !gpu_planar) {
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
        if (d.inter && !independent_gpu_planar) {
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
            /* The one-deep pixel undo, taken HERE: after everything that reads
             * the atlas and before Pass B writes it.  The whole picture is
             * copied rather than the coded tiles, because the coded set is
             * E1c's decision and the host does not learn it until after this
             * submit -- and copying everything is EQUIVALENT, because a tile's
             * atlas pixels change only when the tile is coded, so every
             * position ends up holding exactly the pixels it had before its own
             * most recent coded write-back.  That is the one-deep-per-TILE log
             * ADR-0029 section 7 asks for, at one buffer copy on the encoder's
             * PC. */
            /* Not on a PICTURE frame: it replaces EVERY atlas pixel, so the
             * one-deep snapshot has nothing useful to preserve -- and
             * `note_materialised()` has already made the frame a boundary the
             * undo log refuses to roll back across, so the snapshot could
             * never be used. */
            if (d.atlas && !d.picture_frame) {
                barrier_compute_transfer(cb, true);
                VkBufferCopy snap{};
                snap.srcOffset = 0;
                snap.dstOffset = (VkDeviceSize)d.ring.slot_u16 * 2u;
                snap.size = (VkDeviceSize)d.ring.slot_u16 * 2u;
                vkCmdCopyBuffer(cb, d.b_ring.buf, d.b_ring.buf, 1, &snap);
                barrier_compute_transfer(cb, false);
            }
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_b.pipe);
            /* A PICTURE frame reconstructs EVERY tile, so it needs the set
             * bound to the full order rather than the coded list. */
            const VkDescriptorSet sb = d.picture_frame ? d.s_b_full : d.s_b;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d.p_b.layout, 0, 1, &sb, 0, nullptr);
            vkCmdPushConstants(cb, d.p_b.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               (uint32_t)sizeof d.bpush, &d.bpush);
            /* A PICTURE frame takes the ORDINARY dispatch: 13.12.11 step 2
             * reconstructs EVERY tile, skipped ones included, because step 3
             * then makes the whole reconstruction the atlas.  The coded-only
             * dispatch below is an ATLAS-frame rule and applying it here would
             * leave every skipped position holding the PREVIOUS atlas's pixels
             * while its entry claimed this frame's pose. */
            if (d.atlas && !d.picture_frame) {
                /* [SYN] 13.12.3 step 3: a WARP_SKIP tile writes NOTHING to the
                 * atlas.  Running Pass B over it would store its warped
                 * predictor while its `C` still composes back to an older
                 * source frame, so the pixels and the matrix would describe
                 * different poses and every later prediction of that tile
                 * would be warped twice -- the chaining the model exists to
                 * remove.  So the dispatch is over the CODED tiles only, whose
                 * count is not known until E1c has run, which is what the
                 * indirect dispatch is for.
                 *
                 * This is also, on the decoder, where the 8.8 ms goes. */
                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1,
                                     &mb, 0, nullptr, 0, nullptr);
                vkCmdDispatchIndirect(cb, d.b_indirect.buf, 0);
            } else {
                vkCmdDispatch(cb, d.ntiles, 1, 1);
            }
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
        // Lite has no host table selection. Unless checking coefficients or
        // running trellis, no CPU consumer needs this full-frame readback.
        // Keep coefficients on the GPU for the following entropy pass.
        if ((!d.entropy_lite && !independent_gpu_planar) || check || d.trellis) {
            VkBufferCopy cc{0, 0, d.coef_bytes};
            vkCmdCopyBuffer(cb, d.b_coef.buf, d.b_stage_coef.buf, 1, &cc);
        }
        if (nxe_time) gpu_ts_end(d, cb, 10u);
        if (!d.dev.submit_and_wait(cb, err)) {
            std::fprintf(stderr, "E0/E3 submit: %s\n", err.c_str());
            return false;
        }
        if (nxe_time) d.gpu_ms_1 = gpu_ts_read(d, 10u);
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
        /* The head's rotation between this frame's view and its predecessor's,
         * for the motion-scaled skip threshold the NEXT frame will use.  It is
         * read here, before publish() overwrites the slot, and it is the only
         * floating-point quantity on this path -- encoder-side, non-normative,
         * and inert unless Cheats 5 is switched on. */
        if (d.cfg.motion_skip_gain_q8 > 0 && d.views.have && frame_number > 0) {
            const View &a = d.views.slot[(frame_number - 1u) & 3u][0];
            const View &b = d.views.cur[0];
            /* |2 * acos(|<qa, qb>|)| is the angle between two orientations.
             * The dot is clamped because a quantised pose pair can leave it a
             * hair outside [-1, 1] and acos would then be NaN. */
            double dot = a.qx * b.qx + a.qy * b.qy + a.qz * b.qz + a.qw * b.qw;
            if (dot < 0) dot = -dot;
            if (dot > 1.0) dot = 1.0;
            const double ang = 2.0 * std::acos(dot);
            const double q = ang * 256.0;
            d.atlas_motion_q8 = (int32_t)(q > 1e6 ? 1e6 : q);
        }
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
            if (back[t].mode != (uint32_t)nxvw::kModeIntra &&
                back[t].mode != (uint32_t)NXE_MODE_PLANAR)
                any_inter = true;
        }
        /* What the CLIENT will hold, once this frame reaches it.  A frame
         * every tile of which came out INTRA reconstructs from nothing, so it
         * is held whatever happened to its nominal reference -- which is
         * exactly the resync frame, and getting this wrong would keep the
         * encoder in INTRA for ever after one drop.  Any other frame is held
         * only if the frame it predicted from is (nxe_inter.h, rule 2). */
        d.heldst.publish(frame_number, any_inter ? d.cur_pred_fn : -1);

        /* [SYN] 13.12.3 step 3, the write-back, and the whole of the atlas's
         * cost model is in which tiles it touches.  A tile whose mode produced
         * reconstructed samples -- INTRA, WARP_MV, STATIC_MV -- resets its
         * entry to the identity at this frame.  A WARP_SKIP tile writes
         * NOTHING: not pixels, not metadata.  That is not an optimisation, it
         * is the definition; writing a skipped tile's warped predictor back
         * would be exactly the chaining the model exists to remove, and would
         * leave the pixels and `C` describing different poses.
         *
         * The PIXEL half is Pass B's ring store, which ran in the submit
         * above; this is the metadata half and the undo snapshot that goes
         * with it. */
        if (d.atlas && d.picture_frame) {
            /* [SYN] 13.12.11 step 3: the reconstructed picture BECOMES the
             * atlas.  Every position, not only the coded ones -- the whole
             * picture was reconstructed -- so `C := I`, `gen := 0`,
             * `valid := 1`, `base_sourced := 0`, `res_level := 0` and
             * `src_frame := N` everywhere, with `static` taken from THIS
             * frame's mode.
             *
             * It runs AFTER the coded tiles would have been written back,
             * which is the ordering that keeps the supersede test of 13.12.6
             * from firing on the frame's own tiles.  There is no undo
             * snapshot and no coded list: the frame is a generation boundary,
             * so a receipt for it cannot be answered by rolling back. */
            d.picture_modes.assign(d.ntiles,
                                   AtlasTable::kPictureNotCoded);
            for (uint32_t t = 0; t < d.ntiles; ++t) {
                const uint32_t m = f.jobs[t].mode;
                if (m != (uint32_t)nxvw::kModeWarpSkip)
                    d.picture_modes[t] = (uint8_t)m;
            }
            d.atlas_tab.picture_frame(d.picture_modes.data(), frame_number);
            Impl::CodedFrame &cl =
                d.atlas_coded[frame_number % (uint32_t)AtlasUndo::kDepth];
            cl.frame = frame_number;
            cl.used = 1;
            cl.tiles.clear();
        } else if (d.atlas) {
            Impl::CodedFrame &cl =
                d.atlas_coded[frame_number % (uint32_t)AtlasUndo::kDepth];
            cl.frame = frame_number;
            cl.used = 1;
            cl.tiles.clear();
            for (uint32_t t = 0; t < d.ntiles; ++t) {
                const uint32_t m = f.jobs[t].mode;
                if (m == (uint32_t)nxvw::kModeWarpSkip) continue;
                /* The snapshot is the entry as it was BEFORE this write-back,
                 * which is what a receipt for this frame rolls back to. */
                d.atlas_undo.note_coded(t, frame_number, d.atlas_tab.e[t]);
                d.atlas_tab.code_tile(t, frame_number, (int)m,
                                      (int)f.jobs[t].res_level);
                cl.tiles.push_back(t);
            }
        }
    }


    /* The frame report, from the modes this frame settled on.  It is filled
     * for every frame, atlas or not, so a caller can log one way. */
    {
        nxvc_vke_frame_report &r = d.last_report;
        const uint32_t wd = r.worst_disp_q4;   /* set by the trigger above */
        std::memset(&r, 0, sizeof r);
        r.worst_disp_q4 = d.cfg.atlas_mode ? wd : 0u;
        r.frame_number = frame_number;
        r.tiles = d.ntiles;
        r.mode = !d.atlas ? NXVC_VKE_FRAME_NON_ATLAS
                          : (d.picture_frame ? NXVC_VKE_FRAME_PICTURE
                                             : NXVC_VKE_FRAME_ATLAS);
        for (uint32_t t = 0; t < d.ntiles; ++t) {
            switch (f.jobs[t].mode) {
                case (uint32_t)nxvw::kModeWarpSkip:  ++r.skip; break;
                case (uint32_t)nxvw::kModeStaticMv:  ++r.static_mv; break;
                case (uint32_t)nxvw::kModeWarpMv:    ++r.warp_mv; break;
                case (uint32_t)nxvw::kModeIntra:     ++r.intra; break;
                default: break;
            }
        }
        r.coded = d.ntiles - r.skip;
        /* 13.12.11 step 1 warps every position; an ATLAS frame warps none. */
        r.assembled = (r.mode == NXVC_VKE_FRAME_PICTURE) ? d.ntiles : 0u;
        /* r.bytes is filled after the frame is packed; see below. */
    }

    if (d.admission_stats && d.atlas && ++d.admission_frames >= 60) {
        std::fprintf(stderr, "nxvc atlas admission/eye (first rejection; aged allowed; optimistic bypass L/R %llu/%llu): L invalid %llu unconfirmed %llu aged %llu refresh %llu admitted %llu missing %llu no-ref %llu displacement %llu; R invalid %llu unconfirmed %llu aged %llu refresh %llu admitted %llu missing %llu no-ref %llu displacement %llu\n",
                (unsigned long long)d.optimistic_bypassed[0], (unsigned long long)d.optimistic_bypassed[1],
                (unsigned long long)d.admission[0][0], (unsigned long long)d.admission[0][1],
                (unsigned long long)d.admission[0][2], (unsigned long long)d.admission[0][3],
                (unsigned long long)d.admission[0][4], (unsigned long long)d.admission[0][5],
                (unsigned long long)d.admission[0][6], (unsigned long long)d.admission[0][7],
                (unsigned long long)d.admission[1][0], (unsigned long long)d.admission[1][1],
                (unsigned long long)d.admission[1][2], (unsigned long long)d.admission[1][3],
                (unsigned long long)d.admission[1][4], (unsigned long long)d.admission[1][5],
                (unsigned long long)d.admission[1][6], (unsigned long long)d.admission[1][7]);
        std::memset(d.admission, 0, sizeof d.admission);
        std::memset(d.optimistic_bypassed, 0, sizeof d.optimistic_bypassed);
        d.admission_frames = 0;
    }

    /* The AGE form of the cap needs the modes this frame actually coded, so
     * it is updated here rather than at the decision: a position that coded
     * INTRA restarts at zero, every other position ages by one. */
    if (d.inter && d.cfg.drift_refresh) {
        if (d.age_since_intra.size() != (size_t)d.ntiles)
            d.age_since_intra.assign(d.ntiles, 0u);
        for (uint32_t t = 0; t < d.ntiles; ++t) {
            if (f.jobs[t].mode == (uint32_t)nxvw::kModeIntra)
                d.age_since_intra[t] = 0u;
            else if (d.age_since_intra[t] != 0xffffffffu)
                ++d.age_since_intra[t];
        }
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

    /* ---- effort 2: the trellis, and the pass structure it needs.
     *
     * The CPU model's `e3_tile_trellis` is, per tile, a PLAIN quantisation, a
     * table-set choice from it, and then the trellis against that set.  Tiles
     * are independent, so on the device the three become three whole-frame
     * steps: the plain E3 that has just run, a host choice over every tile, and
     * a second E3 with `fp.trellis` set.  Same order, same result, one dispatch
     * instead of a loop.
     *
     * Everything about WHICH tables is host-side and unchanged: this only moves
     * the quantisation onto the device. */
    auto run_e3_trellis = [&](bool from_defaults) -> bool {
        {
            /* The per-tile set from the PLAIN coefficients the device just
             * wrote, and the rate model the trellis will price against.  See
             * prepare_trellis_pass: the ORDER of those steps is the part that
             * is easy to get wrong. */
            std::vector<int32_t> rate(8 * 32 * 16 + 8, 0);
            prepare_trellis_pass(f, (const int16_t *)d.b_stage_coef.map,
                                 from_defaults, rate.data());
            std::memcpy(d.b_rate.map, rate.data(), rate.size() * 4);
        }
        nxe_frame_params tfp = fp;
        tfp.trellis = 1u;
        VkCommandBuffer cb = d.dev.begin();
        std::memcpy(d.b_stage_small.map, f.jobs.data(),
                    f.jobs.size() * sizeof(nxe_tile_job));
        VkBufferCopy cj{0, 0, f.jobs.size() * sizeof(nxe_tile_job)};
        vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_jobs.buf, 1, &cj);
        copy_up(cb, d.b_stage_small, d.b_params, &tfp, sizeof tfp, 1 << 18);
        d.dev.barrier_transfer_to_compute(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e3.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d.p_e3.layout, 0, 1, &d.s_e3, 0, nullptr);
        vkCmdDispatch(cb, d.ntiles, 1, 1);
        d.dev.barrier_compute_to_host(cb);
        VkBufferCopy cc2{0, 0, d.coef_bytes};
        vkCmdCopyBuffer(cb, d.b_coef.buf, d.b_stage_coef.buf, 1, &cc2);
        if (!d.dev.submit_and_wait(cb, err)) {
            std::fprintf(stderr, "E3 trellis submit: %s\n", err.c_str());
            return false;
        }
        /* The parameter record on the device is the trellis one now; the E4/E5
         * upload below puts the frame's own back -- and so is the job array's
         * `table_set` under Lite, which this puts back. */
        restore_lite_variant(f);
        return true;
    };
    if (d.trellis && !run_e3_trellis(true)) return false;

    /* The table-set choice reads the coefficients where E3 left them: the
     * staging buffer is host-cached, and copying seven megabytes into f.coef
     * first -- which nothing else on this path reads -- cost more than the
     * choice it fed.  The `check` path above has already copied them, because
     * it compares them against the CPU model. */
    /* ENTROPY_LITE has no probability tables: the tile header's `table_set`
     * field names the VARIANT, which setup() already put on every job, and
     * running the choice here would overwrite it with a table index. */
    if (!d.entropy_lite && !independent_gpu_planar)
        choose_table_sets(f, check ? f.coef.data()
                                   : (const int16_t *)d.b_stage_coef.map);
    /* Custom tables (tool bit 6) are trained on the histogram the choice above
     * has just built, so they cost no second walk of the coefficients.  They
     * rewrite f.tabs, f.fp.tables_present and f.fp.table_bytes, which is why
     * the parameter record and the tables are uploaded again below rather than
     * only before E3. */
    if (!d.entropy_lite && !independent_gpu_planar) train_table_sets(f);
    /* Pass two, against the TRAINED sets -- and only when there are trained
     * sets to be against.  Without custom tables the tables never moved, so a
     * second pass reaches the same coefficients by the same arithmetic. */
    if (d.trellis && f.custom_tables && !d.entropy_lite) {
        /* The plain quantisation pass two's per-tile choice reads.  fp still
         * has trellis 0 here, so this is the ordinary E3. */
        {
            VkCommandBuffer cb = d.dev.begin();
            copy_up(cb, d.b_stage_small, d.b_params, &fp, sizeof fp, 1 << 18);
            d.dev.barrier_transfer_to_compute(cb);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_e3.pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d.p_e3.layout, 0, 1, &d.s_e3, 0, nullptr);
            vkCmdDispatch(cb, d.ntiles, 1, 1);
            d.dev.barrier_compute_to_host(cb);
            VkBufferCopy cc3{0, 0, d.coef_bytes};
            vkCmdCopyBuffer(cb, d.b_coef.buf, d.b_stage_coef.buf, 1, &cc3);
            if (!d.dev.submit_and_wait(cb, err)) {
                std::fprintf(stderr, "E3 pass2 plain submit: %s\n", err.c_str());
                return false;
            }
        }
        if (!run_e3_trellis(false)) return false;
        /* The final per-tile choice, against the trained sets and without
         * restoring the built-in ones first -- ref's emit pass. */
        finish_trellis_pass(f, (const int16_t *)d.b_stage_coef.map);
    }
    fp.tables_present = f.fp.tables_present;
    fp.table_bytes = f.fp.table_bytes;
    auto t3 = clk::now();

    /* ---- the jobs go back with their table sets, then E4, E2 and E5. */
    {
        VkCommandBuffer cb = d.dev.begin();
        if (nxe_time) gpu_ts_begin(d, cb, 12u);
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
        if (nxe_time) gpu_ts_end(d, cb, 12u);
        if (!d.dev.submit_and_wait(cb, err)) {
            std::fprintf(stderr, "E4/E5 submit: %s\n", err.c_str());
            return false;
        }
        if (nxe_time) d.gpu_ms_2 = gpu_ts_read(d, 12u);
        std::memcpy(f.tile_bytes.data(),
                    (uint8_t *)d.b_stage_small.map + (1 << 19), d.ntiles * 4);
    }

    auto t4 = clk::now();
    uint32_t run = 0;
    for (uint32_t t = 0; t < d.ntiles; ++t) run += f.tile_bytes[t];
    uint32_t total = nxe_e5_frame_bytes(&fp, run);
    /* [SYN] 3.1.2.  nxe_e5_frame_bytes() charges a 12-byte header to EVERY row
     * structure; with row_present the shader emits one only for the rows it
     * names, so the host has to subtract the rest or it copies past the end of
     * the frame -- and the length it copies is the length the next frame
     * starts at, which is why getting this wrong shows up as a malformed
     * bitstream two frames later rather than as a short frame here.
     *
     * Presence is decided from `tile_bytes` on exactly the rule the shader
     * uses on E2's prefix of the same numbers: a coded tile always occupies at
     * least its 8-byte header, so a row that spans no bytes coded nothing. */
    if (fp.rowpresent_bytes) {
        const uint32_t rowgroups = f.fp.tiles_y * f.fp.eyes;
        uint32_t absent = 0;
        for (uint32_t g = 0; g < rowgroups; ++g) {
            uint32_t bytes = 0;
            for (uint32_t c = 0; c < f.fp.tiles_x; ++c)
                bytes += f.tile_bytes[g * f.fp.tiles_x + c];
            if (bytes == 0) ++absent;
        }
        if (absent == 0) {
            /* Nothing was elided, so E5's `rp_eff` emitted NO bitmap and
             * cleared flags bit 4 -- the frame is spelled exactly as a
             * tool-off frame, which is what the reference emits and what makes
             * enabling the tool cost nothing.  `nxe_e5_frame_bytes()` charged
             * the bitmap, so give it back or the copy runs past the frame and
             * the NEXT frame starts in the wrong place. */
            total -= fp.rowpresent_bytes;
        } else {
            total -= NXE_ROW_HEADER_BYTES * absent;
        }
    }
    f.out.assign(total, 0);
    std::memcpy(f.out.data(), d.b_out.map, total);
    /* The report's byte count, HERE rather than with the rest of it: the
     * census is settled as soon as the modes come back, but the frame is not
     * packed until now, and reading `f.out` earlier reports the PREVIOUS
     * frame's size -- which is exactly what it did before this line. */
    d.last_report.bytes = (uint32_t)f.out.size();

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
    if (nxe_time) {
        std::fprintf(stderr,
                     "nxe: E0 %.3f  passes to E3 %.2f  coef read %.2f  "
                     "table sets %.2f  E4/E5 %.2f  frame out %.2f  total %.2f ms\n",
                     d.e0_ms, ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4),
                     ms(t4, t5), ms(t0, t5));
        /* The DEVICE's own execution, from the queue's timestamps, beside the
         * host wall clock above.  The two answer different questions: the
         * wall clock is what a serial file-driven harness takes per frame --
         * submits, fence waits, host copies, the table-set choice and whatever
         * else shares the box -- while these three are what the GPU actually
         * ran.  Reporting only the first is how a 20 ms/frame figure gets
         * quoted for a 2 ms encoder. */
        std::fprintf(stderr,
                     "nxe-gpu: E0..PassB %.3f  E4/E5 %.3f  assemble %.3f  "
                     "gpu total %.3f ms\n",
                     d.gpu_ms_1, d.gpu_ms_2, d.gpu_ms_asm,
                     d.gpu_ms_1 + d.gpu_ms_2 + d.gpu_ms_asm);
        d.gpu_ms_asm = 0.0;
    }
    if (d.planar_cadence && std::getenv("NXVC_PLANAR_CADENCE_TRACE")) {
        const auto *meta = static_cast<const uint32_t *>(d.b_planar_cadence.map);
        uint32_t refresh = 0, reuse = 0, hot = 0, max_age = 0;
        for (uint32_t t = 0; t < d.ntiles; ++t) {
            const auto *m = meta + t * 64u;
            if (!(m[0] & 1u)) continue; // centre does not use this cache
            const uint32_t age = frame_number - m[1];
            refresh += age == 0; reuse += age != 0;
            hot += (m[0] & 2u) != 0; max_age = std::max(max_age, age);
        }
        std::fprintf(stderr, "cadence: frame %u fit %u reuse %u hot %u max_age %u\n",
                     frame_number, refresh, reuse, hot, max_age);
    }
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
    /* The luma plane is the common case and sits at the start of the slot, so
     * a luma-sized read is a prefix of a whole-slot read.  The bound is the
     * SLOT rather than the luma plane because atlas_pixel_digest() wants every
     * plane and would otherwise submit once per plane for one buffer; the
     * staging buffer is `coef_bytes`, which is twice a slot at every
     * configuration this encoder builds, so a slot always fits. */
    if (count > (size_t)d.ring.slot_u16) return false;
    if (count * 2u > d.coef_bytes) return false;
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

bool VkEncoder::read_displayed_luma(uint32_t frame_number,
                                    std::vector<uint16_t> &out) {
    Impl &d = *p_;
    if (!d.inter || !d.ok) return false;
    const int stride = d.ring.stride[0];
    const int h = d.bpush.imageH;
    const int eye_w = d.ring.planeW[0];
    const int cols = (int)d.atlas_geom.cols_per_eye * d.atlas_geom.eyes;
    std::vector<uint16_t> plane((size_t)stride * (size_t)h, 0u);
    const uint32_t slot = d.atlas ? 0u : (frame_number & 3u);
    if (!read_ring_luma(slot, plane.data(), plane.size())) return false;
    if (d.atlas) {
        atlas_display_luma(d.atlas_tab, plane.data(), stride, eye_w, h, out);
        return true;
    }
    /* No ATLAS: the displayed picture is the reconstruction itself.  Re-tiled
     * so that the caller compares like with like against a tile-major
     * source. */
    const int cpe = (int)d.atlas_geom.cols_per_eye
                        ? (int)d.atlas_geom.cols_per_eye
                        : (int)(eye_w + 63) / 64;
    out.assign((size_t)d.ntiles * 64u * 64u, 0u);
    const int ncols = cols ? cols : cpe;
    for (uint32_t t = 0; t < d.ntiles; ++t) {
        const int row = (int)(t / (uint32_t)ncols);
        const int rem = (int)(t % (uint32_t)ncols);
        const int eye = rem / cpe;
        const int col = rem % cpe;
        uint16_t *dst = &out[(size_t)t * 64u * 64u];
        for (int v = 0; v < 64; ++v)
            for (int u = 0; u < 64; ++u) {
                int x = eye * eye_w + col * 64 + u;
                int y = row * 64 + v;
                if (x >= eye * eye_w + eye_w) x = eye * eye_w + eye_w - 1;
                if (y >= h) y = h - 1;
                dst[(size_t)v * 64 + (size_t)u] =
                    plane[(size_t)y * (size_t)stride + (size_t)x];
            }
    }
    return true;
}

/* ------------------------------------------------------ the normative output
 *
 * [SYN] 13.12: the atlas is what conformance compares, and the stream is only
 * the means by which two implementations arrive at one.  These two calls are
 * what let a test compare this encoder's SHADOW atlas against the atlas
 * nxv-dec builds from the very stream this encoder emitted -- which is the
 * property 13.12.3 actually requires, and which byte-identity of the stream
 * does not imply.  An encoder whose shadow has drifted still emits legal
 * frames; it simply predicts, some frames later, from pixels the client does
 * not have. */
bool VkEncoder::atlas_table(std::vector<uint8_t> &out) const {
    const Impl &d = *p_;
    if (!d.atlas || !d.ok) return false;
    const size_t n = d.atlas_tab.e.size();
    out.assign(n * 64u, 0u);
    for (size_t t = 0; t < n; ++t) {
        const AtlasEntry &e = d.atlas_tab.e[t];
        uint8_t *p = out.data() + t * 64u;
        /* Field by field, little-endian, rather than a memcpy of the struct:
         * the wire form of 13.12.1 must not depend on this compiler's padding
         * or on the host's byte order. */
        for (int k = 0; k < 9; ++k) {
            const uint32_t v = (uint32_t)e.C[k];
            p[k * 4 + 0] = (uint8_t)(v & 0xff);
            p[k * 4 + 1] = (uint8_t)((v >> 8) & 0xff);
            p[k * 4 + 2] = (uint8_t)((v >> 16) & 0xff);
            p[k * 4 + 3] = (uint8_t)((v >> 24) & 0xff);
        }
        p[36] = (uint8_t)(e.src_frame & 0xff);
        p[37] = (uint8_t)((e.src_frame >> 8) & 0xff);
        p[38] = (uint8_t)((e.src_frame >> 16) & 0xff);
        p[39] = (uint8_t)((e.src_frame >> 24) & 0xff);
        p[40] = (uint8_t)(e.gen & 0xff);
        p[41] = (uint8_t)((e.gen >> 8) & 0xff);
        p[42] = e.flags;
        p[43] = e.res_level;
        /* 44..63 stay zero: `out` was assigned zeroed and v1 writes no depth. */
    }
    return true;
}

uint64_t VkEncoder::atlas_disp_forced() const { return p_->disp_forced; }

/* [SYN] 13.12.11 step 1: assemble the atlas into ONE COHERENT PICTURE, at the
 * pose the atlas holds it at -- frame N-1's.
 *
 * Every valid entry is rebased through its OWN `C` (13.12.10), un-advanced,
 * and the results are written into `dst_slot` as a picture.  A position whose
 * entry is invalid yields mid-grey, exactly as 13.12.5's display does, which
 * is what Pass W already produces for a tile with no usable reference.
 *
 * It is Pass W followed by Pass B over EVERY tile with the residual forced to
 * zero: Pass W warps each tile out of the atlas through its own matrix, and
 * Pass B with zero coefficients stores that prediction -- `reconstruct_skip`.
 * So the assembly is the two passes the encoder already has, pointed at
 * different slots, rather than a fourth warp kernel that would have to agree
 * with them bit for bit.
 *
 * THIS IS WHERE THE 8.8 ms GOES.  A PICTURE frame pays a full-picture warp
 * that an ATLAS frame does not, which is the whole of the trade 13.12.11
 * describes.  It is submitted on its own and waited on rather than chained
 * into the frame's command buffer, because the warp records and the
 * coefficient buffer it needs are the SAME buffers the frame body then
 * overwrites; keeping both live at once would mean a second copy of each, and
 * this path is the expensive one by construction. */
bool VkEncoder::assemble_atlas_picture(Frame &f, const WarpBuildInfo &bi_in,
                                       uint32_t dst_slot, std::string &err) {
    Impl &d = *p_;

    WarpBuildInfo bi = bi_in;
    bi.atlas = 1;              /* per-tile matrices: each entry's own C */
    bi.ref_slot = 0;           /* the atlas itself is the source */
    WarpParams wp;
    build_warp_params(bi, d.ring, wp);
    /* Where Pass B stores.  build_warp_params() wrote `frame_number & 3`; the
     * assembly writes the scratch slot instead. */
    wp.w[(size_t)NXVW_WARP_HDR_RING + 3] = dst_slot;
    /* The entries' own C, NOT advanced: the atlas is assembled at the pose it
     * holds, and this frame's matrix is applied afterwards by the ordinary
     * process warping the assembled picture. */
    atlas_build_matrices(d.atlas_tab, bi.width, bi.height, bi.cw, bi.ch, wp);
    for (uint32_t t = 0; t < d.ntiles; ++t) {
        set_tile_mode(wp, t, nxvw::kModeWarpSkip, 0, 0);
        /* An invalid entry has no pixels to rebase.  `refBase` of all-ones is
         * Pass W's "no usable reference", which fills the tile with mid-grey
         * -- which is what 13.12.11 step 1 asks for, by reference to
         * 13.12.5. */
        if (!d.atlas_tab.valid(t))
            wp.w[wp.tile_word(t) + 6] = 0xffffffffu;
    }

    const size_t wb = wp.bytes();
    const size_t warp_stage_off = (size_t)1 << 20;
    std::memcpy((uint8_t *)d.b_stage_small.map + warp_stage_off,
                wp.w.data(), wb);

    /* Pass B's TILE RECORDS, built here on the host.  They normally come from
     * E1c, which has not run for the assembly and whose records still describe
     * the previous frame -- so Pass B would reconstruct last frame's decisions
     * into the assembled picture.  Every record says WARP_SKIP, which with the
     * zeroed residual is `reconstruct_skip`: store the prediction.  The layout
     * is E1c's write_rec(). */
    std::vector<uint32_t> recs((size_t)d.ntiles * 4u, 0u);
    for (uint32_t t = 0; t < d.ntiles; ++t) {
        const nxe_tile_job &j = f.jobs[t];
        uint32_t w0 = (j.col & 0xfffu) << 4;
        w0 |= (j.eye & 1u) << 2;
        uint32_t w1 = (uint32_t)nxvw::kModeWarpSkip & 7u;
        w1 |= (j.chroma444 & 1u) << 5;
        w1 |= (j.nsub_log2 & 7u) << 17;
        recs[(size_t)t * 4u + 0u] = w0;
        recs[(size_t)t * 4u + 1u] = w1;
        recs[(size_t)t * 4u + 2u] = 1u << 8;
        recs[(size_t)t * 4u + 3u] = 0xffffffffu;
    }
    const size_t rb = recs.size() * sizeof(uint32_t);
    const size_t rec_stage_off = warp_stage_off + wb;
    std::memcpy((uint8_t *)d.b_stage_small.map + rec_stage_off,
                recs.data(), rb);

    nxvw::NxvwWarpPush push = warp_push(bi, d.ring);

    const bool tm = std::getenv("NXE_TIME") != nullptr;
    VkCommandBuffer cb = d.dev.begin();
    if (tm) gpu_ts_begin(d, cb, 14u);
    VkBufferCopy cw{warp_stage_off, 0, wb};
    vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_warp.buf, 1, &cw);
    VkBufferCopy cr{rec_stage_off, 0, rb};
    vkCmdCopyBuffer(cb, d.b_stage_small.buf, d.b_tilerecs.buf, 1, &cr);
    /* The residual must be ZERO, or Pass B would add whatever the previous
     * frame left in the coefficient buffer to the assembled picture. */
    vkCmdFillBuffer(cb, d.b_coef.buf, 0, VK_WHOLE_SIZE, 0u);
    barrier_compute_transfer(cb, false);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_w.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_w.layout,
                            0, 1, &d.s_w, 0, nullptr);
    vkCmdPushConstants(cb, d.p_w.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       (uint32_t)sizeof push, &push);
    vkCmdDispatch(cb, d.ntiles, 1, 1);
    d.dev.barrier_compute_to_compute(cb);

    /* Over EVERY tile, directly.  The indirect dispatch of an ATLAS frame
     * covers the CODED tiles only; here every position is being materialised,
     * which is the point. */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_b.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.p_b.layout,
                            0, 1, &d.s_b_full, 0, nullptr);
    vkCmdPushConstants(cb, d.p_b.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       (uint32_t)sizeof d.bpush, &d.bpush);
    vkCmdDispatch(cb, d.ntiles, 1, 1);
    if (tm) gpu_ts_end(d, cb, 14u);

    if (!d.dev.submit_and_wait(cb, err)) return false;
    if (tm) d.gpu_ms_asm = gpu_ts_read(d, 14u);
    (void)f;
    return true;
}

bool VkEncoder::last_picture_frame() const { return p_->picture_frame; }

const nxvc_vke_frame_report &VkEncoder::last_frame_report() const {
    return p_->last_report;
}

bool VkEncoder::atlas_layout(nxvc_vke_atlas_layout &out) const {
    const Impl &d = *p_;
    if (!d.ok || !d.atlas) return false;
    std::memset(&out, 0, sizeof out);
    const RingLayout &rl = d.ring;
    const int chroma420 = d.cfg.chroma444 ? 0 : 1;
    const int eyes = d.atlas_geom.eyes ? d.atlas_geom.eyes : 1;
    const int height = d.atlas_geom.height;

    out.plane_count = (uint32_t)rl.nplanes;
    out.bytes_per_sample = 2u;
    /* The shaders address the ring as packed pairs; this is the fact that
     * makes the stride's even padding load-bearing rather than cosmetic. */
    out.samples_per_uint = 2u;
    out.slot_bytes = (uint32_t)((size_t)rl.slot_u16 * 2u);
    out.eyes = (uint32_t)eyes;
    out.tiles_x = (uint32_t)d.atlas_geom.cols_per_eye;
    out.tiles_y = (uint32_t)d.atlas_geom.rows;

    for (int p = 0; p < rl.nplanes && p < 4; ++p) {
        nxvc_vke_atlas_plane_layout &pl = out.plane[p];
        /* Exactly the terms atlas_tile_regions() uses -- `rl.off[p]`,
         * `rl.stride[p]`, `rl.planeW[p]` and the same chroma halving -- so the
         * reported layout and the copies cannot disagree. */
        pl.offset_u16 = (uint32_t)rl.off[p];
        pl.stride_u16 = (uint32_t)rl.stride[p];
        pl.offset_bytes = (uint32_t)((size_t)rl.off[p] * 2u);
        pl.stride_bytes = (uint32_t)((size_t)rl.stride[p] * 2u);
        pl.width = (uint32_t)rl.planeW[p];
        pl.height = (uint32_t)(((p == 1 || p == 2) && chroma420)
                                   ? (height + 1) / 2
                                   : height);
        /* `x0 = eye * rl.planeW[p] + ...` in atlas_tile_regions(): the eye
         * stride IS the per-eye plane width.  Reported as its own field
         * because a caller reading only `width` cannot tell whether the plane
         * spans one eye or the pair. */
        pl.eye_stride = (uint32_t)rl.planeW[p];
        pl.tile_extent = (uint32_t)nxvw::nxvw_inter_plane_full(p, chroma420);
    }
    return true;
}

bool VkEncoder::atlas_layout_roundtrip(std::string &err) {
    Impl &d = *p_;
    if (!d.ok || !d.atlas) { err = "needs an ATLAS encoder"; return false; }
    nxvc_vke_atlas_layout L;
    if (!atlas_layout(L)) { err = "atlas_layout() refused"; return false; }

    /* The buffer is built from the ACCESSOR'S numbers and nothing else -- no
     * RingLayout, no nxvw_ring_layout() -- and the copy is made by the
     * PRODUCTION region builder.  So this pins the one property a caller
     * depends on and cannot check for itself: that the layout the encoder
     * reports addresses the same samples the encoder's own copies do.  A
     * second derivation of the layout that is subtly wrong passes every other
     * check in the codebase and shows up as a wrong picture much later. */
    const size_t slot_u16 = (size_t)d.ring.slot_u16;
    if (L.slot_bytes != slot_u16 * 2u) {
        err = "slot_bytes disagrees with the ring slot";
        return false;
    }
    if ((size_t)L.slot_bytes > d.coef_bytes) {
        err = "staging buffer too small for a slot";
        return false;
    }

    /* A value that depends on the plane and BOTH coordinates, so a wrong
     * stride, a wrong plane origin and a wrong eye column all land on a
     * different number rather than an equal one. */
    auto pattern = [](uint32_t p, uint32_t x, uint32_t y) -> uint16_t {
        return (uint16_t)((p * 7919u + y * 131u + x * 17u + 1u) & 0xffffu);
    };
    const uint16_t kBackground = 0xABCDu;

    auto stage = [&](void) -> uint16_t * {
        return (uint16_t *)d.b_stage_coef.map;
    };
    auto copy_whole_slot_from_stage = [&](void) -> bool {
        VkCommandBuffer cb = d.dev.begin();
        VkBufferCopy c{0, 0, (VkDeviceSize)L.slot_bytes};
        vkCmdCopyBuffer(cb, d.b_stage_coef.buf, d.b_ring.buf, 1, &c);
        std::string e2;
        if (!d.dev.submit_and_wait(cb, e2)) { err = e2; return false; }
        return true;
    };

    /* 1. Fill the whole atlas slot with a background the pattern never
     *    produces, so "was this sample written?" is answerable. */
    for (size_t i = 0; i < slot_u16; ++i) stage()[i] = kBackground;
    if (!copy_whole_slot_from_stage()) return false;

    /* 2. Build a slot-shaped patch image from the accessor alone. */
    for (size_t i = 0; i < slot_u16; ++i) stage()[i] = 0u;
    const uint32_t eyes = L.eyes ? L.eyes : 1u;
    for (uint32_t p = 0; p < L.plane_count; ++p) {
        const nxvc_vke_atlas_plane_layout &pl = L.plane[p];
        for (uint32_t y = 0; y < pl.height; ++y)
            for (uint32_t x = 0; x < pl.width * eyes; ++x) {
                const size_t idx =
                    (size_t)pl.offset_u16 + (size_t)y * pl.stride_u16 + x;
                if (idx >= slot_u16) { err = "layout runs past the slot"; return false; }
                stage()[idx] = pattern(p, x, y);
            }
    }

    /* 3. Copy a CHECKERBOARD of tiles through the production builder, with
     *    src_offset 0 -- the atlas_write_tiles contract exactly.  A
     *    checkerboard means every patched tile borders unpatched ones, so an
     *    off-by-one in a stride or an eye origin bleeds into a neighbour and
     *    is caught rather than being invisibly self-consistent. */
    const int cols = (int)L.tiles_x * (int)eyes;
    std::vector<uint8_t> patched((size_t)cols * L.tiles_y, 0u);
    std::vector<VkBufferCopy> regs;
    for (uint32_t t = 0; t < (uint32_t)patched.size(); ++t) {
        const uint32_t row = t / (uint32_t)cols;
        const uint32_t rem = t % (uint32_t)cols;
        const uint32_t col = rem % L.tiles_x;
        if (((row + col) & 1u) != 0u) continue;
        patched[t] = 1u;
        atlas_tile_regions(d.ring, (int)eyes, (int)L.tiles_x,
                           d.cfg.chroma444 ? 0 : 1, d.atlas_geom.height, t, 0u,
                           0u, regs);
    }
    {
        VkCommandBuffer cb = d.dev.begin();
        for (size_t i = 0; i < regs.size(); i += 4096) {
            const uint32_t n = (uint32_t)std::min<size_t>(4096, regs.size() - i);
            vkCmdCopyBuffer(cb, d.b_stage_coef.buf, d.b_ring.buf, n, &regs[i]);
        }
        std::string e2;
        if (!d.dev.submit_and_wait(cb, e2)) { err = e2; return false; }
    }

    /* 4. Read the atlas back and check every sample of every plane. */
    std::vector<uint16_t> got(slot_u16, 0u);
    if (!read_ring_luma(0u, got.data(), got.size())) {
        err = "readback failed";
        return false;
    }
    size_t nchecked = 0, nwritten = 0;
    for (uint32_t p = 0; p < L.plane_count; ++p) {
        const nxvc_vke_atlas_plane_layout &pl = L.plane[p];
        for (uint32_t eye = 0; eye < eyes; ++eye)
            for (uint32_t row = 0; row < L.tiles_y; ++row)
                for (uint32_t col = 0; col < L.tiles_x; ++col) {
                    const uint32_t t =
                        row * (uint32_t)cols + eye * L.tiles_x + col;
                    const uint32_t x0 = eye * pl.eye_stride + col * pl.tile_extent;
                    const uint32_t y0 = row * pl.tile_extent;
                    /* The clip of the header comment, per eye. */
                    const int w = (int)std::min(pl.tile_extent,
                                                pl.width - col * pl.tile_extent);
                    const int h = (int)std::min(pl.tile_extent,
                                                pl.height - row * pl.tile_extent);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) {
                            const size_t idx = (size_t)pl.offset_u16 +
                                               (size_t)(y0 + (uint32_t)y) *
                                                   pl.stride_u16 +
                                               (size_t)(x0 + (uint32_t)x);
                            const uint16_t want =
                                patched[t] ? pattern(p, x0 + (uint32_t)x,
                                                     y0 + (uint32_t)y)
                                           : kBackground;
                            ++nchecked;
                            if (patched[t]) ++nwritten;
                            if (got[idx] != want) {
                                char b[256];
                                std::snprintf(
                                    b, sizeof b,
                                    "plane %u tile %u (eye %u row %u col %u) "
                                    "sample (%d,%d): got 0x%04x want 0x%04x",
                                    p, t, eye, row, col, x, y, got[idx], want);
                                err = b;
                                return false;
                            }
                        }
                }
    }
    std::printf("-- atlas layout round-trip: %zu samples checked over %u "
                "planes, %zu written by the patch, %zu left as background\n",
                nchecked, L.plane_count, nwritten, nchecked - nwritten);
    return true;
}

bool VkEncoder::atlas_write_tiles(uint32_t eye, uint32_t first_tile,
                                  uint32_t count, VkBuffer src,
                                  uint64_t src_offset, uint32_t src_frame,
                                  uint32_t *applied, uint32_t *superseded,
                                  std::string &err) {
    Impl &d = *p_;
    if (applied) *applied = 0;
    if (superseded) *superseded = 0;
    if (!d.ok) { err = "encoder not created"; return false; }
    if (!d.atlas) { err = "atlas_write_tiles needs ATLAS"; return false; }
    if (src == VK_NULL_HANDLE) { err = "src buffer is null"; return false; }
    const int eyes = d.atlas_geom.eyes ? d.atlas_geom.eyes : 1;
    if ((int)eye >= eyes) { err = "eye out of range"; return false; }
    const uint32_t per_eye =
        (uint32_t)(d.atlas_geom.cols_per_eye * d.atlas_geom.rows);
    if (count == 0) return true;               /* an empty run is a no-op */
    if (first_tile >= per_eye || count > per_eye - first_tile) {
        err = "tile run leaves the eye";
        return false;
    }
    /* The run is contiguous WITHIN THE EYE, which with two eyes is not
     * contiguous pair-wide -- the tile order of Annex D D-3 interleaves the
     * eyes every `cols_per_eye` -- so the pair-wide index is derived per tile
     * and the table is updated per tile.
     *
     * SUPERSEDE IS PER TILE AND IT IS A DROP, NOT AN ERROR ([SYN] 13.12.9,
     * "Ordering").  A base picture reaches the encoder through a hardware
     * decoder with its own latency -- 2.76 ms mean and 5.63 ms p99 in the
     * measurement the clause cites -- while coded tiles come down the path
     * they always did, so a patch arriving behind a coded write of the same
     * position is the ORDINARY case and not a caller error.  The rule is
     * `src_frame` must be strictly greater than the one the position already
     * holds; a write that does not advance the position has been overtaken and
     * is dropped, and the counts say how the run split.  Refusing the call
     * instead would make the encoder's shadow diverge from the decoder's
     * atlas, which drops silently and carries on. */
    const int cols = d.atlas_geom.cols_per_eye * eyes;
    uint32_t nap = 0, nsup = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t idx = first_tile + k;
        const uint32_t row = idx / (uint32_t)d.atlas_geom.cols_per_eye;
        const uint32_t col = idx % (uint32_t)d.atlas_geom.cols_per_eye;
        const uint32_t t =
            row * (uint32_t)cols + eye * (uint32_t)d.atlas_geom.cols_per_eye +
            col;
        if (t >= d.atlas_tab.e.size()) {
            err = "tile index out of range";
            return false;
        }
        const AtlasEntry &a = d.atlas_tab.e[t];
        if ((a.flags & kAtlasValid) && src_frame > 0 &&
            a.src_frame >= src_frame) {
            ++nsup;
            continue;               /* superseded: dropped, not applied */
        }
        Impl::BaseWrite bw;
        bw.src = src;
        bw.src_offset = (VkDeviceSize)src_offset;
        bw.first_tile = t;
        bw.count = 1;
        bw.src_frame = src_frame;
        d.atlas_base_writes.push_back(bw);
        /* The table is updated NOW, not when the copy runs.  The encoder's
         * shadow has to describe the atlas the next encode will predict from,
         * and the next encode is what runs the copy; deferring both would make
         * the decision pass see the old entry. */
        d.atlas_tab.write_base_tile(t, src_frame);
        /* A patched position's undo snapshot is gone: the pixels it would
         * restore are no longer the ones the client holds.  Dropping the
         * snapshot makes a later rollback INVALIDATE the entry instead, which
         * is the safe direction -- an invalid tile is coded INTRA and a
         * wrongly-held one is a prediction from a picture nobody has. */
        if (t < d.atlas_undo.s.size()) d.atlas_undo.s[t].used = 0;
        ++nap;
    }
    if (applied) *applied = nap;
    if (superseded) *superseded = nsup;
    return true;
}

bool VkEncoder::atlas_pixel_digest(uint8_t out[32]) {
    Impl &d = *p_;
    if (!d.atlas || !d.ok) return false;
    /* The whole of slot 0 -- the atlas lives at one generation, so slot 0 IS
     * the atlas -- read back once and digested plane by plane.  Reading per
     * plane would submit three times for one buffer. */
    std::vector<uint16_t> slot((size_t)d.ring.slot_u16, 0u);
    if (!read_ring_luma(0u, slot.data(), slot.size())) return false;

    const int eyes = d.atlas_geom.eyes ? d.atlas_geom.eyes : 1;
    const int lumaH = d.bpush.imageH;
    const int chromaH = d.cfg.chroma444 ? lumaH : (lumaH + 1) / 2;
    for (int pl = 0; pl < 4; ++pl) {
        uint64_t h = 1469598103934665603ull;
        if (pl < d.ring.nplanes) {
            /* `planeW` is PER EYE and the atlas spans both, exactly as the
             * reference's does; the ring is padded and the reference is not,
             * so the row walk uses the ring stride and the digest covers only
             * the `w` real samples of each row. */
            const int w = d.ring.planeW[pl] * eyes;
            const int ht = pl == 0 ? lumaH : chromaH;
            const int stride = d.ring.stride[pl];
            const uint16_t *base = slot.data() + d.ring.off[pl];
            for (int y = 0; y < ht; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint16_t v = base[(size_t)y * (size_t)stride + (size_t)x];
                    h = (h ^ (uint64_t)(v & 0xff)) * 1099511628211ull;
                    h = (h ^ (uint64_t)(v >> 8)) * 1099511628211ull;
                }
        }
        for (int k = 0; k < 8; ++k) out[pl * 8 + k] = (uint8_t)(h >> (8 * k));
    }
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
    if (!d.atlas || held) return;

    /* ADR-0029 section 7.  A negative report under ATLAS invalidates exactly
     * the tiles the lost frame CODED, per tile, and nothing else -- every
     * other tile position's entry is still bit-identical to the client's and
     * continues to be.  There is no cascade along a prediction chain and no
     * INTRA resync: the frame-level `not_held` sweep above still runs because
     * the held record is shared with the non-atlas path, but under ATLAS it
     * decides nothing, because eligibility is per tile.
     *
     * For each tile the frame coded, roll the shadow entry back to the
     * generation before it, replaying the advances since.  When the log can no
     * longer produce that generation -- the frame has aged out, or the tile
     * has been coded AGAIN since, so the one-deep snapshot is a later one --
     * the entry is invalidated instead.  That second case is the residual risk
     * the ADR names: during the round trip before the report arrives the
     * encoder predicted from a generation the client does not hold, and the
     * only sound answer is to code the tile INTRA.  Invalidating is that
     * answer, and it is the safe direction: an INTRA tile costs bytes, a
     * wrongly-held one costs a refusal. */
    const Impl::CodedFrame &cl =
        d.atlas_coded[frame_number % (uint32_t)AtlasUndo::kDepth];
    if (!cl.used || cl.frame != frame_number) return;
    for (uint32_t t : cl.tiles) {
        AtlasEntry back{};
        if (d.atlas_undo.rollback(t, frame_number, d.atlas_now, back)) {
            d.atlas_tab.e[t] = back;
            /* The metadata is the client's again; the PIXELS still hold the
             * lost generation's reconstruction, so the tile is queued for a
             * restore from the undo slot at the head of the next encode. */
            d.atlas_restore.push_back(t);
        } else {
            d.atlas_tab.e[t].flags &= (uint8_t)~kAtlasValid;
        }
    }
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
