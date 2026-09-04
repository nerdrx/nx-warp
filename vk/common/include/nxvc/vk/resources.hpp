// nxvc/vk/resources.hpp - buffers, images, samplers.
//
// No VMA.  The codec allocates a handful of large, long-lived resources per
// stream (coefficient buffers, reference images, the send ring) and nothing
// per frame, so a suballocator would be dead weight in a library that must
// also build with nothing but the NDK.  Every allocation here is one
// VkDeviceMemory with a dedicated-allocation hint where the driver asks for
// one.
#pragma once

#include <nxvc/vk/context.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace nxvc::vk {

// ------------------------------------------------------------------ buffers
enum class BufferKind {
    // VRAM.  Coefficients, tile records, reference data.
    DeviceLocal,
    // 3.6 send ring: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED, so that
    // sendmmsg() reads straight out of the mapping through the data cache.
    // Falls back to non-cached host-visible with `cached()` false, at which
    // point the caller must stage through a device-local buffer instead.
    HostCached,
    // Upload staging: HOST_VISIBLE | HOST_COHERENT, write-combined is fine.
    HostUpload,
    // Readback for tests and the diff harness.
    HostReadback,
    // The device-local host-visible BAR heap.  Rejected for the send ring in
    // 3.6 (host *reads* of write-combined memory are slow) but right for
    // small per-frame uniform rings the GPU only reads.
    DeviceLocalHostVisible,
};

class Buffer {
public:
    Buffer() = default;
    Buffer(const Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
           BufferKind kind, std::string debug_name = {});
    ~Buffer();
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] VkBuffer handle() const noexcept { return buffer_; }
    [[nodiscard]] VkDeviceMemory memory() const noexcept { return memory_; }
    [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
    [[nodiscard]] void* mapped() const noexcept { return mapped_; }
    [[nodiscard]] bool cached() const noexcept { return cached_; }
    [[nodiscard]] bool coherent() const noexcept { return coherent_; }
    [[nodiscard]] VkDeviceAddress deviceAddress() const;

    template <class T>
    [[nodiscard]] std::span<T> span() const {
        return std::span<T>(static_cast<T*>(mapped_), size_ / sizeof(T));
    }

    // No-ops on coherent memory.
    void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
    void invalidate(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;

    [[nodiscard]] VkDescriptorBufferInfo descriptor(
        VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE) const noexcept {
        return VkDescriptorBufferInfo{buffer_, offset, range};
    }
    [[nodiscard]] VkBufferMemoryBarrier barrier(VkAccessFlags src, VkAccessFlags dst) const noexcept;

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
    bool cached_ = false;
    bool coherent_ = true;
};

// ------------------------------------------------------------------- images
struct ImageDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;              // 2 for a stereo pair
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    // STORAGE for the compute passes, SAMPLED where a sampler tap is allowed
    // (the hybrid base layer of 3.5 and the reprojection hand-off of 3.10).
    VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    uint32_t mip_levels = 1;
    std::string debug_name;
};

class Image {
public:
    Image() = default;
    Image(const Context& ctx, const ImageDesc& desc);
    ~Image();
    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    [[nodiscard]] VkImage handle() const noexcept { return image_; }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkDeviceMemory memory() const noexcept { return memory_; }
    [[nodiscard]] VkFormat format() const noexcept { return desc_.format; }
    [[nodiscard]] uint32_t width() const noexcept { return desc_.width; }
    [[nodiscard]] uint32_t height() const noexcept { return desc_.height; }
    [[nodiscard]] uint32_t layers() const noexcept { return desc_.layers; }
    [[nodiscard]] VkImageLayout layout() const noexcept { return layout_; }

    [[nodiscard]] VkDescriptorImageInfo descriptor(
        VkImageLayout l, VkSampler s = VK_NULL_HANDLE) const noexcept {
        return VkDescriptorImageInfo{s, view_, l};
    }

    // Records the barrier and updates the tracked layout.  Layout tracking is
    // a convenience for setup code; the per-frame passes state their barriers
    // explicitly and do not use it.
    void transition(VkCommandBuffer cmd, VkImageLayout new_layout,
                    VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                    VkPipelineStageFlags dst_stage, VkAccessFlags dst_access);
    void setLayout(VkImageLayout l) noexcept { layout_ = l; }

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    ImageDesc desc_{};
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

// A plain nearest-neighbour clamped sampler.  3.7 forbids sampler taps in the
// normative path; this exists for the hybrid base layer and for K2b.
class Sampler {
public:
    Sampler() = default;
    Sampler(const Context& ctx, VkFilter filter,
            VkSamplerAddressMode mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            const void* pnext = nullptr);
    ~Sampler();
    Sampler(Sampler&&) noexcept;
    Sampler& operator=(Sampler&&) noexcept;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    [[nodiscard]] VkSampler handle() const noexcept { return sampler_; }

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

}  // namespace nxvc::vk
