#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <lz4.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <zstd.h>
using C = std::chrono::steady_clock;
static void ck(VkResult r, const char* s) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(s) + " Vulkan " + std::to_string(r));
}
static std::vector<uint32_t> spv(const char* p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error(std::string("open ") + p);
    auto n = f.tellg();
    if (n <= 0 || n % 4)
        throw std::runtime_error("bad SPIR-V");
    std::vector<uint32_t> x(size_t(n) / 4);
    f.seekg(0);
    f.read((char*)x.data(), n);
    return x;
}
struct B {
    VkBuffer b{};
    VkDeviceMemory m{};
    VkDeviceSize n{};
    void* map{};
};
struct App {
    VkInstance in{};
    VkPhysicalDevice ph{};
    VkDevice d{};
    VkQueue q{};
    uint32_t family{}, valid{};
    float period{};
    VkPhysicalDeviceMemoryProperties mp{};
    VkCommandPool cp{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkQueryPool qp{};
    VkImage image{};
    VkDeviceMemory im{};
    VkImageView view{};
    VkSampler sampler{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout pl{};
    VkPipeline pipe{};
    VkShaderModule sm{};
    VkDescriptorPool dp{};
    VkDescriptorSet ds{};
    std::vector<B> bs;
    ~App() {
        if (d)
            vkDeviceWaitIdle(d);
        if (d) {
            if (cp)
                vkDestroyCommandPool(d, cp, 0);
            if (dp)
                vkDestroyDescriptorPool(d, dp, 0);
            if (pipe)
                vkDestroyPipeline(d, pipe, 0);
            if (sm)
                vkDestroyShaderModule(d, sm, 0);
            if (pl)
                vkDestroyPipelineLayout(d, pl, 0);
            if (dsl)
                vkDestroyDescriptorSetLayout(d, dsl, 0);
            if (sampler)
                vkDestroySampler(d, sampler, 0);
            if (view)
                vkDestroyImageView(d, view, 0);
            if (image)
                vkDestroyImage(d, image, 0);
            if (im)
                vkFreeMemory(d, im, 0);
            if (qp)
                vkDestroyQueryPool(d, qp, 0);
            if (fence)
                vkDestroyFence(d, fence, 0);
            for (auto& b : bs) {
                if (b.map)
                    vkUnmapMemory(d, b.m);
                if (b.b)
                    vkDestroyBuffer(d, b.b, 0);
                if (b.m)
                    vkFreeMemory(d, b.m, 0);
            }
            vkDestroyDevice(d, 0);
        }
        if (in)
            vkDestroyInstance(in, 0);
    }
    uint32_t mt(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("no memory type");
    }
    B buffer(VkDeviceSize n, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, bool mapped) {
        B x;
        x.n = n;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = n;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ck(vkCreateBuffer(d, &ci, 0, &x.b), "buffer");
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(d, x.b, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mt(mr.memoryTypeBits, want);
        ck(vkAllocateMemory(d, &ai, 0, &x.m), "buffer memory");
        ck(vkBindBufferMemory(d, x.b, x.m, 0), "bind buffer");
        if (mapped)
            ck(vkMapMemory(d, x.m, 0, VK_WHOLE_SIZE, 0, &x.map), "map");
        bs.push_back(x);
        return x;
    }
    void submit() {
        ck(vkEndCommandBuffer(cmd), "end cmd");
        ck(vkResetFences(d, 1, &fence), "reset fence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ck(vkQueueSubmit(q, 1, &si, fence), "submit");
        ck(vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
    }
};
struct S {
    double gpu, all, record, submit, wait, query, readback, lz, zstd, cpu;
    size_t lzbytes, zstdbytes, selected;
};
int main(int ac, char** av) {
    try {
        if (ac != 10)
            throw std::runtime_error("usage: astc-safety input.rgba output-prefix srcW srcH "
                                     "targetW targetH fit quality shader.spv");
        std::string inp = av[1], prefix = av[2];
        uint32_t sw = std::stoul(av[3]), sh = std::stoul(av[4]), w = std::stoul(av[5]),
                 h = std::stoul(av[6]), fit = std::stoul(av[7]), quality = std::stoul(av[8]);
        if (!sw || !sh || !w || !h || fit > 3 || quality > 6)
            throw std::runtime_error("invalid dimensions or encoder settings");
        size_t srcN = size_t(sw) * sh * 4;
        uint64_t blocks = ((w + 7) / 8) * ((h + 7) / 8), outN = blocks * 16;
        std::ifstream fi(inp, std::ios::binary | std::ios::ate);
        if (!fi || uint64_t(fi.tellg()) != srcN)
            throw std::runtime_error("input RGBA byte count mismatch");
        std::vector<uint8_t> src(srcN);
        fi.seekg(0);
        fi.read((char*)src.data(), src.size());
        if (!fi)
            throw std::runtime_error("read input");
        App a;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "astc-safety";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app;
        ck(vkCreateInstance(&ii, 0, &a.in), "instance");
        uint32_t np = 0;
        ck(vkEnumeratePhysicalDevices(a.in, &np, 0), "devices");
        if (!np)
            throw std::runtime_error("no Vulkan devices");
        std::vector<VkPhysicalDevice> ps(np);
        ck(vkEnumeratePhysicalDevices(a.in, &np, ps.data()), "devices");
        int best = -1;
        VkPhysicalDeviceProperties props{};
        for (auto p : ps) {
            VkPhysicalDeviceProperties x;
            vkGetPhysicalDeviceProperties(p, &x);
            uint32_t nq = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, 0);
            std::vector<VkQueueFamilyProperties> qs(nq);
            vkGetPhysicalDeviceQueueFamilyProperties(p, &nq, qs.data());
            for (uint32_t j = 0; j < nq; j++)
                if ((qs[j].queueFlags & VK_QUEUE_COMPUTE_BIT) && qs[j].timestampValidBits) {
                    int score = x.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 10 : 0;
                    if (std::string(x.deviceName).find("7900 XTX") != std::string::npos)
                        score += 100;
                    if (score > best) {
                        best = score;
                        a.ph = p;
                        a.family = j;
                        a.valid = qs[j].timestampValidBits;
                        props = x;
                    }
                }
        }
        if (best < 0)
            throw std::runtime_error("no compute queue with timestamps");
        a.period = props.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(a.ph, &a.mp);
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(a.ph, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            throw std::runtime_error("RGBA8 linear sampling unsupported");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = a.family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        ck(vkCreateDevice(a.ph, &di, 0, &a.d), "device");
        vkGetDeviceQueue(a.d, a.family, 0, &a.q);
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent = {sw, sh, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ck(vkCreateImage(a.d, &ic, 0, &a.image), "image");
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(a.d, a.image, &mr);
        VkMemoryAllocateInfo ia{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ia.allocationSize = mr.size;
        ia.memoryTypeIndex = a.mt(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ck(vkAllocateMemory(a.d, &ia, 0, &a.im), "image memory");
        ck(vkBindImageMemory(a.d, a.image, a.im, 0), "bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = a.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ic.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ck(vkCreateImageView(a.d, &vi, 0, &a.view), "image view");
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ck(vkCreateSampler(a.d, &sci, 0, &a.sampler), "sampler");
        auto staging = a.buffer(
            srcN, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        memcpy(staging.map, src.data(), srcN);
        auto output =
            a.buffer(outN, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        auto readback = a.buffer(
            outN, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = a.family;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ck(vkCreateCommandPool(a.d, &pci, 0, &a.cp), "command pool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = a.cp;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        ck(vkAllocateCommandBuffers(a.d, &cai, &a.cmd), "command buffer");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ck(vkCreateFence(a.d, &fci, 0, &a.fence), "fence");
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 4;
        ck(vkCreateQueryPool(a.d, &qci, 0, &a.qp), "queries");
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.srcAccessMask = 0;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = a.image;
        ib.subresourceRange = vi.subresourceRange;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ck(vkBeginCommandBuffer(a.cmd, &begin), "begin upload");
        vkCmdPipelineBarrier(a.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &ib);
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {sw, sh, 1};
        vkCmdCopyBufferToImage(a.cmd, staging.b, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &bic);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(a.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 0, 0, 1, &ib);
        a.submit();
        VkDescriptorSetLayoutBinding binds[3]{};
        for (int i = 0; i < 3; i++) {
            binds[i].binding = i;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            binds[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 3;
        dl.pBindings = binds;
        ck(vkCreateDescriptorSetLayout(a.d, &dl, 0, &a.dsl), "descriptor layout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &a.dsl;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &range;
        ck(vkCreatePipelineLayout(a.d, &pl, 0, &a.pl), "pipeline layout");
        auto code = spv(av[9]);
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = code.size() * 4;
        smi.pCode = code.data();
        ck(vkCreateShaderModule(a.d, &smi, 0, &a.sm), "shader module");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = a.sm;
        stage.pName = "main";
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = stage;
        cpi.layout = a.pl;
        ck(vkCreateComputePipelines(a.d, 0, 1, &cpi, 0, &a.pipe), "pipeline");
        VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        ck(vkCreateDescriptorPool(a.d, &dpi, 0, &a.dp), "descriptor pool");
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = a.dp;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &a.dsl;
        ck(vkAllocateDescriptorSets(a.d, &dai, &a.ds), "descriptor set");
        VkDescriptorImageInfo iminfo{a.sampler, a.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bo{output.b, 0, outN};
        VkWriteDescriptorSet wr[3]{};
        for (int i = 0; i < 3; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = a.ds;
            wr[i].dstBinding = i;
            wr[i].descriptorCount = 1;
            if (i < 2) {
                wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                wr[i].pImageInfo = &iminfo;
            } else {
                wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr[i].pBufferInfo = &bo;
            }
        }
        vkUpdateDescriptorSets(a.d, 3, wr, 0, 0);
        int cnmax = LZ4_compressBound(int(outN));
        std::vector<char> compressed(cnmax), zcompressed(ZSTD_compressBound(outN));
        std::vector<uint8_t> result(outN);
        ZSTD_CCtx* zctx = ZSTD_createCCtx();
        if (!zctx)
            throw std::runtime_error("Zstd context allocation failed");
        std::vector<S> ss;
        constexpr int warm = 12, count = 20;
        int lastLz = 0;
        size_t lastZstd = 0, lastSelected = 0;
        const char* selectedCodec = "raw";
        for (int i = 0; i < warm + count; i++) {
            auto call0 = C::now();
            ck(vkResetCommandBuffer(a.cmd, 0), "reset command");
            ck(vkBeginCommandBuffer(a.cmd, &begin), "begin encode");
            vkCmdResetQueryPool(a.cmd, a.qp, 0, 4);
            vkCmdWriteTimestamp(a.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, a.qp, 0);
            vkCmdWriteTimestamp(a.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, a.qp, 1);
            vkCmdBindPipeline(a.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.pipe);
            vkCmdBindDescriptorSets(a.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.pl, 0, 1, &a.ds, 0, 0);
            uint32_t pc[5]{w, h, fit, quality, 1};
            vkCmdPushConstants(a.cmd, a.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc);
            vkCmdDispatch(a.cmd, uint32_t((blocks + 63) / 64), 1, 1);
            vkCmdWriteTimestamp(a.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, a.qp, 2);
            VkBufferMemoryBarrier ob{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            ob.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            ob.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            ob.srcQueueFamilyIndex = ob.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ob.buffer = output.b;
            ob.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(a.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 1, &ob, 0, 0);
            VkBufferCopy bc{0, 0, outN};
            vkCmdCopyBuffer(a.cmd, output.b, readback.b, 1, &bc);
            VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hb.buffer = readback.b;
            hb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(a.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, 0, 1, &hb, 0, 0);
            vkCmdWriteTimestamp(a.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, a.qp, 3);
            ck(vkEndCommandBuffer(a.cmd), "end command");
            auto recordEnd = C::now();
            ck(vkResetFences(a.d, 1, &a.fence), "reset fence");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &a.cmd;
            auto submit0 = C::now();
            ck(vkQueueSubmit(a.q, 1, &si, a.fence), "queue submit");
            auto submitEnd = C::now();
            ck(vkWaitForFences(a.d, 1, &a.fence, VK_TRUE, UINT64_MAX), "wait fence");
            auto waitEnd = C::now();
            uint64_t q[4];
            auto query0 = C::now();
            ck(vkGetQueryPoolResults(a.d, a.qp, 0, 4, sizeof(q), q, sizeof(uint64_t),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
               "query results");
            auto queryEnd = C::now();
            auto delta = [&](int x, int y) {
                uint64_t mask = a.valid == 64 ? UINT64_MAX : ((uint64_t(1) << a.valid) - 1);
                return double((q[y] - q[x]) & mask) * a.period / 1e6;
            };
            auto read0 = C::now();
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = readback.m;
            range.size = VK_WHOLE_SIZE;
            ck(vkInvalidateMappedMemoryRanges(a.d, 1, &range), "invalidate");
            memcpy(result.data(), readback.map, outN);
            auto readEnd = C::now();
            auto l0 = C::now();
            lastLz = LZ4_compress_default((const char*)result.data(), compressed.data(), int(outN),
                                          cnmax);
            auto l1 = C::now();
            if (lastLz <= 0)
                throw std::runtime_error("LZ4 failed");
            lastZstd = ZSTD_compressCCtx(zctx, zcompressed.data(), zcompressed.size(),
                                         result.data(), outN, 3);
            auto z1 = C::now();
            if (ZSTD_isError(lastZstd) || !lastZstd)
                throw std::runtime_error("Zstd failed");
            auto z2 = C::now();
            lastSelected = outN;
            selectedCodec = "raw";
            if (size_t(lastLz) < lastSelected) {
                lastSelected = lastLz;
                selectedCodec = "lz4";
            }
            if (lastZstd * 100 <= lastSelected * 90) {
                lastSelected = lastZstd;
                selectedCodec = "zstd";
            }
            auto callEnd = C::now();
            if (i >= warm)
                ss.push_back(
                    {delta(1, 2), delta(0, 3),
                     std::chrono::duration<double, std::milli>(recordEnd - call0).count(),
                     std::chrono::duration<double, std::milli>(submitEnd - submit0).count(),
                     std::chrono::duration<double, std::milli>(waitEnd - submitEnd).count(),
                     std::chrono::duration<double, std::milli>(queryEnd - query0).count(),
                     std::chrono::duration<double, std::milli>(readEnd - read0).count(),
                     std::chrono::duration<double, std::milli>(l1 - l0).count(),
                     std::chrono::duration<double, std::milli>(z2 - l1).count(),
                     std::chrono::duration<double, std::milli>(callEnd - call0).count(),
                     size_t(lastLz), lastZstd, lastSelected});
        }
        auto pct = [](std::vector<double> v, double p) {
            std::sort(v.begin(), v.end());
            return v[size_t((v.size() - 1) * p)];
        };
        std::vector<double> g, all, rec, sub, wait, query, read, lz, zstd, cpu;
        for (auto& s : ss) {
            g.push_back(s.gpu);
            all.push_back(s.all);
            rec.push_back(s.record);
            sub.push_back(s.submit);
            wait.push_back(s.wait);
            query.push_back(s.query);
            read.push_back(s.readback);
            lz.push_back(s.lz);
            zstd.push_back(s.zstd);
            cpu.push_back(s.cpu);
        }
        std::ofstream csv(prefix + ".csv");
        csv << "sample,gpu_dispatch_ms,gpu_dispatch_readback_ms,record_ms,submit_api_ms,fence_wait_"
               "ms,query_ms,invalidate_readback_ms,lz4_ms,zstd_ms,cpu_complete_ms,lz4_bytes,zstd_"
               "bytes,selected_bytes\n";
        for (size_t i = 0; i < ss.size(); i++) {
            auto& s = ss[i];
            csv << i << ',' << s.gpu << ',' << s.all << ',' << s.record << ',' << s.submit << ','
                << s.wait << ',' << s.query << ',' << s.readback << ',' << s.lz << ',' << s.zstd
                << ',' << s.cpu << ',' << s.lzbytes << ',' << s.zstdbytes << ',' << s.selected
                << '\n';
        }
        uint8_t header[16] = {0x13,
                              0xAB,
                              0xA1,
                              0x5C,
                              8,
                              8,
                              1,
                              uint8_t(w),
                              uint8_t(w >> 8),
                              uint8_t(w >> 16),
                              uint8_t(h),
                              uint8_t(h >> 8),
                              uint8_t(h >> 16),
                              1,
                              0,
                              0};
        auto save = [&](const std::string& path, const void* data, size_t size) {
            std::ofstream o(path, std::ios::binary);
            o.write((const char*)data, size);
            if (!o)
                throw std::runtime_error("write " + path);
        };
        save(prefix + ".astc", header, sizeof(header));
        {
            std::ofstream o(prefix + ".astc", std::ios::binary | std::ios::app);
            o.write((char*)result.data(), outN);
        }
        save(prefix + ".lz4", compressed.data(), lastLz);
        save(prefix + ".zst", zcompressed.data(), lastZstd);
        if (std::string(selectedCodec) == "lz4")
            save(prefix + ".selected", compressed.data(), lastLz);
        else if (std::string(selectedCodec) == "zstd")
            save(prefix + ".selected", zcompressed.data(), lastZstd);
        else
            save(prefix + ".selected", result.data(), outN);
        ZSTD_freeCCtx(zctx);
        auto meanBytes = [&](auto f) {
            double n = 0;
            for (auto& s : ss)
                n += f(s);
            return n / ss.size();
        };
        std::cout << "device=" << props.deviceName << " source=" << sw << 'x' << sh
                  << " target=" << w << 'x' << h << " fit=" << fit << " quality=" << quality
                  << " warmup=" << warm << " samples=" << count
                  << " upload=excluded codec=" << selectedCodec
                  << " mean_selected_bytes=" << meanBytes([](S& s) { return double(s.selected); })
                  << "\n"
                  << "GPU dispatch ms median/p95=" << pct(g, .5) << '/' << pct(g, .95)
                  << " dispatch+readback=" << pct(all, .5) << '/' << pct(all, .95)
                  << "\nCPU ms median/p95 record=" << pct(rec, .5) << '/' << pct(rec, .95)
                  << " submitAPI=" << pct(sub, .5) << '/' << pct(sub, .95)
                  << " fenceWait=" << pct(wait, .5) << '/' << pct(wait, .95)
                  << " query=" << pct(query, .5) << '/' << pct(query, .95)
                  << " invalidate+copy=" << pct(read, .5) << '/' << pct(read, .95)
                  << " LZ4=" << pct(lz, .5) << '/' << pct(lz, .95) << " Zstd=" << pct(zstd, .5)
                  << '/' << pct(zstd, .95) << " total=" << pct(cpu, .5) << '/' << pct(cpu, .95)
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
