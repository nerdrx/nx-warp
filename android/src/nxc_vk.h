// Vulkan 1.1 presentation for the client shell.
//
// A plain fullscreen swapchain on the NativeActivity window. NO OpenXR: this app
// is the pre-integration test vehicle, and PAPER 4.3 is explicit that frameless
// presentation lives inside WiVRn's reprojection pass, not in a runtime we get
// to change. When WiVRn NX integration happens, this class is replaced by
// WiVRn's own swapchain and the decoder + frame ring move across unchanged.
//
// Per frame the renderer records exactly:
//   1. Decoder::record_pass_a   (no-op in the placeholder)
//   2. Decoder::record_pass_b   (writes the 2-plane 4:2:0 YCbCr output)
//   3. the present dispatch     (YCbCr -> RGBA)
//   4. the HUD dispatch         (overlays text on the RGBA image)
//   5. one vkCmdBlitImage into the acquired swapchain image
// A compute-then-blit path rather than a render pass: the image never has to
// enter a framebuffer, which on a tiler saves a full-image load/store per frame
// for the sake of an overlay (PAPER 3.2, 3.7).
//
// The decoder's output is YCbCr because that is what the real codec emits and
// what WiVRn already samples from MediaCodec (PAPER 3.5). Step 3 exists only
// because this shell presents to a plain RGB swapchain; in the WiVRn
// integration the reprojection pass samples the planes directly and step 3
// disappears.
#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "nxc_config.h"
#include "nxc_decoder.h"
#include "nxc_font.h"

struct ANativeWindow;

namespace nxc {

struct GpuInfo {
    char     device_name[256] = {};
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint32_t vendor_id = 0;
    uint32_t subgroup_size = 0;      // PAPER 3.2.6 / 3.7
    bool     subgroup_size_control = false;
    float    timestamp_period_ns = 0;
    bool     timestamps_supported = false;
    // Needed to write R8_UNORM / R8G8_UNORM storage images, i.e. to let the
    // decoder emit 2-plane 4:2:0 at all.
    bool     storage_extended_formats = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(ANativeWindow* window, const AppConfig& cfg);
    void shutdown();
    bool ready() const { return device_ != VK_NULL_HANDLE && swapchain_ != VK_NULL_HANDLE; }

    // Called when the surface goes away (app backgrounded) and comes back.
    void surface_lost();
    bool surface_regained(ANativeWindow* window);

    // One frame. `tile_meta` is the frame ring's snapshot (TRANSPORT.md 7.3),
    // `hud` the character grid. Returns false if the swapchain needs rebuilding
    // and could not be rebuilt this frame; the caller simply tries again.
    bool render(const std::vector<uint32_t>& tile_meta, const TextCanvas& hud,
                uint16_t frame_id);

    // GPU time of the two decode dispatches from the previous frame, in
    // microseconds. 0 if timestamps are unsupported.
    uint32_t last_decode_us() const { return last_decode_us_; }

    const GpuInfo& gpu() const { return gpu_; }
    Decoder* decoder() { return decoder_.get(); }
    VkExtent2D extent() const { return extent_; }

private:
    bool create_instance();
    bool create_surface(ANativeWindow* window);
    bool pick_device();
    bool create_device();
    bool create_swapchain();
    void destroy_swapchain();
    bool create_images();
    void destroy_images();
    bool create_buffers();
    bool create_descriptors();
    bool create_pipelines();
    bool create_frames();
    void update_descriptors();
    bool alloc_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                     VkImage* img, VkDeviceMemory* mem, VkImageView* view);

    bool alloc_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer* buf,
                      VkDeviceMemory* mem, void** mapped);
    int  find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const;
    VkShaderModule make_module(const uint32_t* code, size_t bytes);

    AppConfig cfg_;
    GpuInfo   gpu_;

    VkInstance       instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_     = VK_NULL_HANDLE;
    VkDevice         device_   = VK_NULL_HANDLE;
    uint32_t         queue_family_ = 0;
    VkQueue          queue_    = VK_NULL_HANDLE;
    VkCommandPool    pool_     = VK_NULL_HANDLE;

    VkSwapchainKHR   swapchain_ = VK_NULL_HANDLE;
    VkFormat         sc_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D       extent_{0, 0};
    std::vector<VkImage>     sc_images_;
    std::vector<VkSemaphore> sc_render_done_;   // one per swapchain image

    // The decoder's two output planes, and the RGBA image the present pass
    // writes and the HUD draws on.
    VkImage        luma_img_ = VK_NULL_HANDLE;   VkDeviceMemory luma_mem_ = VK_NULL_HANDLE;   VkImageView luma_view_ = VK_NULL_HANDLE;
    VkImage        chroma_img_ = VK_NULL_HANDLE; VkDeviceMemory chroma_mem_ = VK_NULL_HANDLE; VkImageView chroma_view_ = VK_NULL_HANDLE;
    VkImage        rgba_img_ = VK_NULL_HANDLE;   VkDeviceMemory rgba_mem_ = VK_NULL_HANDLE;   VkImageView rgba_view_ = VK_NULL_HANDLE;
    VkSampler      plane_sampler_ = VK_NULL_HANDLE;
    VkExtent2D     chroma_extent_{0, 0};
    bool           images_initialised_ = false;

    // Host-visible SSBOs.
    VkBuffer       meta_buf_ = VK_NULL_HANDLE;  VkDeviceMemory meta_mem_ = VK_NULL_HANDLE;  void* meta_map_ = nullptr;
    VkBuffer       hud_buf_  = VK_NULL_HANDLE;  VkDeviceMemory hud_mem_  = VK_NULL_HANDLE;  void* hud_map_  = nullptr;
    VkBuffer       font_buf_ = VK_NULL_HANDLE;  VkDeviceMemory font_mem_ = VK_NULL_HANDLE;  void* font_map_ = nullptr;
    VkDeviceSize   meta_bytes_ = 0, hud_bytes_ = 0, font_bytes_ = 0;

    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;

    // Present pass: two sampled planes in, one RGBA storage image out.
    VkDescriptorSetLayout present_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet       present_set_    = VK_NULL_HANDLE;
    VkPipelineLayout      present_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            present_pipe_   = VK_NULL_HANDLE;

    // HUD pass: RGBA storage image, text SSBO, font SSBO.
    VkDescriptorSetLayout hud_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet       hud_set_    = VK_NULL_HANDLE;
    VkPipelineLayout      hud_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline            hud_pipe_   = VK_NULL_HANDLE;

    static constexpr uint32_t kFramesInFlight = 2;
    struct Frame {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        VkSemaphore     image_available = VK_NULL_HANDLE;
        VkQueryPool     queries = VK_NULL_HANDLE;
        bool            queries_valid = false;
    };
    Frame    frames_[kFramesInFlight];
    uint32_t frame_index_ = 0;
    uint32_t last_decode_us_ = 0;

    std::unique_ptr<Decoder> decoder_;
    ANativeWindow* window_ = nullptr;
};

}  // namespace nxc
