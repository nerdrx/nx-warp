// nxvc_vk_decoder: the two-dispatch Vulkan compute decoder.
//
//   upload  ->  Pass A (rANS entropy decode)  ->  Pass B (reconstruction)
//           ->  output images  ->  optional readback
//
// One command buffer per frame, one timeline-semaphore signal, timestamp
// queries around each dispatch.  Everything above the tile payload is parsed
// on the host by nxvc_vkdec_parse.cpp.
//
// The Vulkan boilerplate here is deliberately minimal and self-contained: it
// is the same shape the Pass A and Pass B harnesses already carry.  When
// vk/common's context / resources / pipeline helpers settle, instance and
// device creation, the buffer allocator and the pipeline cache in this file
// are the pieces that should be deleted in favour of nxvc_vk_context_create()
// and nxvc::vk::Buffer / Pipeline; the decode path itself does not change.
// See vk/decoder/README.md, "Relationship to vk/common".
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nxvc/nxvc_vk.h"
#include "nxvc_vkdec_parse.h"
#include "passA/syntax_constants.h"
#include "passB/syntax_constants.h"

#include "rans_decode.spv.h"
#include "reconstruct.spv.h"

namespace {

using nxvcvk::FrameParse;
using nxvcvk::LaneGroup;
using nxvcvk::StreamInfo;

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(
               clock::now().time_since_epoch())
        .count();
}

struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

struct Img {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
};

}  // namespace

// ---------------------------------------------------------------------------
struct nxvc_vk_decoder {
    // ---- device
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    bool own_instance = false, own_device = false;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    uint32_t subgroup_size = 0;
    bool has_size_control = false;
    bool have_timestamps = false;
    float ts_period = 0.f;

    // ---- config
    uint32_t want_output = NXVC_VKD_OUT_AUTO;
    uint32_t out_format = NXVC_VKD_OUT_RGBA8;  // resolved kOut* value
    uint32_t flags = 0;
    uint32_t read_ptr_mode = nxwarp_passA::kReadPtrBallot;

    // ---- per-frame Vulkan objects
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t timeline_value = 0;
    VkQueryPool queries = VK_NULL_HANDLE;

    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslA = VK_NULL_HANDLE, dslB = VK_NULL_HANDLE;
    VkPipelineLayout plA = VK_NULL_HANDLE, plB = VK_NULL_HANDLE;
    VkShaderModule smA = VK_NULL_HANDLE, smB = VK_NULL_HANDLE;
    VkDescriptorSet dsetA = VK_NULL_HANDLE, dsetB = VK_NULL_HANDLE;
    std::map<uint32_t, VkPipeline> pipesA;  // key: lanes
    std::map<uint64_t, VkPipeline> pipesB;  // key: (format << 32) | storeWords

    // ---- buffers
    Buf staging, bBits, bDesc, bTables, bCoef, bCbf, bStatus, bRecs, bWgt,
        bRead;
    Img imgRgba, imgRgb10, imgLuma, imgCbCr;

    // ---- stream state
    bool have_stream = false;
    StreamInfo si{};
    FrameParse fp{};
    bool resources_ready = false;
    // Byte layout of the staging buffer.
    VkDeviceSize offBits = 0, offDesc = 0, offTables = 0, offRecs = 0,
                 offWgt = 0;
    // Byte layout of the readback buffer.
    VkDeviceSize rbLuma = 0, rbCbCr = 0, rbRgba = 0, rbBytes = 0;
    bool need_alpha_pass = false;  // second Pass B dispatch for the A channel

    nxvc_vkd_stats stats{};
    std::string err = "";
    std::string device_name = "";
};

