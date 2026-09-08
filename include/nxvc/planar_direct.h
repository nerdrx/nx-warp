#ifndef NXVC_PLANAR_DIRECT_H
#define NXVC_PLANAR_DIRECT_H

#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace nxvc {

// Experimental, synchronous renderer for complete R2/coarse PLANAR frames
// with zero slopes. Borrows the device and graphics queue; the caller must
// serialize queue access and keep them alive until this object is destroyed.
// Frames containing other tile modes are rejected, never partially published.
class PlanarDirect {
public:
    struct Stats {
        double parse_ms = 0.0;
        double upload_ms = 0.0;
        double gpu_ms = -1.0;
        double total_ms = 0.0;
        uint32_t tiles = 0;
    };

    PlanarDirect(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family);
    ~PlanarDirect();
    PlanarDirect(const PlanarDirect&) = delete;
    PlanarDirect& operator=(const PlanarDirect&) = delete;

    // Geometry is immutable after successful configuration. width() includes
    // both eyes for stereo; each eye and the height must be 64-pixel aligned.
    void configure(const uint8_t* header, size_t len);
    uint32_t width() const;
    uint32_t height() const;
    // Target must be caller-owned RGBA8 UNORM, COLOR_ATTACHMENT | SAMPLED,
    // idle and exclusively writable. Prior contents are discarded. Returns
    // after GPU completion with the target in SHADER_READ_ONLY_OPTIMAL layout.
    Stats render(const uint8_t* frame, size_t len, VkImage target, VkImageView view, uint32_t width,
                 uint32_t height);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace nxvc

#endif
