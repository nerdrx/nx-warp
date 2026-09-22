#include "../bench/src/core/nxb_vk.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
    const auto n = f.tellg();
    if (n < 0) { std::fprintf(stderr, "cannot size %s\n", path); std::exit(2); }
    std::vector<uint8_t> out(static_cast<size_t>(n));
    f.seekg(0);
    if (!out.empty() && !f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()))) {
        std::fprintf(stderr, "cannot read %s\n", path); std::exit(2);
    }
    return out;
}

static uint32_t u32(const char* s)
{
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 10);
    if (!*s || !end || *end || v == 0 || v > std::numeric_limits<uint32_t>::max()) {
        std::fprintf(stderr, "bad dimension: %s\n", s); std::exit(2);
    }
    return uint32_t(v);
}

struct Push { uint32_t width, height; };

int main(int argc, char** argv)
{
    if (argc != 8 && argc != 9) {
        std::fprintf(stderr, "usage: %s shader.spv descriptors.bin blocks.bin cpu.rgba width height gpu.rgba\n", argv[0]);
        return 2;
    }
    const bool encode = argc == 9 && std::string(argv[8]) == "--encode";
    if (argc == 9 && !encode) return 2;
    const uint32_t width = u32(argv[5]), height = u32(argv[6]);
    const size_t pixels = size_t(width) * size_t(height);
    if (pixels > std::numeric_limits<size_t>::max() / 4) return 2;
    auto shaderBytes = readFile(argv[1]);
    auto descriptors = readFile(argv[2]);
    auto blocks = readFile(argv[3]);
    auto cpu = readFile(argv[4]);
    if (encode) {
        if (descriptors.empty() || descriptors.size()%16 || blocks.size()!=pixels*4 ||
            cpu.size()!=descriptors.size()/16*20) return 2;
        const auto* jobs=reinterpret_cast<const uint32_t*>(descriptors.data());
        for(size_t i=0;i<descriptors.size()/16;++i) {
            uint32_t x=jobs[i*4], y=jobs[i*4+1], scale=jobs[i*4+2], offset=jobs[i*4+3];
            if ((scale!=1 && scale!=2 && scale!=4) || size_t(x)+8*scale>width ||
                size_t(y)+8*scale>height || offset!=i*5) return 2;
        }
    } else {
    if (shaderBytes.empty() || shaderBytes.size() % 4 || descriptors.empty() || descriptors.size() % 4 ||
        blocks.empty() || blocks.size() % 20 || (width % 32) || (height % 32) ||
        cpu.size() != pixels * 4) {
        std::fprintf(stderr, "invalid input size\n"); return 2;
    }
    const size_t tileCount = size_t(width / 32) * size_t(height / 32);
    if (descriptors.size() / 4 != tileCount) {
        std::fprintf(stderr, "descriptor table size mismatch\n"); return 2;
    }
    const auto* desc = reinterpret_cast<const uint32_t*>(descriptors.data());
    const size_t blockWords = blocks.size() / 4;
    for (size_t i = 0; i < tileCount; ++i) {
        const uint32_t shift = desc[i] >> 30;
        const uint32_t blocksPerRow = 4u >> shift;
        const size_t blockCount = size_t(blocksPerRow) * blocksPerRow;
        if (shift > 2 || size_t(desc[i] & 0x3fffffffu) + 5 * blockCount > blockWords) {
            std::fprintf(stderr, "descriptor %zu points outside block table\n", i); return 2;
        }
    }

    }
    nxb::VkCtx ctx;
    if (!ctx.create({}, {}, true)) return 2;
    if (!ctx.info.timestampValidBits) {
        std::fprintf(stderr, "queue has no timestamp support\n"); ctx.destroy(); return 2;
    }
    nxb::Buffer tiles = ctx.createBuffer(descriptors.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    nxb::Buffer data = ctx.createBuffer(blocks.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    nxb::Buffer output = ctx.createBuffer(cpu.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::copy(descriptors.begin(), descriptors.end(), static_cast<uint8_t*>(tiles.mapped));
    std::copy(blocks.begin(), blocks.end(), static_cast<uint8_t*>(data.mapped));

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i != 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3; lci.pBindings = bindings;
    VkDescriptorSetLayout layout; NXB_VK(vkCreateDescriptorSetLayout(ctx.dev, &lci, nullptr, &layout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    VkDescriptorPool pool; NXB_VK(vkCreateDescriptorPool(ctx.dev, &pci, nullptr, &pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = pool; sai.descriptorSetCount = 1; sai.pSetLayouts = &layout;
    VkDescriptorSet set; NXB_VK(vkAllocateDescriptorSets(ctx.dev, &sai, &set));
    VkDescriptorBufferInfo bi[3]{{tiles.buf, 0, VK_WHOLE_SIZE}, {data.buf, 0, VK_WHOLE_SIZE}, {output.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i != 3; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[i], nullptr};
    }
    vkUpdateDescriptorSets(ctx.dev, 3, writes, 0, nullptr);

    VkShaderModule sm = ctx.shader(reinterpret_cast<const uint32_t*>(shaderBytes.data()), shaderBytes.size());
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &layout;
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pipelineLayout; NXB_VK(vkCreatePipelineLayout(ctx.dev, &plci, nullptr, &pipelineLayout));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cpi.layout = pipelineLayout;
    VkPipeline pipeline; NXB_VK(vkCreateComputePipelines(ctx.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP; qci.queryCount = 2;
    VkQueryPool queries; NXB_VK(vkCreateQueryPool(ctx.dev, &qci, nullptr, &queries));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = ctx.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd; NXB_VK(vkAllocateCommandBuffers(ctx.dev, &cai, &cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    NXB_VK(vkCreateFence(ctx.dev, &fci, nullptr, &fence));
    std::vector<double> ns; ns.reserve(50);
    const uint32_t gx = encode ? (uint32_t(descriptors.size()/16)+63)/64 : (width+7)/8;
    const uint32_t gy = encode ? 1 : (height+7)/8;
    for (int run = 0; run < 60; ++run) {
        NXB_VK(vkResetFences(ctx.dev, 1, &fence));
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        NXB_VK(vkBeginCommandBuffer(cmd, &cbi));
        vkCmdResetQueryPool(cmd, queries, 0, 2);
        VkBufferMemoryBarrier hostToCompute{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostToCompute.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        hostToCompute.srcQueueFamilyIndex = hostToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostToCompute.buffer = tiles.buf; hostToCompute.size = VK_WHOLE_SIZE;
        VkBufferMemoryBarrier h2c[2] = {hostToCompute, hostToCompute}; h2c[1].buffer = data.buf;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 2, h2c, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        Push push{width, height}; vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 0);
        vkCmdDispatch(cmd, gx, gy, 1);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
        VkBufferMemoryBarrier computeToHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        computeToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; computeToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        computeToHost.srcQueueFamilyIndex = computeToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        computeToHost.buffer = output.buf; computeToHost.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &computeToHost, 0, nullptr);
        NXB_VK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        NXB_VK(vkQueueSubmit(ctx.queue, 1, &submit, fence));
        NXB_VK(vkWaitForFences(ctx.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        uint64_t ts[2]{}; NXB_VK(vkGetQueryPoolResults(ctx.dev, queries, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        uint64_t ticks = ts[1] - ts[0];
        if (ctx.info.timestampValidBits < 64) ticks &= (uint64_t(1) << ctx.info.timestampValidBits) - 1;
        if (run >= 10) ns.push_back(double(ticks) * double(ctx.info.timestampPeriod));
    }
    std::vector<uint8_t> gpu(cpu.size()); std::copy_n(static_cast<uint8_t*>(output.mapped), gpu.size(), gpu.begin());
    std::ofstream out(argv[7], std::ios::binary); out.write(reinterpret_cast<const char*>(gpu.data()), std::streamsize(gpu.size()));
    if (!out) { std::fprintf(stderr, "cannot write GPU output\n"); return 2; }
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) { return ns[std::min<size_t>(ns.size() - 1, size_t(p * ns.size()))]; };
    size_t mismatches = 0;
    for (size_t i = 0; i < cpu.size()/4; ++i)
        mismatches += *reinterpret_cast<const uint32_t*>(gpu.data() + i * 4) !=
                      *reinterpret_cast<const uint32_t*>(cpu.data() + i * 4);
    std::printf("device=%s timestampPeriod=%g validBits=%u dispatch_ns_p50=%.0f dispatch_ns_p95=%.0f mismatched_words=%zu\n",
                ctx.info.name.c_str(), double(ctx.info.timestampPeriod), ctx.info.timestampValidBits, pct(.50), pct(.95), mismatches);
    vkDestroyFence(ctx.dev, fence, nullptr); vkDestroyQueryPool(ctx.dev, queries, nullptr); vkDestroyPipeline(ctx.dev, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.dev, pipelineLayout, nullptr); vkDestroyShaderModule(ctx.dev, sm, nullptr);
    vkDestroyDescriptorPool(ctx.dev, pool, nullptr); vkDestroyDescriptorSetLayout(ctx.dev, layout, nullptr);
    ctx.destroyBuffer(tiles); ctx.destroyBuffer(data); ctx.destroyBuffer(output); ctx.destroy();
    return mismatches ? 1 : 0;
}
