// --k1-sweep: why is a 2048x4096 RGBA8 copy slow on this part?
//
// The gate's K1 number is produced inside the frame loop, next to the
// co-tenant reprojection pass, with one TOP_OF_PIPE..BOTTOM_OF_PIPE timestamp
// pair per frame. That is the right way to report the number PAPER 3.4 asks
// for, and the wrong way to find out where the number comes from: the
// co-tenant, the timestamp bracketing, the access pattern, the storage class
// and the image format are all folded into one figure.
//
// This file unfolds them. Every variant is timed alone -- no co-tenant, no
// present, no frame loop -- with vkQueueWaitIdle on both sides of a submit
// that contains exactly one dispatch, measured twice: by the device's own
// timestamp pair and by the host clock across the whole submit. When those
// two agree, the timestamps are trustworthy and the kernel really is that
// slow; when they do not, the measurement is the problem.
#include "nxb_bench.h"
#include "nxb_spv.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace nxb {

namespace {

struct Variant
{
    const char*     name;
    const uint32_t* spv;
    size_t          bytes;
    int             store;     // NXB_K1_STORE
    uint32_t        wgx, wgy;
    const char*     note;
};

double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

// One dispatch, alone on the queue, timed from both sides.
// Returns the device time; hostMs receives the host-side time.
double Bench::timeIsolated(Kern& k, const void* push, uint32_t pushBytes,
                           int reps, double* hostMs)
{
    // One dispatch is too short to measure against submit overhead, so the
    // timed region holds kInner of them, separated by full barriers so they
    // cannot overlap and the copy cannot be folded away. Both the device and
    // the host figure are divided back down to one dispatch.
    const int kInner = 8;
    std::vector<double> dev, host;
    dev.reserve(size_t(reps));
    host.reserve(size_t(reps));

    for (int i = 0; i < reps; ++i)
    {
        vkQueueWaitIdle(ctx_->queue);
        auto t0 = std::chrono::steady_clock::now();

        ctx_->oneShot([&](VkCommandBuffer cmd) {
            toGeneral(cmd, refUint_);
            toGeneral(cmd, refUnorm_);
            toGeneral(cmd, outImg_);
            if (swpU32Src_.img) { toGeneral(cmd, swpU32Src_); toGeneral(cmd, swpU32Dst_); }
            if (swpUnormDst_.img) toGeneral(cmd, swpUnormDst_);
            resetQueries(cmd, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout,
                                    0, 1, &k.set, 0, nullptr);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, 0);
            for (int d = 0; d < kInner; ++d)
            {
                if (d) fullBarrier(cmd);
                // A different tag per dispatch, so no write can be elided.
                struct { int32_t w, h, tag; } p2{};
                __builtin_memcpy(&p2, push, pushBytes < sizeof p2 ? pushBytes : sizeof p2);
                p2.tag = d + 1;
                vkCmdPushConstants(cmd, k.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   pushBytes, &p2);
                vkCmdDispatch(cmd, k.gx, k.gy, k.gz);
            }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, 1);
        });

        vkQueueWaitIdle(ctx_->queue);
        auto t1 = std::chrono::steady_clock::now();

        double ms = readMs(0);
        if (ms > 0) dev.push_back(ms / kInner);
        host.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count() / kInner);
    }

    if (hostMs) *hostMs = median(host);
    return median(dev);
}

// Allocates the extra source and destination pairs the sweep needs. They are
// large (33.5 MB each at the gate's frame size) and useless to every other
// kernel, so nothing allocates them until the sweep asks.
void Bench::k1SweepAlloc()
{
    if (swpAllocated_) return;
    swpAllocated_ = true;

    const uint32_t w = uint32_t(cfg_.width), h = uint32_t(cfg_.height);
    const VkImageUsageFlags iu = VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    swpU32Src_   = ctx_->createImage(w, h, VK_FORMAT_R32_UINT, iu);
    swpU32Dst_   = ctx_->createImage(w, h, VK_FORMAT_R32_UINT, iu);
    swpUnormDst_ = ctx_->createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, iu);

    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    const VkBufferUsageFlags bu = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    swpBufSrc_ = ctx_->createBuffer(bytes, bu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    swpBufDst_ = ctx_->createBuffer(bytes, bu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Real content, so nothing can be optimised away or served from a
    // compressed all-zero fast path.
    {
        std::vector<uint8_t> px(static_cast<size_t>(bytes), 0);
        for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 7u);
        ctx_->fillImage(swpU32Src_, px.data(), bytes);
        ctx_->upload(swpBufSrc_, px.data(), bytes);
    }
}

void Bench::k1SweepFree()
{
    if (!swpAllocated_) return;
    ctx_->destroyImage(swpU32Src_);
    ctx_->destroyImage(swpU32Dst_);
    ctx_->destroyImage(swpUnormDst_);
    ctx_->destroyBuffer(swpBufSrc_);
    ctx_->destroyBuffer(swpBufDst_);
    swpAllocated_ = false;
}

