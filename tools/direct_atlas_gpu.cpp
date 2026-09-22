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
    if (argc != 9) {
        std::fprintf(stderr, "usage: %s shader.spv descriptors.bin blocks.bin cpu.rgba width height gpu.rgba sample.spv\n", argv[0]);
        return 2;
    }
    const uint32_t width = u32(argv[5]), height = u32(argv[6]);
    const size_t pixels = size_t(width) * size_t(height);
    if (pixels > std::numeric_limits<size_t>::max() / 4) return 2;
    auto shaderBytes = readFile(argv[1]);
    auto descriptors = readFile(argv[2]);
    auto blocks = readFile(argv[3]);
    auto cpu = readFile(argv[4]);
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

    nxb::VkCtx ctx;
    if (!ctx.create({}, {}, true)) return 2;
    if (!ctx.info.timestampValidBits) {
        std::fprintf(stderr, "queue has no timestamp support\n"); ctx.destroy(); return 2;
    }
    nxb::Buffer tiles = ctx.createBuffer(descriptors.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    nxb::Buffer data = ctx.createBuffer(blocks.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    nxb::Buffer output = ctx.createBuffer(pixels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::copy(descriptors.begin(), descriptors.end(), static_cast<uint8_t*>(tiles.mapped));
    std::copy(blocks.begin(), blocks.end(), static_cast<uint8_t*>(data.mapped));

    auto atlas = ctx.createImage((width/32)*34, (height/32)*34, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ctx.oneShot([&](VkCommandBuffer cmd){ nxb::toGeneral(cmd, atlas); });
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter=sci.minFilter=VK_FILTER_LINEAR; sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler; NXB_VK(vkCreateSampler(ctx.dev,&sci,nullptr,&sampler));
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i != 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 5; lci.pBindings = bindings;
    VkDescriptorSetLayout layout; NXB_VK(vkCreateDescriptorSetLayout(ctx.dev, &lci, nullptr, &layout));
    VkDescriptorPoolSize ps[3]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1; pci.poolSizeCount = 3; pci.pPoolSizes = ps;
    VkDescriptorPool pool; NXB_VK(vkCreateDescriptorPool(ctx.dev, &pci, nullptr, &pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = pool; sai.descriptorSetCount = 1; sai.pSetLayouts = &layout;
    VkDescriptorSet set; NXB_VK(vkAllocateDescriptorSets(ctx.dev, &sai, &set));
    VkDescriptorBufferInfo bi[3]{{tiles.buf, 0, VK_WHOLE_SIZE}, {data.buf, 0, VK_WHOLE_SIZE}, {output.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo storage{VK_NULL_HANDLE,atlas.view,VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo sampled{sampler,atlas.view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[5]{};
    for(uint32_t i=0;i<5;i++) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1;
        writes[i].descriptorType=bindings[i].descriptorType;
        if(i<2) writes[i].pBufferInfo=&bi[i];
        else if(i==2) writes[i].pImageInfo=&storage;
        else if(i==3) writes[i].pImageInfo=&sampled;
        else writes[i].pBufferInfo=&bi[2];
    }
    vkUpdateDescriptorSets(ctx.dev,5,writes,0,nullptr);

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
    auto sampleBytes=readFile(argv[8]);
    VkShaderModule sampleModule=ctx.shader(reinterpret_cast<const uint32_t*>(sampleBytes.data()),sampleBytes.size());
    cpi.stage.module=sampleModule;
    VkPipeline samplePipeline; NXB_VK(vkCreateComputePipelines(ctx.dev,VK_NULL_HANDLE,1,&cpi,nullptr,&samplePipeline));
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP; qci.queryCount = 3;
    VkQueryPool queries; NXB_VK(vkCreateQueryPool(ctx.dev, &qci, nullptr, &queries));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = ctx.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd; NXB_VK(vkAllocateCommandBuffers(ctx.dev, &cai, &cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    NXB_VK(vkCreateFence(ctx.dev, &fci, nullptr, &fence));
    std::vector<double> ns, unpackNs, sampleNs; ns.reserve(50);
    const uint32_t gx = (width + 7) / 8, gy = (height + 7) / 8;
    for (int run = 0; run < 60; ++run) {
        NXB_VK(vkResetFences(ctx.dev, 1, &fence));
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        NXB_VK(vkBeginCommandBuffer(cmd, &cbi));
        vkCmdResetQueryPool(cmd, queries, 0, 3);
        VkBufferMemoryBarrier hostToCompute{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostToCompute.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        hostToCompute.srcQueueFamilyIndex = hostToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostToCompute.buffer = tiles.buf; hostToCompute.size = VK_WHOLE_SIZE;
        VkBufferMemoryBarrier h2c[2] = {hostToCompute, hostToCompute}; h2c[1].buffer = data.buf;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 2, h2c, 0, nullptr);
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.oldLayout=ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        ib.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        ib.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        ib.image=atlas.img; ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        Push push{width, height}; vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 0);
        vkCmdDispatch(cmd, (atlas.w+7)/8, (atlas.h+7)/8, 1);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
        ib.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&ib);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,samplePipeline);
        vkCmdDispatch(cmd,gx,gy,1);
        vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,queries,2);
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
        uint64_t ts[3]{}; NXB_VK(vkGetQueryPoolResults(ctx.dev, queries, 0, 3, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        uint64_t ticks = ts[2] - ts[0];
        if (ctx.info.timestampValidBits < 64) ticks &= (uint64_t(1) << ctx.info.timestampValidBits) - 1;
        if (run >= 10) {
            ns.push_back(double(ticks)*ctx.info.timestampPeriod);
            const uint64_t mask=ctx.info.timestampValidBits==64?~uint64_t(0):(uint64_t(1)<<ctx.info.timestampValidBits)-1;
            unpackNs.push_back(double((ts[1]-ts[0])&mask)*ctx.info.timestampPeriod);
            sampleNs.push_back(double((ts[2]-ts[1])&mask)*ctx.info.timestampPeriod);
        }
    }
    std::vector<uint8_t> gpu(pixels * 4); std::copy_n(static_cast<uint8_t*>(output.mapped), gpu.size(), gpu.begin());
    std::ofstream out(argv[7], std::ios::binary); out.write(reinterpret_cast<const char*>(gpu.data()), std::streamsize(gpu.size()));
    if (!out) { std::fprintf(stderr, "cannot write GPU output\n"); return 2; }
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) { return ns[std::min<size_t>(ns.size() - 1, size_t(p * ns.size()))]; };
    size_t mismatches=0; unsigned maxError=0;
    for(size_t i=0;i<gpu.size();i++) { unsigned e=unsigned(std::abs(int(gpu[i])-int(cpu[i]))); maxError=std::max(maxError,e); mismatches+=(e>1); }
    std::sort(unpackNs.begin(),unpackNs.end()); std::sort(sampleNs.begin(),sampleNs.end());
    std::printf("device=%s total_ns_p50=%.0f total_ns_p95=%.0f unpack_ns_p50=%.0f sample_ns_p50=%.0f channels_error_gt1=%zu max_channel_error=%u\n",
        ctx.info.name.c_str(),pct(.5),pct(.95),unpackNs[25],sampleNs[25],mismatches,maxError);
    vkDestroyPipeline(ctx.dev,samplePipeline,nullptr); vkDestroyShaderModule(ctx.dev,sampleModule,nullptr);
    vkDestroySampler(ctx.dev,sampler,nullptr); ctx.destroyImage(atlas);
    vkDestroyFence(ctx.dev, fence, nullptr); vkDestroyQueryPool(ctx.dev, queries, nullptr); vkDestroyPipeline(ctx.dev, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.dev, pipelineLayout, nullptr); vkDestroyShaderModule(ctx.dev, sm, nullptr);
    vkDestroyDescriptorPool(ctx.dev, pool, nullptr); vkDestroyDescriptorSetLayout(ctx.dev, layout, nullptr);
    ctx.destroyBuffer(tiles); ctx.destroyBuffer(data); ctx.destroyBuffer(output); ctx.destroy();
    return mismatches ? 1 : 0;
}
