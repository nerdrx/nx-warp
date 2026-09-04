// resources.cpp - buffers, images, samplers.
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <algorithm>
#include <utility>

namespace nxvc::vk {
namespace {

struct MemoryChoice {
    VkMemoryPropertyFlags required;
    VkMemoryPropertyFlags preferred;
    bool map;
};

MemoryChoice memoryChoice(BufferKind k) {
    switch (k) {
        case BufferKind::DeviceLocal:
            return {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false};
        case BufferKind::HostCached:
            // 3.6: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED, so the network
            // thread's reads come out of the data cache.  Coherent is required
            // rather than preferred because the alternative is an explicit
            // invalidate on every read of a ring the GPU is still writing.
            return {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true};
        case BufferKind::HostUpload:
            return {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    0, true};
        case BufferKind::HostReadback:
            return {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true};
        case BufferKind::DeviceLocalHostVisible:
            return {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true};
    }
    return {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false};
}

void nameObject(const Context& ctx, uint64_t handle, VkObjectType type,
                const std::string& name) {
    if (name.empty()) return;
    static PFN_vkSetDebugUtilsObjectNameEXT fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        fn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(ctx.instance(), "vkSetDebugUtilsObjectNameEXT"));
    }
    if (!fn) return;
    VkDebugUtilsObjectNameInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name.c_str();
    fn(ctx.device(), &info);
}

}  // namespace