namespace {

using D = nxvc_vk_decoder;

nxvc_vkd_status seterr(D *d, nxvc_vkd_status st, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    d->err = b;
    return st;
}

#define VKTRY(d, expr)                                                    \
    do {                                                                  \
        VkResult _r = (expr);                                             \
        if (_r != VK_SUCCESS)                                             \
            return seterr((d), NXVC_VKD_ERR_VULKAN, "%s failed: VkResult %d", \
                          #expr, (int)_r);                                \
    } while (0)

// ------------------------------------------------------------------ memory
int find_memory(const D *d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (d->memProps.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

void destroy_buf(D *d, Buf &b) {
    if (b.mapped) vkUnmapMemory(d->dev, b.mem);
    if (b.buf) vkDestroyBuffer(d->dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(d->dev, b.mem, nullptr);
    b = Buf{};
}

nxvc_vkd_status make_buf(D *d, Buf &b, VkDeviceSize size,
                         VkBufferUsageFlags usage, bool host_visible) {
    if (b.buf && b.size >= size) return NXVC_VKD_OK;
    destroy_buf(d, b);
    if (size == 0) size = 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKTRY(d, vkCreateBuffer(d->dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(d->dev, b.buf, &mr);
    VkMemoryPropertyFlags want =
        host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int mt = find_memory(d, mr.memoryTypeBits, want);
    if (mt < 0 && !host_visible) mt = find_memory(d, mr.memoryTypeBits, 0);
    if (mt < 0)
        return seterr(d, NXVC_VKD_ERR_NOMEM, "no memory type for a %llu B buffer",
                      (unsigned long long)size);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    VKTRY(d, vkAllocateMemory(d->dev, &ai, nullptr, &b.mem));
    VKTRY(d, vkBindBufferMemory(d->dev, b.buf, b.mem, 0));
    b.size = size;
    if (host_visible)
        VKTRY(d, vkMapMemory(d->dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return NXVC_VKD_OK;
}

void destroy_img(D *d, Img &i) {
    if (i.view) vkDestroyImageView(d->dev, i.view, nullptr);
    if (i.img) vkDestroyImage(d->dev, i.img, nullptr);
    if (i.mem) vkFreeMemory(d->dev, i.mem, nullptr);
    i = Img{};
}

nxvc_vkd_status make_img(D *d, Img &im, VkFormat fmt, uint32_t w, uint32_t h) {
    destroy_img(d, im);
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKTRY(d, vkCreateImage(d->dev, &ii, nullptr, &im.img));
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(d->dev, im.img, &mr);
    int mt = find_memory(d, mr.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memory(d, mr.memoryTypeBits, 0);
    if (mt < 0) return seterr(d, NXVC_VKD_ERR_NOMEM, "no memory type for image");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    VKTRY(d, vkAllocateMemory(d->dev, &ai, nullptr, &im.mem));
    VKTRY(d, vkBindImageMemory(d->dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKTRY(d, vkCreateImageView(d->dev, &vi, nullptr, &im.view));
    im.w = w;
    im.h = h;
    im.fmt = fmt;
    return NXVC_VKD_OK;
}

// ------------------------------------------------------------ device setup
bool name_matches(const char *name, const char *want) {
    if (!want || !*want) return true;
    std::string a(name), b(want);
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

nxvc_vkd_status create_device(D *d, const nxvc_vkd_create_info *ci) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc_vk_decoder";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (ci->flags & NXVC_VKD_FLAG_VALIDATION) {
        ii.enabledLayerCount = 1;
        ii.ppEnabledLayerNames = &layer;
    }
    VkResult r = vkCreateInstance(&ii, nullptr, &d->inst);
    if (r != VK_SUCCESS)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "vkCreateInstance failed: VkResult %d", (int)r);
    d->own_instance = true;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d->inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(d->inst, &n, devs.data());
    for (VkPhysicalDevice pd : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        // Pass A stores int16 and the decoder signals a timeline semaphore,
        // both core from 1.2; the self-created path does not carry the 1.1
        // extension fallbacks because every target driver is 1.2 or newer.
        // An adopted device (create_info.device) bypasses all of this.
        if (p.apiVersion < VK_API_VERSION_1_2) continue;
        if (!name_matches(p.deviceName, ci->device_name)) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; ++q) {
            if (!(qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            d->phys = pd;
            d->qfam = q;
            break;
        }
        if (d->phys) break;
    }
    if (!d->phys)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "no Vulkan 1.2 device with a compute queue%s%s",
                      ci->device_name ? " matching " : "",
                      ci->device_name ? ci->device_name : "");

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(d->phys, &props);
    const bool has13 = props.apiVersion >= VK_API_VERSION_1_3;

    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = has13 ? (void *)&f13 : nullptr;
    VkPhysicalDeviceVulkan11Features f11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(d->phys, &f2);
    if (!f2.features.shaderInt16)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "%s lacks shaderInt16", props.deviceName);
    if (!f11.storageBuffer16BitAccess)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "%s lacks storageBuffer16BitAccess", props.deviceName);
    if (!f12.timelineSemaphore)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "%s lacks timelineSemaphore", props.deviceName);
    d->has_size_control = has13 && f13.subgroupSizeControl &&
                          f13.computeFullSubgroups;

    float prio = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = d->qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features e13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.subgroupSizeControl = d->has_size_control;
    e13.computeFullSubgroups = d->has_size_control;
    VkPhysicalDeviceVulkan12Features e12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.timelineSemaphore = VK_TRUE;
    e12.pNext = has13 ? (void *)&e13 : nullptr;
    VkPhysicalDeviceVulkan11Features e11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.storageBuffer16BitAccess = VK_TRUE;
    e11.pNext = &e12;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.features.shaderInt16 = VK_TRUE;
    e2.pNext = &e11;

    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &e2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    r = vkCreateDevice(d->phys, &di, nullptr, &d->dev);
    if (r != VK_SUCCESS)
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkCreateDevice failed: %d",
                      (int)r);
    d->own_device = true;
    vkGetDeviceQueue(d->dev, d->qfam, 0, &d->queue);
    return NXVC_VKD_OK;
}

nxvc_vkd_status probe_device(D *d) {
    vkGetPhysicalDeviceProperties(d->phys, &d->props);
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memProps);
    d->device_name = d->props.deviceName;
    d->have_timestamps = d->props.limits.timestampComputeAndGraphics != 0 &&
                         d->props.limits.timestampPeriod > 0.f;
    d->ts_period = d->props.limits.timestampPeriod;

    VkPhysicalDeviceSubgroupProperties sg{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(d->phys, &p2);
    d->subgroup_size = sg.subgroupSize;
    const VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                        VK_SUBGROUP_FEATURE_BALLOT_BIT;
    if ((sg.supportedOperations & need) != need)
        d->read_ptr_mode = nxwarp_passA::kReadPtrLdsFallback;
    if (d->flags & NXVC_VKD_FLAG_LDS_FALLBACK)
        d->read_ptr_mode = nxwarp_passA::kReadPtrLdsFallback;
    if (d->props.limits.maxComputeWorkGroupInvocations < 256)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "device allows only %u workgroup invocations, Pass B "
                      "needs 256",
                      d->props.limits.maxComputeWorkGroupInvocations);
    return NXVC_VKD_OK;
}

// --------------------------------------------------------------- pipelines
nxvc_vkd_status make_layouts(D *d) {
    auto set_layout = [&](int nbuf, int nimg, VkDescriptorSetLayout *out) {
        std::vector<VkDescriptorSetLayoutBinding> b((size_t)(nbuf + nimg));
        for (int i = 0; i < nbuf + nimg; ++i) {
            b[(size_t)i].binding = (uint32_t)i;
            b[(size_t)i].descriptorType =
                i < nbuf ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                         : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[(size_t)i].descriptorCount = 1;
            b[(size_t)i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = (uint32_t)b.size();
        ci.pBindings = b.data();
        return vkCreateDescriptorSetLayout(d->dev, &ci, nullptr, out);
    };
    VKTRY(d, set_layout(6, 0, &d->dslA));
    VKTRY(d, set_layout(3, 4, &d->dslB));

    VkPushConstantRange pcA{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &d->dslA;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcA;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plA));

    VkPushConstantRange pcB{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)sizeof(nxvw::NxvwPassBPush)};
    pl.pSetLayouts = &d->dslB;
    pl.pPushConstantRanges = &pcB;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plB));

    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(rans_decode_spv);
    sm.pCode = rans_decode_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smA));
    sm.codeSize = sizeof(reconstruct_spv);
    sm.pCode = reconstruct_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smB));

    VkDescriptorPoolSize sz[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 2;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sz;
    VKTRY(d, vkCreateDescriptorPool(d->dev, &dp, nullptr, &d->dpool));
    VkDescriptorSetLayout ls[2] = {d->dslA, d->dslB};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo da{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = d->dpool;
    da.descriptorSetCount = 2;
    da.pSetLayouts = ls;
    VKTRY(d, vkAllocateDescriptorSets(d->dev, &da, sets));
    d->dsetA = sets[0];
    d->dsetB = sets[1];
    return NXVC_VKD_OK;
}

nxvc_vkd_status pipeline_a(D *d, uint32_t lanes, VkPipeline *out) {
    auto it = d->pipesA.find(lanes);
    if (it != d->pipesA.end()) {
        *out = it->second;
        return NXVC_VKD_OK;
    }
    // The ballot path needs a subgroup at least as wide as one tile's lane
    // cluster; otherwise the cluster straddles subgroups and the prefix count
    // is wrong.  The LDS fallback produces identical offsets with no subgroup
    // op at all (vk/decoder/passA/README.md).
    uint32_t mode = d->read_ptr_mode;
    if (d->subgroup_size < lanes) mode = nxwarp_passA::kReadPtrLdsFallback;
    const uint32_t tpg = nxwarp_passA::nxs_tiles_per_group(lanes);
    const uint32_t data[3] = {mode, tpg, lanes};
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec{3, me, sizeof(data), data};

    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    // DISPATCH_BASE lets one dispatch cover a contiguous slice of the tile
    // descriptor array, which is how the frame's tiles are grouped by lane
    // count without an extra push constant.
    ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = d->smA;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = &spec;
    // The lane cluster must not straddle a subgroup; a partial trailing
    // subgroup would break the ballot prefix (passA/README.md).
    if (d->has_size_control && mode == nxwarp_passA::kReadPtrBallot)
        ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    ci.layout = d->plA;
    VkPipeline p = VK_NULL_HANDLE;
    VKTRY(d, vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &p));
    d->pipesA[lanes] = p;
    *out = p;
    return NXVC_VKD_OK;
}

