// external.cpp - platform-neutral bits plus the POSIX fd path (Linux/Monado).
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <cstring>
#include <utility>

namespace nxvc::vk {

ExternalImage::~ExternalImage() { destroy(); }

void ExternalImage::destroy() noexcept {
    if (!ctx_) return;
    VkDevice d = ctx_->device();
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
    if (view_) vkDestroyImageView(d, view_, nullptr);
    if (image_) vkDestroyImage(d, image_, nullptr);
    if (memory_) vkFreeMemory(d, memory_, nullptr);
    if (ycbcr_ && ctx_->fns().destroyYcbcrConversion)
        ctx_->fns().destroyYcbcrConversion(d, ycbcr_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    ycbcr_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

ExternalImage::ExternalImage(ExternalImage&& o) noexcept
    : ctx_(o.ctx_), image_(o.image_), view_(o.view_), memory_(o.memory_),
      sampler_(o.sampler_), ycbcr_(o.ycbcr_), width_(o.width_), height_(o.height_),
      format_(o.format_), external_format_(o.external_format_) {
    o.ctx_ = nullptr;
    o.image_ = VK_NULL_HANDLE;
    o.view_ = VK_NULL_HANDLE;
    o.memory_ = VK_NULL_HANDLE;
    o.sampler_ = VK_NULL_HANDLE;
    o.ycbcr_ = VK_NULL_HANDLE;
}

ExternalImage& ExternalImage::operator=(ExternalImage&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        image_ = o.image_;
        view_ = o.view_;
        memory_ = o.memory_;
        sampler_ = o.sampler_;
        ycbcr_ = o.ycbcr_;
        width_ = o.width_;
        height_ = o.height_;
        format_ = o.format_;
        external_format_ = o.external_format_;
        o.ctx_ = nullptr;
        o.image_ = VK_NULL_HANDLE;
        o.view_ = VK_NULL_HANDLE;
        o.memory_ = VK_NULL_HANDLE;
        o.sampler_ = VK_NULL_HANDLE;
        o.ycbcr_ = VK_NULL_HANDLE;
    }
    return *this;
}

ExternalSupport externalSupport(const Context& ctx) {
    const Probe& p = ctx.probe();
    ExternalSupport s;
    s.memory_fd = (p.caps & NXVC_VK_CAP_EXTERNAL_MEMORY_FD) && ctx.fns().getMemoryFd;
    s.dmabuf = s.memory_fd && ctx.hasDeviceExtension(
                                  VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    s.semaphore_fd =
        (p.caps & NXVC_VK_CAP_EXTERNAL_SEMAPHORE_FD) && ctx.fns().importSemaphoreFd;
    s.sync_fd = s.semaphore_fd;
    s.memory_win32 = (p.caps & NXVC_VK_CAP_EXTERNAL_MEMORY_WIN32) != 0;
    s.semaphore_win32 = (p.caps & NXVC_VK_CAP_EXTERNAL_SEM_WIN32) != 0;
    s.ahardware_buffer = (p.caps & NXVC_VK_CAP_ANDROID_HW_BUFFER) != 0;
    s.ycbcr_conversion = (p.caps & NXVC_VK_CAP_YCBCR_CONVERSION) != 0 &&
                         ctx.fns().createYcbcrConversion != nullptr;
#if !defined(__linux__) && !defined(__ANDROID__)
    s.memory_fd = s.dmabuf = s.semaphore_fd = s.sync_fd = false;
#endif
#if !defined(_WIN32)
    s.memory_win32 = s.semaphore_win32 = false;
#endif
#if !defined(__ANDROID__)
    s.ahardware_buffer = false;
#endif
    return s;
}

std::string ExternalSupport::toString() const {
    std::string o;
    const auto add = [&](const char* n, bool v) {
        if (!v) return;
        if (!o.empty()) o += ", ";
        o += n;
    };
    add("memory_fd", memory_fd);
    add("dmabuf", dmabuf);
    add("semaphore_fd", semaphore_fd);
    add("sync_fd", sync_fd);
    add("memory_win32", memory_win32);
    add("semaphore_win32", semaphore_win32);
    add("ahardware_buffer", ahardware_buffer);
    add("ycbcr_conversion", ycbcr_conversion);
    return o.empty() ? std::string("(none)") : o;
}

std::vector<const char*> externalDeviceExtensions(bool want_fd, bool want_ahb,
                                                  bool want_win32) {
    std::vector<const char*> out;
#if defined(__linux__) || defined(__ANDROID__)
    if (want_fd) {
        out.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        out.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        out.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        out.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    }
#else
    (void)want_fd;
#endif
#if defined(__ANDROID__)
    if (want_ahb) {
        out.push_back("VK_ANDROID_external_memory_android_hardware_buffer");
        out.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        out.push_back(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
    }
#else
    (void)want_ahb;
#endif
#if defined(_WIN32)
    if (want_win32) {
        out.push_back("VK_KHR_external_memory_win32");
        out.push_back("VK_KHR_external_semaphore_win32");
        out.push_back(VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
    }
#else
    (void)want_win32;
#endif
    return out;
}

namespace external {

#if defined(__linux__) || defined(__ANDROID__)

ImportedBuffer importBufferFd(const Context& ctx, int fd, VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              VkExternalMemoryHandleTypeFlagBits type) {
    if (!ctx.fns().getMemoryFdProperties)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "VK_KHR_external_memory_fd not enabled");

    ImportedBuffer out;
    out.size = size;

    VkExternalMemoryBufferCreateInfo embci{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    embci.handleTypes = type;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &embci;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    NXVC_VK_CHECK(vkCreateBuffer(ctx.device(), &bci, nullptr, &out.buffer));

    VkMemoryFdPropertiesKHR fdprops{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    NXVC_VK_CHECK(ctx.fns().getMemoryFdProperties(ctx.device(), type, fd, &fdprops));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx.device(), out.buffer, &req);
    const uint32_t bits = req.memoryTypeBits & fdprops.memoryTypeBits;
    if (bits == 0) {
        vkDestroyBuffer(ctx.device(), out.buffer, nullptr);
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "imported fd has no memory type in common with the buffer");
    }

    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.handleType = type;
    imp.fd = fd;  // Vulkan takes ownership on success
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.buffer = out.buffer;
    imp.pNext = &ded;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx.findMemoryType(bits, 0);
    const VkResult r = vkAllocateMemory(ctx.device(), &mai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(ctx.device(), out.buffer, nullptr);
        throwVk(r, "vkAllocateMemory(import fd)", __FILE__, __LINE__);
    }
    NXVC_VK_CHECK(vkBindBufferMemory(ctx.device(), out.buffer, out.memory, 0));
    return out;
}

void destroyImportedBuffer(const Context& ctx, ImportedBuffer& b) noexcept {
    if (b.buffer) vkDestroyBuffer(ctx.device(), b.buffer, nullptr);
    if (b.memory) vkFreeMemory(ctx.device(), b.memory, nullptr);
    b = ImportedBuffer{};
}

ExternalImage importImageFd(const Context& ctx, int fd, uint32_t width,
                            uint32_t height, VkFormat format,
                            VkImageUsageFlags usage,
                            std::optional<uint64_t> drm_modifier,
                            VkExternalMemoryHandleTypeFlagBits type) {
    if (!ctx.fns().getMemoryFdProperties)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "VK_KHR_external_memory_fd not enabled");

    ExternalImage out;
    out.ctx_ = &ctx;
    out.width_ = width;
    out.height_ = height;
    out.format_ = format;

    VkExternalMemoryImageCreateInfo emici{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = type;

    VkImageDrmFormatModifierExplicitCreateInfoEXT drm{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    VkSubresourceLayout plane{};

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &emici;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (drm_modifier && ctx.hasDeviceExtension(
                            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) {
        // One plane: the compositor hands us a single-plane RGBA target.
        // Multi-plane dma-bufs would need one VkSubresourceLayout per plane.
        plane.offset = 0;
        plane.rowPitch = static_cast<VkDeviceSize>(width) * 4;
        drm.drmFormatModifier = *drm_modifier;
        drm.drmFormatModifierPlaneCount = 1;
        drm.pPlaneLayouts = &plane;
        drm.pNext = &emici;
        ici.pNext = &drm;
        ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    } else {
        ici.tiling = VK_IMAGE_TILING_LINEAR;
    }
    NXVC_VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &out.image_));

    VkMemoryFdPropertiesKHR fdprops{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    NXVC_VK_CHECK(ctx.fns().getMemoryFdProperties(ctx.device(), type, fd, &fdprops));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), out.image_, &req);
    const uint32_t bits = req.memoryTypeBits & fdprops.memoryTypeBits;
    if (bits == 0)
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "imported dma-buf has no memory type in common with the image");

    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.handleType = type;
    imp.fd = fd;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = out.image_;
    imp.pNext = &ded;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx.findMemoryType(bits, 0);
    NXVC_VK_CHECK(vkAllocateMemory(ctx.device(), &mai, nullptr, &out.memory_));
    NXVC_VK_CHECK(vkBindImageMemory(ctx.device(), out.image_, out.memory_, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out.image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NXVC_VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &out.view_));
    return out;
}

int exportMemoryFd(const Context& ctx, VkDeviceMemory memory,
                   VkExternalMemoryHandleTypeFlagBits type) {
    if (!ctx.fns().getMemoryFd)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkGetMemoryFdKHR absent");
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.memory = memory;
    gi.handleType = type;
    int fd = -1;
    NXVC_VK_CHECK(ctx.fns().getMemoryFd(ctx.device(), &gi, &fd));
    return fd;
}

void importSemaphoreFd(const Context& ctx, VkSemaphore sem, int fd,
                       VkExternalSemaphoreHandleTypeFlagBits type, bool temporary) {
    if (!ctx.fns().importSemaphoreFd)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkImportSemaphoreFdKHR absent");
    VkImportSemaphoreFdInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    ii.semaphore = sem;
    ii.handleType = type;
    ii.fd = fd;
    // SYNC_FD import is *required* by the spec to be temporary; asking for
    // permanent there is a validation error, so force it.
    ii.flags = (temporary || type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)
                   ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT
                   : 0;
    NXVC_VK_CHECK(ctx.fns().importSemaphoreFd(ctx.device(), &ii));
}

int exportSemaphoreFd(const Context& ctx, VkSemaphore sem,
                      VkExternalSemaphoreHandleTypeFlagBits type) {
    if (!ctx.fns().getSemaphoreFd)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkGetSemaphoreFdKHR absent");
    VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    gi.semaphore = sem;
    gi.handleType = type;
    int fd = -1;
    NXVC_VK_CHECK(ctx.fns().getSemaphoreFd(ctx.device(), &gi, &fd));
    return fd;
}

#endif  // __linux__ || __ANDROID__

}  // namespace external
}  // namespace nxvc::vk
