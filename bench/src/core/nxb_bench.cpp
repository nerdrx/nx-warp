#include "nxb_bench.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include "nxb_spv.h"

namespace nxb { void genCoefficients(std::vector<int16_t>&, int, uint64_t); }

namespace nxb {

// ------------------------------------------------------------------ names
static const char* kNames[KID_COUNT] = {
    "K1 copy", "K2 gather-4", "K2b sampler", "K3 idct", "K4 rans", "K5 full", "K6 hybrid"
};
static const char* kDescs[KID_COUNT] = {
    "8.39 Mpixel RGBA8 image to image via compute",
    "warp coordinate + bit-exact 4-load bilinear + store",
    "same with one sampler tap (informational)",
    "Pass B without prediction: load, dequant, 8x8 int DCT through LDS, store",
    "Pass A on random symbol streams, 8 lanes per tile, all 2048 tiles",
    "Pass A + Pass B as designed",
    "MediaCodec HEVC base imported from AHardwareBuffer, plus Pass C"
};
const char* kidName(int k) { return (k >= 0 && k < KID_COUNT) ? kNames[k] : "?"; }
const char* kidDesc(int k) { return (k >= 0 && k < KID_COUNT) ? kDescs[k] : "?"; }

double percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = p * double(v.size() - 1);
    size_t lo = size_t(idx);
    size_t hi = std::min(lo + 1, v.size() - 1);
    double f = idx - double(lo);
    return v[lo] * (1.0 - f) + v[hi] * f;
}

// ------------------------------------------------------------ push blocks
namespace {
struct PushSize      { int32_t w, h; };
struct PushWarp      { int32_t w, h, tilesX; };
struct PushPassB     { int32_t w, h, tilesX, coefWords; };
struct PushRans      { int32_t tileCount, symsPerLane, bitWords, tabWords; };
struct PushRepro     { int32_t w, h; float jx, jy; };
struct PushPassC     { int32_t w, h, tilesX, deltaWords; };

// Deadzone quantiser step, PAPER 1.5: step = 2^(QP/6), tabulated x16 so the
// dequant shift by 4 lands on exactly q * step.
int qstep16(int qp)
{
    static const int base[6] = {16, 18, 20, 23, 25, 29};
    int shift = qp / 6;
    if (shift > 20) shift = 20;
    return base[qp % 6] << shift;
}
} // namespace

