// vk_min.h -- the smallest amount of Vulkan the encoder test harness needs.
//
// SPDX-License-Identifier: Apache-2.0
//
// This is deliberately throwaway scaffolding.  `vk/common` will grow a real
// device/allocator/pipeline layer (capability probe, pipeline cache, timeline
// and external-memory helpers, paper 3.10) and this file should be deleted the
// day it lands.  Nothing here is on any hot path: it exists so the GPU-vs-CPU
// diff harness can run before the shared layer exists, which is the ordering
// paper 3.9 asks for -- the diff test is what makes the kernels trustworthy,
// so it must not wait on infrastructure.
//
// Scope: one compute queue, no swapchain, no surface, no window (the harness is
// headless by construction), synchronous submits, one allocation per resource.

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vkmin {

const char *result_str(VkResult r);

struct Buffer {
    VkBuffer       buf  = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    VkDeviceSize   size = 0;
    void          *map  = nullptr;   // non-null for host-visible buffers
};

struct Image {
    VkImage        img  = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    uint32_t       w = 0, h = 0;
    VkFormat       fmt = VK_FORMAT_UNDEFINED;
};

struct Pipeline {
    VkPipeline            pipe   = VK_NULL_HANDLE;
    VkPipelineLayout      layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl    = VK_NULL_HANDLE;
};

struct DeviceInfo {
    std::string name;
    uint32_t    vendor_id = 0;
    uint32_t    device_id = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t    subgroup_size = 0;
    uint32_t    api_version = 0;
    std::string driver;
    // A2B10G10R10_UINT_PACK32 as a storage image needs this feature; without
    // it the RGB10A2 half of E0 cannot be tested on this device.
    bool        extended_storage_formats = false;
};

class Device {
public:
    bool create(uint32_t index, bool validation, std::string &err);
    void destroy();

    // Enumerate without keeping a device alive.  Returns false (with err set)
    // if no ICD is present at all, which the harness reports as "skipped".
    static bool enumerate(std::vector<DeviceInfo> &out, std::string &err);

    VkDevice         handle() const { return dev_; }
    VkPhysicalDevice phys() const { return phys_; }
    VkQueue          queue() const { return queue_; }
    uint32_t         queue_family() const { return qfam_; }
    const DeviceInfo &info() const { return info_; }
    float            timestamp_period() const { return ts_period_; }
    bool             timestamps_valid() const { return ts_valid_; }
    uint32_t         max_workgroup_invocations() const { return max_wg_inv_; }
    uint32_t         max_shared_memory() const { return max_shared_; }
    bool             supports_storage_format(VkFormat fmt) const;

    // ------------------------------------------------------------ resources
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       bool host_visible, Buffer &out, std::string &err);
    void destroy_buffer(Buffer &b);

    bool create_storage_image(uint32_t w, uint32_t h, VkFormat fmt,
                              Image &out, std::string &err);
    void destroy_image(Image &i);

    bool create_pipeline(const uint32_t *spv, size_t spv_bytes,
                         const std::vector<VkDescriptorType> &bindings,
                         uint32_t push_bytes, Pipeline &out, std::string &err);
    void destroy_pipeline(Pipeline &p);

    VkDescriptorPool create_descriptor_pool(uint32_t max_sets,
                                            uint32_t storage_buffers,
                                            uint32_t storage_images);
    VkDescriptorSet allocate_set(VkDescriptorPool pool, VkDescriptorSetLayout dsl);

    // ---------------------------------------------------------- command flow
    VkCommandBuffer begin();
    // Submits, waits for idle, frees.  Returns the elapsed GPU time in
    // milliseconds for the query pair [0,1] if `ms` is non-null and the pool
    // was used, otherwise leaves it untouched.
    bool submit_and_wait(VkCommandBuffer cb, std::string &err);

    VkQueryPool create_timestamp_pool(uint32_t count);
    bool read_timestamps(VkQueryPool pool, uint32_t count, std::vector<uint64_t> &out);

    void barrier_compute_to_compute(VkCommandBuffer cb);
    void barrier_transfer_to_compute(VkCommandBuffer cb);
    void barrier_compute_to_host(VkCommandBuffer cb);

    uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags want) const;

private:
    VkInstance       inst_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_  = VK_NULL_HANDLE;
    VkDevice         dev_   = VK_NULL_HANDLE;
    VkQueue          queue_ = VK_NULL_HANDLE;
    VkCommandPool    pool_  = VK_NULL_HANDLE;
    uint32_t         qfam_  = 0;
    VkPhysicalDeviceMemoryProperties memprops_{};
    DeviceInfo       info_{};
    float            ts_period_ = 0.0f;
    bool             ts_valid_  = false;
    uint32_t         max_wg_inv_ = 0;
    uint32_t         max_shared_ = 0;
    std::vector<VkQueryPool> qpools_;
};

} // namespace vkmin
