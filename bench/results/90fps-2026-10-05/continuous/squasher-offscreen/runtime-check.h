#pragma once

#include "compositor/layer_squasher.h"
#include "driver/wivrn_hmd.h"
#include "utils/wivrn_vk_bundle.h"
#include "vk/allocation.h"
#include "vk/vk_allocator.h"
#include "util/comp_render_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

// Record one actual production do_layers dispatch, then sample its real sRGB
// views into host-visible storage, submit, wait for retirement and verify samples.
inline std::array<float, 40> record_squasher_color_check(
        wivrn::vk_bundle & vkb,
        wivrn::layer_squasher & squasher,
        std::array<image_allocation, 2> & source_images,
        wivrn::wivrn_hmd & hmd,
        const comp_frame & frame,
        const comp_layer_accum & layers,
        uint32_t side,
        const std::filesystem::path & shader_path)
{
    if (layers.layer_count != 1 || layers.layers[0].data.type != XRT_LAYER_PROJECTION ||
        layers.layers[0].data.timestamp != int64_t(frame.predicted_display_time_ns))
        throw std::runtime_error("fixture must supply one timestamp-matched stereo projection layer");
    if (is_layer_view_space(&layers.layers[0].data))
            throw std::runtime_error("view-space layer would invoke HMD tracking callbacks");

    std::ifstream shader_file(shader_path, std::ios::binary | std::ios::ate);
    if (!shader_file)
        throw std::runtime_error("cannot open output sampling SPIR-V");
    const auto shader_bytes = shader_file.tellg();
    if (shader_bytes <= 0 || shader_bytes % 4 != 0)
        throw std::runtime_error("invalid output sampling SPIR-V size");
    std::vector<uint32_t> shader_code(size_t(shader_bytes) / 4);
    shader_file.seekg(0);
    shader_file.read(reinterpret_cast<char *>(shader_code.data()), shader_bytes);
    if (!shader_file)
        throw std::runtime_error("failed reading output sampling SPIR-V");

    vk::raii::ShaderModule shader{vkb.device, vk::ShaderModuleCreateInfo{
        .codeSize = shader_code.size() * sizeof(uint32_t), .pCode = shader_code.data()}};
    vk::raii::Sampler sampler{vkb.device, vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.f}};

    std::array bindings{
        vk::DescriptorSetLayoutBinding{.binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{.binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute}};
    vk::raii::DescriptorSetLayout set_layout{vkb.device, vk::DescriptorSetLayoutCreateInfo{
        .bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data()}};
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t)};
    vk::raii::PipelineLayout pipeline_layout{vkb.device, vk::PipelineLayoutCreateInfo{
        .setLayoutCount = 1, .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range}};
    vk::raii::Pipeline pipeline{vkb.device, nullptr, vk::ComputePipelineCreateInfo{
        .stage = vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eCompute, .module = *shader, .pName = "main"},
        .layout = *pipeline_layout}};

    vk::raii::DescriptorPool descriptor_pool{vkb.device, vk::DescriptorPoolCreateInfo{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = 2, .poolSizeCount = 2,
        .pPoolSizes = std::array{
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 2},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 2}}.data()}};
    const std::array layouts{*set_layout, *set_layout};
    auto sets = vkb.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
        .descriptorPool = *descriptor_pool, .descriptorSetCount = 2, .pSetLayouts = layouts.data()});
    buffer_allocation samples{vkb.device, vk::BufferCreateInfo{
        .size = sizeof(float) * 40, .usage = vk::BufferUsageFlagBits::eStorageBuffer},
        VmaAllocationCreateInfo{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                .usage = VMA_MEMORY_USAGE_AUTO,
                                .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT}};
    std::fill_n(samples.data<float>(), 40, std::numeric_limits<float>::quiet_NaN());
    if (vmaFlushAllocation(vk_allocator::instance(), samples, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("sample buffer flush failed");
    const auto output_views = squasher.get_views();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const vk::DescriptorImageInfo image_info{*sampler, output_views[eye],
                                                  vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorBufferInfo buffer_info{samples, 0, sizeof(float) * 40};
        const std::array writes{
            vk::WriteDescriptorSet{.dstSet = *sets[eye], .dstBinding = 0,
                .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &image_info},
            vk::WriteDescriptorSet{.dstSet = *sets[eye], .dstBinding = 1,
                .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &buffer_info}};
        vkb.device.updateDescriptorSets(writes, {});
    }

    vk::raii::CommandPool pool{vkb.device, vk::CommandPoolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = vkb.queue.family_index}};
    auto command_buffers = vkb.device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
        .commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
    auto & command = command_buffers.front();
    command.begin(vk::CommandBufferBeginInfo{});

    std::array<vk::ImageMemoryBarrier, 2> to_clear{};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        to_clear[eye] = vk::ImageMemoryBarrier{
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_images[eye],
            .subresourceRange = vk::ImageSubresourceRange{
                vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_clear);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        vk::ClearColorValue color{};
        color.float32[eye == 0 ? 0 : 1] = 1.f;
        color.float32[3] = 1.f;
        command.clearColorImage(source_images[eye], vk::ImageLayout::eTransferDstOptimal,
            color, vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
        to_clear[eye].srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        to_clear[eye].dstAccessMask = vk::AccessFlagBits::eShaderRead;
        to_clear[eye].oldLayout = vk::ImageLayout::eTransferDstOptimal;
        to_clear[eye].newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, to_clear);

    (void)squasher.do_layers(vkb.device, command, hmd, 11'111'111, frame, layers,
        xrt_rect{.extent{.w = int32_t(side), .h = int32_t(side)}});
    vk::ImageMemoryBarrier output_ready{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = squasher.get_image(),
        .subresourceRange = vk::ImageSubresourceRange{
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2}};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, output_ready);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout,
            0, *sets[eye], {});
        const uint32_t offset = eye * 5;
        command.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute,
            0, vk::ArrayProxy<const uint32_t>(offset));
        command.dispatch(5, 1, 1);
    }
    vk::BufferMemoryBarrier host_ready{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = samples,
        .offset = 0, .size = sizeof(float) * 40};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eHost, {}, {}, host_ready, {});
    command.end();

    vk::raii::Fence fence{vkb.device, vk::FenceCreateInfo{}};
    const vk::CommandBuffer raw_command = *command;
    vkb.queue.queue.submit(vk::SubmitInfo{.commandBufferCount = 1,
        .pCommandBuffers = &raw_command}, *fence);
    if (vkb.device.waitForFences(*fence, true, 5'000'000'000ULL) != vk::Result::eSuccess) {
        vkb.device.waitIdle(); // retire before owned resource destruction
        throw std::runtime_error("squasher fixture GPU fence did not complete");
    }

    if (vmaInvalidateAllocation(vk_allocator::instance(), samples, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("sample buffer invalidate failed");
    std::array<float, 40> result{};
    std::copy_n(samples.data<float>(), result.size(), result.data());
    for (uint32_t eye = 0; eye < 2; ++eye)
        for (uint32_t sample = 0; sample < 5; ++sample) {
            const auto i = size_t(eye * 20 + sample * 4);
            const std::array<float, 4> expected{
                eye == 0 ? 1.f : 0.f, eye == 1 ? 1.f : 0.f, 0.f, 1.f};
            for (size_t c = 0; c < 4; ++c)
                if (!std::isfinite(result[i + c]) || std::abs(result[i + c] - expected[c]) > 0.02f)
                    throw std::runtime_error("sampled compositor output did not match solid eye color");
        }
    return result;
}
