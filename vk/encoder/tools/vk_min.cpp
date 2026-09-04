// vk_min.cpp -- see vk_min.h.
//
// SPDX-License-Identifier: Apache-2.0

#include "vk_min.h"

#include <cstdio>
#include <cstring>

namespace vkmin {

const char *result_str(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "VK_ERROR_<other>";
    }
}

static bool mk_instance(VkInstance &inst, bool validation, std::string &err)
{
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "nxvc-stats-test";
    ai.apiVersion = VK_API_VERSION_1_1;   // subgroup ops are core 1.1

    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    if (validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
    }
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r == VK_ERROR_LAYER_NOT_PRESENT && validation) {
        ci.enabledLayerCount = 0;
        r = vkCreateInstance(&ci, nullptr, &inst);
    }
    if (r != VK_SUCCESS) {
        err = std::string("vkCreateInstance: ") + result_str(r);
        return false;
    }
    return true;
}

static void fill_info(VkPhysicalDevice pd, DeviceInfo &di)
{
    VkPhysicalDeviceSubgroupProperties sg{};
    sg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    di.name = p2.properties.deviceName;
    di.vendor_id = p2.properties.vendorID;
    di.device_id = p2.properties.deviceID;
    di.type = p2.properties.deviceType;
    di.api_version = p2.properties.apiVersion;
    di.subgroup_size = sg.subgroupSize;

    VkPhysicalDeviceFeatures f{};
    vkGetPhysicalDeviceFeatures(pd, &f);
    di.extended_storage_formats = f.shaderStorageImageExtendedFormats == VK_TRUE;
}

bool Device::enumerate(std::vector<DeviceInfo> &out, std::string &err)
{
    VkInstance inst = VK_NULL_HANDLE;
    if (!mk_instance(inst, false, err)) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    if (n) vkEnumeratePhysicalDevices(inst, &n, pds.data());
    out.clear();
    for (auto pd : pds) {
        DeviceInfo di;
        fill_info(pd, di);
        out.push_back(di);
    }
    vkDestroyInstance(inst, nullptr);
    if (out.empty()) {
        err = "no Vulkan physical devices (no ICD?)";
        return false;
    }
    return true;
}

bool Device::create(uint32_t index, bool validation, std::string &err)
{
    if (!mk_instance(inst_, validation, err)) return false;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    if (n == 0) { err = "no Vulkan physical devices (no ICD?)"; return false; }
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst_, &n, pds.data());
    if (index >= n) { err = "device index out of range"; return false; }
    phys_ = pds[index];
    fill_info(phys_, info_);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_, &props);
    ts_period_ = props.limits.timestampPeriod;
    max_wg_inv_ = props.limits.maxComputeWorkGroupInvocations;
    max_shared_ = props.limits.maxComputeSharedMemorySize;
    vkGetPhysicalDeviceMemoryProperties(phys_, &memprops_);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qfp.data());
    bool found = false;
    // Prefer a compute-only family: on RADV that is an async compute ring, which
    // keeps the harness off the graphics queue and closer to how the encoder
    // will actually be scheduled (paper 3.6 allows a dedicated compute queue).
    for (uint32_t i = 0; i < qn; ++i) {
        if ((qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            qfam_ = i; found = true; break;
        }
    }
    if (!found) {
        for (uint32_t i = 0; i < qn; ++i) {
            if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam_ = i; found = true; break; }
        }
    }
    if (!found) { err = "no compute queue family"; return false; }
    ts_valid_ = qfp[qfam_].timestampValidBits > 0;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfam_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    // The only optional feature the analysis kernels need: RGB10A2 as a
    // storage image for E0's 10-bit import path.  Everything else they use --
    // subgroup basic and arithmetic ops -- is core Vulkan 1.1.
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderStorageImageExtendedFormats =
        info_.extended_storage_formats ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabled;

    VkResult r = vkCreateDevice(phys_, &dci, nullptr, &dev_);
    if (r != VK_SUCCESS) { err = std::string("vkCreateDevice: ") + result_str(r); return false; }
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qfam_;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    r = vkCreateCommandPool(dev_, &pci, nullptr, &pool_);
    if (r != VK_SUCCESS) { err = "vkCreateCommandPool failed"; return false; }
    return true;
}

