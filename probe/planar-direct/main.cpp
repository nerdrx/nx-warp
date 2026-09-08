// Offscreen fresh-frame PLANAR rendering experiment. No decoder GPU passes.
// SPDX-License-Identifier: Apache-2.0
#include "nxvc_vkdec_parse.h"
#include "passB/syntax_constants.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>

static void check(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Vulkan error " + std::to_string(r));
}
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("cannot open " + path);
    auto n = f.tellg();
    if (n < 0)
        throw std::runtime_error("cannot size " + path);
    std::vector<uint8_t> out(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    if (!f)
        throw std::runtime_error("cannot read " + path);
    return out;
}
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
struct Buffer {
    VkBuffer b{};
    VkDeviceMemory mem{};
    void* ptr{};
    VkDeviceSize bytes{};
};
struct Push {
    int32_t qp, chroma_qp, width, height;
};
struct DrawTiming {
    double gpu = -1, record = 0, submit = 0, fence = 0, query = 0;
};
struct TileSpan {
    uint32_t first, count;
};

struct Probe {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{}, timestamp_bits{};
    float timestamp_period{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkQueryPool queries{};
    VkDescriptorSetLayout dsl{};
    VkDescriptorPool dp{};
    VkDescriptorSet ds{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkRenderPass pass{};
    VkImage image{};
    VkDeviceMemory image_mem{};
    VkImageView view{};
    VkFramebuffer fb{};
    Buffer planar, records;
    uint32_t w{}, h{};
    bool spin = std::getenv("NX_PLANAR_SPIN") != nullptr;
    bool gpu_palette = std::getenv("NX_PLANAR_GPU_PALETTE") != nullptr;
    bool tile_mode = gpu_palette || std::getenv("NX_PLANAR_TILE") != nullptr;
    bool foveated = std::getenv("NX_PLANAR_FOVEATED") != nullptr;
    bool compact = !gpu_palette && (tile_mode || std::getenv("NX_PLANAR_COMPACT") != nullptr);
    bool flat = !gpu_palette && (compact || std::getenv("NX_PLANAR_FLAT") != nullptr);
    bool reuse_commands = std::getenv("NX_PLANAR_REUSE_COMMANDS") != nullptr;
    bool queue_priority_requested = false;
    VkQueueGlobalPriorityEXT queue_priority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
    bool owns_context = true;
    bool have_recorded_push = false;
    Push recorded_push{};
    bool image_initialized = false;
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
                return i;
        throw std::runtime_error("no compatible memory type");
    }
    Buffer buffer(VkDeviceSize n, VkBufferUsageFlags use) {
        Buffer out{};
        out.bytes = n;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = n;
        bi.usage = use;
        check(vkCreateBuffer(device, &bi, nullptr, &out.b));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, out.b, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memory_type(req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &ai, nullptr, &out.mem));
        check(vkBindBufferMemory(device, out.b, out.mem, 0));
        check(vkMapMemory(device, out.mem, 0, n, 0, &out.ptr));
        return out;
    }
    void free_buffer(Buffer& b) {
        if (b.ptr)
            vkUnmapMemory(device, b.mem);
        if (b.b)
            vkDestroyBuffer(device, b.b, nullptr);
        if (b.mem)
            vkFreeMemory(device, b.mem, nullptr);
        b = {};
    }
    VkShaderModule shader(const std::string& path) {
        auto bytes = read_file(path);
        if (bytes.size() % 4)
            throw std::runtime_error("bad SPIR-V size");
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes.size();
        ci.pCode = words.data();
        VkShaderModule mod{};
        check(vkCreateShaderModule(device, &ci, nullptr, &mod));
        return mod;
    }
    void init(uint32_t width, uint32_t height, uint32_t tiles, const std::string& dir,
              const Probe* shared = nullptr) {
        w = width;
        h = height;
        if (!shared) {
            const char* requested = std::getenv("NX_PLANAR_QUEUE_PRIORITY");
            if (requested && requested[0]) {
                queue_priority_requested = true;
                if (std::strcmp(requested, "high") == 0)
                    queue_priority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
                else if (std::strcmp(requested, "realtime") == 0)
                    queue_priority = VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT;
                else
                    throw std::runtime_error("NX_PLANAR_QUEUE_PRIORITY must be high or realtime");
            }
        }
        if (shared) {
            instance = shared->instance;
            physical = shared->physical;
            device = shared->device;
            queue = shared->queue;
            family = shared->family;
            timestamp_bits = shared->timestamp_bits;
            timestamp_period = shared->timestamp_period;
            owns_context = false;
        } else {
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "nx-planar-direct";
            app.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            ii.pApplicationInfo = &app;
            check(vkCreateInstance(&ii, nullptr, &instance));
            uint32_t n = 0;
            check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
            std::vector<VkPhysicalDevice> devices(n);
            check(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
            for (auto p : devices) {
                uint32_t nq = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, nullptr);
                std::vector<VkQueueFamilyProperties> qs(nq);
                vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, qs.data());
                for (uint32_t i = 0; i < nq; ++i)
                    if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                        physical = p;
                        family = i;
                        timestamp_bits = qs[i].timestampValidBits;
                        break;
                    }
                if (physical)
                    break;
            }
            if (!physical)
                throw std::runtime_error("no graphics device");
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical, &props);
            timestamp_period = props.limits.timestampPeriod;
            std::fprintf(stderr, "device=%s output=%ux%u timestamp_bits=%u\n", props.deviceName, w,
                         h, timestamp_bits);
            float priority = 1;
            VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qi.queueFamilyIndex = family;
            qi.queueCount = 1;
            qi.pQueuePriorities = &priority;
            VkDeviceQueueGlobalPriorityCreateInfoEXT global_priority{
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT};
            const char* priority_ext = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
            if (queue_priority_requested) {
                uint32_t ne = 0;
                check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, nullptr));
                std::vector<VkExtensionProperties> exts(ne);
                check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, exts.data()));
                bool supported = false;
                for (const auto& ext : exts)
                    supported = supported || std::strcmp(ext.extensionName, priority_ext) == 0;
                if (!supported)
                    throw std::runtime_error("NX_PLANAR_QUEUE_PRIORITY requested but "
                                             "VK_EXT_global_priority is unavailable");
                global_priority.globalPriority = queue_priority;
                qi.pNext = &global_priority;
                std::fprintf(stderr, "requested_queue_priority=%s\n",
                             queue_priority == VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT ? "REALTIME"
                                                                                     : "HIGH");
            }
            VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            di.queueCreateInfoCount = 1;
            di.pQueueCreateInfos = &qi;
            if (queue_priority_requested) {
                di.enabledExtensionCount = 1;
                di.ppEnabledExtensionNames = &priority_ext;
            }
            VkResult create_result = vkCreateDevice(physical, &di, nullptr, &device);
            if (create_result != VK_SUCCESS && queue_priority_requested)
                throw std::runtime_error(
                    "NX_PLANAR_QUEUE_PRIORITY rejected by Vulkan (unsupported or not permitted): " +
                    std::to_string(create_result));
            check(create_result);
            vkGetDeviceQueue(device, family, 0, &queue);
        }
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &ca, &cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fi, nullptr, &fence));
        if (timestamp_bits) {
            VkQueryPoolCreateInfo qu{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qu.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qu.queryCount = 2;
            check(vkCreateQueryPool(device, &qu, nullptr, &queries));
        }
        planar = buffer(VkDeviceSize(tiles) * 26 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        records = buffer(VkDeviceSize(tiles) * sizeof(nxvw::NxvwTileRec),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 2;
        dl.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &dsl));
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        check(vkCreateDescriptorPool(device, &dpi, nullptr, &dp));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = dp;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &dsl;
        check(vkAllocateDescriptorSets(device, &da, &ds));
        VkDescriptorBufferInfo db[2]{{planar.b, 0, planar.bytes}, {records.b, 0, records.bytes}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = ds;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &db[i];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent = {w, h, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   (foveated ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
        check(vkCreateImage(device, &ic, nullptr, &image));
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device, image, &mr);
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mi.allocationSize = mr.size;
        mi.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &mi, nullptr, &image_mem));
        check(vkBindImageMemory(device, image, image_mem, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ic.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &view));
        VkAttachmentDescription ad{};
        ad.format = ic.format;
        ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = foveated ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad.initialLayout =
            foveated ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        ad.finalLayout = foveated ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ar;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = foveated ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                        : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        deps[0].srcAccessMask = foveated ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &ad;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = deps;
        check(vkCreateRenderPass(device, &rp, nullptr, &pass));
        VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fc.renderPass = pass;
        fc.attachmentCount = 1;
        fc.pAttachments = &view;
        fc.width = w;
        fc.height = h;
        fc.layers = 1;
        check(vkCreateFramebuffer(device, &fc, nullptr, &fb));
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(Push)};
        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lc.setLayoutCount = 1;
        lc.pSetLayouts = &dsl;
        lc.pushConstantRangeCount = 1;
        lc.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(device, &lc, nullptr, &layout));
        VkShaderModule vs = shader(dir + (gpu_palette ? "/gpu-palette.vert.spv"
                                          : tile_mode ? "/tile.vert.spv"
                                                      : "/planar.vert.spv")),
                       fs = shader(dir + (tile_mode ? "/tile.frag.spv"
                                          : compact ? "/compact.frag.spv"
                                          : flat    ? "/flat.frag.spv"
                                                    : "/planar.frag.spv"));
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = i ? fs : vs;
            stages[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vertex{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, float(w), float(h), 0, 1};
        VkRect2D scissor{{0, 0}, {w, h}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.pViewports = &viewport;
        vp.scissorCount = 1;
        vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo sample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        sample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo bc{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bc.attachmentCount = 1;
        bc.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vertex;
        gp.pInputAssemblyState = &assembly;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &sample;
        gp.pColorBlendState = &bc;
        gp.layout = layout;
        gp.renderPass = pass;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline));
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
    }
    void begin() {
        check(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = reuse_commands ? 0 : VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &bi));
        if (foveated && !image_initialized) {
            VkImageMemoryBarrier to_clear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_clear.srcAccessMask = 0;
            to_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_clear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_clear.image = image;
            to_clear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_clear);
            VkClearColorValue clear{};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                 &to_clear.subresourceRange);
            to_clear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_clear.dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_clear.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_clear.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &to_clear);
        }
    }
    DrawTiming submit_wait(bool end_command) {
        DrawTiming t;
        auto a = Clock::now();
        if (end_command)
            check(vkEndCommandBuffer(cmd));
        auto b = Clock::now();
        check(vkResetFences(device, 1, &fence));
        auto c = Clock::now();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &si, fence));
        auto d = Clock::now();
        if (spin) {
            VkResult status;
            do {
                status = vkGetFenceStatus(device, fence);
            } while (status == VK_NOT_READY);
            check(status);
        } else
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        auto e = Clock::now();
        t.record = ms(a, b);
        t.submit = ms(c, d);
        t.fence = ms(d, e);
        return t;
    }
    DrawTiming pending_timing{};
    void submit_draw(const Push& push) {
        pending_timing = {};
        auto a = Clock::now();
        bool rerecord = !reuse_commands || !have_recorded_push ||
                        std::memcmp(&push, &recorded_push, sizeof(push)) != 0;
        if (rerecord) {
            begin();
            if (queries) {
                vkCmdResetQueryPool(cmd, queries, 0, 2);
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
            }
            VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            ri.renderPass = pass;
            ri.framebuffer = fb;
            ri.renderArea = {{0, 0}, {w, h}};
            vkCmdBeginRenderPass(cmd, &ri, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(cmd, tile_mode ? 6 : 3, tile_mode ? (w / 64) * (h / 64) : 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            if (queries)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
            check(vkEndCommandBuffer(cmd));
            recorded_push = push;
            have_recorded_push = true;
        }
        pending_timing.record = ms(a, Clock::now());
        check(vkResetFences(device, 1, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        auto b = Clock::now();
        check(vkQueueSubmit(queue, 1, &submit, fence));
        pending_timing.submit = ms(b, Clock::now());
        if (foveated && !image_initialized)
            have_recorded_push = false;  // Do not replay the one-time image clear.
        image_initialized = true;
    }
    DrawTiming draw_spans(const Push& push, const std::vector<TileSpan>& spans) {
        have_recorded_push = false;  // This recording replaces any cached full draw.
        auto record_start = Clock::now();
        begin();
        if (queries) {
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        }
        VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        ri.renderPass = pass;
        ri.framebuffer = fb;
        ri.renderArea = {{0, 0}, {w, h}};
        vkCmdBeginRenderPass(cmd, &ri, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        for (const TileSpan& s : spans)
            vkCmdDraw(cmd, 6, s.count, 0, s.first);
        vkCmdEndRenderPass(cmd);
        if (queries)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        check(vkEndCommandBuffer(cmd));
        DrawTiming t;
        t.record = ms(record_start, Clock::now());
        check(vkResetFences(device, 1, &fence));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        auto submit_start = Clock::now();
        check(vkQueueSubmit(queue, 1, &si, fence));
        t.submit = ms(submit_start, Clock::now());
        auto wait_start = Clock::now();
        if (spin) {
            VkResult status;
            do {
                status = vkGetFenceStatus(device, fence);
            } while (status == VK_NOT_READY);
            check(status);
        } else
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        t.fence = ms(wait_start, Clock::now());
        auto query_start = Clock::now();
        if (queries) {
            uint64_t ticks[2]{};
            check(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks,
                                        sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            uint64_t delta = ticks[1] - ticks[0];
            if (timestamp_bits < 64)
                delta &= (uint64_t(1) << timestamp_bits) - 1;
            t.gpu = double(delta) * timestamp_period / 1e6;
        }
        t.query = ms(query_start, Clock::now());
        image_initialized = true;
        return t;
    }
    DrawTiming finish_draw() {
        DrawTiming t = pending_timing;
        auto a = Clock::now();
        if (spin) {
            VkResult status;
            do {
                status = vkGetFenceStatus(device, fence);
            } while (status == VK_NOT_READY);
            check(status);
        } else
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        t.fence = ms(a, Clock::now());
        if (queries) {
            auto q = Clock::now();
            uint64_t ticks[2]{};
            check(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks,
                                        sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            uint64_t delta = ticks[1] - ticks[0];
            if (timestamp_bits < 64)
                delta &= (uint64_t(1) << timestamp_bits) - 1;
            t.gpu = double(delta) * timestamp_period / 1e6;
            t.query = ms(q, Clock::now());
        }
        return t;
    }
    DrawTiming draw(const Push& push) {
        submit_draw(push);
        return finish_draw();
    }
    void prepare(const nxvcvk::FrameParse& fp, uint32_t ntiles) {
        if (gpu_palette) {
            for (uint32_t t = 0; t < ntiles; ++t) {
                const uint32_t header = fp.planar[26 * t];
                if ((header & 3u) != 0u || (header & 8u) != 0u)
                    throw std::runtime_error("gpu palette requires R2 coarse tiles");
            }
            std::memcpy(planar.ptr, fp.planar.data(), planar.bytes);
            std::memcpy(records.ptr, fp.recs.data(), records.bytes);
            return;
        }
        if (!compact)
            std::memcpy(planar.ptr, fp.planar.data(), planar.bytes);
        if (!flat)
            std::memcpy(records.ptr, fp.recs.data(), records.bytes);
        else {
            // The flat-region experiment intentionally discards nonzero slopes.
            // Quantize once per region, not once for every displayed pixel.
            auto* palette = static_cast<uint32_t*>(records.ptr);
            for (uint32_t t = 0; t < ntiles; ++t) {
                const auto* body = fp.planar.data() + 26 * t;
                int delta = int((fp.recs[t].w1 >> 8) & 63);
                if (delta >= 32)
                    delta -= 64;
                int qp = std::clamp(fp.push.baseQp + delta, 0, 63);
                int cqp = std::clamp(qp + fp.push.chromaQpOff, 0, 63);
                int regions = int(body[0] & 3) + 2;
                if (compact && (regions != 2 || (body[0] & 8)))
                    throw std::runtime_error("compact requires R2 coarse tiles");
                for (int r = 0; r < (compact ? 2 : 4); ++r) {
                    if (r >= regions) {
                        palette[t * 4 + r] = 0xff000000u;
                        continue;
                    }
                    int v[3]{};
                    for (int plane = 0; plane < 3; ++plane) {
                        int i = (r * 3 + plane) * 3;
                        int q = int((body[17 + (i >> 2)] >> ((i & 3) * 8)) & 255);
                        if (q >= 128)
                            q -= 256;
                        int step = nxvw::kQStep[(plane ? cqp : qp) >> 1];
                        v[plane] = std::clamp(128 + ((q * step + 8) >> 4), 0, 255);
                    }
                    // Match the existing atlas consumer's full-range BT.709 matrix.
                    float cb = v[1] - 128.f, cr = v[2] - 128.f;
                    auto byte = [](float x) { return uint32_t(std::clamp(int(x + 0.5f), 0, 255)); };
                    palette[t * 4 + r + (compact ? 2 : 0)] =
                        byte(v[0] + 1.5748f * cr) | (byte(v[0] - .1873f * cb - .4681f * cr) << 8) |
                        (byte(v[0] + 1.8556f * cb) << 16) | 0xff000000u;
                }
                if (compact) {
                    palette[t * 4] = body[1];
                    palette[t * 4 + 1] = body[2];
                }
            }
        }
    }
    void readback(const std::string& path) {
        Buffer out = buffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        begin();
        if (foveated) {
            VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_src.image = image;
            to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_src);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.b, 1, &copy);
        submit_wait(true);
        auto* pixels = static_cast<const char*>(out.ptr);
        std::ofstream raw(path + ".rgba", std::ios::binary);
        raw.write(pixels, out.bytes);
        std::ofstream ppm(path + ".ppm", std::ios::binary);
        ppm << "P6\n" << w << " " << h << "\n255\n";
        for (uint64_t i = 0; i < uint64_t(w) * h; ++i) {
            ppm.write(pixels + 4 * i, 3);
        }
        if (!raw || !ppm)
            throw std::runtime_error("readback write failed");
        free_buffer(out);
    }
    ~Probe() {
        if (device) {
            vkDeviceWaitIdle(device);
            free_buffer(planar);
            free_buffer(records);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyFramebuffer(device, fb, nullptr);
            vkDestroyRenderPass(device, pass, nullptr);
            vkDestroyImageView(device, view, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, image_mem, nullptr);
            vkDestroyDescriptorPool(device, dp, nullptr);
            vkDestroyDescriptorSetLayout(device, dsl, nullptr);
            vkDestroyQueryPool(device, queries, nullptr);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            if (owns_context)
                vkDestroyDevice(device, nullptr);
        }
        if (instance && owns_context)
            vkDestroyInstance(instance, nullptr);
    }
};
int main(int argc, char** argv) try {
    if (argc < 3 || argc > 5) {
        std::fprintf(stderr,
                     "usage: nx-planar-direct STREAM SHADER_DIR [MAX_FRAMES] [READBACK_PREFIX]\n");
        return 2;
    }
    auto data = read_file(argv[1]);
    nxvcvk::StreamInfo si{};
    size_t offset = 0;
    if (nxvcvk::parse_stream_header(data.data(), data.size(), si, &offset) != NXVC_VKD_OK)
        throw std::runtime_error("invalid stream header");
    // Restrict this proof to independent complete pictures and aligned stereo tiles.
    if (si.bit_depth != 8 || si.chroma != 0 || si.color_transform != 0 || si.alpha ||
        si.width % 64 || si.height % 64)
        throw std::runtime_error("requires aligned 8-bit 420 CT_NONE without alpha");
    const bool foveated = std::getenv("NX_PLANAR_FOVEATED") != nullptr;
    const bool foveated_single_pass = std::getenv("NX_PLANAR_FOVEATED_SINGLE_PASS") != nullptr;
    if (foveated_single_pass && !foveated)
        throw std::runtime_error("NX_PLANAR_FOVEATED_SINGLE_PASS requires NX_PLANAR_FOVEATED");
    if (foveated) {
        if (std::getenv("NX_PLANAR_ASYNC"))
            throw std::runtime_error("NX_PLANAR_FOVEATED is synchronous; reject NX_PLANAR_ASYNC");
        if (!std::getenv("NX_PLANAR_TILE") && !std::getenv("NX_PLANAR_GPU_PALETTE"))
            throw std::runtime_error("NX_PLANAR_FOVEATED requires the tile renderer");
        if (si.eyes != 2 || si.tiles_x == 0 || si.tiles_y == 0 ||
            si.tile_count != 2u * si.tiles_x * si.tiles_y)
            throw std::runtime_error("NX_PLANAR_FOVEATED requires a side-by-side 2-eye tile grid");
    }
    nxvcvk::InterCtx inter;
    inter.resize(si.tile_count);
    Probe p;
    p.init(si.width * si.eyes, si.height, si.tile_count, argv[2]);
    uint32_t limit = argc > 3 ? std::stoul(argv[3]) : UINT32_MAX, n = 0;
    struct Sample {
        uint32_t frame, bytes;
        double parse, upload, render, total, interval;
        DrawTiming timing;
        double arrival_late;
        uint32_t tiles_rendered = 0, tiles_skipped = 0, rings_rendered = 0;
        int64_t peripheral_age_max = 0;
    };
    std::vector<Sample> samples;
    samples.reserve(std::min(limit, 4096u));
    std::fprintf(stderr, "representation=%s\n",
                 p.gpu_palette ? "GPU vertex-fed flat-region approximation"
                 : p.tile_mode ? "vertex-fed flat-region approximation"
                 : p.compact   ? "compact flat-region approximation"
                 : p.flat      ? "flat-region approximation"
                               : "exact PLANAR coded samples");
    const char* pace_text = std::getenv("NX_PLANAR_PACE_FPS");
    char* pace_end = nullptr;
    errno = 0;
    double pace_fps = pace_text ? std::strtod(pace_text, &pace_end) : 0.0;
    if (pace_text && (errno || pace_end == pace_text || *pace_end || !std::isfinite(pace_fps) ||
                      pace_fps <= 0.0))
        throw std::runtime_error("NX_PLANAR_PACE_FPS must be a finite positive number");
    auto pace_origin = Clock::now();
    auto admit = [&](uint32_t index, Clock::time_point& scheduled) {
        if (pace_fps > 0.0) {
            auto period = std::chrono::duration<double>(1.0 / pace_fps);
            scheduled = pace_origin + std::chrono::duration_cast<Clock::duration>(period * index);
            if (std::getenv("NX_PLANAR_PACE_SPIN")) {
                while (Clock::now() < scheduled) {
                }
            } else
                std::this_thread::sleep_until(scheduled);
            return std::max(0.0, ms(scheduled, Clock::now()));
        }
        scheduled = Clock::now();
        return 0.0;
    };
    if (std::getenv("NX_PLANAR_ASYNC")) {
        Probe child;
        child.init(si.width * si.eyes, si.height, si.tile_count, argv[2], &p);
        Probe* slots[2] = {&p, &child};
        struct Pending {
            bool live = false;
            uint32_t frame = 0, bytes = 0;
            Clock::time_point start, scheduled, parsed, uploaded;
            double arrival_late = 0;
            Push push{};
        } pending[2];
        auto benchmark_start = Clock::now();
        pace_origin = benchmark_start;
        auto previous_completion = benchmark_start;
        auto finish = [&](int s) {
            if (!pending[s].live)
                return;
            DrawTiming tm = slots[s]->finish_draw();
            auto end = Clock::now();
            samples[pending[s].frame] = {pending[s].frame,
                                         pending[s].bytes,
                                         ms(pending[s].start, pending[s].parsed),
                                         ms(pending[s].parsed, pending[s].uploaded),
                                         ms(pending[s].uploaded, end),
                                         ms(pending[s].scheduled, end),
                                         ms(previous_completion, end),
                                         tm,
                                         pending[s].arrival_late};
            previous_completion = end;
            pending[s].live = false;
        };
        while (offset < data.size() && n < limit) {
            int slot = int(n & 1u);
            // Include slot pressure in unpaced latency. At a fixed cadence,
            // observe outstanding work before sleeping for the next arrival.
            Clock::time_point scheduled = Clock::now();
            finish(slot);
            if (pace_fps > 0.0) {
                finish(1 - slot);
                auto period = std::chrono::duration<double>(1.0 / pace_fps);
                scheduled = pace_origin + std::chrono::duration_cast<Clock::duration>(period * n);
                if (std::getenv("NX_PLANAR_PACE_SPIN")) {
                    while (Clock::now() < scheduled) {
                    }
                } else
                    std::this_thread::sleep_until(scheduled);
            }
            double arrival_late = std::max(0.0, ms(scheduled, Clock::now()));
            auto start = Clock::now();
            Probe& sp = *slots[slot];
            nxvcvk::FrameParse fp{};
            auto st = nxvcvk::parse_frame(si, data.data() + offset, data.size() - offset, false, fp,
                                          &inter);
            if (st != NXVC_VKD_OK)
                throw std::runtime_error(nxvcvk::last_parse_reject_text());
            if (fp.tiles_planar != si.tile_count || fp.recs.size() != si.tile_count ||
                fp.planar.size() * 4 != sp.planar.bytes || !fp.frame_bytes)
                throw std::runtime_error("requires complete all-PLANAR frame");
            auto parsed = Clock::now();
            sp.prepare(fp, si.tile_count);
            auto uploaded = Clock::now();
            Push push{fp.push.baseQp, fp.push.chromaQpOff, int32_t(sp.w), int32_t(sp.h)};
            samples.resize(n + 1);
            pending[slot] = {true,   n,        fp.frame_bytes, start, scheduled,
                             parsed, uploaded, arrival_late,   push};
            sp.submit_draw(push);
            offset += fp.frame_bytes;
            ++n;
        }
        // Drain by frame order even for an odd number of frames.
        int first =
            pending[0].live && (!pending[1].live || pending[0].frame < pending[1].frame) ? 0 : 1;
        finish(first);
        finish(1 - first);
        auto benchmark_end = Clock::now();
        if (!n)
            throw std::runtime_error("no frames decoded");
        std::fprintf(stderr, "pipeline_slots=2 inclusive_wall_ms=%.6f inclusive_fps=%.6f\n",
                     ms(benchmark_start, benchmark_end),
                     1000.0 * n / ms(benchmark_start, benchmark_end));
        std::puts("frame,bytes,parse_ms,upload_ms,render_wait_ms,gpu_ms,total_ms,record_ms,submit_"
                  "ms,fence_ms,query_ms,interval_ms,arrival_late_ms");
        for (const auto& x : samples)
            std::printf("%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", x.frame,
                        x.bytes, x.parse, x.upload, x.render, x.timing.gpu, x.total,
                        x.timing.record, x.timing.submit, x.timing.fence, x.timing.query,
                        x.interval, x.arrival_late);
        std::fprintf(stderr, "complete parsed_frames=%u rendered_frames=%u file_exhausted=%d\n", n,
                     n, offset == data.size());
        if (argc > 4) {
            int last = int((n - 1) & 1u);
            slots[last]->readback(argv[4]);
        }
        return 0;
    }
    auto benchmark_start = Clock::now();
    pace_origin = benchmark_start;
    auto previous_completion = benchmark_start;
    uint32_t foveated_max_rings = UINT32_MAX;
    if (const char* r = std::getenv("NX_PLANAR_FOVEATED_MAX_RINGS")) {
        char* end = nullptr;
        errno = 0;
        unsigned long v = std::strtoul(r, &end, 10);
        if (errno || end == r || *end || v > UINT32_MAX)
            throw std::runtime_error("NX_PLANAR_FOVEATED_MAX_RINGS must be a nonnegative integer");
        foveated_max_rings = uint32_t(v);
    }
    std::vector<std::vector<TileSpan>> foveated_rings;
    std::vector<std::vector<TileSpan>> foveated_prefixes;
    if (foveated) {
        const uint32_t cols = si.eyes * si.tiles_x;
        uint32_t max_ring = 0;
        for (uint32_t t = 0; t < si.tile_count; ++t) {
            uint32_t row = t / cols, col = t % cols;
            uint32_t local = col % si.tiles_x;
            max_ring = std::max(max_ring,
                                uint32_t(std::max(std::abs(2 * int(local) - int(si.tiles_x - 1)),
                                                  std::abs(2 * int(row) - int(si.tiles_y - 1)))));
        }
        constexpr uint32_t kBands = 4;
        foveated_rings.resize(kBands);
        for (uint32_t ring = 0; ring < kBands; ++ring) {
            for (uint32_t row = 0; row < si.tiles_y; ++row) {
                uint32_t col = 0;
                while (col < cols) {
                    uint32_t local = col % si.tiles_x;
                    uint32_t radius = std::max(std::abs(2 * int(local) - int(si.tiles_x - 1)),
                                               std::abs(2 * int(row) - int(si.tiles_y - 1)));
                    uint32_t band = std::min(kBands - 1, radius * kBands / (max_ring + 1));
                    if (band != ring) {
                        ++col;
                        continue;
                    }
                    uint32_t first = row * cols + col, count = 0;
                    do {
                        ++count;
                        ++col;
                        if (col == cols)
                            break;
                        local = col % si.tiles_x;
                        radius = std::max(std::abs(2 * int(local) - int(si.tiles_x - 1)),
                                          std::abs(2 * int(row) - int(si.tiles_y - 1)));
                        uint32_t next_band = std::min(kBands - 1, radius * kBands / (max_ring + 1));
                        if (next_band != band)
                            break;
                    } while (true);
                    foveated_rings[ring].push_back({first, count});
                }
            }
        }
        if (foveated_single_pass) {
            foveated_prefixes.resize(kBands);
            for (uint32_t prefix = 0; prefix < kBands; ++prefix)
                for (uint32_t ring = 0; ring <= prefix; ++ring)
                    foveated_prefixes[prefix].insert(foveated_prefixes[prefix].end(),
                                                     foveated_rings[ring].begin(),
                                                     foveated_rings[ring].end());
        }
    }
    double foveated_budget = pace_fps > 0.0 ? 1000.0 / pace_fps : 1000.0 / 90.0;
    if (const char* b = std::getenv("NX_PLANAR_FOVEATED_BUDGET_MS")) {
        char* end = nullptr;
        errno = 0;
        foveated_budget = std::strtod(b, &end);
        if (errno || end == b || *end || !std::isfinite(foveated_budget) || foveated_budget <= 0)
            throw std::runtime_error("NX_PLANAR_FOVEATED_BUDGET_MS must be finite and positive");
    }
    std::vector<int64_t> last_rendered(si.tile_count, -1);
    // A transient stall must not permanently disable the periphery. Keep a
    // separate recent upper estimate for each band, expiring after 32 frames.
    struct BatchCost {
        uint32_t frame;
        double ms;
    };
    std::vector<std::vector<BatchCost>> recent_costs(4);
    std::vector<std::vector<BatchCost>> recent_prefix_costs(4);
    double full_single_pass_cost = 0.5;
    uint64_t foveated_rendered_total = 0, foveated_skipped_total = 0;
    uint32_t foveated_bands_total = 0;
    benchmark_start = Clock::now();
    pace_origin = benchmark_start;
    previous_completion = benchmark_start;
    while (offset < data.size() && n < limit) {
        Clock::time_point scheduled;
        double arrival_late = admit(n, scheduled);
        auto start = Clock::now();
        nxvcvk::FrameParse fp{};
        auto st =
            nxvcvk::parse_frame(si, data.data() + offset, data.size() - offset, false, fp, &inter);
        if (st != NXVC_VKD_OK)
            throw std::runtime_error(nxvcvk::last_parse_reject_text());
        if (fp.tiles_planar != si.tile_count || fp.recs.size() != si.tile_count ||
            fp.planar.size() * 4 != p.planar.bytes || !fp.frame_bytes)
            throw std::runtime_error("requires complete all-PLANAR frame");
        auto parsed = Clock::now();
        p.prepare(fp, si.tile_count);
        auto uploaded = Clock::now();
        Push push{fp.push.baseQp, fp.push.chromaQpOff, int32_t(p.w), int32_t(p.h)};
        DrawTiming timing{};
        uint32_t tiles_rendered = si.tile_count, tiles_skipped = 0, rings_rendered = 0;
        int64_t max_age = 0;
        if (!foveated || n == 0) {
            if (foveated_single_pass) {
                auto render_start = Clock::now();
                timing = p.draw(push);
                full_single_pass_cost = ms(render_start, Clock::now());
                recent_prefix_costs.back().push_back({n, full_single_pass_cost});
                rings_rendered = 4;
            } else {
                timing = p.draw(push);
            }
            if (foveated)
                std::fill(last_rendered.begin(), last_rendered.end(), int64_t(n));
        } else if (foveated_single_pass) {
            const uint32_t max_prefix = foveated_max_rings >= 3 ? 4 : foveated_max_rings + 1;
            uint32_t prefix = 0;
            auto render_start = Clock::now();
            for (uint32_t candidate = max_prefix; candidate >= 1; --candidate) {
                auto& history = recent_prefix_costs[candidate - 1];
                history.erase(std::remove_if(history.begin(), history.end(),
                                             [&](const BatchCost& x) { return n - x.frame >= 32; }),
                              history.end());
                double estimate = 0.5;
                if (!history.empty()) {
                    for (const auto& cost : history)
                        estimate = std::max(estimate, cost.ms * 1.25);
                } else {
                    const auto& spans = foveated_prefixes[candidate - 1];
                    uint32_t tiles = 0;
                    for (const TileSpan& s : spans)
                        tiles += s.count;
                    estimate += full_single_pass_cost * double(tiles) / double(si.tile_count);
                }
                double elapsed = ms(scheduled, Clock::now());
                if (candidate == 1 || elapsed + estimate <= foveated_budget) {
                    prefix = candidate;
                    break;
                }
            }
            timing = prefix == 4 ? p.draw(push) : p.draw_spans(push, foveated_prefixes[prefix - 1]);
            double wallcost = ms(render_start, Clock::now());
            recent_prefix_costs[prefix - 1].push_back({n, wallcost});
            rings_rendered = prefix;
            for (const TileSpan& s : foveated_prefixes[prefix - 1])
                for (uint32_t t = s.first; t < s.first + s.count; ++t)
                    last_rendered[t] = n;
            tiles_rendered = 0;
            for (int64_t age : last_rendered)
                if (age == int64_t(n))
                    ++tiles_rendered;
            tiles_skipped = si.tile_count - tiles_rendered;
            for (int64_t age : last_rendered)
                max_age = std::max(max_age, int64_t(n) - age);
            foveated_rendered_total += tiles_rendered;
            foveated_skipped_total += tiles_skipped;
            foveated_bands_total += rings_rendered;
        } else {
            auto render_start = Clock::now();
            DrawTiming accumulated{};
            accumulated.gpu = p.queries ? 0 : -1;
            for (uint32_t ring = 0; ring < foveated_rings.size(); ++ring) {
                if (ring > foveated_max_rings)
                    break;
                if (foveated_rings[ring].empty())
                    continue;
                double elapsed = ms(scheduled, Clock::now());
                auto& history = recent_costs[ring];
                history.erase(std::remove_if(history.begin(), history.end(),
                                             [&](const BatchCost& x) { return n - x.frame > 32; }),
                              history.end());
                double estimated_ms = 0.5;
                for (const auto& cost : history)
                    estimated_ms = std::max(estimated_ms, cost.ms * 1.25);
                if (ring != 0 && elapsed + estimated_ms > foveated_budget)
                    break;
                timing = p.draw_spans(push, foveated_rings[ring]);
                accumulated.record += timing.record;
                accumulated.submit += timing.submit;
                accumulated.fence += timing.fence;
                accumulated.query += timing.query;
                if (timing.gpu >= 0)
                    accumulated.gpu += timing.gpu;
                ++rings_rendered;
                for (const TileSpan& s : foveated_rings[ring])
                    for (uint32_t t = s.first; t < s.first + s.count; ++t)
                        last_rendered[t] = n;
                history.push_back({n, ms(render_start, Clock::now())});
                render_start = Clock::now();
            }
            timing = accumulated;
            tiles_rendered = 0;
            for (int64_t age : last_rendered)
                if (age == int64_t(n))
                    ++tiles_rendered;
            tiles_skipped = si.tile_count - tiles_rendered;
            for (int64_t age : last_rendered)
                max_age = std::max(max_age, int64_t(n) - age);
            foveated_rendered_total += tiles_rendered;
            foveated_skipped_total += tiles_skipped;
            foveated_bands_total += rings_rendered;
        }
        auto end = Clock::now();
        double total = ms(scheduled, end);
        samples.push_back({n, fp.frame_bytes, ms(start, parsed), ms(parsed, uploaded),
                           ms(uploaded, end), total, ms(previous_completion, end), timing,
                           arrival_late, tiles_rendered, tiles_skipped, rings_rendered, max_age});
        previous_completion = end;
        offset += fp.frame_bytes;
        ++n;
    }
    auto benchmark_end = Clock::now();
    if (!n)
        throw std::runtime_error("no frames decoded");
    std::fprintf(stderr, "inclusive_wall_ms=%.6f inclusive_fps=%.6f\n",
                 ms(benchmark_start, benchmark_end),
                 1000.0 * n / ms(benchmark_start, benchmark_end));
    std::puts("frame,bytes,parse_ms,upload_ms,render_wait_ms,gpu_ms,total_ms,record_ms,submit_ms,"
              "fence_ms,query_ms,interval_ms,arrival_late_ms,tiles_rendered,tiles_skipped,rings_"
              "rendered,peripheral_age_max");
    for (const auto& x : samples) {
        std::printf("%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%lld\n",
                    x.frame, x.bytes, x.parse, x.upload, x.render, x.timing.gpu, x.total,
                    x.timing.record, x.timing.submit, x.timing.fence, x.timing.query, x.interval,
                    x.arrival_late, x.tiles_rendered, x.tiles_skipped, x.rings_rendered,
                    static_cast<long long>(x.peripheral_age_max));
    }
    if (argc > 4)
        p.readback(argv[4]);
    if (foveated)
        std::fprintf(stderr,
                     "foveated bands=4 budget_ms=%.3f rendered_tiles=%llu skipped_tiles=%llu "
                     "submitted_bands=%u max_rings=%u single_pass=%d\n",
                     foveated_budget, static_cast<unsigned long long>(foveated_rendered_total),
                     static_cast<unsigned long long>(foveated_skipped_total), foveated_bands_total,
                     foveated_max_rings, foveated_single_pass);
    std::fprintf(stderr, "complete parsed_frames=%u rendered_frames=%u file_exhausted=%d\n", n, n,
                 offset == data.size());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
}
