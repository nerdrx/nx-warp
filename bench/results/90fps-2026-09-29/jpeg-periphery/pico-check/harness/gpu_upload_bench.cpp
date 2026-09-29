#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kSource = 544, kTarget = 2176, kWarmup = 12, kFrames = 100;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkDeviceSize kSourceBytes = VkDeviceSize(kSource) * kSource * 4;

void check(VkResult r, const char * what)
{
	if (r != VK_SUCCESS)
		throw std::runtime_error(std::string(what) + " failed: " + std::to_string(r));
}

std::vector<uint32_t> read_spv(const char * path)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f)
		throw std::runtime_error(std::string("cannot open ") + path);
	const auto n = f.tellg();
	if (n <= 0 || n % 4)
		throw std::runtime_error(std::string("invalid SPIR-V: ") + path);
	std::vector<uint32_t> words(size_t(n) / 4);
	f.seekg(0);
	f.read(reinterpret_cast<char *>(words.data()), n);
	if (!f)
		throw std::runtime_error(std::string("cannot read ") + path);
	return words;
}

uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties p{};
	vkGetPhysicalDeviceMemoryProperties(gpu, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	throw std::runtime_error("required Vulkan memory type unavailable");
}

struct buffer
{
	VkBuffer handle{};
	VkDeviceMemory memory{};
	void * mapped{};
};