void Device::destroy()
{
    if (dev_) {
        vkDeviceWaitIdle(dev_);
        for (auto q : qpools_) vkDestroyQueryPool(dev_, q, nullptr);
        qpools_.clear();
        if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
        vkDestroyDevice(dev_, nullptr);
        dev_ = VK_NULL_HANDLE;
    }
    if (inst_) { vkDestroyInstance(inst_, nullptr); inst_ = VK_NULL_HANDLE; }
}

bool Device::supports_storage_format(VkFormat fmt) const
{
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys_, fmt, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

uint32_t Device::find_memory(uint32_t bits, VkMemoryPropertyFlags want) const
{
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (memprops_.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

bool Device::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                           bool host_visible, Buffer &out, std::string &err)
{
    if (size == 0) size = 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(dev_, &bi, nullptr, &out.buf);
    if (r != VK_SUCCESS) { err = "vkCreateBuffer failed"; return false; }

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev_, out.buf, &mr);

    // HOST_CACHED is what paper 3.6 wants for the E5 output ring (host reads
    // must be cached, not write-combined); ask for it and fall back.
    VkMemoryPropertyFlags want =
        host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t mt = find_memory(mr.memoryTypeBits, want);
    if (mt == UINT32_MAX && host_visible)
        mt = find_memory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX && !host_visible)
        mt = find_memory(mr.memoryTypeBits, 0);
    if (mt == UINT32_MAX) { err = "no suitable memory type"; return false; }

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    r = vkAllocateMemory(dev_, &ai, nullptr, &out.mem);
    if (r != VK_SUCCESS) { err = std::string("vkAllocateMemory: ") + result_str(r); return false; }
    vkBindBufferMemory(dev_, out.buf, out.mem, 0);
    out.size = size;
    if (host_visible) {
        r = vkMapMemory(dev_, out.mem, 0, VK_WHOLE_SIZE, 0, &out.map);
        if (r != VK_SUCCESS) { err = "vkMapMemory failed"; return false; }
    }
    return true;
}

void Device::destroy_buffer(Buffer &b)
{
    if (b.map) { vkUnmapMemory(dev_, b.mem); b.map = nullptr; }
    if (b.buf) { vkDestroyBuffer(dev_, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
    if (b.mem) { vkFreeMemory(dev_, b.mem, nullptr); b.mem = VK_NULL_HANDLE; }
}

bool Device::create_storage_image(uint32_t w, uint32_t h, VkFormat fmt,
                                  Image &out, std::string &err)
{
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(dev_, &ii, nullptr, &out.img);
    if (r != VK_SUCCESS) { err = std::string("vkCreateImage: ") + result_str(r); return false; }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev_, out.img, &mr);
    uint32_t mt = find_memory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = find_memory(mr.memoryTypeBits, 0);
    if (mt == UINT32_MAX) { err = "no image memory type"; return false; }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    r = vkAllocateMemory(dev_, &ai, nullptr, &out.mem);
    if (r != VK_SUCCESS) { err = "image vkAllocateMemory failed"; return false; }
    vkBindImageMemory(dev_, out.img, out.mem, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    r = vkCreateImageView(dev_, &vi, nullptr, &out.view);
    if (r != VK_SUCCESS) { err = "vkCreateImageView failed"; return false; }
    out.w = w; out.h = h; out.fmt = fmt;
    return true;
}

void Device::destroy_image(Image &i)
{
    if (i.view) { vkDestroyImageView(dev_, i.view, nullptr); i.view = VK_NULL_HANDLE; }
    if (i.img)  { vkDestroyImage(dev_, i.img, nullptr); i.img = VK_NULL_HANDLE; }
    if (i.mem)  { vkFreeMemory(dev_, i.mem, nullptr); i.mem = VK_NULL_HANDLE; }
}

bool Device::create_pipeline(const uint32_t *spv, size_t spv_bytes,
                             const std::vector<VkDescriptorType> &bindings,
                             uint32_t push_bytes, Pipeline &out, std::string &err)
{
    std::vector<VkDescriptorSetLayoutBinding> lb(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        lb[i] = {};
        lb[i].binding = (uint32_t)i;
        lb[i].descriptorType = bindings[i];
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = (uint32_t)lb.size();
    dli.pBindings = lb.data();
    VkResult r = vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &out.dsl);
    if (r != VK_SUCCESS) { err = "vkCreateDescriptorSetLayout failed"; return false; }

    VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes };
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &out.dsl;
    if (push_bytes) { pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr; }
    r = vkCreatePipelineLayout(dev_, &pli, nullptr, &out.layout);
    if (r != VK_SUCCESS) { err = "vkCreatePipelineLayout failed"; return false; }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = spv_bytes;
    smi.pCode = spv;
    VkShaderModule sm = VK_NULL_HANDLE;
    r = vkCreateShaderModule(dev_, &smi, nullptr, &sm);
    if (r != VK_SUCCESS) { err = "vkCreateShaderModule failed"; return false; }

    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.layout = out.layout;
    r = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &out.pipe);
    vkDestroyShaderModule(dev_, sm, nullptr);
    if (r != VK_SUCCESS) { err = std::string("vkCreateComputePipelines: ") + result_str(r); return false; }
    return true;
}