nxvc_vkd_status pipeline_b(D *d, uint32_t fmt, uint32_t store_words,
                           VkPipeline *out) {
    uint64_t key = ((uint64_t)fmt << 32) | store_words;
    auto it = d->pipesB.find(key);
    if (it != d->pipesB.end()) {
        *out = it->second;
        return NXVC_VKD_OK;
    }
    size_t lds = (size_t)store_words * 4 + 512;
    if (lds > d->props.limits.maxComputeSharedMemorySize)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "Pass B needs %zu B of shared memory, device offers %u B",
                      lds, d->props.limits.maxComputeSharedMemorySize);
    const uint32_t data[2] = {fmt, store_words};
    VkSpecializationMapEntry me[2] = {{0, 0, 4}, {1, 4, 4}};
    VkSpecializationInfo spec{2, me, sizeof(data), data};
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = d->smB;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = &spec;
    ci.layout = d->plB;
    VkPipeline p = VK_NULL_HANDLE;
    VKTRY(d, vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &p));
    d->pipesB[key] = p;
    *out = p;
    return NXVC_VKD_OK;
}

// ------------------------------------------------------------- resources
VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) / a * a;
}

nxvc_vkd_status ensure_bits(D *d, VkDeviceSize bytes);

nxvc_vkd_status check_storage_format(D *d, VkFormat f) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(d->phys, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "VkFormat %d is not usable as a storage image", (int)f);
    return NXVC_VKD_OK;
}

