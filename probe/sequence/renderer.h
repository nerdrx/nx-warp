#pragma once

#include <nxvc/nxvc_vk.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>

namespace nx_probe {

// Headless consumer for the decoder's R8 atlas view.  The caller owns the
// adopted device and submits decode work on the same queue.  No XR objects are
// used here; this is deliberately a small graphics-only probe.
class Renderer {
public:
    Renderer(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t queue_family,
             std::filesystem::path vertex_spv, std::filesystem::path fragment_spv,
             uint32_t width = 2160, uint32_t height = 2160);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Records both eye views.  A previous submission is waited before the
    // output images or command buffer are reused.  If completion is non-null,
    // ownership of the newly created fence is returned to the caller.
    bool draw(const nxvc_vkd_atlas_images& atlas, VkBuffer table, VkDeviceSize table_bytes,
              VkFence* completion = nullptr);
    bool wait();
    bool readback_ppm(uint32_t eye, const std::filesystem::path& path);
    VkImage output_image(uint32_t eye) const;
    VkImageView output_view(uint32_t eye) const;
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    uint32_t width_ = 0, height_ = 0;
};

}  // namespace nx_probe
