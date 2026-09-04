// pipeline.cpp - shader modules, specialization constants, compute pipelines,
// descriptor plumbing, a file-backed pipeline cache.
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace nxvc::vk {

// ----------------------------------------------------------- SpecConstants
SpecConstants& SpecConstants::put(uint32_t id, uint32_t raw) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].constantID == id) {
            data_[i] = raw;
            return *this;
        }
    }
    entries_.push_back(VkSpecializationMapEntry{
        id, static_cast<uint32_t>(data_.size() * sizeof(uint32_t)), sizeof(uint32_t)});
    data_.push_back(raw);
    return *this;
}

SpecConstants& SpecConstants::set(uint32_t id, uint32_t v) { return put(id, v); }

SpecConstants& SpecConstants::set(uint32_t id, int32_t v) {
    uint32_t raw;
    std::memcpy(&raw, &v, 4);
    return put(id, raw);
}

SpecConstants& SpecConstants::set(uint32_t id, float v) {
    uint32_t raw;
    std::memcpy(&raw, &v, 4);
    return put(id, raw);
}

SpecConstants& SpecConstants::set(uint32_t id, bool v) {
    return put(id, v ? 1u : 0u);  // VkBool32
}

VkSpecializationInfo SpecConstants::info() const noexcept {
    VkSpecializationInfo si{};
    si.mapEntryCount = static_cast<uint32_t>(entries_.size());
    si.pMapEntries = entries_.data();
    si.dataSize = data_.size() * sizeof(uint32_t);
    si.pData = data_.data();
    return si;
}

// ------------------------------------------------------------ ShaderModule
ShaderModule::ShaderModule(const Context& ctx, std::span<const uint32_t> code,
                           std::string debug_name)
    : ctx_(&ctx) {
    if (code.empty()) throw Error(NXVC_VK_ERR_ARG, "empty SPIR-V blob");
    if (code[0] != 0x07230203u)
        throw Error(NXVC_VK_ERR_ARG, "blob is not SPIR-V (bad magic)");
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    NXVC_VK_CHECK(vkCreateShaderModule(ctx.device(), &ci, nullptr, &module_));
    (void)debug_name;
}

void ShaderModule::destroy() noexcept {
    if (ctx_ && module_) vkDestroyShaderModule(ctx_->device(), module_, nullptr);
    module_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

ShaderModule::~ShaderModule() { destroy(); }

ShaderModule::ShaderModule(ShaderModule&& o) noexcept : ctx_(o.ctx_), module_(o.module_) {
    o.ctx_ = nullptr;
    o.module_ = VK_NULL_HANDLE;
}

ShaderModule& ShaderModule::operator=(ShaderModule&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        module_ = o.module_;
        o.ctx_ = nullptr;
        o.module_ = VK_NULL_HANDLE;
    }
    return *this;
}

// ------------------------------------------------------ DescriptorSetLayout
DescriptorSetLayout::DescriptorSetLayout(
    const Context& ctx, std::span<const VkDescriptorSetLayoutBinding> bindings,
    VkDescriptorSetLayoutCreateFlags flags)
    : ctx_(&ctx) {
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.flags = flags;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    NXVC_VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &layout_));
}

void DescriptorSetLayout::destroy() noexcept {
    if (ctx_ && layout_) vkDestroyDescriptorSetLayout(ctx_->device(), layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

DescriptorSetLayout::~DescriptorSetLayout() { destroy(); }

DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout&& o) noexcept
    : ctx_(o.ctx_), layout_(o.layout_) {
    o.ctx_ = nullptr;
    o.layout_ = VK_NULL_HANDLE;
}

DescriptorSetLayout& DescriptorSetLayout::operator=(DescriptorSetLayout&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        layout_ = o.layout_;
        o.ctx_ = nullptr;
        o.layout_ = VK_NULL_HANDLE;
    }
    return *this;
}