void Device::destroy_pipeline(Pipeline &p)
{
    if (p.pipe)   { vkDestroyPipeline(dev_, p.pipe, nullptr); p.pipe = VK_NULL_HANDLE; }
    if (p.layout) { vkDestroyPipelineLayout(dev_, p.layout, nullptr); p.layout = VK_NULL_HANDLE; }
    if (p.dsl)    { vkDestroyDescriptorSetLayout(dev_, p.dsl, nullptr); p.dsl = VK_NULL_HANDLE; }
}

VkDescriptorPool Device::create_descriptor_pool(uint32_t max_sets,
                                                uint32_t storage_buffers,
                                                uint32_t storage_images)
{
    std::vector<VkDescriptorPoolSize> sizes;
    if (storage_buffers) sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storage_buffers });
    if (storage_images)  sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage_images });
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = max_sets;
    pi.poolSizeCount = (uint32_t)sizes.size();
    pi.pPoolSizes = sizes.data();
    VkDescriptorPool p = VK_NULL_HANDLE;
    vkCreateDescriptorPool(dev_, &pi, nullptr, &p);
    return p;
}

VkDescriptorSet Device::allocate_set(VkDescriptorPool pool, VkDescriptorSetLayout dsl)
{
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl;
    VkDescriptorSet s = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(dev_, &ai, &s);
    return s;
}

VkCommandBuffer Device::begin()
{
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev_, &ai, &cb);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

bool Device::submit_and_wait(VkCommandBuffer cb, std::string &err)
{
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev_, &fi, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r = vkQueueSubmit(queue_, 1, &si, fence);
    if (r != VK_SUCCESS) {
        err = std::string("vkQueueSubmit: ") + result_str(r);
        vkDestroyFence(dev_, fence, nullptr);
        return false;
    }
    r = vkWaitForFences(dev_, 1, &fence, VK_TRUE, 60ull * 1000 * 1000 * 1000);
    vkDestroyFence(dev_, fence, nullptr);
    vkFreeCommandBuffers(dev_, pool_, 1, &cb);
    if (r != VK_SUCCESS) { err = std::string("vkWaitForFences: ") + result_str(r); return false; }
    return true;
}

VkQueryPool Device::create_timestamp_pool(uint32_t count)
{
    VkQueryPoolCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = count;
    VkQueryPool p = VK_NULL_HANDLE;
    if (vkCreateQueryPool(dev_, &qi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    qpools_.push_back(p);
    return p;
}

bool Device::read_timestamps(VkQueryPool pool, uint32_t count, std::vector<uint64_t> &out)
{
    out.assign(count, 0);
    VkResult r = vkGetQueryPoolResults(dev_, pool, 0, count,
                                       count * sizeof(uint64_t), out.data(),
                                       sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return r == VK_SUCCESS;
}

void Device::barrier_compute_to_compute(VkCommandBuffer cb)
{
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void Device::barrier_transfer_to_compute(VkCommandBuffer cb)
{
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void Device::barrier_compute_to_host(VkCommandBuffer cb)
{
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
}

} // namespace vkmin