// ------------------------------------------------------------------- init
bool Bench::init(VkCtx& ctx, const Config& cfg)
{
    ctx_ = &ctx;
    cfg_ = cfg;

    tilesX_ = cfg.width  / 64;
    tilesY_ = cfg.height / 64;
    tileCount_ = tilesX_ * tilesY_;

    // 0.5 symbols/pixel over a 64x64 tile is 2048 symbols, 256 per lane.
    symsPerLane_ = int(cfg.symbolsPerPixel * 64.0 * 64.0) / kRansLanes;
    symsPerLane_ = (symsPerLane_ / 16) * 16;
    if (symsPerLane_ < 16) symsPerLane_ = 16;

    NXB_LOG("frame %dx%d, %d tiles (%dx%d), %d symbols/lane",
            cfg.width, cfg.height, tileCount_, tilesX_, tilesY_, symsPerLane_);

    const VkImageUsageFlags storageUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags sampledUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    refUint_  = ctx.createImage(uint32_t(cfg.width), uint32_t(cfg.height),
                                VK_FORMAT_R8G8B8A8_UINT, storageUsage);
    refUnorm_ = ctx.createImage(uint32_t(cfg.width), uint32_t(cfg.height),
                                VK_FORMAT_R8G8B8A8_UNORM, sampledUsage);
    outImg_   = ctx.createImage(uint32_t(cfg.width), uint32_t(cfg.height),
                                VK_FORMAT_R8G8B8A8_UINT, storageUsage);
    reproSrc_ = ctx.createImage(uint32_t(cfg.reproW), uint32_t(cfg.reproH),
                                VK_FORMAT_R8G8B8A8_UNORM,
                                sampledUsage | VK_IMAGE_USAGE_STORAGE_BIT);
    reproDst_ = ctx.createImage(uint32_t(cfg.reproW), uint32_t(cfg.reproH),
                                VK_FORMAT_R8G8B8A8_UNORM, storageUsage);

    bool wantK6 = (cfg.kernelMask & (1u << K6_HYBRID)) != 0;
    if (wantK6)
    {
        prevResid_ = ctx.createImage(uint32_t(cfg.width), uint32_t(cfg.height),
                                     VK_FORMAT_R8G8B8A8_UINT, storageUsage);
        newResid_  = ctx.createImage(uint32_t(cfg.width), uint32_t(cfg.height),
                                     VK_FORMAT_R8G8B8A8_UINT, storageUsage);
    }

    // ---------------------------------------------------------- image data
    // Content is irrelevant to timing (only addresses reach the cache), but a
    // smooth field keeps the images meaningful if they are ever dumped.
    {
        size_t n = size_t(cfg.width) * size_t(cfg.height);
        std::vector<uint8_t> px(n * 4);
        for (int y = 0; y < cfg.height; ++y)
            for (int x = 0; x < cfg.width; ++x)
            {
                size_t o = (size_t(y) * size_t(cfg.width) + size_t(x)) * 4;
                int v = ((x * 3 + y * 5) >> 3) ^ (x >> 4) ^ (y >> 5);
                px[o + 0] = uint8_t(v);
                px[o + 1] = uint8_t(128 + ((x >> 2) & 63) - 32);
                px[o + 2] = uint8_t(128 + ((y >> 2) & 63) - 32);
                px[o + 3] = 255;
            }
        ctx.fillImage(refUint_, px.data(), VkDeviceSize(px.size()));
        ctx.fillImage(refUnorm_, px.data(), VkDeviceSize(px.size()));
        if (wantK6)
        {
            std::fill(px.begin(), px.end(), uint8_t(128));
            ctx.fillImage(prevResid_, px.data(), VkDeviceSize(px.size()));
        }
    }
    {
        size_t n = size_t(cfg.reproW) * size_t(cfg.reproH) * 4;
        std::vector<uint8_t> px(n);
        for (size_t i = 0; i < n; ++i) px[i] = uint8_t(i * 7u);
        ctx.fillImage(reproSrc_, px.data(), VkDeviceSize(n));
    }

    // ---------------------------------------------------------- buffers
    const VkBufferUsageFlags ssbo =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkMemoryPropertyFlags devLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    // Coefficients: dense int16, 8 KB per tile, 16.8 MB per frame (PAPER 3.2.5)
    VkDeviceSize coefBytes = VkDeviceSize(tileCount_) * NXB_COEFS_PER_TILE_BYTES;
    coef_ = ctx.createBuffer(coefBytes, ssbo | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, devLocal);
    {
        // Sparse and DC-heavy: what a quantised residual actually looks like.
        std::vector<int16_t> c;
        genCoefficients(c, tileCount_, cfg.seed);
        ctx.upload(coef_, c.data(), VkDeviceSize(c.size() * sizeof(int16_t)));
    }

    // Dequant table: scale[QP][pos], one multiply and shift in the shader.
    deq_ = ctx.createBuffer(64 * 64 * sizeof(int32_t), ssbo, devLocal);
    {
        // Flat weighting matrix in v1; PAPER 1.5 reserves four built-ins.
        std::vector<int32_t> tab(64 * 64);
        for (int qp = 0; qp < 64; ++qp)
            for (int pos = 0; pos < 64; ++pos)
                tab[size_t(qp) * 64 + size_t(pos)] = (qstep16(qp) * 16) >> 4;
        ctx.upload(deq_, tab.data(), VkDeviceSize(tab.size() * sizeof(int32_t)));
    }

    // Tile records: four Q4 corner displacements from a smooth global warp,
    // so the gather-4 footprint looks like a pose warp rather than noise.
    tileRec_ = ctx.createBuffer(VkDeviceSize(tileCount_) * 32, ssbo, devLocal);
    {
        struct Rec { int32_t corner[4]; uint32_t qp, mode, flags, pad; };
        std::vector<Rec> recs{};
        recs.resize(size_t(tileCount_));
        auto disp = [&](double tx, double ty) {
            // +-6 pixels, smooth over the frame: a yaw/pitch delta plus a
            // little parallax. Q4 fixed point.
            double dx = 5.0 * std::sin(tx * 0.21 + 0.7) + 1.5 * std::cos(ty * 0.13);
            double dy = 4.0 * std::cos(ty * 0.17) - 1.0 * std::sin(tx * 0.09);
            int qx = int(std::lround(dx * 16.0));
            int qy = int(std::lround(dy * 16.0));
            return uint32_t(uint16_t(int16_t(qx))) | (uint32_t(uint16_t(int16_t(qy))) << 16);
        };
        for (int ty = 0; ty < tilesY_; ++ty)
            for (int tx = 0; tx < tilesX_; ++tx)
            {
                Rec& r = recs[size_t(ty) * size_t(tilesX_) + size_t(tx)];
                r.corner[0] = int32_t(disp(tx,     ty));
                r.corner[1] = int32_t(disp(tx + 1, ty));
                r.corner[2] = int32_t(disp(tx,     ty + 1));
                r.corner[3] = int32_t(disp(tx + 1, ty + 1));
                r.qp = uint32_t(cfg.qp);
                r.mode = 1;   // inter
                r.flags = 0;
                r.pad = 0;
            }
        ctx.upload(tileRec_, recs.data(), VkDeviceSize(recs.size() * sizeof(Rec)));
    }

    // rANS streams
    tables_.build();
    stream_ = ransBuildStreams(tables_, tileCount_, symsPerLane_, cfg.seed, false);
    NXB_LOG("rANS: %zu bytes total, %.1f B/tile, %.2f bits/symbol",
            stream_.payloadBytes, double(stream_.payloadBytes) / double(tileCount_),
            double(stream_.payloadBytes) * 8.0 / (double(tileCount_) * double(symsPerLane_ * kRansLanes)));

    bits_ = ctx.createBuffer(VkDeviceSize(stream_.words.size() * 4), ssbo, devLocal);
    ctx.upload(bits_, stream_.words.data(), VkDeviceSize(stream_.words.size() * 4));
    tileOff_ = ctx.createBuffer(VkDeviceSize(stream_.offsets.size() * 4), ssbo, devLocal);
    ctx.upload(tileOff_, stream_.offsets.data(), VkDeviceSize(stream_.offsets.size() * 4));
    {
        auto packed = tables_.packed();
        tab_ = ctx.createBuffer(VkDeviceSize(packed.size() * 4), ssbo, devLocal);
        ctx.upload(tab_, packed.data(), VkDeviceSize(packed.size() * 4));
    }

    if (wantK6)
    {
        size_t n = size_t(cfg.width) * size_t(cfg.height);
        delta_ = ctx.createBuffer(VkDeviceSize(n * 2), ssbo, devLocal);
        std::vector<int16_t> d(n);
        uint64_t s = cfg.seed ^ 0xabcdefull;
        for (size_t i = 0; i < n; ++i)
        {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            d[i] = int16_t(int32_t(s >> 40) % 24);
        }
        ctx.upload(delta_, d.data(), VkDeviceSize(n * 2));
    }

    // ---------------------------------------------------------- sampler
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.25f;
        NXB_VK(vkCreateSampler(ctx.dev, &si, nullptr, &linearSampler_));
    }

    // ---------------------------------------------------------- descriptors
    {
        VkDescriptorPoolSize sizes[3] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
        };
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 16;
        ci.poolSizeCount = 3;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        NXB_VK(vkCreateDescriptorPool(ctx.dev, &ci, nullptr, &descPool_));
    }

    auto SI = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };
    auto CS = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };
    auto SB = [](uint32_t b) {
        return VkDescriptorSetLayoutBinding{b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    };

    uint32_t gxPix = uint32_t((cfg.width  + 7) / 8);
    uint32_t gyPix = uint32_t((cfg.height + 7) / 8);

    kCopy_ = makeKern(k1_copy_spv, sizeof k1_copy_spv, {SI(0), SI(1)}, sizeof(PushSize), false);
    kCopy_.gx = gxPix; kCopy_.gy = gyPix;

    kWarp_ = makeKern(k2_warp_spv, sizeof k2_warp_spv, {SI(0), SI(1), SB(2)}, sizeof(PushWarp), false);
    kWarp_.gx = gxPix; kWarp_.gy = gyPix;

    kSampler_ = makeKern(k2b_sampler_spv, sizeof k2b_sampler_spv,
                         {CS(0), SI(1), SB(2)}, sizeof(PushWarp), false);
    kSampler_.gx = gxPix; kSampler_.gy = gyPix;

    kIdct_ = makeKern(passb_k3_spv, sizeof passb_k3_spv,
                      {SB(0), SB(1), SB(2), SI(3)}, sizeof(PushPassB), false);
    kIdct_.gx = uint32_t(tilesX_); kIdct_.gy = uint32_t(tilesY_);

    kPassB_ = makeKern(passb_k5_spv, sizeof passb_k5_spv,
                       {SB(0), SB(1), SB(2), SI(3), SI(4)}, sizeof(PushPassB), false);
    kPassB_.gx = uint32_t(tilesX_); kPassB_.gy = uint32_t(tilesY_);

    kRans_ = makeKern(k4_rans_spv, sizeof k4_rans_spv,
                      {SB(0), SB(1), SB(2), SB(3)}, sizeof(PushRans), true);
    kRans_.gx = uint32_t((tileCount_ + 7) / 8);

    kRepro_ = makeKern(reproject_spv, sizeof reproject_spv, {CS(0), SI(1)}, sizeof(PushRepro), false);
    kRepro_.gx = uint32_t((cfg.reproW + 7) / 8);
    kRepro_.gy = uint32_t((cfg.reproH + 7) / 8);

    kernOf_[K1_COPY]     = &kCopy_;
    kernOf_[K2_GATHER4]  = &kWarp_;
    kernOf_[K2B_SAMPLER] = &kSampler_;
    kernOf_[K3_IDCT]     = &kIdct_;
    kernOf_[K4_RANS]     = &kRans_;
    kernOf_[K5_FULL]     = &kPassB_;   // K5 records kRans_ then kPassB_
    kernOf_[K6_HYBRID]   = &kPassC_;

    writeSets();

    if (wantK6)
        setHybridBase(VK_NULL_HANDLE, VK_NULL_HANDLE);   // synthetic stand-in

    // ---------------------------------------------------------- queries
    {
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = kSlots * 2;
        NXB_VK(vkCreateQueryPool(ctx.dev, &ci, nullptr, &queryPool_));
    }
    return true;
}