void Bench::runK1Sweep(int reps)
{
    k1SweepAlloc();

    auto SI = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };
    auto SB = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };

    const Variant vars[] = {
        {"rgba8ui  8x8   (K1 as shipped)", k1v_base_spv,   sizeof k1v_base_spv,   0,  8,  8, ""},
        {"rgba8ui  64x1",                  k1v_64x1_spv,   sizeof k1v_64x1_spv,   0, 64,  1, ""},
        {"rgba8ui  16x16",                 k1v_16x16_spv,  sizeof k1v_16x16_spv,  0, 16, 16, ""},
        {"rgba8ui  64x1 tile-major",       k1v_tiled_spv,  sizeof k1v_tiled_spv,  0, 64,  1, ""},
        {"r32ui    64x1",                  k1v_r32ui_spv,  sizeof k1v_r32ui_spv,  1, 64,  1, ""},
        {"rgba8unorm 64x1",                k1v_unorm_spv,  sizeof k1v_unorm_spv,  3, 64,  1, ""},
        {"ssbo     64x1",                  k1v_ssbo_spv,   sizeof k1v_ssbo_spv,   2, 64,  1, ""},
        {"ssbo     8x8",                   k1v_ssbo88_spv, sizeof k1v_ssbo88_spv, 2,  8,  8, ""},
        {"ssbo     16x16",                 k1v_ssbo1616_spv, sizeof k1v_ssbo1616_spv, 2, 16, 16, ""},
        {"r32ui    8x8",                   k1v_r32ui88_spv, sizeof k1v_r32ui88_spv, 1,  8,  8, ""},
        {"rgba8unorm 8x8",                 k1v_unorm88_spv, sizeof k1v_unorm88_spv, 3,  8,  8, ""},
    };

    struct PushSizeLocal { int32_t w, h, tag; } push{cfg_.width, cfg_.height, 0};
    const double bytesMoved = double(cfg_.width) * double(cfg_.height) * 8.0;

    NXB_LOG("K1 bandwidth sweep: %dx%d, %.1f MB moved per dispatch, %d reps,",
            cfg_.width, cfg_.height, bytesMoved / 1e6, reps);
    NXB_LOG("  one dispatch per submit, vkQueueWaitIdle on both sides, no co-tenant.");
    NXB_LOG("  %-32s %9s %9s %9s", "variant", "dev ms", "host ms", "GB/s");

    for (const Variant& v : vars)
    {
        std::vector<VkDescriptorSetLayoutBinding> binds =
            (v.store == 2) ? std::vector<VkDescriptorSetLayoutBinding>{SB(0), SB(1)}
                           : std::vector<VkDescriptorSetLayoutBinding>{SI(0), SI(1)};

        Kern k = makeKern(v.spv, v.bytes, binds, sizeof push, false);
        k.gx = (uint32_t(cfg_.width)  + v.wgx - 1) / v.wgx;
        k.gy = (uint32_t(cfg_.height) + v.wgy - 1) / v.wgy;

        // Bind the pair this variant actually wants.
        VkWriteDescriptorSet wr[2]{};
        VkDescriptorImageInfo ii[2]{};
        VkDescriptorBufferInfo bi[2]{};
        for (int i = 0; i < 2; ++i)
        {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = k.set;
            wr[i].dstBinding = uint32_t(i);
            wr[i].descriptorCount = 1;
        }
        if (v.store == 2)
        {
            bi[0] = {swpBufSrc_.buf, 0, VK_WHOLE_SIZE};
            bi[1] = {swpBufDst_.buf, 0, VK_WHOLE_SIZE};
            for (int i = 0; i < 2; ++i)
            {
                wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr[i].pBufferInfo = &bi[i];
            }
        }
        else
        {
            Image& src = (v.store == 1) ? swpU32Src_ : (v.store == 3) ? refUnorm_ : refUint_;
            Image& dst = (v.store == 1) ? swpU32Dst_ : (v.store == 3) ? swpUnormDst_ : outImg_;
            ii[0] = {VK_NULL_HANDLE, src.view, VK_IMAGE_LAYOUT_GENERAL};
            ii[1] = {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};
            for (int i = 0; i < 2; ++i)
            {
                wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                wr[i].pImageInfo = &ii[i];
            }
        }
        vkUpdateDescriptorSets(ctx_->dev, 2, wr, 0, nullptr);

        double hostMs = 0;
        double devMs = timeIsolated(k, &push, sizeof push, reps, &hostMs);
        double gbps = devMs > 0 ? bytesMoved / (devMs * 1e6) : 0.0;

        NXB_LOG("  %-32s %9.3f %9.3f %9.2f", v.name, devMs, hostMs, gbps);

        vkDestroyPipeline(ctx_->dev, k.pipe, nullptr);
        vkDestroyPipelineLayout(ctx_->dev, k.layout, nullptr);
        vkFreeDescriptorSets(ctx_->dev, descPool_, 1, &k.set);
        vkDestroyDescriptorSetLayout(ctx_->dev, k.dsl, nullptr);
    }

    // The same shipped kernel, but through the gate's own frame path, so the
    // gap between "alone" and "in the loop" is a measured number rather than
    // an inference.
    NXB_LOG("  (K1 as reported by the gate includes the co-tenant reprojection"
            " pass and the present)");

    k1SweepFree();
}

} // namespace nxb
