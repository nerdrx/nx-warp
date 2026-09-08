#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace nx_sequence {

// A deliberately plain adopted-device context.  The renderer owns its
// command resources; this object only supplies the queue used by decoder and
// renderer submissions.
struct VkContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    bool fdm_enabled = false;

    bool create();
    void destroy();
};

}  // namespace nx_sequence
