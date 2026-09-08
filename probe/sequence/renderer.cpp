#include "renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace nx_probe {
namespace {

struct Push {
    int32_t rect[8];
    float scale[4], bias[4], post[4], motion[4], glow[4], deband[4];
};
static_assert(sizeof(Push) == 128);

struct Vertex {
    float px, py;
    uint32_t u, v, tile;
};
static_assert(sizeof(Vertex) == 20);

template <class T>
bool ok(T r) {
    return r == VK_SUCCESS;
}

uint32_t memory_type(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

std::vector<uint32_t> read_spv(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const auto n = f.tellg();
    if (n <= 0 || (n % 4) != 0)
        return {};
    std::vector<uint32_t> v(static_cast<size_t>(n) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), n);
    return f ? v : std::vector<uint32_t>{};
}

}  // namespace

struct Renderer::Impl {
    VkPhysicalDevice p;
    VkDevice d;
    VkQueue q;
    uint32_t qf, w, h;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe[2]{};
    VkRenderPass pass = VK_NULL_HANDLE;
    VkSampler ysam = VK_NULL_HANDLE, csam = VK_NULL_HANDLE;
    VkBuffer mesh = VK_NULL_HANDLE;
    VkDeviceMemory mesh_mem = VK_NULL_HANDLE;
    VkDeviceSize mesh_bytes = 0;
    uint32_t mesh_count = 0;
    VkImage out[2]{};
    VkImageView out_view[2]{};
    VkDeviceMemory out_mem[2]{};
    VkFramebuffer fb[2]{};
    VkImage density = VK_NULL_HANDLE;
    VkImageView density_view = VK_NULL_HANDLE;
    VkDeviceMemory density_mem = VK_NULL_HANDLE;
    bool use_density = false;
    bool out_initialized[2]{};
    VkFence pending = VK_NULL_HANDLE;
    std::vector<Vertex> vertices;
    std::filesystem::path vspv, fspv;
    bool valid = false;
    VkFormat output_format = VK_FORMAT_R8G8B8A8_SRGB;
    bool fast_srgb = false;
    uint32_t cached_aw = 0, cached_ah = 0;

