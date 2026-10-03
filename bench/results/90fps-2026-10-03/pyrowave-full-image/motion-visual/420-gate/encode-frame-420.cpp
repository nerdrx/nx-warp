#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include "pyrowave_encoder.h"
#include "vk/vk_allocator.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

struct Plane {
    vk::raii::Image image{nullptr};
    vk::raii::DeviceMemory memory{nullptr};
    vk::raii::ImageView view{nullptr};
    uint32_t width{}, height{};
};

static uint32_t memory_type(vk::raii::PhysicalDevice &pd, uint32_t mask,
                            vk::MemoryPropertyFlags required) {
    auto props = pd.getMemoryProperties();
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((mask & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    throw std::runtime_error("no compatible Vulkan memory type");
}

static Plane make_plane(vk::raii::PhysicalDevice &pd, vk::raii::Device &dev,
                        uint32_t width, uint32_t height) {
    Plane p;
    p.width = width;
    p.height = height;
    p.image = vk::raii::Image(dev, {
        .imageType = vk::ImageType::e2D, .format = vk::Format::eR8Unorm,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined});
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(VkDevice(*dev), VkImage(*p.image), &req);
    p.memory = vk::raii::DeviceMemory(dev, {
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type(pd, req.memoryTypeBits,
                                       vk::MemoryPropertyFlagBits::eDeviceLocal)});
    vkBindImageMemory(VkDevice(*dev), VkImage(*p.image), VkDeviceMemory(*p.memory), 0);
    p.view = vk::raii::ImageView(dev, {
        .image = *p.image, .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Unorm,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .levelCount = 1, .layerCount = 1}});
    return p;
}

int main(int argc, char **argv) try {
    if (argc != 6)
        throw std::runtime_error("usage: encoder input.yuv width height 420 output.pyrowave");
    const std::string input_path = argv[1], output_path = argv[5];
    const uint32_t width = std::stoul(argv[2]), height = std::stoul(argv[3]);
    if (std::string(argv[4]) != "420" || width % 64 || height % 64)
        throw std::runtime_error("format must be 444; dimensions must be multiples of 64");
    const uint32_t cw=width/2, ch=height/2;
    const size_t plane_size = size_t(width) * height, chroma_size=size_t(cw)*ch;
    std::vector<uint8_t> source(plane_size + 2*chroma_size);
    std::ifstream fin(input_path, std::ios::binary);
    if (!fin.read(reinterpret_cast<char *>(source.data()), source.size()) ||
        fin.peek() != std::ifstream::traits_type::eof())
        throw std::runtime_error("input size does not match one planar 4:2:0 frame");

    vk::raii::Context ctx;
    vk::ApplicationInfo ai{.pApplicationName = "PyroWave feature-correct host encoder",
                           .apiVersion = VK_API_VERSION_1_3};
    vk::raii::Instance instance(ctx, {.pApplicationInfo = &ai});
    auto pds = instance.enumeratePhysicalDevices();
    if (pds.empty()) throw std::runtime_error("no Vulkan physical device");
    auto &pd = pds.front();
    auto [supported, supported11, supported12, supported13] =
        pd.getFeatures2<vk::PhysicalDeviceFeatures2,
                        vk::PhysicalDeviceVulkan11Features,
                        vk::PhysicalDeviceVulkan12Features,
                        vk::PhysicalDeviceVulkan13Features>();
    if (!supported.features.shaderInt16 || !supported11.storageBuffer16BitAccess ||
        !supported12.storageBuffer8BitAccess ||
        !supported13.computeFullSubgroups)
        throw std::runtime_error("device lacks an encoder-required core, Vulkan 1.1, 1.2, or 1.3 feature");
    vk::PhysicalDeviceFeatures enabled_core{};
    enabled_core.shaderInt16 = VK_TRUE;
    vk::PhysicalDeviceVulkan11Features enabled11{};
    enabled11.storageBuffer16BitAccess = VK_TRUE;
    vk::PhysicalDeviceVulkan12Features enabled12{};
    enabled12.storageBuffer8BitAccess = VK_TRUE;
    enabled12.shaderFloat16 = supported12.shaderFloat16;
    vk::PhysicalDeviceVulkan13Features enabled13{};
    enabled13.subgroupSizeControl = supported13.subgroupSizeControl;
    enabled13.computeFullSubgroups = VK_TRUE;
    enabled11.pNext = &enabled12;
    enabled12.pNext = &enabled13;

    uint32_t qf = UINT32_MAX;
    auto qprops = pd.getQueueFamilyProperties();
    for (uint32_t i = 0; i < qprops.size(); ++i)
        if (qprops[i].queueFlags & vk::QueueFlagBits::eCompute) { qf = i; break; }
    if (qf == UINT32_MAX) throw std::runtime_error("no compute queue");
    const float priority = 1.f;
    vk::DeviceQueueCreateInfo qi{.queueFamilyIndex = qf, .queueCount = 1,
                                 .pQueuePriorities = &priority};
    vk::DeviceCreateInfo di{.pNext = &enabled11, .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qi, .pEnabledFeatures = &enabled_core};
    vk::raii::Device dev(pd, di);
    auto queue = dev.getQueue(qf, 0);
    std::cerr << "gpu=" << pd.getProperties().deviceName.data()
              << " api=" << VK_VERSION_MAJOR(pd.getProperties().apiVersion) << "."
              << VK_VERSION_MINOR(pd.getProperties().apiVersion)
              << " shaderInt16=enabled storageBuffer16BitAccess=enabled storageBuffer8BitAccess=enabled shaderFloat16="
              << bool(enabled12.shaderFloat16) << " subgroupSizeControl="
              << bool(enabled13.subgroupSizeControl)
              << " computeFullSubgroups=enabled\n";

    VmaAllocatorCreateInfo vma{};
    vma.instance = VkInstance(*instance);
    vma.physicalDevice = VkPhysicalDevice(*pd);
    vma.device = VkDevice(*dev);
    vma.vulkanApiVersion = VK_API_VERSION_1_3;
    vk_allocator allocator(vma, false);
    std::array<Plane, 3> planes{make_plane(pd, dev, width, height),
                                make_plane(pd, dev, cw, ch),
                                make_plane(pd, dev, cw, ch)};
    vk::raii::Buffer staging(dev, {.size = source.size(),
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
        .sharingMode = vk::SharingMode::eExclusive});
    VkMemoryRequirements breq{};
    vkGetBufferMemoryRequirements(VkDevice(*dev), VkBuffer(*staging), &breq);
    vk::raii::DeviceMemory staging_memory(dev, {.allocationSize = breq.size,
        .memoryTypeIndex = memory_type(pd, breq.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent)});
    vkBindBufferMemory(VkDevice(*dev), VkBuffer(*staging),
                       VkDeviceMemory(*staging_memory), 0);
    void *mapped = nullptr;
    vkMapMemory(VkDevice(*dev), VkDeviceMemory(*staging_memory), 0,
                source.size(), 0, &mapped);
    std::memcpy(mapped, source.data(), source.size());
    vkUnmapMemory(VkDevice(*dev), VkDeviceMemory(*staging_memory));

    PyroWave::Encoder encoder(pd, dev, width, height,
                              PyroWave::ChromaSubsampling::Chroma420);
    constexpr size_t target_size = 694328;
    const size_t meta_size = encoder.get_meta_required_size();
    const size_t bitstream_size = target_size + 2 * meta_size;
    auto make_host_buffer = [&](size_t size) {
        vk::raii::Buffer b(dev, {.size = size,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferSrc |
                     vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive});
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(VkDevice(*dev), VkBuffer(*b), &req);
        vk::raii::DeviceMemory mem(dev, {.allocationSize = req.size,
            .memoryTypeIndex = memory_type(pd, req.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent)});
        vkBindBufferMemory(VkDevice(*dev), VkBuffer(*b), VkDeviceMemory(*mem), 0);
        return std::pair{std::move(b), std::move(mem)};
    };
    auto [meta, meta_memory] = make_host_buffer(meta_size);
    auto [bits, bits_memory] = make_host_buffer(bitstream_size);
    vk::raii::CommandPool pool(dev, {.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                     .queueFamilyIndex = qf});
    auto cmds = dev.allocateCommandBuffers({.commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
    auto &cmd = cmds[0];
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    std::array<vk::ImageMemoryBarrier, 3> to_transfer{}, to_general{};
    for (size_t i = 0; i < planes.size(); ++i) {
        to_transfer[i] = {.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = *planes[i].image,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .levelCount = 1, .layerCount = 1}};
        to_general[i] = {.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = *planes[i].image,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .levelCount = 1, .layerCount = 1}};
    }
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_transfer);
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < planes.size(); ++i) {
        vk::BufferImageCopy region{.bufferOffset = offset,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .layerCount = 1},
            .imageExtent = {planes[i].width, planes[i].height, 1}};
        cmd.copyBufferToImage(*staging, *planes[i].image,
                              vk::ImageLayout::eTransferDstOptimal, region);
        offset += i == 0 ? plane_size : chroma_size;
    }
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, to_general);
    PyroWave::Encoder::ViewBuffers views{*planes[0].view, *planes[1].view,
                                         *planes[2].view};
    PyroWave::Encoder::BitstreamBuffers buffers{
        .meta = {.buffer = *meta, .size = meta_size},
        .bitstream = {.buffer = *bits, .size = bitstream_size},
        .target_size = target_size};
    if (!encoder.encode(cmd, views, buffers))
        throw std::runtime_error("PyroWave Encoder::encode failed");
    cmd.end();
    vk::FenceCreateInfo fence_info{.sType = vk::StructureType::eFenceCreateInfo};
    vk::raii::Fence fence(dev, fence_info);
    queue.submit(vk::SubmitInfo{.commandBufferCount = 1,
                                .pCommandBuffers = &*cmd}, *fence);
    VkFence f = VkFence(*fence);
    if (vkWaitForFences(VkDevice(*dev), 1, &f, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        throw std::runtime_error("encoder fence wait failed");

    void *meta_map = nullptr, *bits_map = nullptr;
    vkMapMemory(VkDevice(*dev), VkDeviceMemory(*meta_memory), 0, meta_size, 0, &meta_map);
    vkMapMemory(VkDevice(*dev), VkDeviceMemory(*bits_memory), 0, bitstream_size, 0, &bits_map);
    const size_t max_packets = encoder.compute_num_packets(meta_map, bitstream_size);
    std::vector<PyroWave::Encoder::Packet> packets(max_packets);
    std::vector<uint8_t> payload(bitstream_size);
    const size_t n_packets = encoder.packetize(packets.data(), bitstream_size,
        payload.data(), payload.size(), meta_map, bits_map);
    if (!n_packets) throw std::runtime_error("encoder produced no packets");
    const auto &last = packets[n_packets - 1];
    const size_t payload_size = last.offset + last.size;
    vkUnmapMemory(VkDevice(*dev), VkDeviceMemory(*meta_memory));
    vkUnmapMemory(VkDevice(*dev), VkDeviceMemory(*bits_memory));

    std::ofstream out(output_path, std::ios::binary);
    const char magic[8] = {'P','Y','R','O','H','A','A','R'};
    const int32_t header[8] = {int32_t(width), int32_t(height), 0, 0, 1, 25, 1, 0};
    const uint32_t packet_size = uint32_t(payload_size);
    out.write(magic, sizeof(magic));
    out.write(reinterpret_cast<const char *>(header), sizeof(header));
    out.write(reinterpret_cast<const char *>(&packet_size), sizeof(packet_size));
    out.write(reinterpret_cast<const char *>(payload.data()), payload_size);
    if (!out) throw std::runtime_error("writing output failed");
    std::cout << "gpu=" << pd.getProperties().deviceName.data()
              << " frame=" << width << "x" << height
              << " chroma=420 target=" << target_size
              << " packet_count=" << n_packets
              << " packet_bytes=" << payload_size
              << " file_bytes=" << (payload_size + 44)
              << " file=" << output_path << "\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
}