// ---------------------------------------------------------- ComputePipeline
ComputePipeline::ComputePipeline(const Context& ctx, const ComputePipelineDesc& desc,
                                 VkPipelineCache cache)
    : ctx_(&ctx) {
    const Probe& p = ctx.probe();

    // Decide the subgroup size first: the shader is told the same number it
    // will actually run at, which is the whole point of 3.2.6's spec-constant
    // rule.
    uint32_t pin = desc.required_subgroup_size;
    if (pin == ComputePipelineDesc::kAuto) pin = p.required_subgroup_size;
    const bool can_pin = (p.caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL) && pin != 0 &&
                         pin >= p.subgroup_size_min && pin <= p.subgroup_size_max;
    if (!can_pin) pin = 0;
    subgroup_size_ = pin ? pin : p.subgroup_size;
    if (subgroup_size_ == 0) subgroup_size_ = p.subgroup_size_min;

    SpecConstants spec = desc.spec;
    if (desc.inject_standard_spec) {
        spec.set(kSpecSubgroupSize, subgroup_size_);
        spec.set(kSpecTileSize, desc.tile_size);
        spec.set(kSpecClusterWidth, uint32_t{NXVC_VK_CLUSTER_WIDTH});
        spec.set(kSpecTenBit, desc.ten_bit ? 1u : 0u);
    }
    const VkSpecializationInfo sinfo = spec.info();

    ShaderModule module(ctx, desc.spirv, desc.debug_name);

    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = static_cast<uint32_t>(desc.set_layouts.size());
    lci.pSetLayouts = desc.set_layouts.data();
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, desc.push_constant_size};
    if (desc.push_constant_size) {
        if (desc.push_constant_size > p.max_push_constants_size)
            throw Error(NXVC_VK_ERR_UNSUPPORTED,
                        "push constant block exceeds maxPushConstantsSize");
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pcr;
    }
    NXVC_VK_CHECK(vkCreatePipelineLayout(ctx.device(), &lci, nullptr, &layout_));

    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsi{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rsi.requiredSubgroupSize = pin;

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module.handle();
    ci.stage.pName = desc.entry_point;
    ci.stage.pSpecializationInfo = spec.empty() ? nullptr : &sinfo;
    if (pin) ci.stage.pNext = &rsi;
    // REQUIRE_FULL_SUBGROUPS where 3.2.6 asks for it: it guarantees every
    // subgroup in the workgroup is fully populated, which is what makes the
    // ballot masks of the cluster emulation total.
    if (desc.require_full_subgroups && (p.caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL))
        ci.stage.flags |=
            VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    ci.layout = layout_;
    NXVC_VK_CHECK(
        vkCreateComputePipelines(ctx.device(), cache, 1, &ci, nullptr, &pipeline_));
}

void ComputePipeline::destroy() noexcept {
    if (!ctx_) return;
    if (pipeline_) vkDestroyPipeline(ctx_->device(), pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(ctx_->device(), layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

ComputePipeline::~ComputePipeline() { destroy(); }

ComputePipeline::ComputePipeline(ComputePipeline&& o) noexcept
    : ctx_(o.ctx_), pipeline_(o.pipeline_), layout_(o.layout_),
      subgroup_size_(o.subgroup_size_) {
    o.ctx_ = nullptr;
    o.pipeline_ = VK_NULL_HANDLE;
    o.layout_ = VK_NULL_HANDLE;
}

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        pipeline_ = o.pipeline_;
        layout_ = o.layout_;
        subgroup_size_ = o.subgroup_size_;
        o.ctx_ = nullptr;
        o.pipeline_ = VK_NULL_HANDLE;
        o.layout_ = VK_NULL_HANDLE;
    }
    return *this;
}

void ComputePipeline::bind(VkCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
}

void ComputePipeline::push(VkCommandBuffer cmd, const void* data, uint32_t size,
                           uint32_t offset) const {
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, offset, size, data);
}

// ------------------------------------------------------------ PipelineCache
PipelineCache::PipelineCache(const Context& ctx, const std::string& path)
    : ctx_(&ctx), path_(path) {
    std::vector<char> blob;
    if (std::ifstream f{path, std::ios::binary}) {
        f.seekg(0, std::ios::end);
        const auto n = f.tellg();
        if (n > 0) {
            blob.resize(static_cast<size_t>(n));
            f.seekg(0);
            f.read(blob.data(), n);
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    // A cache blob from another driver or device is rejected by the driver's
    // own header check; a corrupt one can crash it, so validate the header
    // fields we can before handing it over.
    if (blob.size() >= 32) {
        uint32_t header_size = 0, header_version = 0;
        std::memcpy(&header_size, blob.data(), 4);
        std::memcpy(&header_version, blob.data() + 4, 4);
        if (header_size > blob.size() || header_version != 1) {
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
        }
    } else if (!blob.empty()) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
    }
    NXVC_VK_CHECK(vkCreatePipelineCache(ctx.device(), &ci, nullptr, &cache_));
}

void PipelineCache::save() const {
    if (!cache_ || path_.empty()) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(ctx_->device(), cache_, &n, nullptr) != VK_SUCCESS || !n)
        return;
    std::vector<char> blob(n);
    if (vkGetPipelineCacheData(ctx_->device(), cache_, &n, blob.data()) != VK_SUCCESS)
        return;
    if (std::ofstream f{path_, std::ios::binary}) f.write(blob.data(), static_cast<std::streamsize>(n));
}