    bool make_density() {
        VkPhysicalDeviceFragmentDensityMapFeaturesEXT f{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f;
        vkGetPhysicalDeviceFeatures2(p, &f2);
        if (!f.fragmentDensityMap || !f.fragmentDensityMapNonSubsampledImages)
            return false;
        VkPhysicalDeviceFragmentDensityMapPropertiesEXT props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &props;
        vkGetPhysicalDeviceProperties2(p, &p2);
        const uint32_t tw = std::max(1u, props.minFragmentDensityTexelSize.width);
        const uint32_t th = std::max(1u, props.minFragmentDensityTexelSize.height);
        VkFormatProperties format_props{};
        vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_R8G8_UNORM, &format_props);
        if (!(format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT))
            return false;
        VkExtent3D ex{(w + tw - 1) / tw, (h + th - 1) / th, 1};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8_UNORM;
        ci.extent = ex;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!ok(vkCreateImage(d, &ci, nullptr, &density)))
            return false;
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(d, density, &mr);
        uint32_t mt = memory_type(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX)
            return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mt;
        if (!ok(vkAllocateMemory(d, &ai, nullptr, &density_mem)) ||
            !ok(vkBindImageMemory(d, density, density_mem, 0)))
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = density;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!ok(vkCreateImageView(d, &vi, nullptr, &density_view)))
            return false;
        const VkDeviceSize n = VkDeviceSize(ex.width) * ex.height * 2;
        VkBuffer sb = VK_NULL_HANDLE;
        VkDeviceMemory sm = VK_NULL_HANDLE;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = n;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!ok(vkCreateBuffer(d, &bi, nullptr, &sb)))
            return false;
        vkGetBufferMemoryRequirements(d, sb, &mr);
        mt =
            memory_type(p, mr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX)
            return false;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mt;
        if (!ok(vkAllocateMemory(d, &ai, nullptr, &sm)) || !ok(vkBindBufferMemory(d, sb, sm, 0)))
            return false;
        void* ptr = nullptr;
        if (!ok(vkMapMemory(d, sm, 0, n, 0, &ptr)))
            return false;
        auto* density_bytes = static_cast<uint8_t*>(ptr);
        for (uint32_t y = 0; y < ex.height; ++y)
            for (uint32_t x = 0; x < ex.width; ++x) {
                const float dx = (float(x) + .5f) / ex.width - .5f,
                            dy = (float(y) + .5f) / ex.height - .5f;
                const bool center = dx * dx + dy * dy < .08f;
                density_bytes[2 * (y * ex.width + x)] = center ? 255 : 127;
                density_bytes[2 * (y * ex.width + x) + 1] = center ? 255 : 127;
            }
        vkUnmapMemory(d, sm);
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        VkCommandBuffer c = VK_NULL_HANDLE;
        if (!ok(vkAllocateCommandBuffers(d, &ca, &c)))
            return false;
        VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!ok(vkBeginCommandBuffer(c, &cb)))
            return false;
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.image = density;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &ib);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = ex;
        vkCmdCopyBufferToImage(c, sb, density, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, 0, nullptr, 0,
                             nullptr, 1, &ib);
        if (!ok(vkEndCommandBuffer(c)))
            return false;
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        if (!ok(vkCreateFence(d, &fc, nullptr, &fence)))
            return false;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        bool good = ok(vkQueueSubmit(q, 1, &si, fence)) &&
                    ok(vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(d, fence, nullptr);
        vkFreeCommandBuffers(d, pool, 1, &c);
        vkDestroyBuffer(d, sb, nullptr);
        vkFreeMemory(d, sm, nullptr);
        return good;
    }

    bool make_mesh(uint32_t aw, uint32_t ah) {
        if (mesh && cached_aw == aw && cached_ah == ah)
            return true;
        const uint32_t pw = aw / 2, ph = ah;
        const uint32_t visible_w = std::min(pw, w), visible_h = std::min(ph, h);
        if (!pw || !ph)
            return false;
        const uint32_t nx = (pw + 63) / 64, ny = (ph + 63) / 64;
        vertices.clear();
        vertices.reserve(size_t(nx) * ny * 6);
        auto v = [&](uint32_t x, uint32_t y, uint32_t t) {
            return Vertex{-1.f + 2.f * x / float(w), -1.f + 2.f * y / float(h), x, y, t};
        };
        for (uint32_t y = 0; y < ny; ++y)
            for (uint32_t x = 0; x < nx; ++x) {
                uint32_t x0 = x * 64, y0 = y * 64, x1 = std::min(visible_w, (x + 1) * 64),
                         y1 = std::min(visible_h, (y + 1) * 64);
                if (x0 >= visible_w || y0 >= visible_h)
                    continue;
                uint32_t t = y * nx + x;
                auto tl = v(x0, y0, t), tr = v(x1, y0, t), bl = v(x0, y1, t), br = v(x1, y1, t);
                vertices.insert(vertices.end(), {tl, bl, br, tl, br, tr});
            }
        VkDeviceSize bytes = vertices.size() * sizeof(Vertex);
        auto upload = [&]() {
            void* ptr = nullptr;
            if (vkMapMemory(d, mesh_mem, 0, bytes, 0, &ptr) != VK_SUCCESS)
                return false;
            std::memcpy(ptr, vertices.data(), bytes);
            vkUnmapMemory(d, mesh_mem);
            cached_aw = aw;
            cached_ah = ah;
            return true;
        };
        if (mesh && bytes <= mesh_bytes) {
            mesh_count = vertices.size();
            return upload();
        }
        if (mesh) {
            vkDestroyBuffer(d, mesh, nullptr);
            vkFreeMemory(d, mesh_mem, nullptr);
            mesh = VK_NULL_HANDLE;
        }
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (!ok(vkCreateBuffer(d, &bi, nullptr, &mesh)))
            return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(d, mesh, &mr);
        uint32_t mt =
            memory_type(p, mr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX)
            return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mt;
        if (!ok(vkAllocateMemory(d, &ai, nullptr, &mesh_mem)) ||
            !ok(vkBindBufferMemory(d, mesh, mesh_mem, 0)))
            return false;
        mesh_bytes = bytes;
        mesh_count = vertices.size();
        return upload();
    }
};