Bench::Kern Bench::makeKern(const uint32_t* spv, size_t bytes,
                            const std::vector<VkDescriptorSetLayoutBinding>& binds,
                            uint32_t pushBytes, bool requireFullSubgroups)
{
    Kern k;
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = uint32_t(binds.size());
    dci.pBindings = binds.data();
    NXB_VK(vkCreateDescriptorSetLayout(ctx_->dev, &dci, nullptr, &k.dsl));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &k.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    NXB_VK(vkCreatePipelineLayout(ctx_->dev, &pli, nullptr, &k.layout));

    VkShaderModule mod = ctx_->shader(spv, bytes);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = k.layout;

    // PAPER 3.2.6: use REQUIRE_FULL_SUBGROUPS where offered. Only legal when
    // the local size is a multiple of the maximum subgroup size, so a 128-wide
    // part running a 64-thread group silently keeps the default behaviour --
    // which is fine, because the 8-lane clusters do not care.
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT reqSize{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT};
    if (requireFullSubgroups && ctx_->info.subgroupSizeControl)
    {
        uint32_t want = cfg_.forceSubgroupSize;
        if (want && want >= ctx_->info.subgroupMin && want <= ctx_->info.subgroupMax &&
            (want & (want - 1)) == 0 && (64u % want) == 0)
        {
            reqSize.requiredSubgroupSize = want;
            ci.stage.pNext = &reqSize;
            ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
            NXB_LOG("rANS kernel: forcing subgroup size %u", want);
        }
        else if (!want && ctx_->info.subgroupMax > 0 && (64u % ctx_->info.subgroupMax) == 0)
        {
            ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
        }
        else if (want)
        {
            NXB_LOG("requested subgroup size %u is not supported (min %u max %u); "
                    "using the driver default", want,
                    ctx_->info.subgroupMin, ctx_->info.subgroupMax);
        }
    }

    NXB_VK(vkCreateComputePipelines(ctx_->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &k.pipe));
    vkDestroyShaderModule(ctx_->dev, mod, nullptr);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &k.dsl;
    NXB_VK(vkAllocateDescriptorSets(ctx_->dev, &ai, &k.set));
    return k;
}