// ------------------------------------------------------------------ Buffer
Buffer::Buffer(const Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
               BufferKind kind, std::string debug_name)
    : ctx_(&ctx), size_(size) {
    if (size == 0) throw Error(NXVC_VK_ERR_ARG, "zero-size buffer");

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    NXVC_VK_CHECK(vkCreateBuffer(ctx.device(), &bci, nullptr, &buffer_));

    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkMemoryDedicatedRequirements ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    req.pNext = &ded;
    VkBufferMemoryRequirementsInfo2 rinfo{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
    rinfo.buffer = buffer_;
    vkGetBufferMemoryRequirements2(ctx.device(), &rinfo, &req);

    const MemoryChoice mc = memoryChoice(kind);
    const uint32_t type = ctx.findMemoryType(req.memoryRequirements.memoryTypeBits,
                                             mc.required, mc.preferred);
    const VkMemoryPropertyFlags flags = ctx.memoryProperties().memoryTypes[type].propertyFlags;
    cached_ = (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    coherent_ = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.memoryRequirements.size;
    mai.memoryTypeIndex = type;

    VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    VkMemoryAllocateFlagsInfo fai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    const void** tail = &mai.pNext;
    if (ded.prefersDedicatedAllocation || ded.requiresDedicatedAllocation) {
        dai.buffer = buffer_;
        *tail = &dai;
        tail = &dai.pNext;
    }
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        fai.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        *tail = &fai;
        tail = &fai.pNext;
    }
    NXVC_VK_CHECK(vkAllocateMemory(ctx.device(), &mai, nullptr, &memory_));
    NXVC_VK_CHECK(vkBindBufferMemory(ctx.device(), buffer_, memory_, 0));

    if (mc.map && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        NXVC_VK_CHECK(vkMapMemory(ctx.device(), memory_, 0, VK_WHOLE_SIZE, 0, &mapped_));

    nameObject(ctx, reinterpret_cast<uint64_t>(buffer_), VK_OBJECT_TYPE_BUFFER,
               debug_name);
}

void Buffer::destroy() noexcept {
    if (!ctx_) return;
    if (mapped_) vkUnmapMemory(ctx_->device(), memory_);
    if (buffer_) vkDestroyBuffer(ctx_->device(), buffer_, nullptr);
    if (memory_) vkFreeMemory(ctx_->device(), memory_, nullptr);
    buffer_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    mapped_ = nullptr;
    ctx_ = nullptr;
}

Buffer::~Buffer() { destroy(); }

Buffer::Buffer(Buffer&& o) noexcept
    : ctx_(o.ctx_), buffer_(o.buffer_), memory_(o.memory_), size_(o.size_),
      mapped_(o.mapped_), cached_(o.cached_), coherent_(o.coherent_) {
    o.ctx_ = nullptr;
    o.buffer_ = VK_NULL_HANDLE;
    o.memory_ = VK_NULL_HANDLE;
    o.mapped_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        buffer_ = o.buffer_;
        memory_ = o.memory_;
        size_ = o.size_;
        mapped_ = o.mapped_;
        cached_ = o.cached_;
        coherent_ = o.coherent_;
        o.ctx_ = nullptr;
        o.buffer_ = VK_NULL_HANDLE;
        o.memory_ = VK_NULL_HANDLE;
        o.mapped_ = nullptr;
    }
    return *this;
}

VkDeviceAddress Buffer::deviceAddress() const {
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = buffer_;
    return vkGetBufferDeviceAddress(ctx_->device(), &i);
}

void Buffer::flush(VkDeviceSize offset, VkDeviceSize size) const {
    if (coherent_ || !mapped_) return;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = memory_;
    r.offset = offset;
    r.size = size;
    NXVC_VK_CHECK(vkFlushMappedMemoryRanges(ctx_->device(), 1, &r));
}

void Buffer::invalidate(VkDeviceSize offset, VkDeviceSize size) const {
    if (coherent_ || !mapped_) return;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = memory_;
    r.offset = offset;
    r.size = size;
    NXVC_VK_CHECK(vkInvalidateMappedMemoryRanges(ctx_->device(), 1, &r));
}

VkBufferMemoryBarrier Buffer::barrier(VkAccessFlags src, VkAccessFlags dst) const noexcept {
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer_;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    return b;
}

// ------------------------------------------------------------------- Image
Image::Image(const Context& ctx, const ImageDesc& desc) : ctx_(&ctx), desc_(desc) {
    if (desc.width == 0 || desc.height == 0)
        throw Error(NXVC_VK_ERR_ARG, "zero-size image");

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = desc.format;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = desc.mip_levels;
    ici.arrayLayers = desc.layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = desc.tiling;
    ici.usage = desc.usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    NXVC_VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &image_));

    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkMemoryDedicatedRequirements ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    req.pNext = &ded;
    VkImageMemoryRequirementsInfo2 rinfo{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    rinfo.image = image_;
    vkGetImageMemoryRequirements2(ctx.device(), &rinfo, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.memoryRequirements.size;
    mai.memoryTypeIndex = ctx.findMemoryType(req.memoryRequirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    if (ded.prefersDedicatedAllocation || ded.requiresDedicatedAllocation) {
        dai.image = image_;
        mai.pNext = &dai;
    }
    NXVC_VK_CHECK(vkAllocateMemory(ctx.device(), &mai, nullptr, &memory_));
    NXVC_VK_CHECK(vkBindImageMemory(ctx.device(), image_, memory_, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_;
    vci.viewType = desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = desc.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mip_levels, 0,
                            desc.layers};
    NXVC_VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &view_));

    nameObject(ctx, reinterpret_cast<uint64_t>(image_), VK_OBJECT_TYPE_IMAGE,
               desc.debug_name);
}

void Image::destroy() noexcept {
    if (!ctx_) return;
    if (view_) vkDestroyImageView(ctx_->device(), view_, nullptr);
    if (image_) vkDestroyImage(ctx_->device(), image_, nullptr);
    if (memory_) vkFreeMemory(ctx_->device(), memory_, nullptr);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

Image::~Image() { destroy(); }

Image::Image(Image&& o) noexcept
    : ctx_(o.ctx_), desc_(std::move(o.desc_)), image_(o.image_), view_(o.view_),
      memory_(o.memory_), layout_(o.layout_) {
    o.ctx_ = nullptr;
    o.image_ = VK_NULL_HANDLE;
    o.view_ = VK_NULL_HANDLE;
    o.memory_ = VK_NULL_HANDLE;
}

Image& Image::operator=(Image&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        desc_ = std::move(o.desc_);
        image_ = o.image_;
        view_ = o.view_;
        memory_ = o.memory_;
        layout_ = o.layout_;
        o.ctx_ = nullptr;
        o.image_ = VK_NULL_HANDLE;
        o.view_ = VK_NULL_HANDLE;
        o.memory_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Image::transition(VkCommandBuffer cmd, VkImageLayout new_layout,
                       VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                       VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = layout_;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc_.mip_levels, 0,
                          desc_.layers};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    layout_ = new_layout;
}

// ----------------------------------------------------------------- Sampler
Sampler::Sampler(const Context& ctx, VkFilter filter, VkSamplerAddressMode mode,
                 const void* pnext)
    : ctx_(&ctx) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.pNext = pnext;
    sci.magFilter = filter;
    sci.minFilter = filter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = mode;
    sci.addressModeV = mode;
    sci.addressModeW = mode;
    sci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    NXVC_VK_CHECK(vkCreateSampler(ctx.device(), &sci, nullptr, &sampler_));
}

void Sampler::destroy() noexcept {
    if (ctx_ && sampler_) vkDestroySampler(ctx_->device(), sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

Sampler::~Sampler() { destroy(); }

Sampler::Sampler(Sampler&& o) noexcept : ctx_(o.ctx_), sampler_(o.sampler_) {
    o.ctx_ = nullptr;
    o.sampler_ = VK_NULL_HANDLE;
}

Sampler& Sampler::operator=(Sampler&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        sampler_ = o.sampler_;
        o.ctx_ = nullptr;
        o.sampler_ = VK_NULL_HANDLE;
    }
    return *this;
}

}  // namespace nxvc::vk
