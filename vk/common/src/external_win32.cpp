// external_win32.cpp - Win32 handle interop, docs/PAPER.md 3.8.
//
// The Windows helper gets per-eye ID3D11Texture2D from SteamVR, copies into a
// texture it created with D3D11_RESOURCE_MISC_SHARED_NTHANDLE, and hands the
// shared NT handle here.  Synchronisation is a D3D11.4 shared fence imported
// as a *timeline* semaphore (D3D12_FENCE handle type), which puts the D3D11
// copy and the Vulkan encode on one timeline.
//
// Compiled only on Windows; an empty translation unit elsewhere.  The build
// cross-compiles this with llvm-mingw as a compile-only check, so it must not
// depend on the D3D11 headers -- handles come in as void*.
#if defined(_WIN32)

#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <cstring>

namespace nxvc::vk::external {

ExternalImage importImageWin32(const Context& ctx, void* handle, uint32_t width,
                               uint32_t height, VkFormat format,
                               VkImageUsageFlags usage,
                               VkExternalMemoryHandleTypeFlagBits type) {
    if (!ctx.fns().getMemoryWin32HandleProperties)
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "VK_KHR_external_memory_win32 not enabled");

    ExternalImage out;
    out.ctx_ = &ctx;
    out.width_ = width;
    out.height_ = height;
    out.format_ = format;

    // 3.8: the import must be checked with vkGetPhysicalDeviceImageFormatProperties2
    // before creating the image, because a D3D11 texture's tiling is opaque to
    // us and a mismatched format is a device-lost rather than an error.
    VkPhysicalDeviceExternalImageFormatInfo eifi{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    eifi.handleType = type;
    VkPhysicalDeviceImageFormatInfo2 ifi{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    ifi.pNext = &eifi;
    ifi.format = format;
    ifi.type = VK_IMAGE_TYPE_2D;
    ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
    ifi.usage = usage;
    VkExternalImageFormatProperties eifp{
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ifp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    ifp.pNext = &eifp;
    NXVC_VK_CHECK(
        vkGetPhysicalDeviceImageFormatProperties2(ctx.physicalDevice(), &ifi, &ifp));
    if (!(eifp.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "driver cannot import this handle type for this format");
    const bool dedicated_required =
        (eifp.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;

    VkExternalMemoryImageCreateInfo emici{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = type;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &emici;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    NXVC_VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &out.image_));

    VkMemoryWin32HandlePropertiesKHR hprops{
        VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    NXVC_VK_CHECK(ctx.fns().getMemoryWin32HandleProperties(ctx.device(), type, handle,
                                                           &hprops));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), out.image_, &req);
    const uint32_t bits = req.memoryTypeBits & hprops.memoryTypeBits;
    if (bits == 0)
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "shared handle has no memory type in common with the image");

    VkImportMemoryWin32HandleInfoKHR imp{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imp.handleType = type;
    imp.handle = handle;  // not consumed; the caller still closes it
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = out.image_;
    // D3D11 texture imports are always dedicated in practice; honour the
    // driver's DEDICATED_ONLY bit and default to dedicated anyway.
    (void)dedicated_required;
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

void importSemaphoreWin32(const Context& ctx, VkSemaphore sem, void* handle,
                          VkExternalSemaphoreHandleTypeFlagBits type) {
    if (!ctx.fns().importSemaphoreWin32Handle)
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "VK_KHR_external_semaphore_win32 not enabled");
    VkImportSemaphoreWin32HandleInfoKHR ii{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    ii.semaphore = sem;
    ii.handleType = type;
    ii.handle = handle;
    // A D3D12_FENCE import is permanent: the fence outlives any one submit,
    // which is what makes it usable as the shared timeline of 3.8.
    ii.flags = 0;
    NXVC_VK_CHECK(ctx.fns().importSemaphoreWin32Handle(ctx.device(), &ii));
}

void* exportSemaphoreWin32(const Context& ctx, VkSemaphore sem,
                           VkExternalSemaphoreHandleTypeFlagBits type) {
    if (!ctx.fns().getSemaphoreWin32Handle)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkGetSemaphoreWin32HandleKHR absent");
    VkSemaphoreGetWin32HandleInfoKHR gi{
        VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    gi.semaphore = sem;
    gi.handleType = type;
    void* handle = nullptr;
    NXVC_VK_CHECK(ctx.fns().getSemaphoreWin32Handle(ctx.device(), &gi, &handle));
    return handle;
}

void* exportMemoryWin32(const Context& ctx, VkDeviceMemory memory,
                        VkExternalMemoryHandleTypeFlagBits type) {
    if (!ctx.fns().getMemoryWin32Handle)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkGetMemoryWin32HandleKHR absent");
    VkMemoryGetWin32HandleInfoKHR gi{
        VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gi.memory = memory;
    gi.handleType = type;
    void* handle = nullptr;
    NXVC_VK_CHECK(ctx.fns().getMemoryWin32Handle(ctx.device(), &gi, &handle));
    return handle;
}

}  // namespace nxvc::vk::external

#endif  // _WIN32