// Keep the implementation in a compact, explicit Vulkan object owner.  The
// constructor is intentionally fallible through the `valid` flag: sequence
// can report one clean probe failure rather than throwing across its C ABI.
Renderer::Renderer(VkPhysicalDevice p, VkDevice d, VkQueue q, uint32_t qf, std::filesystem::path v,
                   std::filesystem::path f, uint32_t w, uint32_t h)
    : impl_(new Impl{p, d, q, qf, w, h}), physical_(p), device_(d), queue_(q), queue_family_(qf),
      width_(w), height_(h) {
    impl_->vspv = std::move(v);
    impl_->fspv = std::move(f);
    impl_->fast_srgb = std::getenv("NX_SEQUENCE_FAST_SRGB") &&
                       std::atoi(std::getenv("NX_SEQUENCE_FAST_SRGB")) == 1;
    if (impl_->fast_srgb)
        std::fprintf(stderr, "nx-sequence fast-srgb=1 (approximate cubic)\n");
    if (const char* u = std::getenv("NX_SEQUENCE_UNORM"); u && std::atoi(u) == 1) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        const VkFormatFeatureFlags need =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((fp.optimalTilingFeatures & need) != need)
            return;
        impl_->output_format = VK_FORMAT_R8G8B8A8_UNORM;
        std::fprintf(stderr, "nx-sequence output=UNORM\n");
    } else {
        std::fprintf(stderr, "nx-sequence output=SRGB\n");
    }
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = qf;
    if (!ok(vkCreateCommandPool(d, &pi, nullptr, &impl_->pool)))
        return;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = impl_->pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (!ok(vkAllocateCommandBuffers(d, &ca, &impl_->cmd)))
        return;
    VkDescriptorSetLayoutBinding b[] = {
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = b;
    if (!ok(vkCreateDescriptorSetLayout(d, &li, nullptr, &impl_->dsl)))
        return;
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 2;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    if (!ok(vkCreateDescriptorPool(d, &dp, nullptr, &impl_->dp)))
        return;
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = impl_->dp;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &impl_->dsl;
    if (!ok(vkAllocateDescriptorSets(d, &da, &impl_->ds)))
        return;
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &impl_->dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (!ok(vkCreatePipelineLayout(d, &pl, nullptr, &impl_->pl)))
        return;
    VkAttachmentDescription ad{};
    ad.format = impl_->output_format;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription density_ad{};
    density_ad.format = VK_FORMAT_R8G8_UNORM;
    density_ad.samples = VK_SAMPLE_COUNT_1_BIT;
    density_ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    density_ad.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    density_ad.initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    density_ad.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    impl_->use_density =
        std::getenv("NX_SEQUENCE_FDM") && std::strcmp(std::getenv("NX_SEQUENCE_FDM"), "1") == 0;
    if (impl_->use_density && !impl_->make_density())
        return;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference density_ref{1, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ar;
    VkRenderPassFragmentDensityMapCreateInfoEXT density_info{
        VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
    density_info.fragmentDensityMapAttachment = density_ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkAttachmentDescription attachments[2] = {ad, density_ad};
    rp.attachmentCount = impl_->use_density ? 2u : 1u;
    rp.pAttachments = attachments;
    rp.pNext = impl_->use_density ? &density_info : nullptr;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (!ok(vkCreateRenderPass(d, &rp, nullptr, &impl_->pass)))
        return;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    if (!ok(vkCreateSampler(d, &si, nullptr, &impl_->ysam)) ||
        !ok(vkCreateSampler(d, &si, nullptr, &impl_->csam)))
        return;
    auto vv = read_spv(impl_->vspv), ff = read_spv(impl_->fspv);
    if (vv.empty() || ff.empty())
        return;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = vv.size() * 4;
    sm.pCode = vv.data();
    VkShaderModule vm = VK_NULL_HANDLE;
    if (!ok(vkCreateShaderModule(d, &sm, nullptr, &vm)))
        return;
    sm.codeSize = ff.size() * 4;
    sm.pCode = ff.data();
    VkShaderModule fm = VK_NULL_HANDLE;
    if (!ok(vkCreateShaderModule(d, &sm, nullptr, &fm))) {
        vkDestroyShaderModule(d, vm, nullptr);
        return;
    }
    struct Spec {
        int eye;
        VkBool32 srgb;
        VkBool32 vertex;
        VkBool32 fast_srgb;
    } spec{};
    VkSpecializationMapEntry me[] = {{1, offsetof(Spec, srgb), sizeof(VkBool32)},
                                     {6, offsetof(Spec, eye), sizeof(int)},
                                     {9, offsetof(Spec, vertex), sizeof(VkBool32)},
                                     {10, offsetof(Spec, fast_srgb), sizeof(VkBool32)}};
    spec.srgb = impl_->output_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_TRUE : VK_FALSE;
    spec.vertex = VK_TRUE;
    spec.fast_srgb = impl_->fast_srgb ? VK_TRUE : VK_FALSE;
    VkSpecializationInfo si2{4, me, sizeof(spec), &spec};
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             nullptr,
             0,
             VK_SHADER_STAGE_VERTEX_BIT,
             vm,
             "main",
             &si2};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             nullptr,
             0,
             VK_SHADER_STAGE_FRAGMENT_BIT,
             fm,
             "main",
             &si2};
    VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                              {1, 0, VK_FORMAT_R32G32_UINT, 8},
                                              {2, 0, VK_FORMAT_R32_UINT, 16}};
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vs.viewportCount = 1;
    vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo ra{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    ra.polygonMode = VK_POLYGON_MODE_FILL;
    ra.cullMode = VK_CULL_MODE_NONE;
    ra.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vs;
    gp.pRasterizationState = &ra;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &ds;
    gp.layout = impl_->pl;
    gp.renderPass = impl_->pass;
    for (int e = 0; e < 2; ++e) {
        spec.eye = e;
        if (!ok(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &impl_->pipe[e])))
            return;
    }
    vkDestroyShaderModule(d, vm, nullptr);
    vkDestroyShaderModule(d, fm, nullptr);
    impl_->valid = true;
}