nxvc_vkd_status make_resources(D *d) {
    const StreamInfo &si = d->si;
    const uint32_t ntiles = si.tile_count;
    const bool chroma420 = si.chroma == 0;
    const uint32_t coef_stride = (uint32_t)nxvw::nxvw_coef_stride_i16(
        chroma420 ? 1 : 0, si.alpha ? 1 : 0);
    const uint32_t cbf_words = nxwarp_passA::kCbfWordsPerTile;

    // Resolve the output format.
    uint32_t want = d->want_output;
    if (want == NXVC_VKD_OUT_AUTO)
        want = (chroma420 && si.color_transform == 0) ? NXVC_VKD_OUT_YCBCR420
                                                      : NXVC_VKD_OUT_RGBA8;
    if (want == NXVC_VKD_OUT_YCBCR420 &&
        !(chroma420 && si.color_transform == 0))
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "the two-plane 4:2:0 output needs a 4:2:0 stream with no "
                      "colour transform");
    d->out_format = want == NXVC_VKD_OUT_RGBA8      ? (uint32_t)nxvw::kOutRgba8
                    : want == NXVC_VKD_OUT_RGB10A2 ? (uint32_t)nxvw::kOutRgb10A2
                                                   : (uint32_t)nxvw::kOutYcbcr420;
    // The two-plane path writes no alpha.  A stream that carries one gets a
    // second Pass B dispatch in the RGBA8 format, whose A channel is exactly
    // the alpha plane at its full 64x64-per-tile extent -- the same value the
    // reference decoder writes into plane 3.
    d->need_alpha_pass =
        (d->out_format == (uint32_t)nxvw::kOutYcbcr420) && si.alpha != 0;

    const uint32_t W = si.width, H = si.height;
    const uint32_t CW = (W + 1) / 2, CH = (H + 1) / 2;

    // ---- images
    nxvc_vkd_status st;
    // Pass A's binding 0 must point at a real buffer before the descriptor
    // writes below; ensure_bits() grows it again when a frame needs more.
    if ((st = ensure_bits(d, 1u << 16))) return st;
    const bool needRgba = d->out_format == (uint32_t)nxvw::kOutRgba8 ||
                          d->need_alpha_pass;
    const bool needRgb10 = d->out_format == (uint32_t)nxvw::kOutRgb10A2;
    const bool needYuv = d->out_format == (uint32_t)nxvw::kOutYcbcr420;
    if (needRgba && (st = check_storage_format(d, VK_FORMAT_R8G8B8A8_UINT)))
        return st;
    if (needRgb10 &&
        (st = check_storage_format(d, VK_FORMAT_A2B10G10R10_UINT_PACK32)))
        return st;
    if (needYuv) {
        if ((st = check_storage_format(d, VK_FORMAT_R8_UINT))) return st;
        if ((st = check_storage_format(d, VK_FORMAT_R8G8_UINT))) return st;
    }
    // Unused bindings still have to point at a real storage image, so the
    // formats the frame does not write get a 1x1 placeholder.
    if ((st = make_img(d, d->imgRgba, VK_FORMAT_R8G8B8A8_UINT,
                       needRgba ? W : 1, needRgba ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgRgb10, VK_FORMAT_A2B10G10R10_UINT_PACK32,
                       needRgb10 ? W : 1, needRgb10 ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgLuma, VK_FORMAT_R8_UINT, needYuv ? W : 1,
                       needYuv ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgCbCr, VK_FORMAT_R8G8_UINT, needYuv ? CW : 1,
                       needYuv ? CH : 1)))
        return st;

    // ---- buffers
    const VkBufferUsageFlags kSsbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkDeviceSize coefBytes = (VkDeviceSize)ntiles * coef_stride * 2;
    if ((st = make_buf(d, d->bCoef, coefBytes, kSsbo, false))) return st;
    if ((st = make_buf(d, d->bCbf, (VkDeviceSize)ntiles * cbf_words * 4, kSsbo,
                       false)))
        return st;
    // Host-visible: one uint per tile, written once by Pass A and read on the
    // CPU right after the wait, so it costs nothing to keep it mappable.
    // One uint per *descriptor slot*, which is the padded, lane-grouped array
    // Pass A dispatches over -- not the tile count.
    if ((st = make_buf(d, d->bStatus, (VkDeviceSize)(ntiles + 64) * 4,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)))
        return st;
    if ((st = make_buf(d, d->bDesc,
                       (VkDeviceSize)(ntiles + 64) *
                           nxwarp_passA::kTileDescUints * 4,
                       kSsbo, false)))
        return st;
    if ((st = make_buf(d, d->bTables,
                       (VkDeviceSize)nxwarp_passA::kNumTableSets *
                           nxwarp_passA::kNumCtx * nxwarp_passA::kNumSym * 4,
                       kSsbo, false)))
        return st;
    if ((st = make_buf(d, d->bRecs, (VkDeviceSize)ntiles * 16, kSsbo, false)))
        return st;
    if ((st = make_buf(d, d->bWgt, 128 * 4, kSsbo, false))) return st;

    // ---- readback
    if (d->flags & NXVC_VKD_FLAG_READBACK) {
        VkDeviceSize o = 0;
        d->rbLuma = o;
        if (needYuv) o += align_up((VkDeviceSize)W * H, 256);
        d->rbCbCr = o;
        if (needYuv) o += align_up((VkDeviceSize)CW * CH * 2, 256);
        d->rbRgba = o;
        if (needRgba || needRgb10) o += align_up((VkDeviceSize)W * H * 4, 256);
        d->rbBytes = o ? o : 4;
        if ((st = make_buf(d, d->bRead, d->rbBytes,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)))
            return st;
    }

    // ---- descriptor writes
    VkDescriptorBufferInfo a[6] = {{d->bBits.buf, 0, VK_WHOLE_SIZE},
                                   {d->bDesc.buf, 0, VK_WHOLE_SIZE},
                                   {d->bTables.buf, 0, VK_WHOLE_SIZE},
                                   {d->bCoef.buf, 0, VK_WHOLE_SIZE},
                                   {d->bCbf.buf, 0, VK_WHOLE_SIZE},
                                   {d->bStatus.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorBufferInfo b[3] = {{d->bCoef.buf, 0, VK_WHOLE_SIZE},
                                   {d->bRecs.buf, 0, VK_WHOLE_SIZE},
                                   {d->bWgt.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo im[4] = {
        {VK_NULL_HANDLE, d->imgRgba.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgRgb10.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgLuma.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgCbCr.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[13]{};
    uint32_t nw = 0;
    for (int i = 0; i < 6; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetA;
        w[nw].dstBinding = (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &a[i];
        ++nw;
    }
    for (int i = 0; i < 3; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &b[i];
        ++nw;
    }
    for (int i = 0; i < 4; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = (uint32_t)(3 + i);
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[nw].pImageInfo = &im[i];
        ++nw;
    }
    vkUpdateDescriptorSets(d->dev, nw, w, 0, nullptr);
    d->resources_ready = true;
    return NXVC_VKD_OK;
}

// The bitstream buffer follows the frame size, so it is (re)made per frame
// when a frame is bigger than anything seen before.  Growing it invalidates
// descriptor binding 0 of Pass A, which is rewritten here.
nxvc_vkd_status ensure_bits(D *d, VkDeviceSize bytes) {
    // Pass A reads the bitstream as uints and may touch up to 16 bytes past
    // the last tile byte (vk/decoder/passA/README.md).
    VkDeviceSize want = align_up(bytes + 16, 4096);
    if (d->bBits.buf && d->bBits.size >= want) return NXVC_VKD_OK;
    nxvc_vkd_status st =
        make_buf(d, d->bBits, want,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 false);
    if (st) return st;
    if (d->dsetA) {
        VkDescriptorBufferInfo bi{d->bBits.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = d->dsetA;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d->dev, 1, &w, 0, nullptr);
    }
    return NXVC_VKD_OK;
}

void buffer_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                    VkPipelineStageFlags dst, VkAccessFlags sa,
                    VkAccessFlags da) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = sa;
    mb.dstAccessMask = da;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void image_to_general(VkCommandBuffer cmd, VkImage img) {
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &ib);
}

}  // namespace

// ===========================================================================
// C ABI
// ===========================================================================
extern "C" const char *nxvc_vk_decoder_status_string(nxvc_vkd_status s) {
    switch (s) {
        case NXVC_VKD_OK: return "ok";
        case NXVC_VKD_ERR_ARG: return "bad argument";
        case NXVC_VKD_ERR_UNSUPPORTED: return "unsupported";
        case NXVC_VKD_ERR_VULKAN: return "vulkan error";
        case NXVC_VKD_ERR_NOMEM: return "out of memory";
        case NXVC_VKD_ERR_NO_DEVICE: return "no usable device";
        case NXVC_VKD_ERR_INTERNAL: return "internal error";
        case NXVC_VKD_ERR_BITSTREAM: return "malformed bitstream";
        case NXVC_VKD_ERR_TRUNCATED: return "truncated bitstream";
        case NXVC_VKD_ERR_VERSION: return "unsupported version or tool";
    }
    return "unknown";
}

extern "C" void nxvc_vk_decoder_create_info_default(nxvc_vkd_create_info *ci) {
    if (!ci) return;
    std::memset(ci, 0, sizeof *ci);
    ci->output_format = NXVC_VKD_OUT_AUTO;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_create(
    const nxvc_vkd_create_info *ci, nxvc_vk_decoder **out) {
    if (!ci || !out) return NXVC_VKD_ERR_ARG;
    *out = nullptr;
    D *d = new (std::nothrow) D();
    if (!d) return NXVC_VKD_ERR_NOMEM;
    d->want_output = ci->output_format;
    d->flags = ci->flags;

    nxvc_vkd_status st;
    if (ci->device) {
        d->inst = ci->instance;
        d->phys = ci->physical_device;
        d->dev = ci->device;
        d->queue = ci->queue;
        d->qfam = ci->queue_family;
        if (!d->phys || !d->queue) {
            nxvc_vk_decoder_destroy(d);
            return NXVC_VKD_ERR_ARG;
        }
    } else if ((st = create_device(d, ci))) {
        *out = d;  // hand back the decoder so the caller can read last_error
        return st;
    }
    if ((st = probe_device(d))) {
        *out = d;
        return st;
    }

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = d->qfam;
    if (vkCreateCommandPool(d->dev, &cp, nullptr, &d->pool) != VK_SUCCESS) {
        *out = d;
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkCreateCommandPool failed");
    }
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = d->pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d->dev, &cb, &d->cmd) != VK_SUCCESS) {
        *out = d;
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkAllocateCommandBuffers failed");
    }

    VkSemaphoreTypeCreateInfo sti{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sti.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &sti;
    if (vkCreateSemaphore(d->dev, &sci, nullptr, &d->timeline) != VK_SUCCESS) {
        *out = d;
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "timeline semaphores are not available");
    }

    if (d->have_timestamps) {
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 4;
        if (vkCreateQueryPool(d->dev, &qp, nullptr, &d->queries) != VK_SUCCESS)
            d->have_timestamps = false;
    }

    if ((st = make_layouts(d))) {
        *out = d;
        return st;
    }
    *out = d;
    return NXVC_VKD_OK;
}

extern "C" void nxvc_vk_decoder_destroy(nxvc_vk_decoder *d) {
    if (!d) return;
    if (d->dev) {
        vkDeviceWaitIdle(d->dev);
        for (auto &kv : d->pipesA) vkDestroyPipeline(d->dev, kv.second, nullptr);
        for (auto &kv : d->pipesB) vkDestroyPipeline(d->dev, kv.second, nullptr);
        if (d->smA) vkDestroyShaderModule(d->dev, d->smA, nullptr);
        if (d->smB) vkDestroyShaderModule(d->dev, d->smB, nullptr);
        if (d->plA) vkDestroyPipelineLayout(d->dev, d->plA, nullptr);
        if (d->plB) vkDestroyPipelineLayout(d->dev, d->plB, nullptr);
        if (d->dpool) vkDestroyDescriptorPool(d->dev, d->dpool, nullptr);
        if (d->dslA) vkDestroyDescriptorSetLayout(d->dev, d->dslA, nullptr);
        if (d->dslB) vkDestroyDescriptorSetLayout(d->dev, d->dslB, nullptr);
        if (d->queries) vkDestroyQueryPool(d->dev, d->queries, nullptr);
        if (d->timeline) vkDestroySemaphore(d->dev, d->timeline, nullptr);
        if (d->pool) vkDestroyCommandPool(d->dev, d->pool, nullptr);
        for (Buf *b : {&d->staging, &d->bBits, &d->bDesc, &d->bTables,
                       &d->bCoef, &d->bCbf, &d->bStatus, &d->bRecs, &d->bWgt,
                       &d->bRead})
            destroy_buf(d, *b);
        for (Img *i : {&d->imgRgba, &d->imgRgb10, &d->imgLuma, &d->imgCbCr})
            destroy_img(d, *i);
        if (d->own_device) vkDestroyDevice(d->dev, nullptr);
    }
    if (d->own_instance && d->inst) vkDestroyInstance(d->inst, nullptr);
    delete d;
}

extern "C" const char *nxvc_vk_decoder_last_error(const nxvc_vk_decoder *d) {
    return d ? d->err.c_str() : "null decoder";
}

extern "C" const char *nxvc_vk_decoder_device_name(const nxvc_vk_decoder *d) {
    return d ? d->device_name.c_str() : "";
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_parse_stream_header(
    nxvc_vk_decoder *d, const uint8_t *buf, size_t len, size_t *consumed) {
    if (!d || !buf) return NXVC_VKD_ERR_ARG;
    nxvc_vkd_status st = nxvcvk::parse_stream_header(buf, len, d->si, consumed);
    if (st) return seterr(d, st, "stream header: %s",
                          nxvc_vk_decoder_status_string(st));
    d->have_stream = true;
    d->resources_ready = false;
    st = make_resources(d);
    if (st) return st;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_stream_info(
    const nxvc_vk_decoder *d, nxvc_vkd_stream_info *o) {
    if (!d || !o) return NXVC_VKD_ERR_ARG;
    if (!d->have_stream) return NXVC_VKD_ERR_BITSTREAM;
    const StreamInfo &s = d->si;
    std::memset(o, 0, sizeof *o);
    o->width = s.width;
    o->height = s.height;
    o->chroma = s.chroma;
    o->color_transform = s.color_transform;
    o->color_space = s.color_space;
    o->alpha = s.alpha;
    o->bit_depth = s.bit_depth;
    o->eyes = s.eyes;
    o->num_layers = s.num_layers;
    o->profile = s.profile;
    o->level = s.level;
    o->tools = s.tools;
    o->tiles_x = s.tiles_x;
    o->tiles_y = s.tiles_y;
    o->tile_count = s.tile_count;
    o->chroma_width = s.cw;
    o->chroma_height = s.ch;
    o->ext_len = s.ext_len;
    o->output_format = d->out_format == (uint32_t)nxvw::kOutRgba8
                           ? NXVC_VKD_OUT_RGBA8
                       : d->out_format == (uint32_t)nxvw::kOutRgb10A2
                           ? NXVC_VKD_OUT_RGB10A2
                           : NXVC_VKD_OUT_YCBCR420;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_plane_size(const nxvc_vk_decoder *d,
                                                      int plane, uint32_t *w,
                                                      uint32_t *h) {
    if (!d || !w || !h || plane < 0 || plane > 3) return NXVC_VKD_ERR_ARG;
    if (!d->have_stream) return NXVC_VKD_ERR_BITSTREAM;
    if (plane == 1 || plane == 2) {
        *w = d->si.cw;
        *h = d->si.ch;
    } else {
        *w = d->si.width;
        *h = d->si.height;
    }
    return NXVC_VKD_OK;
}

extern "C" VkSemaphore nxvc_vk_decoder_timeline(const nxvc_vk_decoder *d) {
    return d ? d->timeline : VK_NULL_HANDLE;
}
extern "C" uint64_t nxvc_vk_decoder_timeline_value(const nxvc_vk_decoder *d) {
    return d ? d->timeline_value : 0;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_wait(nxvc_vk_decoder *d,
                                                uint64_t timeout_ns) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (d->timeline_value == 0) return NXVC_VKD_OK;
    uint64_t v = d->timeline_value;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &d->timeline;
    wi.pValues = &v;
    VkResult r = vkWaitSemaphores(d->dev, &wi, timeout_ns);
    if (r == VK_TIMEOUT) return NXVC_VKD_ERR_INTERNAL;
    if (r != VK_SUCCESS)
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkWaitSemaphores: %d", (int)r);
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_images(const nxvc_vk_decoder *d,
                                                  nxvc_vkd_images *o) {
    if (!d || !o) return NXVC_VKD_ERR_ARG;
    std::memset(o, 0, sizeof *o);
    auto add = [&](const Img &i) {
        o->image[o->count] = i.img;
        o->view[o->count] = i.view;
        o->format[o->count] = i.fmt;
        o->width[o->count] = i.w;
        o->height[o->count] = i.h;
        ++o->count;
    };
    if (d->out_format == (uint32_t)nxvw::kOutYcbcr420) {
        add(d->imgLuma);
        add(d->imgCbCr);
        if (d->need_alpha_pass) add(d->imgRgba);
    } else if (d->out_format == (uint32_t)nxvw::kOutRgb10A2) {
        add(d->imgRgb10);
    } else {
        add(d->imgRgba);
    }
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_stats(const nxvc_vk_decoder *d,
                                                 nxvc_vkd_stats *o) {
    if (!d || !o) return NXVC_VKD_ERR_ARG;
    *o = d->stats;
    return NXVC_VKD_OK;
}

// --------------------------------------------------------------- decode
extern "C" nxvc_vkd_status nxvc_vk_decode_frame_ex(nxvc_vk_decoder *d,
                                                   const uint8_t *bytes,
                                                   size_t len,
                                                   uint32_t submit_flags,
                                                   size_t *consumed) {
    if (!d || !bytes) return NXVC_VKD_ERR_ARG;
    if (!d->have_stream || !d->resources_ready)
        return seterr(d, NXVC_VKD_ERR_BITSTREAM,
                      "no stream header parsed yet");
    const double t0 = now_ms();

    // ---- 1. host parse ------------------------------------------------
    FrameParse &fp = d->fp;
    nxvc_vkd_status st = nxvcvk::parse_frame(
        d->si, bytes, len, (d->flags & NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES) != 0,
        fp);
    if (st) return seterr(d, st, "frame: %s",
                          nxvc_vk_decoder_status_string(st));
    if (consumed) *consumed = fp.frame_bytes;
    const double t_parse = now_ms();

    const uint32_t ntiles = d->si.tile_count;
    if ((st = ensure_bits(d, fp.frame_bytes))) return st;

    // ---- 2. staging --------------------------------------------------
    const VkDeviceSize descBytes =
        (VkDeviceSize)fp.desc.size() * sizeof(nxvcvk::TileDesc);
    const VkDeviceSize tabBytes = (VkDeviceSize)fp.cum.size() * 4;
    const VkDeviceSize recBytes = (VkDeviceSize)ntiles * 16;
    VkDeviceSize o = 0;
    d->offBits = o;
    o = align_up(o + fp.frame_bytes, 256);
    d->offDesc = o;
    o = align_up(o + descBytes, 256);
    d->offTables = o;
    o = align_up(o + tabBytes, 256);
    d->offRecs = o;
    o = align_up(o + recBytes, 256);
    d->offWgt = o;
    o = align_up(o + 512, 256);
    if ((st = make_buf(d, d->staging, o, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       true)))
        return st;
    uint8_t *sp = (uint8_t *)d->staging.mapped;
    std::memcpy(sp + d->offBits, bytes, fp.frame_bytes);
    if (descBytes) std::memcpy(sp + d->offDesc, fp.desc.data(), descBytes);
    std::memcpy(sp + d->offTables, fp.cum.data(), tabBytes);
    std::memcpy(sp + d->offRecs, fp.recs.data(), recBytes);
    std::memcpy(sp + d->offWgt, fp.weights, 512);

    // ---- 3. record ----------------------------------------------------
    VKTRY(d, vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKTRY(d, vkBeginCommandBuffer(d->cmd, &bi));
    if (d->have_timestamps) {
        vkCmdResetQueryPool(d->cmd, d->queries, 0, 4);
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            d->queries, 0);
    }

    auto copy = [&](Buf &dst, VkDeviceSize src_off, VkDeviceSize n) {
        if (!n) return;
        VkBufferCopy c{src_off, 0, n};
        vkCmdCopyBuffer(d->cmd, d->staging.buf, dst.buf, 1, &c);
    };
    copy(d->bBits, d->offBits, fp.frame_bytes);
    copy(d->bDesc, d->offDesc, descBytes);
    copy(d->bTables, d->offTables, tabBytes);
    copy(d->bRecs, d->offRecs, recBytes);
    copy(d->bWgt, d->offWgt, 512);
    // A skipped tile gets no Pass A descriptor, so nothing would zero its
    // coefficient slot.  Zero it here; Pass B then reconstructs it as
    // "no coefficients" over the WARP_SKIP record.
    for (uint32_t t : fp.zero_tiles)
        vkCmdFillBuffer(d->cmd, d->bCoef.buf,
                        (VkDeviceSize)t * fp.coef_stride * 2,
                        (VkDeviceSize)fp.coef_stride * 2, 0);
    buffer_barrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    image_to_general(d->cmd, d->imgRgba.img);
    image_to_general(d->cmd, d->imgRgb10.img);
    image_to_general(d->cmd, d->imgLuma.img);
    image_to_general(d->cmd, d->imgCbCr.img);

    // ---- Pass A: one dispatch per distinct lane count -----------------
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->plA, 0,
                            1, &d->dsetA, 0, nullptr);
    uint32_t dispatches = 0;
    for (const LaneGroup &g : fp.groups) {
        VkPipeline p;
        if ((st = pipeline_a(d, g.lanes, &p))) return st;
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        const uint32_t push[4] = {g.limit, fp.frame_nplanes, fp.coef_stride,
                                  fp.cbf_words};
        vkCmdPushConstants(d->cmd, d->plA, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16,
                           push);
        const uint32_t tpg = nxwarp_passA::nxs_tiles_per_group(g.lanes);
        vkCmdDispatchBase(d->cmd, g.first / tpg, 0, 0, g.groups, 1, 1);
        ++dispatches;
    }
    if (d->have_timestamps)
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            d->queries, 1);
    buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // ---- Pass B -------------------------------------------------------
    const uint32_t storeWords =
        (uint32_t)(fp.push.planeWords0 + fp.push.planeWords1 +
                   fp.push.planeWords2 + fp.push.planeWords3);
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->plB, 0,
                            1, &d->dsetB, 0, nullptr);
    vkCmdPushConstants(d->cmd, d->plB, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       (uint32_t)sizeof(nxvw::NxvwPassBPush), &fp.push);
    {
        VkPipeline p;
        if ((st = pipeline_b(d, d->out_format, storeWords, &p))) return st;
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdDispatch(d->cmd, d->si.tiles_x, d->si.tiles_y, 1);
        ++dispatches;
    }
    if (d->need_alpha_pass) {
        // Second reconstruction in the RGBA8 format purely for the A channel;
        // the two-plane 4:2:0 store has nowhere to put alpha.
        VkPipeline p;
        if ((st = pipeline_b(d, (uint32_t)nxvw::kOutRgba8, storeWords, &p)))
            return st;
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdDispatch(d->cmd, d->si.tiles_x, d->si.tiles_y, 1);
        ++dispatches;
    }
    if (d->have_timestamps)
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            d->queries, 2);

    // ---- readback -----------------------------------------------------
    if (d->flags & NXVC_VKD_FLAG_READBACK) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        auto grab = [&](const Img &im, VkDeviceSize off) {
            VkBufferImageCopy c{};
            c.bufferOffset = off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {im.w, im.h, 1};
            vkCmdCopyImageToBuffer(d->cmd, im.img, VK_IMAGE_LAYOUT_GENERAL,
                                   d->bRead.buf, 1, &c);
        };
        if (d->out_format == (uint32_t)nxvw::kOutYcbcr420) {
            grab(d->imgLuma, d->rbLuma);
            grab(d->imgCbCr, d->rbCbCr);
            if (d->need_alpha_pass) grab(d->imgRgba, d->rbRgba);
        } else if (d->out_format == (uint32_t)nxvw::kOutRgb10A2) {
            grab(d->imgRgb10, d->rbRgba);
        } else {
            grab(d->imgRgba, d->rbRgba);
        }
    }
    // Pass A's status words are read back with the same submission.
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }
    if (d->have_timestamps)
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            d->queries, 3);
    VKTRY(d, vkEndCommandBuffer(d->cmd));

    // ---- 4. submit ----------------------------------------------------
    const uint64_t signal = ++d->timeline_value;
    VkTimelineSemaphoreSubmitInfo tsi{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.signalSemaphoreValueCount = 1;
    tsi.pSignalSemaphoreValues = &signal;
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.pNext = &tsi;
    su.commandBufferCount = 1;
    su.pCommandBuffers = &d->cmd;
    su.signalSemaphoreCount = 1;
    su.pSignalSemaphores = &d->timeline;
    VKTRY(d, vkQueueSubmit(d->queue, 1, &su, VK_NULL_HANDLE));
    const double t_submit = now_ms();

    d->stats.parse_ms = t_parse - t0;
    d->stats.submit_ms = t_submit - t_parse;
    d->stats.frame_bytes = fp.frame_bytes;
    d->stats.payload_bytes = fp.payload_bytes;
    d->stats.coef_bytes = (uint64_t)ntiles * fp.coef_stride * 2;
    d->stats.tiles = ntiles;
    d->stats.tiles_skipped = fp.tiles_skipped;
    d->stats.tiles_tskip = fp.tiles_tskip;
    d->stats.lane_groups = (uint32_t)fp.groups.size();
    d->stats.dispatches = dispatches;
    d->stats.pass_a_ms = d->stats.pass_b_ms = d->stats.gpu_ms = 0.0;
    ++d->stats.frames;

    if (submit_flags & NXVC_VKD_SUBMIT_ASYNC) {
        d->stats.total_ms = now_ms() - t0;
        return NXVC_VKD_OK;
    }
    if ((st = nxvc_vk_decoder_wait(d, UINT64_MAX))) return st;

    if (d->have_timestamps) {
        uint64_t ts[4] = {};
        if (vkGetQueryPoolResults(d->dev, d->queries, 0, 4, sizeof ts, ts,
                                  sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT |
                                      VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            const double k = (double)d->ts_period / 1e6;
            d->stats.pass_a_ms = (double)(ts[1] - ts[0]) * k;
            d->stats.pass_b_ms = (double)(ts[2] - ts[1]) * k;
            d->stats.gpu_ms = (double)(ts[3] - ts[0]) * k;
        }
    }
    // Pass A reports per tile.  A non-zero status means the entropy decoder
    // refused that tile's payload; the frame's pixels are not conformant, so
    // the call fails rather than handing back a plausible-looking image.
    if (d->bStatus.mapped) {
        const uint32_t *sw = (const uint32_t *)d->bStatus.mapped;
        for (const LaneGroup &g : fp.groups)
            for (uint32_t i = g.first; i < g.limit; ++i) {
                if (sw[i] == nxwarp_passA::kStatusOk) continue;
                static const char *kNames[5] = {
                    "ok", "truncated payload", "illegal symbol",
                    "bad tile header", "scheduling round overflow"};
                uint32_t c = sw[i] < 5 ? sw[i] : 0;
                return seterr(d, NXVC_VKD_ERR_BITSTREAM,
                              "Pass A refused tile %u (%u lanes): %s",
                              fp.desc_tile[i], g.lanes, kNames[c]);
            }
    }

    d->stats.total_ms = now_ms() - t0;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decode_frame(nxvc_vk_decoder *d,
                                                const uint8_t *bytes,
                                                size_t len, size_t *consumed) {
    return nxvc_vk_decode_frame_ex(d, bytes, len, 0, consumed);
}

// ------------------------------------------------------------- readback
extern "C" nxvc_vkd_status nxvc_vk_decoder_read_planes(
    nxvc_vk_decoder *d, uint8_t *const plane[4], const int32_t stride[4]) {
    if (!d || !plane || !stride) return NXVC_VKD_ERR_ARG;
    if (!(d->flags & NXVC_VKD_FLAG_READBACK))
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "the decoder was created without NXVC_VKD_FLAG_READBACK");
    nxvc_vkd_status st = nxvc_vk_decoder_wait(d, UINT64_MAX);
    if (st) return st;
    const StreamInfo &si = d->si;
    const uint8_t *rb = (const uint8_t *)d->bRead.mapped;
    const uint32_t W = si.width, H = si.height, CW = si.cw, CH = si.ch;

    if (d->out_format == (uint32_t)nxvw::kOutYcbcr420) {
        const uint8_t *Y = rb + d->rbLuma;
        const uint8_t *C = rb + d->rbCbCr;
        if (plane[0])
            for (uint32_t y = 0; y < H; ++y)
                std::memcpy(plane[0] + (size_t)y * stride[0], Y + (size_t)y * W,
                            W);
        for (uint32_t y = 0; y < CH; ++y)
            for (uint32_t x = 0; x < CW; ++x) {
                const uint8_t *s = C + ((size_t)y * CW + x) * 2;
                if (plane[1]) plane[1][(size_t)y * stride[1] + x] = s[0];
                if (plane[2]) plane[2][(size_t)y * stride[2] + x] = s[1];
            }
        if (si.alpha && plane[3]) {
            const uint8_t *A = rb + d->rbRgba;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    plane[3][(size_t)y * stride[3] + x] =
                        A[((size_t)y * W + x) * 4 + 3];
        }
        return NXVC_VKD_OK;
    }

    if (d->out_format == (uint32_t)nxvw::kOutRgb10A2) {
        const uint32_t *px = (const uint32_t *)(rb + d->rbRgba);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                uint32_t v = px[(size_t)y * W + x];
                uint32_t c[3] = {v & 1023u, (v >> 10) & 1023u,
                                 (v >> 20) & 1023u};
                for (int p = 0; p < 3; ++p)
                    if (plane[p])
                        plane[p][(size_t)y * stride[p] + x] =
                            (uint8_t)(c[p] >> 2);
                if (si.alpha && plane[3])
                    plane[3][(size_t)y * stride[3] + x] =
                        (uint8_t)(((v >> 30) & 3u) * 85u);
            }
        return NXVC_VKD_OK;
    }

    const uint8_t *px = rb + d->rbRgba;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t *s = px + ((size_t)y * W + x) * 4;
            for (int p = 0; p < 3; ++p)
                if (plane[p]) plane[p][(size_t)y * stride[p] + x] = s[p];
            if (si.alpha && plane[3])
                plane[3][(size_t)y * stride[3] + x] = s[3];
        }
    (void)CW;
    (void)CH;
    return NXVC_VKD_OK;
}
