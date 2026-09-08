#include "nxvc/planar_direct.h"

#include "nxvc_vkdec_parse.h"
#include "passB/syntax_constants.h"
#include "planar_direct_tile_frag.spv.h"
#include "planar_direct_tile_vert.spv.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t kPlanarTool = 1ull << 35;
constexpr uint64_t kAtlasTool = 1ull << 31;
double elapsed(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
void vkcheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " (VkResult " + std::to_string(r) + ")");
}
struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};
struct Push {
    int frameQP, chromaQPOffset, width, height;
};
}  // namespace

namespace nxvc {
struct PlanarDirect::Impl {
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    nxvcvk::StreamInfo stream{};
    bool configured = false;
    VkCommandPool command_pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkQueryPool queries{};
    uint32_t timestamp_bits{};
    float timestamp_period{};
    VkDescriptorSetLayout dsl{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet ds{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    VkImage framebuffer_image{};
    VkImageView framebuffer_view{};
    HostBuffer planar, palette;
    nxvcvk::InterCtx inter;

    Impl(VkPhysicalDevice p, VkDevice d, VkQueue q, uint32_t f)
        : physical(p), device(d), queue(q), family(f) {
        if (!physical || !device || !queue)
            throw std::invalid_argument("PlanarDirect: invalid Vulkan device");
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        timestamp_period = props.limits.timestampPeriod;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qps(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, qps.data());
        if (family >= n)
            throw std::invalid_argument("PlanarDirect: invalid queue family");
        timestamp_bits = qps[family].timestampValidBits;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkcheck(vkCreateCommandPool(device, &pi, nullptr, &command_pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = command_pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        vkcheck(vkAllocateCommandBuffers(device, &ca, &command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkcheck(vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");
        if (timestamp_bits) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            vkcheck(vkCreateQueryPool(device, &qi, nullptr, &queries), "vkCreateQueryPool");
        }
    }
    ~Impl() {
        destroy_framebuffer();
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (render_pass)
            vkDestroyRenderPass(device, render_pass, nullptr);
        if (pipeline_layout)
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_pool)
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (dsl)
            vkDestroyDescriptorSetLayout(device, dsl, nullptr);
        free_buffer(planar);
        free_buffer(palette);
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        if (command_pool)
            vkDestroyCommandPool(device, command_pool, nullptr);
    }
    void destroy_framebuffer() {
        if (framebuffer)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffer = {};
        framebuffer_image = {};
        framebuffer_view = {};
    }
    uint32_t memory_type(uint32_t bits) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                return i;
        throw std::runtime_error("PlanarDirect: no coherent host memory");
    }
    HostBuffer make_buffer(VkDeviceSize size) {
        HostBuffer b{};
        b.size = size;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkcheck(vkCreateBuffer(device, &bi, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device, b.buffer, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = memory_type(mr.memoryTypeBits);
        vkcheck(vkAllocateMemory(device, &ai, nullptr, &b.memory), "vkAllocateMemory");
        vkcheck(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        vkcheck(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
        return b;
    }
    void free_buffer(HostBuffer& b) {
        if (b.mapped)
            vkUnmapMemory(device, b.memory);
        if (b.buffer)
            vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory)
            vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }
    VkShaderModule shader(const uint32_t* code, size_t words) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = words * 4;
        ci.pCode = code;
        VkShaderModule m{};
        vkcheck(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    }
    void setup(uint32_t w, uint32_t h) {
        planar = make_buffer(VkDeviceSize(stream.tile_count) * nxvw::kPlanarUintsPerTile * 4);
        palette = make_buffer(VkDeviceSize(stream.tile_count) * 4 * 4);
        VkDescriptorSetLayoutBinding bs[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bs[i].descriptorCount = 1;
            bs[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 2;
        dl.pBindings = bs;
        vkcheck(vkCreateDescriptorSetLayout(device, &dl, nullptr, &dsl),
                "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &ps;
        vkcheck(vkCreateDescriptorPool(device, &dp, nullptr, &descriptor_pool),
                "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptor_pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &dsl;
        vkcheck(vkAllocateDescriptorSets(device, &da, &ds), "vkAllocateDescriptorSets");
        VkDescriptorBufferInfo infos[2] = {{planar.buffer, 0, planar.size},
                                           {palette.buffer, 0, palette.size}};
        VkWriteDescriptorSet ws[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[i].dstSet = ds;
            ws[i].dstBinding = i;
            ws[i].descriptorCount = 1;
            ws[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ws[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 2, ws, 0, nullptr);
        VkAttachmentDescription ad{};
        ad.format = VK_FORMAT_R8G8B8A8_UNORM;
        ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ad.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ar;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &ad;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = deps;
        vkcheck(vkCreateRenderPass(device, &rp, nullptr, &render_pass), "vkCreateRenderPass");
        VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &dsl;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        vkcheck(vkCreatePipelineLayout(device, &pl, nullptr, &pipeline_layout),
                "vkCreatePipelineLayout");
        VkShaderModule vs = shader(planar_direct_tile_vert_spv,
                                   sizeof(planar_direct_tile_vert_spv) / 4),
                       fs = shader(planar_direct_tile_frag_spv,
                                   sizeof(planar_direct_tile_frag_spv) / 4);
        VkPipelineShaderStageCreateInfo st[2]{};
        for (int i = 0; i < 2; ++i) {
            st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            st[i].module = i ? fs : vs;
            st[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyns;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cb{};
        cb.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo cbs{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = st;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cbs;
        gp.pDynamicState = &dyn;
        gp.layout = pipeline_layout;
        gp.renderPass = render_pass;
        vkcheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
        (void)w;
        (void)h;
    }
};

PlanarDirect::PlanarDirect(VkPhysicalDevice p, VkDevice d, VkQueue q, uint32_t f)
    : impl_(new Impl(p, d, q, f)) {}
PlanarDirect::~PlanarDirect() {
    delete impl_;
}
uint32_t PlanarDirect::width() const {
    return impl_->stream.width * impl_->stream.eyes;
}
uint32_t PlanarDirect::height() const {
    return impl_->stream.height;
}
void PlanarDirect::configure(const uint8_t* data, size_t len) {
    if (!data)
        throw std::invalid_argument("PlanarDirect::configure: null header");
    nxvcvk::StreamInfo si{};
    size_t used = 0;
    // The parser mask describes the decoder's complete accepted tool set.  PLANAR
    // is then checked below; passing only bit 35 would reject otherwise valid
    // stream-level tools (the camera fixture also carries the normal intra bits).
    auto st = nxvcvk::parse_stream_header(data, len, si, &used);
    if (st != NXVC_VKD_OK)
        throw std::runtime_error(std::string("PlanarDirect header rejected: ") +
                                 nxvcvk::last_parse_reject_text());
    if (si.width == 0 || si.height == 0 || (si.width & 63) || (si.height & 63) ||
        si.bit_depth != 8 || si.chroma != 0 || si.color_transform != 0 || si.alpha != 0 ||
        si.eyes < 1 || si.eyes > 2 || !(si.tools & kPlanarTool) || (si.tools & kAtlasTool))
        throw std::runtime_error(
            "PlanarDirect: requires 64-aligned 8-bit 4:2:0 CT-none no-alpha PLANAR stream");
    if (impl_->configured &&
        (si.width != impl_->stream.width || si.height != impl_->stream.height ||
         si.eyes != impl_->stream.eyes || si.tile_count != impl_->stream.tile_count))
        throw std::logic_error(
            "PlanarDirect::configure: device geometry cannot change after setup");
    impl_->stream = si;
    if (!impl_->configured) {
        impl_->inter.resize(si.tile_count);
        impl_->setup(si.width, si.height);
        impl_->configured = true;
    }
}
PlanarDirect::Stats PlanarDirect::render(const uint8_t* data, size_t len, VkImage target,
                                         VkImageView view, uint32_t w, uint32_t h) {
    if (!impl_->configured)
        throw std::logic_error("PlanarDirect::render before configure");
    if (!data || !target || !view || w != width() || h != height())
        throw std::invalid_argument("PlanarDirect::render: target mismatch");
    Stats out{};
    auto total = Clock::now(), pa = Clock::now();
    nxvcvk::FrameParse fp{};
    auto st = nxvcvk::parse_frame(impl_->stream, data, len, false, fp, &impl_->inter);
    if (st != NXVC_VKD_OK)
        throw std::runtime_error(std::string("PlanarDirect frame rejected: ") +
                                 nxvcvk::last_parse_reject_text());
    if (fp.frame_bytes != len)
        throw std::runtime_error("PlanarDirect: frame length does not match frame_bytes");
    if (fp.recs.size() != impl_->stream.tile_count ||
        fp.planar.size() != size_t(impl_->stream.tile_count) * nxvw::kPlanarUintsPerTile ||
        fp.tiles_planar != impl_->stream.tile_count)
        throw std::runtime_error("PlanarDirect: frame is not all PLANAR");
    auto* pp = static_cast<uint32_t*>(impl_->palette.mapped);
    for (uint32_t t = 0; t < impl_->stream.tile_count; ++t) {
        const uint32_t* b = fp.planar.data() + size_t(t) * 26;
        if ((b[0] & 0x0fu) != 0u)
            throw std::runtime_error("PlanarDirect: only R2 coarse tiles are supported");
        // The tile vertex shader consumes the two 32-cell labels first,
        // followed by the two packed RGBA palette entries.
        pp[t * 4] = b[1];
        pp[t * 4 + 1] = b[2];
        int delta = int((fp.recs[t].w1 >> 8) & 63);
        if (delta >= 32)
            delta -= 64;
        int qp = std::clamp(fp.push.baseQp + delta, 0, 63),
            cqp = std::clamp(qp + fp.push.chromaQpOff, 0, 63);
        for (int r = 0; r < 2; ++r) {
            int v[3]{};
            for (int p = 0; p < 3; ++p) {
                int i = (r * 3 + p) * 3;
                const uint8_t* raw = reinterpret_cast<const uint8_t*>(b + 17) + i;
                if (raw[1] != 0 || raw[2] != 0)
                    throw std::runtime_error("PlanarDirect: nonzero planar slopes are unsupported");
                int q = int(raw[0]);
                if (q >= 128)
                    q -= 256;
                int step = nxvw::kQStep[(p ? cqp : qp) >> 1];
                v[p] = std::clamp(128 + ((q * step + 8) >> 4), 0, 255);
            }
            float cb = v[1] - 128.f, cr = v[2] - 128.f;
            auto by = [](float x) -> uint32_t {
                return uint32_t(std::clamp(int(x + 0.5f), 0, 255));
            };
            pp[t * 4 + 2 + r] = by(v[0] + 1.5748f * cr) |
                                 (by(v[0] - .1873f * cb - .4681f * cr) << 8) |
                                 (by(v[0] + 1.8556f * cb) << 16) | 0xff000000u;
        }
    }
    out.parse_ms = elapsed(pa, Clock::now());
    auto up = Clock::now();
    std::memcpy(impl_->planar.mapped, fp.planar.data(), impl_->planar.size);
    out.upload_ms = elapsed(up, Clock::now());
    out.tiles = impl_->stream.tile_count;
    if (impl_->framebuffer_image != target || impl_->framebuffer_view != view) {
        impl_->destroy_framebuffer();
        VkImageView a = view;
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = impl_->render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &a;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        vkcheck(vkCreateFramebuffer(impl_->device, &fi, nullptr, &impl_->framebuffer),
                "vkCreateFramebuffer");
        impl_->framebuffer_image = target;
        impl_->framebuffer_view = view;
    }
    vkcheck(vkResetCommandBuffer(impl_->command, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkcheck(vkBeginCommandBuffer(impl_->command, &bi), "vkBeginCommandBuffer");
    if (impl_->queries) {
        vkCmdResetQueryPool(impl_->command, impl_->queries, 0, 2);
        vkCmdWriteTimestamp(impl_->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl_->queries, 0);
    }
    VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    ri.renderPass = impl_->render_pass;
    ri.framebuffer = impl_->framebuffer;
    ri.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(impl_->command, &ri, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, float(w), float(h), 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(impl_->command, 0, 1, &vp);
    vkCmdSetScissor(impl_->command, 0, 1, &sc);
    vkCmdBindPipeline(impl_->command, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline);
    vkCmdBindDescriptorSets(impl_->command, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline_layout,
                            0, 1, &impl_->ds, 0, nullptr);
    Push push{int(fp.push.baseQp), int(fp.push.chromaQpOff), int(w), int(h)};
    vkCmdPushConstants(impl_->command, impl_->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(impl_->command, 6, impl_->stream.tile_count, 0, 0);
    vkCmdEndRenderPass(impl_->command);
    if (impl_->queries)
        vkCmdWriteTimestamp(impl_->command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl_->queries,
                            1);
    vkcheck(vkEndCommandBuffer(impl_->command), "vkEndCommandBuffer");
    vkcheck(vkResetFences(impl_->device, 1, &impl_->fence), "vkResetFences");
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &impl_->command;
    vkcheck(vkQueueSubmit(impl_->queue, 1, &sub, impl_->fence), "vkQueueSubmit");
    vkcheck(vkWaitForFences(impl_->device, 1, &impl_->fence, VK_TRUE, UINT64_MAX),
            "vkWaitForFences");
    if (impl_->queries) {
        uint64_t q[2]{};
        if (vkGetQueryPoolResults(
                impl_->device, impl_->queries, 0, 2, sizeof(q), q, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            uint64_t d = q[1] - q[0];
            if (impl_->timestamp_bits < 64)
                d &= (uint64_t(1) << impl_->timestamp_bits) - 1;
            out.gpu_ms = double(d) * impl_->timestamp_period / 1e6;
        }
    }
    out.total_ms = elapsed(total, Clock::now());
    return out;
}
}  // namespace nxvc