Renderer::~Renderer() {
    if (!impl_)
        return;
    wait();
    for (int i = 0; i < 2; ++i) {
        if (impl_->fb[i])
            vkDestroyFramebuffer(device_, impl_->fb[i], nullptr);
        if (impl_->out_view[i])
            vkDestroyImageView(device_, impl_->out_view[i], nullptr);
        if (impl_->out[i])
            vkDestroyImage(device_, impl_->out[i], nullptr);
        if (impl_->out_mem[i])
            vkFreeMemory(device_, impl_->out_mem[i], nullptr);
        if (impl_->pipe[i])
            vkDestroyPipeline(device_, impl_->pipe[i], nullptr);
    }
    if (impl_->density_view)
        vkDestroyImageView(device_, impl_->density_view, nullptr);
    if (impl_->density)
        vkDestroyImage(device_, impl_->density, nullptr);
    if (impl_->density_mem)
        vkFreeMemory(device_, impl_->density_mem, nullptr);
    if (impl_->mesh)
        vkDestroyBuffer(device_, impl_->mesh, nullptr);
    if (impl_->mesh_mem)
        vkFreeMemory(device_, impl_->mesh_mem, nullptr);
    if (impl_->ysam)
        vkDestroySampler(device_, impl_->ysam, nullptr);
    if (impl_->csam)
        vkDestroySampler(device_, impl_->csam, nullptr);
    if (impl_->pass)
        vkDestroyRenderPass(device_, impl_->pass, nullptr);
    if (impl_->pl)
        vkDestroyPipelineLayout(device_, impl_->pl, nullptr);
    if (impl_->dsl)
        vkDestroyDescriptorSetLayout(device_, impl_->dsl, nullptr);
    if (impl_->dp)
        vkDestroyDescriptorPool(device_, impl_->dp, nullptr);
    if (impl_->pool)
        vkDestroyCommandPool(device_, impl_->pool, nullptr);
    delete impl_;
    impl_ = nullptr;
}

