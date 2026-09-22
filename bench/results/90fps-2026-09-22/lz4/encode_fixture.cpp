#include "nxwarp_codec.h"
#include "nxwarp_direct.h"
#include <array>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

static void ok(VkResult r, const char * what)
{
	if (r != VK_SUCCESS)
	{
		std::fprintf(stderr, "%s: %d\n", what, int(r));
		std::abort();
	}
}
static uint32_t mt(VkPhysicalDevice g, uint32_t bits, VkMemoryPropertyFlags f)
{
	VkPhysicalDeviceMemoryProperties p{};
	vkGetPhysicalDeviceMemoryProperties(g, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & f) == f)
			return i;
	std::abort();
}

int main(int argc, char **argv)
{
	VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "direct-gpu-test", 1, "direct-gpu-test", 1, VK_API_VERSION_1_3};
	VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ii.pApplicationInfo = &ai;
	VkInstance instance{};
	ok(vkCreateInstance(&ii, nullptr, &instance), "instance");
	uint32_t ng = 0;
	ok(vkEnumeratePhysicalDevices(instance, &ng, nullptr), "gpus");
	std::vector<VkPhysicalDevice> gs(ng);
	ok(vkEnumeratePhysicalDevices(instance, &ng, gs.data()), "gpus");
	assert(!gs.empty());
	VkPhysicalDevice gpu = gs[0];
	uint32_t nf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, nullptr);
	std::vector<VkQueueFamilyProperties> fs(nf);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nf, fs.data());
	uint32_t family = 0;
	while (family < nf && !(fs[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
		++family;
	assert(family < nf);
	float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VkDevice device{};
	ok(vkCreateDevice(gpu, &dci, nullptr, &device), "device");
	VkQueue queue{};
	vkGetDeviceQueue(device, family, 0, &queue);

	constexpr uint32_t w = 2176, h = 2176, layers = 2;
	constexpr VkDeviceSize yb = w * h, uvb = (w / 2) * (h / 2) * 2, lb = yb + uvb;
	std::vector<uint8_t> pixels(lb * layers);
	assert(argc == 3);
	std::ifstream input(argv[1], std::ios::binary);
	assert(input.read(reinterpret_cast<char *>(pixels.data()), pixels.size()));
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = pixels.size();
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VkBuffer staging{};
	ok(vkCreateBuffer(device, &bci, nullptr, &staging), "staging");
	VkMemoryRequirements br{};
	vkGetBufferMemoryRequirements(device, staging, &br);
	VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, br.size, mt(gpu, br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VkDeviceMemory bm{};
	ok(vkAllocateMemory(device, &bai, nullptr, &bm), "staging memory");
	ok(vkBindBufferMemory(device, staging, bm, 0), "staging bind");
	void * mapped{};
	ok(vkMapMemory(device, bm, 0, pixels.size(), 0, &mapped), "map");
	std::memcpy(mapped, pixels.data(), pixels.size());
	vkUnmapMemory(device, bm);
	const VkFormat formats[] = {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT};
	VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
	fl.viewFormatCount = 5;
	fl.pViewFormats = formats;
	VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	im.pNext = &fl;
	im.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	im.imageType = VK_IMAGE_TYPE_2D;
	im.format = formats[0];
	im.extent = {w, h, 1};
	im.mipLevels = 1;
	im.arrayLayers = layers;
	im.samples = VK_SAMPLE_COUNT_1_BIT;
	im.tiling = VK_IMAGE_TILING_OPTIMAL;
	im.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	im.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage image{};
	ok(vkCreateImage(device, &im, nullptr, &image), "image");
	VkMemoryRequirements ir{};
	vkGetImageMemoryRequirements(device, image, &ir);
	VkMemoryAllocateInfo iai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, ir.size, mt(gpu, ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VkDeviceMemory imem{};
	ok(vkAllocateMemory(device, &iai, nullptr, &imem), "image memory");
	ok(vkBindImageMemory(device, image, imem, 0), "image bind");
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = family;
	VkCommandPool pool{};
	ok(vkCreateCommandPool(device, &pci, nullptr, &pool), "pool");
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer cmd{};
	ok(vkAllocateCommandBuffers(device, &cai, &cmd), "command");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	ok(vkBeginCommandBuffer(cmd, &begin), "begin");
	VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar.image = image;
	bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
	std::array<VkBufferImageCopy, 4> cp{};
	for (uint32_t e = 0; e < layers; ++e)
	{
		cp[e * 2] = {.bufferOffset = e * lb, .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, e, 1}, .imageExtent = {w, h, 1}};
		cp[e * 2 + 1] = {.bufferOffset = e * lb + yb, .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, e, 1}, .imageExtent = {w / 2, h / 2, 1}};
	}
	vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 4, cp.data());
	bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
	ok(vkEndCommandBuffer(cmd), "end");
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence{};
	ok(vkCreateFence(device, &fci, nullptr, &fence), "fence");
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	ok(vkQueueSubmit(queue, 1, &si, fence), "upload");
	ok(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait");

	wivrn::nxwarp_codec_config cfg{};
	cfg.width = w;
	cfg.height = h;
	cfg.eyes = 2;
	auto codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
	for (unsigned rate : {160u, 200u, 500u}) {
		codec->set_target_bitrate(rate * 1000000, 90);
		auto encoded = codec->encode_image_pair(image, 0, 1, 1);
		assert(wivrn::nxwarp_direct::parse_frame({w,h,2},encoded));
		std::ofstream output(std::string(argv[2])+"-"+std::to_string(rate)+".nxdf", std::ios::binary);
		output.write(reinterpret_cast<const char *>(encoded.data()),encoded.size());
		assert(output.good());
	}
	codec.reset();
	vkDeviceWaitIdle(device);
	vkDestroyFence(device, fence, nullptr);
	vkDestroyCommandPool(device, pool, nullptr);
	vkDestroyImage(device, image, nullptr);
	vkFreeMemory(device, imem, nullptr);
	vkDestroyBuffer(device, staging, nullptr);
	vkFreeMemory(device, bm, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	std::puts("direct blocks GPU: ok");
}