void PipelineCache::destroy() noexcept {
    if (ctx_ && cache_) vkDestroyPipelineCache(ctx_->device(), cache_, nullptr);
    cache_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

PipelineCache::~PipelineCache() { destroy(); }

PipelineCache::PipelineCache(PipelineCache&& o) noexcept
    : ctx_(o.ctx_), cache_(o.cache_), path_(std::move(o.path_)) {
    o.ctx_ = nullptr;
    o.cache_ = VK_NULL_HANDLE;
}

PipelineCache& PipelineCache::operator=(PipelineCache&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        cache_ = o.cache_;
        path_ = std::move(o.path_);
        o.ctx_ = nullptr;
        o.cache_ = VK_NULL_HANDLE;
    }
    return *this;
}

// ------------------------------------------------------------ DescriptorSet
DescriptorSet::DescriptorSet(const Context& ctx, VkDescriptorSetLayout layout,
                             std::span<const VkDescriptorPoolSize> sizes)
    : ctx_(&ctx) {
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pci.pPoolSizes = sizes.data();
    NXVC_VK_CHECK(vkCreateDescriptorPool(ctx.device(), &pci, nullptr, &pool_));

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    NXVC_VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &ai, &set_));

    // Reserve so the pointers handed to VkWriteDescriptorSet stay valid.
    buffer_infos_.reserve(64);
    image_infos_.reserve(64);
    writes_.reserve(64);
}

DescriptorSet& DescriptorSet::write(uint32_t binding, VkDescriptorType type,
                                    const VkDescriptorBufferInfo& info) {
    if (buffer_infos_.size() == buffer_infos_.capacity())
        throw Error(NXVC_VK_ERR_ARG, "too many descriptor writes queued");
    buffer_infos_.push_back(info);
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &buffer_infos_.back();
    writes_.push_back(w);
    return *this;
}

DescriptorSet& DescriptorSet::write(uint32_t binding, VkDescriptorType type,
                                    const VkDescriptorImageInfo& info) {
    if (image_infos_.size() == image_infos_.capacity())
        throw Error(NXVC_VK_ERR_ARG, "too many descriptor writes queued");
    image_infos_.push_back(info);
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = &image_infos_.back();
    writes_.push_back(w);
    return *this;
}

void DescriptorSet::flush() {
    if (writes_.empty()) return;
    vkUpdateDescriptorSets(ctx_->device(), static_cast<uint32_t>(writes_.size()),
                           writes_.data(), 0, nullptr);
    writes_.clear();
    buffer_infos_.clear();
    image_infos_.clear();
}

void DescriptorSet::destroy() noexcept {
    if (ctx_ && pool_) vkDestroyDescriptorPool(ctx_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

DescriptorSet::~DescriptorSet() { destroy(); }

DescriptorSet::DescriptorSet(DescriptorSet&& o) noexcept
    : ctx_(o.ctx_), pool_(o.pool_), set_(o.set_), writes_(std::move(o.writes_)),
      buffer_infos_(std::move(o.buffer_infos_)), image_infos_(std::move(o.image_infos_)) {
    o.ctx_ = nullptr;
    o.pool_ = VK_NULL_HANDLE;
    o.set_ = VK_NULL_HANDLE;
}

DescriptorSet& DescriptorSet::operator=(DescriptorSet&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        pool_ = o.pool_;
        set_ = o.set_;
        writes_ = std::move(o.writes_);
        buffer_infos_ = std::move(o.buffer_infos_);
        image_infos_ = std::move(o.image_infos_);
        o.ctx_ = nullptr;
        o.pool_ = VK_NULL_HANDLE;
        o.set_ = VK_NULL_HANDLE;
    }
    return *this;
}

}  // namespace nxvc::vk