bool Renderer::draw(const nxvc_vkd_atlas_images& a, VkBuffer table, VkDeviceSize bytes,
                    VkFence* completion) {
    auto& x = *impl_;
    if (!x.valid || !a.image[0] || !a.view[0] || !a.view[1] || !table)
        return false;
    if (!wait() || !x.make_mesh(a.width[0], a.height[0]))
        return false;
    for (int e = 0; e < 2; ++e)
        if (!x.out[e]) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = x.output_format;
            ci.extent = {width_, height_, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (!ok(vkCreateImage(device_, &ci, nullptr, &x.out[e])))
                return false;
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(device_, x.out[e], &mr);
            uint32_t mt =
                memory_type(physical_, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = mt;
            if (!ok(vkAllocateMemory(device_, &ai, nullptr, &x.out_mem[e])) ||
                !ok(vkBindImageMemory(device_, x.out[e], x.out_mem[e], 0)))
                return false;
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = x.out[e];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ci.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (!ok(vkCreateImageView(device_, &vi, nullptr, &x.out_view[e])))
                return false;
            VkImageView attachments[2] = {x.out_view[e], x.density_view};
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = x.pass;
            fi.attachmentCount = x.use_density ? 2u : 1u;
            fi.pAttachments = attachments;
            fi.width = width_;
            fi.height = height_;
            fi.layers = 1;
            if (!ok(vkCreateFramebuffer(device_, &fi, nullptr, &x.fb[e])))
                return false;
        }
    VkDescriptorImageInfo yi{x.ysam, a.view[0], VK_IMAGE_LAYOUT_GENERAL},
        ci{x.csam, a.view[1], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo ti{table, 0, bytes};
    VkWriteDescriptorSet ws[3]{};
    for (auto& w : ws)
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[0].dstSet = x.ds;
    ws[0].dstBinding = 3;
    ws[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ws[0].descriptorCount = 1;
    ws[0].pImageInfo = &yi;
    ws[1].dstSet = x.ds;
    ws[1].dstBinding = 4;
    ws[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ws[1].descriptorCount = 1;
    ws[1].pImageInfo = &ci;
    ws[2].dstSet = x.ds;
    ws[2].dstBinding = 5;
    ws[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ws[2].descriptorCount = 1;
    ws[2].pBufferInfo = &ti;
    vkUpdateDescriptorSets(device_, 3, ws, 0, nullptr);
    if (!ok(vkResetCommandBuffer(x.cmd, 0)))
        return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!ok(vkBeginCommandBuffer(x.cmd, &bi)))
        return false;
    VkImageMemoryBarrier ib[2]{};
    for (int i = 0; i < 2; ++i) {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        ib[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].image = a.image[i];
        ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkBufferMemoryBarrier tb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    tb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    tb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tb.buffer = table;
    tb.offset = 0;
    tb.size = bytes;
    vkCmdPipelineBarrier(x.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &tb, 2, ib);
    VkViewport vp{0, 0, float(width_), float(height_), 0, 1};
    VkRect2D sc{{0, 0}, {width_, height_}};
    VkBuffer vb = x.mesh;
    VkDeviceSize vo = 0;
    Push pc{};
    uint32_t pw = a.width[0] / 2, ph = a.height[0];
    VkImageMemoryBarrier ob[2]{};
    for (int i = 0; i < 2; ++i) {
        auto& b = ob[i];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = x.out_initialized[i] ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = x.out_initialized[i] ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                           : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = x.out[i];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(x.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         2, ob);
    for (int e = 0; e < 2; ++e) {
        pc.rect[2] = pc.rect[6] = pw;
        pc.rect[3] = pc.rect[7] = ph;
        for (int i = 0; i < 4; ++i)
            pc.scale[i] = 1;
        VkClearValue cv{};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = x.pass;
        rbi.framebuffer = x.fb[e];
        rbi.renderArea = sc;
        rbi.clearValueCount = 1;
        rbi.pClearValues = &cv;
        vkCmdBeginRenderPass(x.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(x.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, x.pipe[e]);
        vkCmdBindDescriptorSets(x.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, x.pl, 0, 1, &x.ds, 0,
                                nullptr);
        vkCmdSetViewport(x.cmd, 0, 1, &vp);
        vkCmdSetScissor(x.cmd, 0, 1, &sc);
        vkCmdPushConstants(x.cmd, x.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdBindVertexBuffers(x.cmd, 0, 1, &vb, &vo);
        vkCmdDraw(x.cmd, x.mesh_count, 1, 0, 0);
        vkCmdEndRenderPass(x.cmd);
    }
    if (!ok(vkEndCommandBuffer(x.cmd)))
        return false;
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!ok(vkCreateFence(device_, &fi, nullptr, &f)))
        return false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &x.cmd;
    if (!ok(vkQueueSubmit(queue_, 1, &si, f))) {
        vkDestroyFence(device_, f, nullptr);
        return false;
    }
    if (completion)
        *completion = f;
    else {
        x.pending = f;
    }
    return true;
}
bool Renderer::wait() {
    auto& x = *impl_;
    if (!x.pending)
        return true;
    bool r = vkWaitForFences(device_, 1, &x.pending, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    vkDestroyFence(device_, x.pending, nullptr);
    x.pending = VK_NULL_HANDLE;
    return r;
}
bool Renderer::readback_ppm(uint32_t eye, const std::filesystem::path& path) {
    auto& x = *impl_;
    if (eye >= 2 || !x.out[eye] || !wait())
        return false;
    VkDeviceSize n = VkDeviceSize(width_) * height_ * 4;
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!ok(vkCreateBuffer(device_, &bi, nullptr, &b)))
        return false;
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, b, &mr);
    uint32_t mt =
        memory_type(physical_, mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    if (!ok(vkAllocateMemory(device_, &ai, nullptr, &m)) ||
        !ok(vkBindBufferMemory(device_, b, m, 0)))
        return false;
    VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkResetCommandBuffer(x.cmd, 0);
    if (!ok(vkBeginCommandBuffer(x.cmd, &cb)))
        return false;
    VkImageMemoryBarrier to{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to.image = x.out[eye];
    to.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(x.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(x.cmd, x.out[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &cp);
    to.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(x.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to);
    if (!ok(vkEndCommandBuffer(x.cmd)))
        return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    vkCreateFence(device_, &fci, nullptr, &f);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &x.cmd;
    if (!ok(vkQueueSubmit(queue_, 1, &si, f)) ||
        !ok(vkWaitForFences(device_, 1, &f, VK_TRUE, UINT64_MAX)))
        return false;
    vkDestroyFence(device_, f, nullptr);
    void* ptr = nullptr;
    if (!ok(vkMapMemory(device_, m, 0, n, 0, &ptr)))
        return false;
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width_ << ' ' << height_ << "\n255\n";
    auto* p = static_cast<uint8_t*>(ptr);
    for (uint32_t y = 0; y < height_; ++y)
        for (uint32_t xx = 0; xx < width_; ++xx)
            out.write(reinterpret_cast<char*>(p + 4 * (y * width_ + xx)), 3);
    vkUnmapMemory(device_, m);
    vkDestroyBuffer(device_, b, nullptr);
    vkFreeMemory(device_, m, nullptr);
    return bool(out);
}
VkImage Renderer::output_image(uint32_t e) const {
    return e < 2 ? impl_->out[e] : VK_NULL_HANDLE;
}
VkImageView Renderer::output_view(uint32_t e) const {
    return e < 2 ? impl_->out_view[e] : VK_NULL_HANDLE;
}

}  // namespace nx_probe