namespace {
VkWriteDescriptorSet wImg(VkDescriptorSet s, uint32_t b, VkDescriptorImageInfo* i)
{
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s; w.dstBinding = b; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = i;
    return w;
}
VkWriteDescriptorSet wSam(VkDescriptorSet s, uint32_t b, VkDescriptorImageInfo* i)
{
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s; w.dstBinding = b; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = i;
    return w;
}
VkWriteDescriptorSet wBuf(VkDescriptorSet s, uint32_t b, VkDescriptorBufferInfo* i)
{
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s; w.dstBinding = b; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = i;
    return w;
}
} // namespace

void Bench::writeSets()
{
    VkDescriptorImageInfo refU{VK_NULL_HANDLE, refUint_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo outI{VK_NULL_HANDLE, outImg_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo refS{linearSampler_, refUnorm_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo rpS {linearSampler_, reproSrc_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo rpD {VK_NULL_HANDLE, reproDst_.view, VK_IMAGE_LAYOUT_GENERAL};

    VkDescriptorBufferInfo coef{coef_.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo deq {deq_.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo rec {tileRec_.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bits{bits_.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo off {tileOff_.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo tab {tab_.buf, 0, VK_WHOLE_SIZE};

    std::vector<VkWriteDescriptorSet> w;
    w.push_back(wImg(kCopy_.set, 0, &refU));
    w.push_back(wImg(kCopy_.set, 1, &outI));

    w.push_back(wImg(kWarp_.set, 0, &refU));
    w.push_back(wImg(kWarp_.set, 1, &outI));
    w.push_back(wBuf(kWarp_.set, 2, &rec));

    w.push_back(wSam(kSampler_.set, 0, &refS));
    w.push_back(wImg(kSampler_.set, 1, &outI));
    w.push_back(wBuf(kSampler_.set, 2, &rec));

    w.push_back(wBuf(kIdct_.set, 0, &coef));
    w.push_back(wBuf(kIdct_.set, 1, &deq));
    w.push_back(wBuf(kIdct_.set, 2, &rec));
    w.push_back(wImg(kIdct_.set, 3, &outI));

    w.push_back(wBuf(kPassB_.set, 0, &coef));
    w.push_back(wBuf(kPassB_.set, 1, &deq));
    w.push_back(wBuf(kPassB_.set, 2, &rec));
    w.push_back(wImg(kPassB_.set, 3, &outI));
    w.push_back(wImg(kPassB_.set, 4, &refU));

    w.push_back(wBuf(kRans_.set, 0, &bits));
    w.push_back(wBuf(kRans_.set, 1, &off));
    w.push_back(wBuf(kRans_.set, 2, &tab));
    w.push_back(wBuf(kRans_.set, 3, &coef));

    w.push_back(wSam(kRepro_.set, 0, &rpS));
    w.push_back(wImg(kRepro_.set, 1, &rpD));

    vkUpdateDescriptorSets(ctx_->dev, uint32_t(w.size()), w.data(), 0, nullptr);
}

void Bench::setHybridBase(VkImageView view, VkSampler sampler)
{
    if (!prevResid_.img) return;   // K6 resources were never allocated

    // Rebuild Pass C so the base-layer binding can carry an immutable YCbCr
    // sampler (the AHardwareBuffer import path) or, with no external image
    // yet, a plain linear sampler over a synthetic stand-in.
    bool external = (view != VK_NULL_HANDLE && sampler != VK_NULL_HANDLE);
    hybridView_ = external ? view : reproSrc_.view;
    hybridSampler_ = external ? sampler : linearSampler_;

    if (kPassC_.pipe)
    {
        vkDeviceWaitIdle(ctx_->dev);
        vkDestroyPipeline(ctx_->dev, kPassC_.pipe, nullptr);
        vkDestroyPipelineLayout(ctx_->dev, kPassC_.layout, nullptr);
        vkFreeDescriptorSets(ctx_->dev, descPool_, 1, &kPassC_.set);
        vkDestroyDescriptorSetLayout(ctx_->dev, kPassC_.dsl, nullptr);
        kPassC_ = {};
    }

    VkSampler immutable = hybridSampler_;
    std::vector<VkDescriptorSetLayoutBinding> binds = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         external ? &immutable : nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    kPassC_ = makeKern(k6_passc_spv, sizeof k6_passc_spv, binds, sizeof(PushPassC), false);
    kPassC_.gx = uint32_t((cfg_.width + 7) / 8);
    kPassC_.gy = uint32_t((cfg_.height + 7) / 8);
    kernOf_[K6_HYBRID] = &kPassC_;

    VkDescriptorImageInfo base{external ? VK_NULL_HANDLE : hybridSampler_,
                               hybridView_, VK_IMAGE_LAYOUT_GENERAL};
    if (external) base.sampler = VK_NULL_HANDLE;   // immutable in the layout
    VkDescriptorImageInfo prev{VK_NULL_HANDLE, prevResid_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo nres{VK_NULL_HANDLE, newResid_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo outI{VK_NULL_HANDLE, outImg_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo rec{tileRec_.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo del{delta_.buf, 0, VK_WHOLE_SIZE};

    std::vector<VkWriteDescriptorSet> w = {
        wSam(kPassC_.set, 0, &base),
        wImg(kPassC_.set, 1, &prev),
        wImg(kPassC_.set, 2, &nres),
        wImg(kPassC_.set, 3, &outI),
        wBuf(kPassC_.set, 4, &rec),
        wBuf(kPassC_.set, 5, &del),
    };
    vkUpdateDescriptorSets(ctx_->dev, uint32_t(w.size()), w.data(), 0, nullptr);
    k6Ready_ = true;
}

// ------------------------------------------------------------- recording
void Bench::resetQueries(VkCommandBuffer cmd, uint32_t slot)
{
    vkCmdResetQueryPool(cmd, queryPool_, slot * 2, 2);
}

void Bench::recordCotenant(VkCommandBuffer cmd, float jx, float jy)
{
    toGeneral(cmd, reproSrc_);
    toGeneral(cmd, reproDst_);
    PushRepro p{cfg_.reproW, cfg_.reproH, jx, jy};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kRepro_.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kRepro_.layout,
                            0, 1, &kRepro_.set, 0, nullptr);
    vkCmdPushConstants(cmd, kRepro_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, kRepro_.gx, kRepro_.gy, 1);
    fullBarrier(cmd);
}

void Bench::recordKernel(VkCommandBuffer cmd, int kid, uint32_t slot)
{
    toGeneral(cmd, refUint_);
    toGeneral(cmd, refUnorm_);
    toGeneral(cmd, outImg_);
    if (prevResid_.img) { toGeneral(cmd, prevResid_); toGeneral(cmd, newResid_); }

    PushSize  pSize{cfg_.width, cfg_.height};
    PushWarp  pWarp{cfg_.width, cfg_.height, tilesX_};
    PushPassB pB{cfg_.width, cfg_.height, tilesX_,
                 int32_t(coef_.size / 4)};
    PushRans  pR{tileCount_, symsPerLane_,
                 int32_t(bits_.size / 4), int32_t(tab_.size / 4)};
    PushPassC pC{cfg_.width, cfg_.height, tilesX_, int32_t(delta_.size / 4)};

    auto bind = [&](Kern& k, const void* push, uint32_t n) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout,
                                0, 1, &k.set, 0, nullptr);
        vkCmdPushConstants(cmd, k.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, n, push);
        vkCmdDispatch(cmd, k.gx, k.gy, k.gz);
    };

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, slot * 2);

    switch (kid)
    {
    case K1_COPY:     bind(kCopy_,    &pSize, sizeof pSize); break;
    case K2_GATHER4:  bind(kWarp_,    &pWarp, sizeof pWarp); break;
    case K2B_SAMPLER: bind(kSampler_, &pWarp, sizeof pWarp); break;
    case K3_IDCT:     bind(kIdct_,    &pB,    sizeof pB);    break;
    case K4_RANS:     bind(kRans_,    &pR,    sizeof pR);    break;
    case K5_FULL:
        // Two dispatches, not one (PAPER 3.2.1). The timestamp pair spans both,
        // with the coefficient buffer round trip between them.
        bind(kRans_, &pR, sizeof pR);
        fullBarrier(cmd);
        bind(kPassB_, &pB, sizeof pB);
        break;
    case K6_HYBRID:   if (k6Ready_) bind(kPassC_, &pC, sizeof pC); break;
    default: break;
    }

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, slot * 2 + 1);
    fullBarrier(cmd);
}

double Bench::readMs(uint32_t slot)
{
    uint64_t ts[2] = {0, 0};
    VkResult r = vkGetQueryPoolResults(ctx_->dev, queryPool_, slot * 2, 2,
                                       sizeof ts, ts, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_SUCCESS) return -1.0;

    uint32_t valid = ctx_->info.timestampValidBits;
    if (valid > 0 && valid < 64)
    {
        uint64_t mask = (uint64_t(1) << valid) - 1;
        ts[0] &= mask;
        ts[1] &= mask;
        if (ts[1] < ts[0]) ts[1] += mask + 1;   // wrap
    }
    return double(ts[1] - ts[0]) * double(ctx_->info.timestampPeriod) * 1e-6;
}

bool Bench::available(int kid, std::string* why) const
{
    if (kid == K6_HYBRID && !k6Ready_)
    {
        if (why) *why = "K6 resources not allocated (pass --k6)";
        return false;
    }
    if ((kid == K4_RANS || kid == K5_FULL))
    {
        if (!ctx_->info.subgroupBallot)
        {
            if (why) *why = "subgroup ballot not supported";
            return false;
        }
        if (ctx_->info.subgroupSize < 8)
        {
            if (why) *why = "subgroup size below 8: unsupported for pure compute (PAPER 3.7)";
            return false;
        }
    }
    return true;
}

double Bench::bytesMoved(int kid) const
{
    double px = double(cfg_.width) * double(cfg_.height);
    switch (kid)
    {
    case K1_COPY:    return px * 8.0;    // one RGBA8 read + one write
    default:         return 0.0;
    }
}

void Bench::destroy()
{
    if (!ctx_) return;
    VkDevice d = ctx_->dev;
    vkDeviceWaitIdle(d);

    auto killKern = [&](Kern& k) {
        if (k.pipe) vkDestroyPipeline(d, k.pipe, nullptr);
        if (k.layout) vkDestroyPipelineLayout(d, k.layout, nullptr);
        if (k.dsl) vkDestroyDescriptorSetLayout(d, k.dsl, nullptr);
        k = {};
    };
    killKern(kCopy_); killKern(kWarp_); killKern(kSampler_); killKern(kIdct_);
    killKern(kRans_); killKern(kPassB_); killKern(kPassC_); killKern(kRepro_);

    if (queryPool_) vkDestroyQueryPool(d, queryPool_, nullptr);
    if (descPool_) vkDestroyDescriptorPool(d, descPool_, nullptr);
    if (linearSampler_) vkDestroySampler(d, linearSampler_, nullptr);

    ctx_->destroyImage(refUint_);  ctx_->destroyImage(refUnorm_);
    ctx_->destroyImage(outImg_);   ctx_->destroyImage(reproSrc_);
    ctx_->destroyImage(reproDst_);
    if (prevResid_.img) ctx_->destroyImage(prevResid_);
    if (newResid_.img)  ctx_->destroyImage(newResid_);

    ctx_->destroyBuffer(coef_);  ctx_->destroyBuffer(deq_);
    ctx_->destroyBuffer(tileRec_); ctx_->destroyBuffer(bits_);
    ctx_->destroyBuffer(tileOff_); ctx_->destroyBuffer(tab_);
    if (delta_.buf) ctx_->destroyBuffer(delta_);
    ctx_ = nullptr;
}

} // namespace nxb
