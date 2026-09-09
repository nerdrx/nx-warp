// Headless Vulkan cost probe for the WiVRn NX client's "Low poly" display filter.
//
// Why this exists: the filter lives in the client's reprojection pass, and that pass
// only runs while an OpenXR session is in a rendering state, which needs the headset
// worn. This runs the same fragment shader on the same GPU with nothing else attached:
// one fullscreen draw over a sampled image, timestamped, with and without the kernel
// baked in through the same specialization constant the client uses. The difference
// between the two is the filter's cost, and every part of the harness that is not the
// filter cancels out of that difference.
//
// It measures a FRAME PAIR two ways at the same 2.37 Mpx: two draws of 1088x1088 (the
// client's per-eye display pass, one draw per eye, which is what it actually does) and
// one draw of 2176x1088.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#define VKC(x)                                                              \
	do {                                                                \
		VkResult r_ = (x);                                          \
		if (r_ != VK_SUCCESS) {                                     \
			fprintf(stderr, "%s:%d %s -> %d\n", __FILE__,       \
			        __LINE__, #x, int(r_));                     \
			exit(1);                                            \
		}                                                           \
	} while (0)

namespace {

constexpr uint32_t kSrc = 2176;  // the decoded per-eye picture the taps step through

VkInstance inst;
VkPhysicalDevice phys;
VkDevice dev;
VkQueue queue;
uint32_t qfam;
VkPhysicalDeviceProperties props;
VkPhysicalDeviceMemoryProperties memprops;

uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
		if ((bits & (1u << i)) &&
		    (memprops.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(1);
}

std::vector<uint32_t> load_spv(const char * path)
{
	FILE * f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint32_t> v(n / 4);
	if (fread(v.data(), 1, n, f) != size_t(n)) { fprintf(stderr, "read %s\n", path); exit(1); }
	fclose(f);
	return v;
}

VkShaderModule make_module(const std::vector<uint32_t> & code)
{
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = code.size() * 4;
	ci.pCode = code.data();
	VkShaderModule m;
	VKC(vkCreateShaderModule(dev, &ci, nullptr, &m));
	return m;
}

struct Push
{
	int32_t rgb_rect[4];
	float deband[4];
};

} // namespace

int main(int argc, char ** argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 200;
	int reps = argc > 2 ? atoi(argv[2]) : 7;
	float levels = argc > 3 ? float(atof(argv[3])) : 0.0f;

	// ---- instance / device -------------------------------------------------
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	app.pApplicationName = "nx-lowpoly-probe";
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VKC(vkCreateInstance(&ici, nullptr, &inst));

	uint32_t n = 0;
	VKC(vkEnumeratePhysicalDevices(inst, &n, nullptr));
	std::vector<VkPhysicalDevice> pds(n);
	VKC(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
	if (!n) { fprintf(stderr, "no vulkan device\n"); return 77; }
	phys = pds[0];
	vkGetPhysicalDeviceProperties(phys, &props);
	vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qfs.data());
	qfam = UINT32_MAX;
	for (uint32_t i = 0; i < qn; ++i)
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
	if (qfam == UINT32_MAX) { fprintf(stderr, "no graphics queue\n"); return 77; }
	if (qfs[qfam].timestampValidBits == 0) {
		fprintf(stderr, "queue has no valid timestamp bits\n");
		return 77;
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	dq.queueFamilyIndex = qfam;
	dq.queueCount = 1;
	dq.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &dq;
	VKC(vkCreateDevice(phys, &dci, nullptr, &dev));
	vkGetDeviceQueue(dev, qfam, 0, &queue);

	printf("device: %s, timestampPeriod %.3f ns\n", props.deviceName,
	       props.limits.timestampPeriod);

	// ---- source image ------------------------------------------------------
	VkImage src;
	VkDeviceMemory srcmem;
	{
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_R8G8B8A8_UNORM;
		ii.extent = {kSrc, kSrc, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKC(vkCreateImage(dev, &ii, nullptr, &src));
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(dev, src, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(vkAllocateMemory(dev, &ai, nullptr, &srcmem));
		VKC(vkBindImageMemory(dev, src, srcmem, 0));
	}

	// Upload the decoded QP 40 frame if it is next to us, otherwise a deterministic
	// pattern. Timing does not depend on the content (no data-dependent taps and no
	// divergent branches), but a real frame keeps the sampler's job honest.
	VkBuffer stage;
	VkDeviceMemory stagemem;
	{
		VkDeviceSize sz = VkDeviceSize(kSrc) * kSrc * 4;
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = sz;
		bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VKC(vkCreateBuffer(dev, &bi, nullptr, &stage));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, stage, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits,
		                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VKC(vkAllocateMemory(dev, &ai, nullptr, &stagemem));
		VKC(vkBindBufferMemory(dev, stage, stagemem, 0));
		void * p = nullptr;
		VKC(vkMapMemory(dev, stagemem, 0, sz, 0, &p));
		uint8_t * px = (uint8_t *)p;
		FILE * f = fopen("frame.rgba", "rb");
		if (f && fread(px, 1, sz, f) == sz) {
			printf("source: frame.rgba\n");
		} else {
			for (uint32_t y = 0; y < kSrc; ++y)
				for (uint32_t x = 0; x < kSrc; ++x) {
					uint32_t i = (y * kSrc + x) * 4;
					uint32_t h = (x * 1103515245u + y * 12345u);
					h ^= h >> 13;
					px[i + 0] = uint8_t(h);
					px[i + 1] = uint8_t(h >> 8);
					px[i + 2] = uint8_t(h >> 16);
					px[i + 3] = 255;
				}
			printf("source: synthetic\n");
		}
		if (f) fclose(f);
		vkUnmapMemory(dev, stagemem);
	}

	VkCommandPool pool;
	{
		VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		ci.queueFamilyIndex = qfam;
		ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VKC(vkCreateCommandPool(dev, &ci, nullptr, &pool));
	}
	auto one_shot = [&](auto && rec) {
		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		VkCommandBuffer cb;
		VKC(vkAllocateCommandBuffers(dev, &ai, &cb));
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VKC(vkBeginCommandBuffer(cb, &bi));
		rec(cb);
		VKC(vkEndCommandBuffer(cb));
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
		VKC(vkQueueWaitIdle(queue));
		vkFreeCommandBuffers(dev, pool, 1, &cb);
	};

	one_shot([&](VkCommandBuffer cb) {
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = src;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
		                     nullptr, 1, &b);
		VkBufferImageCopy r{};
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageExtent = {kSrc, kSrc, 1};
		vkCmdCopyBufferToImage(cb, stage, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
		                     0, nullptr, 1, &b);
	});

	VkImageView srcview;
	{
		VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		ci.image = src;
		ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ci.format = VK_FORMAT_R8G8B8A8_UNORM;
		ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VKC(vkCreateImageView(dev, &ci, nullptr, &srcview));
	}
	VkSampler samp;
	{
		VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
		ci.addressModeU = ci.addressModeV = ci.addressModeW =
		        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		ci.maxLod = 0.25f;
		VKC(vkCreateSampler(dev, &ci, nullptr, &samp));
	}

	// ---- descriptors, render pass, pipelines -------------------------------
	VkDescriptorSetLayout dsl;
	{
		VkDescriptorSetLayoutBinding b{};
		b.binding = 0;
		b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		ci.bindingCount = 1;
		ci.pBindings = &b;
		VKC(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &dsl));
	}
	VkDescriptorPool dpool;
	{
		VkDescriptorPoolSize s{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
		VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		ci.maxSets = 1;
		ci.poolSizeCount = 1;
		ci.pPoolSizes = &s;
		VKC(vkCreateDescriptorPool(dev, &ci, nullptr, &dpool));
	}
	VkDescriptorSet ds;
	{
		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = dpool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &dsl;
		VKC(vkAllocateDescriptorSets(dev, &ai, &ds));
		VkDescriptorImageInfo di{samp, srcview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = ds;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &di;
		vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
	}
	VkPipelineLayout playout;
	{
		VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
		VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		ci.setLayoutCount = 1;
		ci.pSetLayouts = &dsl;
		ci.pushConstantRangeCount = 1;
		ci.pPushConstantRanges = &pcr;
		VKC(vkCreatePipelineLayout(dev, &ci, nullptr, &playout));
	}
	VkRenderPass rp;
	{
		VkAttachmentDescription a{};
		a.format = VK_FORMAT_R8G8B8A8_UNORM;
		a.samples = VK_SAMPLE_COUNT_1_BIT;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sp{};
		sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sp.colorAttachmentCount = 1;
		sp.pColorAttachments = &ar;
		VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		ci.attachmentCount = 1;
		ci.pAttachments = &a;
		ci.subpassCount = 1;
		ci.pSubpasses = &sp;
		VKC(vkCreateRenderPass(dev, &ci, nullptr, &rp));
	}

	auto vs = make_module(load_spv("probe.vert.spv"));
	auto fs = make_module(load_spv("probe.frag.spv"));

	auto make_pipeline = [&](bool lowpoly, int32_t variant) {
		struct Spec { VkBool32 on; int32_t variant; } sv{lowpoly ? VK_TRUE : VK_FALSE, variant};
		VkSpecializationMapEntry me[2]{{0, offsetof(Spec, on), sizeof(VkBool32)},
		                               {1, offsetof(Spec, variant), sizeof(int32_t)}};
		VkSpecializationInfo si{2, me, sizeof(Spec), &sv};
		VkPipelineShaderStageCreateInfo st[2]{};
		st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		st[0].module = vs;
		st[0].pName = "main";
		st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		st[1].module = fs;
		st[1].pName = "main";
		st[1].pSpecializationInfo = &si;

		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &cba;
		VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dsi.dynamicStateCount = 2;
		dsi.pDynamicStates = dyn;

		VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		ci.stageCount = 2;
		ci.pStages = st;
		ci.pVertexInputState = &vi;
		ci.pInputAssemblyState = &ia;
		ci.pViewportState = &vp;
		ci.pRasterizationState = &rs;
		ci.pMultisampleState = &ms;
		ci.pColorBlendState = &cb;
		ci.pDynamicState = &dsi;
		ci.layout = playout;
		ci.renderPass = rp;
		VkPipeline p;
		VKC(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
		return p;
	};
	VkPipeline pipe_base = make_pipeline(false, 0);
	VkPipeline pipe_arr = make_pipeline(true, 0);
	VkPipeline pipe_ref = make_pipeline(true, 1);
	VkPipeline pipe_2x2 = make_pipeline(true, 2);
	VkPipeline pipe_3x3 = make_pipeline(true, 3);

	VkQueryPool qpool;
	{
		VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		ci.queryCount = 2;
		VKC(vkCreateQueryPool(dev, &ci, nullptr, &qpool));
	}

	// ---- one measurement ---------------------------------------------------
	struct Target
	{
		const char * name;
		uint32_t w, h, draws;
	};
	// Both are the same 2.37 Mpx frame pair: the client's actual shape (one draw per
	// eye at the per-eye display size) and a single pair-wide draw.
	Target targets[] = {
	        {"2 x 2160x2160", 4320, 2160, 2},

	};

	auto measure = [&](const Target & t, VkPipeline pipe) -> double {
		// Target image and framebuffer at this size.
		VkImage dst;
		VkDeviceMemory dstmem;
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_R8G8B8A8_UNORM;
		ii.extent = {t.w, t.h, 1};
		ii.mipLevels = ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		VKC(vkCreateImage(dev, &ii, nullptr, &dst));
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(dev, dst, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(vkAllocateMemory(dev, &ai, nullptr, &dstmem));
		VKC(vkBindImageMemory(dev, dst, dstmem, 0));
		VkImageView dv;
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = dst;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VKC(vkCreateImageView(dev, &vi, nullptr, &dv));
		VkFramebuffer fb;
		VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fi.renderPass = rp;
		fi.attachmentCount = 1;
		fi.pAttachments = &dv;
		fi.width = t.w;
		fi.height = t.h;
		fi.layers = 1;
		VKC(vkCreateFramebuffer(dev, &fi, nullptr, &fb));

		Push push{};
		push.rgb_rect[2] = int32_t(kSrc);
		push.rgb_rect[3] = int32_t(kSrc);
		push.deband[1] = 0.85f;   // low poly strength
		push.deband[2] = levels;  // posterise levels

		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VkCommandBuffer cb;
		VKC(vkAllocateCommandBuffers(dev, &cai, &cb));

		// Adreno compiles the pipeline's real binary on first use, and that shows up
		// as a first draw hundreds of times the steady-state cost. One untimed
		// warm-up submit of a single iteration takes it out of every measurement.
		double best = 1e30;
		for (int rep = -1; rep < reps; ++rep) {
			int this_iters = rep < 0 ? 1 : iters;
			VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VKC(vkBeginCommandBuffer(cb, &bi));
			vkCmdResetQueryPool(cb, qpool, 0, 2);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
			for (int it = 0; it < this_iters; ++it) {
				VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
				rbi.renderPass = rp;
				rbi.framebuffer = fb;
				rbi.renderArea = {{0, 0}, {t.w, t.h}};
				vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
				                        playout, 0, 1, &ds, 0, nullptr);
				vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				                   sizeof(Push), &push);
				// t.draws draws covering the target: one per eye, or one
				// for the pair. Each covers its own half.
				for (uint32_t d = 0; d < t.draws; ++d) {
					float dw = float(t.w) / float(t.draws);
					VkViewport vpt{dw * d, 0, dw, float(t.h), 0, 1};
					VkRect2D sc{{int32_t(dw * d), 0},
					            {uint32_t(dw), t.h}};
					vkCmdSetViewport(cb, 0, 1, &vpt);
					vkCmdSetScissor(cb, 0, 1, &sc);
					vkCmdDraw(cb, 3, 1, 0, 0);
				}
				vkCmdEndRenderPass(cb);
			}
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
			VKC(vkEndCommandBuffer(cb));

			VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cb;
			VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
			VKC(vkQueueWaitIdle(queue));

			uint64_t ts[2];
			VKC(vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
			                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
			if (rep < 0)
				continue;
			double ms = double(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6 /
			            double(iters);
			// The best of several repetitions: the worst are thermal and
			// scheduler noise, the best is the cost when the GPU is actually
			// running the work.
			best = std::min(best, ms);
		}

		vkFreeCommandBuffers(dev, pool, 1, &cb);
		vkDestroyFramebuffer(dev, fb, nullptr);
		vkDestroyImageView(dev, dv, nullptr);
		vkDestroyImage(dev, dst, nullptr);
		vkFreeMemory(dev, dstmem, nullptr);
		return best;
	};

 for(const auto &t:targets) {
  for(int run=0;run<4;++run) {
   double a=measure(t,run%2 ? pipe_3x3:pipe_2x2);
   double b=measure(t,run%2 ? pipe_2x2:pipe_3x3);
   printf("run=%d old_ms=%.6f approximate_ms=%.6f\n",run,run%2?b:a,run%2?a:b);
   fflush(stdout);
  }
 }

	vkDeviceWaitIdle(dev);
	return 0;
}
