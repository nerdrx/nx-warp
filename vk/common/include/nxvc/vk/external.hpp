// nxvc/vk/external.hpp - external memory and semaphore interop.
//
// Three platforms, three handle types, one shape:
//
//   Linux   (3.6 fallback, Monado out-of-process): VK_KHR_external_memory_fd
//           with OPAQUE_FD or DMA_BUF, VK_KHR_external_semaphore_fd with
//           OPAQUE_FD or SYNC_FD.
//   Android (3.5 hybrid): AHardwareBuffer from MediaCodec's AImageReader,
//           imported with an *external format* and a YCbCr sampler
//           conversion, plus the acquire sync fd as a binary semaphore.
//   Windows (3.8): the D3D11 shared texture through
//           VK_KHR_external_memory_win32 and the shared D3D11.4 fence
//           through VK_KHR_external_semaphore_win32 as a timeline.
//
// Nothing here is in the normative bit-exact path; it is all plumbing.
#pragma once

#include <nxvc/vk/context.hpp>
#include <nxvc/vk/resources.hpp>

#include <cstdint>
#include <optional>
#include <string>

#if defined(__ANDROID__)
struct AHardwareBuffer;
#endif

namespace nxvc::vk {

// Which interop paths this build + this device can actually do.
struct ExternalSupport {
    bool memory_fd = false;
    bool dmabuf = false;
    bool semaphore_fd = false;
    bool sync_fd = false;
    bool memory_win32 = false;
    bool semaphore_win32 = false;
    bool ahardware_buffer = false;
    bool ycbcr_conversion = false;
    [[nodiscard]] std::string toString() const;
};
ExternalSupport externalSupport(const Context& ctx);

// Forward declarations so the import factories can be friends of the class
// they build.  Default arguments are attached to the real declarations below.
class ExternalImage;
namespace external {
ExternalImage importImageFd(const Context& ctx, int fd, uint32_t width,
                            uint32_t height, VkFormat format,
                            VkImageUsageFlags usage,
                            std::optional<uint64_t> drm_modifier,
                            VkExternalMemoryHandleTypeFlagBits type);
#if defined(__ANDROID__)
ExternalImage importAHardwareBuffer(const Context& ctx, AHardwareBuffer* ahb,
                                    VkImageUsageFlags usage);
#endif
#if defined(_WIN32)
ExternalImage importImageWin32(const Context& ctx, void* handle, uint32_t width,
                               uint32_t height, VkFormat format,
                               VkImageUsageFlags usage,
                               VkExternalMemoryHandleTypeFlagBits type);
#endif
}  // namespace external

// The device extensions the given interop paths need, to be merged into a
// host's own vkCreateDevice call.  Platform-filtered at compile time.
std::vector<const char*> externalDeviceExtensions(bool want_fd, bool want_ahb,
                                                  bool want_win32);

// An image whose memory came from somewhere else.  Owns the VkImage, the
// VkImageView, the imported VkDeviceMemory and (Android) the YCbCr
// conversion; does not own the platform handle it was imported from.
class ExternalImage {
public:
    ExternalImage() = default;
    ~ExternalImage();
    ExternalImage(ExternalImage&&) noexcept;
    ExternalImage& operator=(ExternalImage&&) noexcept;
    ExternalImage(const ExternalImage&) = delete;
    ExternalImage& operator=(const ExternalImage&) = delete;

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkDeviceMemory memory() const noexcept { return memory_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
    [[nodiscard]] VkSamplerYcbcrConversion ycbcr() const noexcept { return ycbcr_; }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    // True when the image was created with VkExternalFormatANDROID and can
    // only ever be read through `sampler()` with the immutable conversion.
    [[nodiscard]] bool externalFormat() const noexcept { return external_format_; }

    // Only the platform import factories may populate one of these.
    friend ExternalImage external::importImageFd(const Context&, int, uint32_t,
                                                 uint32_t, VkFormat,
                                                 VkImageUsageFlags,
                                                 std::optional<uint64_t>,
                                                 VkExternalMemoryHandleTypeFlagBits);
#if defined(__ANDROID__)
    friend ExternalImage external::importAHardwareBuffer(const Context&,
                                                         AHardwareBuffer*,
                                                         VkImageUsageFlags);
#endif
#if defined(_WIN32)
    friend ExternalImage external::importImageWin32(const Context&, void*, uint32_t,
                                                    uint32_t, VkFormat,
                                                    VkImageUsageFlags,
                                                    VkExternalMemoryHandleTypeFlagBits);
#endif

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion ycbcr_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    bool external_format_ = false;
};

// Free functions rather than a class, because every platform's import is a
// one-shot factory with a different argument type.
namespace external {

// ------------------------------------------------------------------ POSIX fd
// Import an opaque-fd or dma-buf allocation as a plain buffer.  Vulkan takes
// ownership of `fd` on success (the spec's transfer-on-import rule); on
// failure the caller still owns it.
struct ImportedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};
ImportedBuffer importBufferFd(const Context& ctx, int fd, VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              VkExternalMemoryHandleTypeFlagBits type =
                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
void destroyImportedBuffer(const Context& ctx, ImportedBuffer& b) noexcept;

// Import a dma-buf as a sampled/storage image with an explicit DRM format
// modifier when the caller knows it, or with LINEAR tiling when it does not.
ExternalImage importImageFd(const Context& ctx, int fd, uint32_t width,
                            uint32_t height, VkFormat format,
                            VkImageUsageFlags usage,
                            std::optional<uint64_t> drm_modifier = std::nullopt,
                            VkExternalMemoryHandleTypeFlagBits type =
                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

// Export a buffer or image allocation we made ourselves.  Returns a new fd
// the caller owns and must close.
int exportMemoryFd(const Context& ctx, VkDeviceMemory memory,
                   VkExternalMemoryHandleTypeFlagBits type =
                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);

// Semaphores.  `SYNC_FD` import is temporary by definition (the spec forces
// VK_SEMAPHORE_IMPORT_TEMPORARY_BIT), which is exactly the 3.5 acquire-fence
// lifetime: wait once, then the semaphore reverts to its prior state.
void importSemaphoreFd(const Context& ctx, VkSemaphore sem, int fd,
                       VkExternalSemaphoreHandleTypeFlagBits type =
                           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                       bool temporary = true);
int exportSemaphoreFd(const Context& ctx, VkSemaphore sem,
                      VkExternalSemaphoreHandleTypeFlagBits type =
                          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);

// ------------------------------------------------------------------ Android
#if defined(__ANDROID__)
// 3.5 step 2.  Imports the AHardwareBuffer MediaCodec handed us: queries its
// properties, builds a VkImage with VkExternalFormatANDROID when the buffer
// carries a vendor-tiled YCbCr format (typically UBWC NV12 on XR2), binds
// imported memory, and creates the VkSamplerYcbcrConversion plus an immutable
// sampler for it.  The caller keeps the AHardwareBuffer alive for the
// lifetime of the returned ExternalImage.
//
// Imports are expensive and the AImageReader pool is small and recycled, so
// callers should cache the result keyed by the AHardwareBuffer pointer.
ExternalImage importAHardwareBuffer(const Context& ctx, AHardwareBuffer* ahb,
                                    VkImageUsageFlags usage =
                                        VK_IMAGE_USAGE_SAMPLED_BIT);

// Query without importing, to decide whether the format is one we can sample.
struct AhbInfo {
    uint64_t allocation_size = 0;
    uint32_t memory_type_bits = 0;
    uint64_t external_format = 0;         // 0 when the format is a real VkFormat
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFormatFeatureFlags format_features = 0;
    VkSamplerYcbcrModelConversion ycbcr_model =
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    VkSamplerYcbcrRange ycbcr_range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    VkChromaLocation x_chroma_offset = VK_CHROMA_LOCATION_COSITED_EVEN;
    VkChromaLocation y_chroma_offset = VK_CHROMA_LOCATION_COSITED_EVEN;
    VkComponentMapping sampler_swizzle{};
    VkFilter suggested_filter = VK_FILTER_LINEAR;
};
AhbInfo describeAHardwareBuffer(const Context& ctx, AHardwareBuffer* ahb);
#endif  // __ANDROID__

// ------------------------------------------------------------------ Windows
#if defined(_WIN32)
// 3.8.  Import the shared NT handle of a D3D11 texture created with
// D3D11_RESOURCE_MISC_SHARED_NTHANDLE.  `handle` stays owned by the caller
// when `take_ownership` is false (the Vulkan spec's "handle is not consumed"
// rule for Win32 handle types).
ExternalImage importImageWin32(const Context& ctx, void* handle,
                               uint32_t width, uint32_t height, VkFormat format,
                               VkImageUsageFlags usage,
                               VkExternalMemoryHandleTypeFlagBits type =
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT);

// Import a shared D3D11.4 fence (ID3D11Device5::CreateFence with
// D3D11_FENCE_FLAG_SHARED) as a *timeline* semaphore, so the D3D11 copy and
// the Vulkan encode become one timeline.
void importSemaphoreWin32(const Context& ctx, VkSemaphore sem, void* handle,
                          VkExternalSemaphoreHandleTypeFlagBits type =
                              VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT);
void* exportSemaphoreWin32(const Context& ctx, VkSemaphore sem,
                           VkExternalSemaphoreHandleTypeFlagBits type =
                               VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT);
void* exportMemoryWin32(const Context& ctx, VkDeviceMemory memory,
                        VkExternalMemoryHandleTypeFlagBits type =
                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT);
#endif  // _WIN32

}  // namespace external
}  // namespace nxvc::vk