buffer make_buffer(VkDevice d, VkPhysicalDevice g, VkDeviceSize size, VkBufferUsageFlags usage)
{
	buffer b;
	VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = size;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	check(vkCreateBuffer(d, &ci, nullptr, &b.handle), "create buffer");
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(d, b.handle, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memory_type(g, req.memoryTypeBits,
	        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	check(vkAllocateMemory(d, &ai, nullptr, &b.memory), "allocate buffer memory");
	check(vkBindBufferMemory(d, b.handle, b.memory, 0), "bind buffer memory");
	check(vkMapMemory(d, b.memory, 0, size, 0, &b.mapped), "map buffer");
	return b;
}

struct image
{
	VkImage handle{};
	VkDeviceMemory memory{};
	VkImageView view{};
	VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

image make_image(VkDevice d, VkPhysicalDevice g, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
	image out;
	VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = kFormat;
	ci.extent = {w, h, 1};
	ci.mipLevels = ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	check(vkCreateImage(d, &ci, nullptr, &out.handle), "create image");
	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(d, out.handle, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check(vkAllocateMemory(d, &ai, nullptr, &out.memory), "allocate image memory");
	check(vkBindImageMemory(d, out.handle, out.memory, 0), "bind image memory");
	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = out.handle;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = kFormat;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	check(vkCreateImageView(d, &vi, nullptr, &out.view), "create image view");
	return out;
}

void image_barrier(VkCommandBuffer cmd, image & img, VkImageLayout next,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                   VkAccessFlags src_access, VkAccessFlags dst_access)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = img.layout;
	b.newLayout = next;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img.handle;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
	img.layout = next;
}

void fill_sources(uint8_t * p)
{
	for (uint32_t eye = 0; eye < 2; ++eye)
		for (uint32_t y = 0; y < kSource; ++y)
			for (uint32_t x = 0; x < kSource; ++x)
			{
				const size_t i = (size_t(eye) * kSource * kSource + size_t(y) * kSource + x) * 4;
				p[i + 0] = uint8_t((x * 37 + y * 13 + eye * 71) & 255);
				p[i + 1] = uint8_t((x * 7 + y * 43 + eye * 29) & 255);
				p[i + 2] = uint8_t(((x ^ y) * 19 + eye * 113) & 255);
				p[i + 3] = 255;
			}
}

uint8_t bilinear(const uint8_t * rgba, uint32_t x, uint32_t y, uint32_t c)
{
	const double sx = (double(x) + 0.5) * kSource / kTarget - 0.5;
	const double sy = (double(y) + 0.5) * kSource / kTarget - 0.5;
	const double fx = std::max(0.0, sx), fy = std::max(0.0, sy);
	const uint32_t x0 = std::min(uint32_t(fx), kSource - 1), y0 = std::min(uint32_t(fy), kSource - 1);
	const uint32_t x1 = std::min(x0 + 1, kSource - 1), y1 = std::min(y0 + 1, kSource - 1);
	const double ax = std::clamp(fx - x0, 0.0, 1.0), ay = std::clamp(fy - y0, 0.0, 1.0);
	auto at = [&](uint32_t px, uint32_t py) { return rgba[(size_t(py) * kSource + px) * 4 + c]; };
	const double a = at(x0, y0) * (1.0 - ax) + at(x1, y0) * ax;
	const double b = at(x0, y1) * (1.0 - ax) + at(x1, y1) * ax;
	return uint8_t(std::lround(a * (1.0 - ay) + b * ay));
}

} // namespace

int main(int argc, char ** argv)
try
{
	if (argc != 3)
	{
		std::fprintf(stderr, "usage: %s sample.vert.spv sample.frag.spv\n", argv[0]);
		return 2;
	}
	const auto vert_spv = read_spv(argv[1]), frag_spv = read_spv(argv[2]);
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "jpeg-periphery-gpu-upload-bench";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance instance{};
	check(vkCreateInstance(&ici, nullptr, &instance), "create instance");
	uint32_t count = 0;
	check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
	if (!count) throw std::runtime_error("no Vulkan physical device");
	std::vector<VkPhysicalDevice> gpus(count);
	check(vkEnumeratePhysicalDevices(instance, &count, gpus.data()), "get devices");
	VkPhysicalDevice gpu = gpus[0];
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(gpu, &props);
	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
	uint32_t family = 0;
	while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
	if (family == family_count) throw std::runtime_error("no graphics queue");
	float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
	VkDevice device{};
	check(vkCreateDevice(gpu, &dci, nullptr, &device), "create device");
	VkQueue queue{};
	vkGetDeviceQueue(device, family, 0, &queue);

	const bool timestamps = families[family].timestampValidBits != 0;
	const uint32_t frame_count = kWarmup + kFrames;
	VkQueryPool queries{};
	if (timestamps)
	{
		VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qp.queryCount = frame_count * 8;
		check(vkCreateQueryPool(device, &qp, nullptr, &queries), "create timestamp pool");
	}
	buffer staging = make_buffer(device, gpu, 2 * kSourceBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	fill_sources(static_cast<uint8_t *>(staging.mapped));
	std::array<image, 2> source{
	        make_image(device, gpu, kSource, kSource, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
	        make_image(device, gpu, kSource, kSource, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)};
	std::array<image, 2> target{
	        make_image(device, gpu, kTarget, kTarget, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
	        make_image(device, gpu, kTarget, kTarget, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)};
	buffer readback = make_buffer(device, gpu, 2 * VkDeviceSize(kTarget) * kTarget * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler sampler{};
	check(vkCreateSampler(device, &sci, nullptr, &sampler), "create linear sampler");
	VkDescriptorSetLayoutBinding db{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo dsci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dsci.bindingCount = 1; dsci.pBindings = &db;
	VkDescriptorSetLayout dsl{};
	check(vkCreateDescriptorSetLayout(device, &dsci, nullptr, &dsl), "create descriptor layout");
	VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 2; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dps;
	VkDescriptorPool pool{};
	check(vkCreateDescriptorPool(device, &dpci, nullptr, &pool), "create descriptor pool");
	std::array<VkDescriptorSetLayout, 2> layouts{dsl, dsl};
	std::array<VkDescriptorSet, 2> sets{};
	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = pool; dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts.data();
	check(vkAllocateDescriptorSets(device, &dsai, sets.data()), "allocate descriptors");
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		VkDescriptorImageInfo ii{sampler, source[eye].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = sets[eye]; wr.dstBinding = 0; wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
		vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
	}

	VkAttachmentDescription attachment{};
	attachment.format = kFormat; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference aref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1; sub.pColorAttachments = &aref;
	VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = 1; rpci.pAttachments = &attachment; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
	VkRenderPass render_pass{};
	check(vkCreateRenderPass(device, &rpci, nullptr, &render_pass), "create render pass");
	std::array<VkFramebuffer, 2> framebuffers{};
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fi.renderPass = render_pass; fi.attachmentCount = 1; fi.pAttachments = &target[eye].view;
		fi.width = fi.height = kTarget; fi.layers = 1;
		check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffers[eye]), "create framebuffer");
	}
	VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	VkShaderModule vert{}, frag{};
	smci.codeSize = vert_spv.size() * 4; smci.pCode = vert_spv.data();
	check(vkCreateShaderModule(device, &smci, nullptr, &vert), "create vertex shader");
	smci.codeSize = frag_spv.size() * 4; smci.pCode = frag_spv.data();
	check(vkCreateShaderModule(device, &smci, nullptr, &frag), "create fragment shader");
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
	VkPipelineLayout pipeline_layout{};
	check(vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout), "create pipeline layout");
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
	stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport viewport{0, 0, float(kTarget), float(kTarget), 0, 1};
	VkRect2D scissor{{0, 0}, {kTarget, kTarget}};
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.pViewports = &viewport; vp.scissorCount = 1; vp.pScissors = &scissor;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xf;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
	VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
	gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.layout = pipeline_layout; gp.renderPass = render_pass;
	VkPipeline pipeline{};
	check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline), "create graphics pipeline");
	VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = family;
	VkCommandPool command_pool{};
	check(vkCreateCommandPool(device, &cpci, nullptr, &command_pool), "create command pool");
	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = command_pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
	VkCommandBuffer cmd{};
	check(vkAllocateCommandBuffers(device, &cbai, &cmd), "allocate command buffer");
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence{};
	check(vkCreateFence(device, &fci, nullptr, &fence), "create fence");
	std::vector<double> wall_ms, upload_ms, sample_ms;
	wall_ms.reserve(kFrames); upload_ms.reserve(kFrames); sample_ms.reserve(kFrames);
	const auto cpu_data = static_cast<const uint8_t *>(staging.mapped);
	for (uint32_t frame = 0; frame < frame_count; ++frame)
	{
		check(vkResetCommandBuffer(cmd, 0), "reset command buffer");
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		check(vkBeginCommandBuffer(cmd, &bi), "begin command buffer");
		for (uint32_t eye = 0; eye < 2; ++eye)
		{
			const uint32_t q = frame * 8 + eye * 4;
			if (timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, q);
			image_barrier(cmd, source[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        source[eye].layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			        VK_PIPELINE_STAGE_TRANSFER_BIT,
			        source[eye].layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
			        VK_ACCESS_TRANSFER_WRITE_BIT);
			VkBufferImageCopy copy{}; copy.bufferOffset = eye * kSourceBytes;
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {kSource, kSource, 1};
			vkCmdCopyBufferToImage(cmd, staging.handle, source[eye].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			image_barrier(cmd, source[eye], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
			if (timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, q + 1);
			if (timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, q + 2);
			if (target[eye].layout == VK_IMAGE_LAYOUT_UNDEFINED)
				image_barrier(cmd, target[eye], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
			VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rb.renderPass = render_pass; rb.framebuffer = framebuffers[eye]; rb.renderArea = {{0, 0}, {kTarget, kTarget}};
			vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &sets[eye], 0, nullptr);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
			target[eye].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			if (timestamps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, q + 3);
		}
		check(vkEndCommandBuffer(cmd), "end command buffer");
		check(vkResetFences(device, 1, &fence), "reset fence");
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
		const auto start = std::chrono::steady_clock::now();
		check(vkQueueSubmit(queue, 1, &si, fence), "submit frame");
		check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait frame");
		const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (frame >= kWarmup)
		{
			wall_ms.push_back(elapsed);
			if (timestamps)
			{
				std::array<uint64_t, 8> q{};
				check(vkGetQueryPoolResults(device, queries, frame * 8, 8, sizeof(q), q.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT), "read timestamps");
				double up = 0, render = 0;
				for (uint32_t eye = 0; eye < 2; ++eye)
				{
					up += double(q[eye * 4 + 1] - q[eye * 4]) * props.limits.timestampPeriod / 1e6;
					render += double(q[eye * 4 + 3] - q[eye * 4 + 2]) * props.limits.timestampPeriod / 1e6;
				}
				upload_ms.push_back(up); sample_ms.push_back(render);
			}
		}
	}

	check(vkResetCommandBuffer(cmd, 0), "reset readback command");
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check(vkBeginCommandBuffer(cmd, &bi), "begin readback command");
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		image_barrier(cmd, target[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferImageCopy cp{}; cp.bufferOffset = VkDeviceSize(eye) * kTarget * kTarget * 4;
		cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.imageExtent = {kTarget, kTarget, 1};
		vkCmdCopyImageToBuffer(cmd, target[eye].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &cp);
	}
	VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	check(vkEndCommandBuffer(cmd), "end readback command");
	check(vkResetFences(device, 1, &fence), "reset readback fence");
	VkSubmitInfo read_si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; read_si.commandBufferCount = 1; read_si.pCommandBuffers = &cmd;
	check(vkQueueSubmit(queue, 1, &read_si, fence), "submit readback");
	check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait readback");

	const auto * pixels = static_cast<const uint8_t *>(readback.mapped);
	uint64_t checksum = 1469598103934665603ull;
	uint32_t mismatches = 0, max_delta = 0;
	for (uint32_t eye = 0; eye < 2; ++eye)
		for (uint32_t y = 0; y < kTarget; ++y)
			for (uint32_t x = 0; x < kTarget; ++x)
				for (uint32_t c = 0; c < 4; ++c)
				{
					const uint8_t got = pixels[(size_t(eye) * kTarget * kTarget + size_t(y) * kTarget + x) * 4 + c];
					const uint8_t expected = bilinear(cpu_data + size_t(eye) * kSourceBytes, x, y, c);
					const uint32_t delta = uint32_t(std::abs(int(got) - int(expected)));
					max_delta = std::max(max_delta, delta);
					if (delta > 2) ++mismatches;
					checksum = (checksum ^ got) * 1099511628211ull;
				}
	auto percentile = [](std::vector<double> v, double p) {
			if (v.empty()) return 0.0;
			std::sort(v.begin(), v.end());
			return v[size_t(std::ceil(p * v.size())) - 1];
	};
	std::printf("device=%s source=%ux%u_stereo_rgba_bytes=%llu target=%ux%u_per_eye frames=%u warmup=%u\n",
	        props.deviceName, kSource, kSource,
	        static_cast<unsigned long long>(2 * kSourceBytes), kTarget, kTarget, kFrames, kWarmup);
	std::printf("fence_ms_p50=%.3f p95=%.3f p99=%.3f gpu_timestamps=%s upload_pair_ms_p50=%.3f p95=%.3f p99=%.3f render_pair_ms_p50=%.3f p95=%.3f p99=%.3f\n",
	        percentile(wall_ms, .50), percentile(wall_ms, .95), percentile(wall_ms, .99), timestamps ? "yes" : "no",
	        percentile(upload_ms, .50), percentile(upload_ms, .95), percentile(upload_ms, .99),
	        percentile(sample_ms, .50), percentile(sample_ms, .95), percentile(sample_ms, .99));
	std::printf("checksum=%016llx channel_errors_over_2=%u max_channel_delta=%u\n",
	        static_cast<unsigned long long>(checksum), mismatches, max_delta);
	return mismatches ? 1 : 0;
}
catch (const std::exception & e)
{
	std::fprintf(stderr, "benchmark error: %s\n", e.what());
	return 2;
}
