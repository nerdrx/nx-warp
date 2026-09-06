// nxtexnative -- can the Pico 4 sample hardware-compressed texture blocks as an
// NX Warp atlas, and what does the display pass cost when it does?
//
// Two questions, two modes:
//
//   --formats   vkGetPhysicalDeviceFormatProperties for every candidate atlas
//               format, reporting whether OPTIMAL tiling offers SAMPLED_IMAGE
//               and, the one that decides the idea, SAMPLED_IMAGE_FILTER_LINEAR.
//               Without linear filtering the sampler cannot do the bilinear tap
//               the display warp needs and the whole patch source is dead.
//
//   --bench     ADR-0029 section 5's display pass -- one warp step from the
//               atlas, per-tile homography from a table, bilinear by the
//               sampler -- over a 2176x1088 atlas (both eyes), writing
//               2176x1088 of output, which is 1088x1088 per eye, i.e. one
//               displayed frame PAIR. The shader is byte-identical between
//               runs; only the VkFormat of the atlas changes. So the delta is
//               the format's sampling cost and nothing else.
//
// Standalone and headless: no window, no app, no surface, no swapchain, no
// MediaCodec. It does not touch wivrn-server or any installed package. It reads
// a .spv beside itself and, optionally, an atlas payload file.
//
// Pattern follows wivrn-nx-hybrid/hybrid-proto/src/nxahbvk.c.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define TILE 64
#define VKC(x)                                                                   \
	do {                                                                         \
		VkResult _r = (x);                                                       \
		if (_r != VK_SUCCESS)                                                    \
		{                                                                        \
			fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, _r);     \
			exit(5);                                                             \
		}                                                                        \
	} while (0)

static VkInstance vki;
static VkPhysicalDevice vkpd;
static VkDevice vkd;
static VkQueue vkq;
static uint32_t qfam;
static float ts_period;
static VkQueryPool qpool;
static VkCommandPool cpool;
static VkCommandBuffer cmd;
static VkFence fence;
static int g_strips = 0;   // --strips: copy as full-width 64-row strips instead of per-tile regions

static int cmpd(const void * a, const void * b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

// ------------------------------------------------------------------ format table

struct fmt {
	VkFormat f;
	const char * name;
	int bw, bh;      // block extent in texels (1x1 = uncompressed)
	int bytes;       // bytes per block
};

static const struct fmt formats[] = {
        {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4_UNORM", 4, 4, 16},
        {VK_FORMAT_ASTC_4x4_SRGB_BLOCK, "ASTC_4x4_SRGB", 4, 4, 16},
        {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, "ASTC_6x6_UNORM", 6, 6, 16},
        {VK_FORMAT_ASTC_6x6_SRGB_BLOCK, "ASTC_6x6_SRGB", 6, 6, 16},
        {VK_FORMAT_ASTC_8x8_UNORM_BLOCK, "ASTC_8x8_UNORM", 8, 8, 16},
        {VK_FORMAT_ASTC_8x8_SRGB_BLOCK, "ASTC_8x8_SRGB", 8, 8, 16},
        {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, "ETC2_R8G8B8_UNORM", 4, 4, 8},
        {VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, "ETC2_R8G8B8_SRGB", 4, 4, 8},
        {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, "ETC2_R8G8B8A1_UNORM", 4, 4, 8},
        {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_R8G8B8A8_UNORM", 4, 4, 16},
        {VK_FORMAT_EAC_R11_UNORM_BLOCK, "EAC_R11_UNORM", 4, 4, 8},
        {VK_FORMAT_EAC_R11G11_UNORM_BLOCK, "EAC_R11G11_UNORM", 4, 4, 16},
        {VK_FORMAT_R8_UNORM, "R8_UNORM", 1, 1, 1},
        {VK_FORMAT_R16_UNORM, "R16_UNORM", 1, 1, 2},
        {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM", 1, 1, 2},
        {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 1, 1, 4},
};
#define NFMT ((int)(sizeof formats / sizeof *formats))

static const struct fmt * find_fmt(const char * n)
{
	for (int i = 0; i < NFMT; i++)
		if (!strcmp(formats[i].name, n))
			return &formats[i];
	return NULL;
}

static size_t fmt_bytes(const struct fmt * f, int w, int h)
{
	size_t bx = (size_t)((w + f->bw - 1) / f->bw);
	size_t by = (size_t)((h + f->bh - 1) / f->bh);
	return bx * by * (size_t)f->bytes;
}

// ------------------------------------------------------------------ vulkan setup

static void vk_init(void)
{
	VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	                        .pApplicationName = "nxtexnative",
	                        .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	                            .pApplicationInfo = &ai};
	VKC(vkCreateInstance(&ici, NULL, &vki));

	uint32_t n = 0;
	vkEnumeratePhysicalDevices(vki, &n, NULL);
	VkPhysicalDevice pds[8];
	if (n > 8)
		n = 8;
	if (n == 0)
	{
		fprintf(stderr, "no vulkan physical device\n");
		exit(5);
	}
	vkEnumeratePhysicalDevices(vki, &n, pds);
	vkpd = pds[0];

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(vkpd, &props);
	ts_period = props.limits.timestampPeriod;
	printf("device        %s\n", props.deviceName);
	printf("api           %u.%u.%u\n", VK_VERSION_MAJOR(props.apiVersion),
	       VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	printf("driver        0x%08x\n", props.driverVersion);
	printf("tsPeriod      %.3f ns\n", ts_period);

	VkPhysicalDeviceFeatures feat;
	vkGetPhysicalDeviceFeatures(vkpd, &feat);
	printf("feat.textureCompressionASTC_LDR  %d\n", feat.textureCompressionASTC_LDR);
	printf("feat.textureCompressionETC2      %d\n", feat.textureCompressionETC2);
	printf("feat.textureCompressionBC        %d\n", feat.textureCompressionBC);

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(vkpd, &qn, NULL);
	VkQueueFamilyProperties qs[8];
	if (qn > 8)
		qn = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(vkpd, &qn, qs);
	qfam = 0;
	for (uint32_t i = 0; i < qn; i++)
		if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			qfam = i;
			break;
		}
	printf("queueFamily   %u  timestampValidBits %u\n", qfam, qs[qfam].timestampValidBits);

	float prio = 1.f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = qfam,
	                               .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkPhysicalDeviceFeatures want = {
	        .textureCompressionASTC_LDR = feat.textureCompressionASTC_LDR,
	        .textureCompressionETC2 = feat.textureCompressionETC2,
	};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	                          .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos = &qci,
	                          .pEnabledFeatures = &want};
	VKC(vkCreateDevice(vkpd, &dci, NULL, &vkd));
	vkGetDeviceQueue(vkd, qfam, 0, &vkq);

	VkQueryPoolCreateInfo qp = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
	                            .queryType = VK_QUERY_TYPE_TIMESTAMP,
	                            .queryCount = 2};
	VKC(vkCreateQueryPool(vkd, &qp, NULL, &qpool));
	VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                              .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                              .queueFamilyIndex = qfam};
	VKC(vkCreateCommandPool(vkd, &cp, NULL, &cpool));
	VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                   .commandPool = cpool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                   .commandBufferCount = 1};
	VKC(vkAllocateCommandBuffers(vkd, &cba, &cmd));
	VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VKC(vkCreateFence(vkd, &fc, NULL, &fence));
}

// ------------------------------------------------------------------ mode: formats

static void mode_formats(void)
{
	printf("\n%-22s %-8s %-8s %-8s %-8s %-8s\n", "format", "opt.SMP", "opt.LIN", "opt.DST",
	       "lin.SMP", "lin.LIN");
	for (int i = 0; i < NFMT; i++)
	{
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(vkpd, formats[i].f, &fp);
		VkFormatFeatureFlags o = fp.optimalTilingFeatures, l = fp.linearTilingFeatures;
		printf("%-22s %-8d %-8d %-8d %-8d %-8d   (opt=0x%08x)\n", formats[i].name,
		       !!(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
		       !!(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
		       !!(o & VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
		       !!(l & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
		       !!(l & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT), o);
	}
	printf("\nlegend: opt.SMP SAMPLED_IMAGE, opt.LIN SAMPLED_IMAGE_FILTER_LINEAR,\n"
	       "        opt.DST TRANSFER_DST, all under VK_IMAGE_TILING_OPTIMAL;\n"
	       "        lin.* the same two under VK_IMAGE_TILING_LINEAR.\n");

	// The image-level question, which is the one that actually binds: can an
	// image of this format, at the atlas size, with SAMPLED usage, be created?
	printf("\n%-22s %s\n", "format", "vkGetPhysicalDeviceImageFormatProperties(SAMPLED, OPTIMAL, 2176x1088)");
	for (int i = 0; i < NFMT; i++)
	{
		VkImageFormatProperties ifp;
		VkResult r = vkGetPhysicalDeviceImageFormatProperties(
		        vkpd, formats[i].f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
		        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ifp);
		if (r == VK_SUCCESS)
			printf("%-22s ok   maxExtent %ux%u  maxMip %u\n", formats[i].name,
			       ifp.maxExtent.width, ifp.maxExtent.height, ifp.maxMipLevels);
		else
			printf("%-22s NOT SUPPORTED (%d)\n", formats[i].name, r);
	}
}

// ------------------------------------------------------------------ mode: bench

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(vkpd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type for bits %x want %x\n", bits, want);
	exit(5);
}

static VkShaderModule load_spv(const char * path)
{
	FILE * f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "cannot open %s\n", path);
		exit(4);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t * buf = malloc((size_t)n);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n)
		exit(4);
	fclose(f);
	VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                               .codeSize = (size_t)n,
	                               .pCode = buf};
	VkShaderModule m;
	VKC(vkCreateShaderModule(vkd, &ci, NULL, &m));
	free(buf);
	return m;
}

struct push {
	int32_t outW, outH, atlasW, atlasH, tilesX;
};

static void bench_one(const struct fmt * f, int W, int H, const char * payload, int iters,
                      int warmup, int nearest, double * out_med, double * out_min)
{
	// --- atlas image
	VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                         .imageType = VK_IMAGE_TYPE_2D,
	                         .format = f->f,
	                         .extent = {(uint32_t)W, (uint32_t)H, 1},
	                         .mipLevels = 1,
	                         .arrayLayers = 1,
	                         .samples = VK_SAMPLE_COUNT_1_BIT,
	                         .tiling = VK_IMAGE_TILING_OPTIMAL,
	                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VkImage atlas;
	VKC(vkCreateImage(vkd, &ici, NULL, &atlas));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(vkd, atlas, &mr);
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                            .allocationSize = mr.size,
	                            .memoryTypeIndex = mem_type(mr.memoryTypeBits,
	                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VkDeviceMemory atlasMem;
	VKC(vkAllocateMemory(vkd, &mai, NULL, &atlasMem));
	VKC(vkBindImageMemory(vkd, atlas, atlasMem, 0));
	printf("  atlas VRAM  %.2f MiB (payload %.2f MiB)\n", mr.size / 1048576.0,
	       fmt_bytes(f, W, H) / 1048576.0);

	// --- output storage image, identical for every format so it cancels out
	VkImageCreateInfo oci = ici;
	oci.format = VK_FORMAT_R8G8B8A8_UINT;
	oci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
	VkImage outImg;
	VKC(vkCreateImage(vkd, &oci, NULL, &outImg));
	vkGetImageMemoryRequirements(vkd, outImg, &mr);
	VkMemoryAllocateInfo omai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                             .allocationSize = mr.size,
	                             .memoryTypeIndex = mem_type(mr.memoryTypeBits,
	                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VkDeviceMemory outMem;
	VKC(vkAllocateMemory(vkd, &omai, NULL, &outMem));
	VKC(vkBindImageMemory(vkd, outImg, outMem, 0));

	// --- staging upload of the atlas payload
	size_t nbytes = fmt_bytes(f, W, H);
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                          .size = nbytes,
	                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
	VkBuffer stage;
	VKC(vkCreateBuffer(vkd, &bci, NULL, &stage));
	vkGetBufferMemoryRequirements(vkd, stage, &mr);
	VkMemoryAllocateInfo smai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                             .allocationSize = mr.size,
	                             .memoryTypeIndex = mem_type(mr.memoryTypeBits,
	                                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VkDeviceMemory stageMem;
	VKC(vkAllocateMemory(vkd, &smai, NULL, &stageMem));
	VKC(vkBindBufferMemory(vkd, stage, stageMem, 0));
	void * sp;
	VKC(vkMapMemory(vkd, stageMem, 0, nbytes, 0, &sp));
	if (payload)
	{
		FILE * pf = fopen(payload, "rb");
		if (!pf)
		{
			fprintf(stderr, "cannot open payload %s\n", payload);
			exit(4);
		}
		size_t got = fread(sp, 1, nbytes, pf);
		fclose(pf);
		if (got < nbytes)
		{
			// tile the payload to fill the atlas
			unsigned char * b = sp;
			if (got == 0)
			{
				fprintf(stderr, "payload %s is empty\n", payload);
				exit(4);
			}
			for (size_t o = got; o < nbytes; o += got)
				memcpy(b + o, b, (o + got <= nbytes) ? got : nbytes - o);
			printf("  payload     %s (%zu B, tiled to fill %zu B)\n", payload, got, nbytes);
		}
		else
			printf("  payload     %s (%zu B)\n", payload, nbytes);
	}
	else
	{
		// Deterministic pseudo-random fill. For UNCOMPRESSED formats this is
		// exactly as good as real content. For compressed formats it is NOT
		// content-faithful and the run is labelled as such.
		unsigned char * b = sp;
		uint32_t s = 12345;
		for (size_t i = 0; i < nbytes; i++)
		{
			s = s * 1664525u + 1013904223u;
			b[i] = (unsigned char)(s >> 24);
		}
		printf("  payload     synthetic pseudo-random\n");
	}
	vkUnmapMemory(vkd, stageMem);

	// --- per-tile homography table
	int tilesX = W / TILE, tilesY = H / TILE, nTiles = tilesX * tilesY;
	size_t tbytes = (size_t)nTiles * 3 * 4 * sizeof(float);
	VkBufferCreateInfo tbci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                           .size = tbytes,
	                           .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
	VkBuffer tbuf;
	VKC(vkCreateBuffer(vkd, &tbci, NULL, &tbuf));
	vkGetBufferMemoryRequirements(vkd, tbuf, &mr);
	VkMemoryAllocateInfo tmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                             .allocationSize = mr.size,
	                             .memoryTypeIndex = mem_type(mr.memoryTypeBits,
	                                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VkDeviceMemory tmem;
	VKC(vkAllocateMemory(vkd, &tmai, NULL, &tmem));
	VKC(vkBindBufferMemory(vkd, tbuf, tmem, 0));
	float * tp;
	VKC(vkMapMemory(vkd, tmem, 0, tbytes, 0, (void **)&tp));
	// A plausible display warp: each tile carries a small rotation+translation
	// homography about its own centre, with a mild perspective term, so the
	// taps are neither perfectly aligned nor degenerate -- which is what makes
	// the texture cache behave like it does in the real pass.
	for (int t = 0; t < nTiles; t++)
	{
		int tx = t % tilesX, ty = t / tilesX;
		double cx = tx * TILE + TILE / 2.0, cy = ty * TILE + TILE / 2.0;
		double a = 0.02 + 0.001 * ((t * 37) % 17);   // radians
		double sx = 1.0 + 0.01 * (((t * 11) % 7) - 3);
		double dx = 1.7 * (((t * 13) % 9) - 4), dy = 1.3 * (((t * 7) % 9) - 4);
		double ca = cos(a) * sx, sa = sin(a) * sx;
		double p0 = 1e-6 * (((t * 5) % 5) - 2), p1 = 1e-6 * (((t * 3) % 5) - 2);
		float * m = tp + (size_t)t * 12;
		m[0] = (float)ca;  m[1] = (float)-sa; m[2] = (float)(cx - ca * cx + sa * cy + dx); m[3] = 0;
		m[4] = (float)sa;  m[5] = (float)ca;  m[6] = (float)(cy - sa * cx - ca * cy + dy); m[7] = 0;
		m[8] = (float)p0;  m[9] = (float)p1;  m[10] = 1.0f;                                m[11] = 0;
	}
	vkUnmapMemory(vkd, tmem);

	// --- views and sampler
	VkImageViewCreateInfo avi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                             .image = atlas,
	                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                             .format = f->f,
	                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VkImageView atlasView;
	VKC(vkCreateImageView(vkd, &avi, NULL, &atlasView));
	VkImageViewCreateInfo ovi = avi;
	ovi.image = outImg;
	ovi.format = VK_FORMAT_R8G8B8A8_UINT;
	VkImageView outView;
	VKC(vkCreateImageView(vkd, &ovi, NULL, &outView));

	VkFilter filt = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	                           .magFilter = filt,
	                           .minFilter = filt,
	                           .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	                           .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .maxLod = 0.25f};
	VkSampler samp;
	VKC(vkCreateSampler(vkd, &sci, NULL, &samp));

	// --- pipeline
	VkDescriptorSetLayoutBinding b[3] = {
	        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                         .bindingCount = 3,
	                                         .pBindings = b};
	VkDescriptorSetLayout dsl;
	VKC(vkCreateDescriptorSetLayout(vkd, &dslci, NULL, &dsl));
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct push)};
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                   .setLayoutCount = 1,
	                                   .pSetLayouts = &dsl,
	                                   .pushConstantRangeCount = 1,
	                                   .pPushConstantRanges = &pcr};
	VkPipelineLayout plyt;
	VKC(vkCreatePipelineLayout(vkd, &plci, NULL, &plyt));

	extern char g_spv_path[];
	VkShaderModule sm = load_spv(g_spv_path);
	VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	                                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	                                              .module = sm,
	                                              .pName = "main"},
	                                    .layout = plyt};
	VkPipeline pipe;
	VKC(vkCreateComputePipelines(vkd, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
	VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	                                   .maxSets = 1,
	                                   .poolSizeCount = 3,
	                                   .pPoolSizes = ps};
	VkDescriptorPool dpool;
	VKC(vkCreateDescriptorPool(vkd, &dpci, NULL, &dpool));
	VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                    .descriptorPool = dpool,
	                                    .descriptorSetCount = 1,
	                                    .pSetLayouts = &dsl};
	VkDescriptorSet ds;
	VKC(vkAllocateDescriptorSets(vkd, &dsai, &ds));

	VkDescriptorImageInfo dii = {samp, atlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo doi = {VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL};
	VkDescriptorBufferInfo dbi = {tbuf, 0, tbytes};
	VkWriteDescriptorSet w[3] = {
	        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
	         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	         .pImageInfo = &dii},
	        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
	         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	         .pImageInfo = &doi},
	        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2,
	         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	         .pBufferInfo = &dbi},
	};
	vkUpdateDescriptorSets(vkd, 3, w, 0, NULL);

	// --- upload + layout transitions, once
	VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VKC(vkBeginCommandBuffer(cmd, &cbi));
	VkImageMemoryBarrier mb[2] = {
	        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	         .image = atlas,
	         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	         .image = outImg,
	         .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
	                     NULL, 0, NULL, 2, mb);
	VkBufferImageCopy bic = {.bufferOffset = 0,
	                         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	                         .imageExtent = {(uint32_t)W, (uint32_t)H, 1}};
	vkCmdCopyBufferToImage(cmd, stage, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	VkImageMemoryBarrier mb2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                            .image = atlas,
	                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, NULL, 0, NULL, 1, &mb2);
	VKC(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	VKC(vkQueueSubmit(vkq, 1, &si, fence));
	VKC(vkWaitForFences(vkd, 1, &fence, VK_TRUE, UINT64_MAX));
	VKC(vkResetFences(vkd, 1, &fence));

	// --- timed dispatches
	struct push pc = {W, H, W, H, tilesX};
	double * samples = malloc(sizeof(double) * (size_t)iters);
	for (int it = 0; it < warmup + iters; it++)
	{
		VKC(vkResetCommandBuffer(cmd, 0));
		VKC(vkBeginCommandBuffer(cmd, &cbi));
		vkCmdResetQueryPool(cmd, qpool, 0, 2);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plyt, 0, 1, &ds, 0, NULL);
		vkCmdPushConstants(cmd, plyt, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
		vkCmdDispatch(cmd, (uint32_t)((W + 7) / 8), (uint32_t)((H + 7) / 8), 1);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
		VKC(vkEndCommandBuffer(cmd));
		VKC(vkQueueSubmit(vkq, 1, &si, fence));
		VKC(vkWaitForFences(vkd, 1, &fence, VK_TRUE, UINT64_MAX));
		VKC(vkResetFences(vkd, 1, &fence));
		uint64_t ts[2];
		VKC(vkGetQueryPoolResults(vkd, qpool, 0, 2, sizeof ts, ts, sizeof(uint64_t),
		                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
		if (it >= warmup)
			samples[it - warmup] = (double)(ts[1] - ts[0]) * ts_period / 1e6;
	}
	qsort(samples, (size_t)iters, sizeof(double), cmpd);
	*out_med = samples[iters / 2];
	*out_min = samples[0];
	printf("  GPU ms/pair min %.3f  p50 %.3f  p90 %.3f  max %.3f   (%d iters, %d tiles, %s)\n",
	       samples[0], samples[iters / 2], samples[(int)(iters * 0.9)], samples[iters - 1], iters,
	       nTiles, nearest ? "NEAREST" : "LINEAR");
	free(samples);

	vkDestroyDescriptorPool(vkd, dpool, NULL);
	vkDestroyPipeline(vkd, pipe, NULL);
	vkDestroyShaderModule(vkd, sm, NULL);
	vkDestroyPipelineLayout(vkd, plyt, NULL);
	vkDestroyDescriptorSetLayout(vkd, dsl, NULL);
	vkDestroySampler(vkd, samp, NULL);
	vkDestroyImageView(vkd, outView, NULL);
	vkDestroyImageView(vkd, atlasView, NULL);
	vkDestroyBuffer(vkd, tbuf, NULL);
	vkFreeMemory(vkd, tmem, NULL);
	vkDestroyBuffer(vkd, stage, NULL);
	vkFreeMemory(vkd, stageMem, NULL);
	vkDestroyImage(vkd, outImg, NULL);
	vkFreeMemory(vkd, outMem, NULL);
	vkDestroyImage(vkd, atlas, NULL);
	vkFreeMemory(vkd, atlasMem, NULL);
}

// ------------------------------------------------------------------ mode: update
//
// The other half of the idea: refreshed tiles arrive as compressed blocks and
// are copied into the atlas image. A compressed image can only be a TRANSFER
// destination (no STORAGE_IMAGE bit on any of these formats), so the copy is
// vkCmdCopyBufferToImage and nothing else.
//
// The binding constraint is alignment. Vulkan requires imageOffset to be a
// multiple of the compressed block extent and imageExtent likewise (or to reach
// the image edge). A 64x64 tile grid gives tile origins at multiples of 64:
//
//   4x4  -> 64 = 16 blocks, origins aligned      -> a tile is independently copyable
//   8x8  -> 64 =  8 blocks, origins aligned      -> a tile is independently copyable
//   6x6  -> 64 = 10.67 blocks, origins NOT aligned
//
// For 6x6 this is not merely an API inconvenience. A 6x6 block that straddles a
// tile boundary holds samples belonging to two tile positions, which under
// ADR-0029 have different source poses and different source frames. There is no
// copy that updates one of them without corrupting the other.

static void mode_update(const struct fmt * f, int W, int H, int nTilesUpd, int iters, int warmup)
{
	int alignedX = (TILE % f->bw) == 0, alignedY = (TILE % f->bh) == 0;
	printf("  block %dx%d  tile %d -> %.2f x %.2f blocks  aligned %s\n", f->bw, f->bh, TILE,
	       (double)TILE / f->bw, (double)TILE / f->bh, (alignedX && alignedY) ? "YES" : "NO");
	if (!(alignedX && alignedY))
	{
		printf("  UNALIGNED: a %d x %d tile is not an integral number of %dx%d blocks.\n"
		       "  Per-tile vkCmdCopyBufferToImage is not expressible, and blocks straddle\n"
		       "  tile boundaries (two source poses in one block). Not measured.\n",
		       TILE, TILE, f->bw, f->bh);
		return;
	}

	VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                         .imageType = VK_IMAGE_TYPE_2D,
	                         .format = f->f,
	                         .extent = {(uint32_t)W, (uint32_t)H, 1},
	                         .mipLevels = 1,
	                         .arrayLayers = 1,
	                         .samples = VK_SAMPLE_COUNT_1_BIT,
	                         .tiling = VK_IMAGE_TILING_OPTIMAL,
	                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VkImage atlas;
	VKC(vkCreateImage(vkd, &ici, NULL, &atlas));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(vkd, atlas, &mr);
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                            .allocationSize = mr.size,
	                            .memoryTypeIndex = mem_type(mr.memoryTypeBits,
	                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VkDeviceMemory atlasMem;
	VKC(vkAllocateMemory(vkd, &mai, NULL, &atlasMem));
	VKC(vkBindImageMemory(vkd, atlas, atlasMem, 0));

	size_t tileBytes = fmt_bytes(f, TILE, TILE);
	size_t total = tileBytes * (size_t)nTilesUpd;
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                          .size = total,
	                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
	VkBuffer stage;
	VKC(vkCreateBuffer(vkd, &bci, NULL, &stage));
	vkGetBufferMemoryRequirements(vkd, stage, &mr);
	VkMemoryAllocateInfo smai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                             .allocationSize = mr.size,
	                             .memoryTypeIndex = mem_type(mr.memoryTypeBits,
	                                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VkDeviceMemory stageMem;
	VKC(vkAllocateMemory(vkd, &smai, NULL, &stageMem));
	VKC(vkBindBufferMemory(vkd, stage, stageMem, 0));
	void * sp;
	VKC(vkMapMemory(vkd, stageMem, 0, total, 0, &sp));
	memset(sp, 0x5a, total);
	vkUnmapMemory(vkd, stageMem);

	int tilesX = W / TILE, tilesY = H / TILE;
	int nregions = nTilesUpd;
	VkBufferImageCopy * regions = malloc(sizeof(VkBufferImageCopy) * (size_t)nTilesUpd);
	if (g_strips)
	{
		// Same total bytes, but laid out as full-width 64-row strips: the
		// fewest legal regions that still cover whole tile rows. This isolates
		// per-region overhead from the actual bytes moved.
		int rows = (nTilesUpd + tilesX - 1) / tilesX;
		if (rows > tilesY)
			rows = tilesY;
		nregions = rows;
		for (int i = 0; i < rows; i++)
		{
			VkBufferImageCopy r = {.bufferOffset = tileBytes * (size_t)i * (size_t)tilesX,
			                       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			                       .imageOffset = {0, i * TILE, 0},
			                       .imageExtent = {(uint32_t)W, TILE, 1}};
			regions[i] = r;
		}
	}
	else
	for (int i = 0; i < nTilesUpd; i++)
	{
		int t = (i * 7919) % (tilesX * tilesY);   // scattered, as a refresh set is
		VkBufferImageCopy r = {.bufferOffset = tileBytes * (size_t)i,
		                       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		                       .imageOffset = {(t % tilesX) * TILE, (t / tilesX) * TILE, 0},
		                       .imageExtent = {TILE, TILE, 1}};
		regions[i] = r;
	}

	VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};

	// bring the atlas to TRANSFER_DST once
	VKC(vkBeginCommandBuffer(cmd, &cbi));
	VkImageMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                           .image = atlas,
	                           .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     NULL, 0, NULL, 1, &mb);
	VKC(vkEndCommandBuffer(cmd));
	VKC(vkQueueSubmit(vkq, 1, &si, fence));
	VKC(vkWaitForFences(vkd, 1, &fence, VK_TRUE, UINT64_MAX));
	VKC(vkResetFences(vkd, 1, &fence));

	double * samples = malloc(sizeof(double) * (size_t)iters);
	for (int it = 0; it < warmup + iters; it++)
	{
		VKC(vkResetCommandBuffer(cmd, 0));
		VKC(vkBeginCommandBuffer(cmd, &cbi));
		vkCmdResetQueryPool(cmd, qpool, 0, 2);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
		vkCmdCopyBufferToImage(cmd, stage, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                       (uint32_t)nregions, regions);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
		VKC(vkEndCommandBuffer(cmd));
		VKC(vkQueueSubmit(vkq, 1, &si, fence));
		VKC(vkWaitForFences(vkd, 1, &fence, VK_TRUE, UINT64_MAX));
		VKC(vkResetFences(vkd, 1, &fence));
		uint64_t ts[2];
		VKC(vkGetQueryPoolResults(vkd, qpool, 0, 2, sizeof ts, ts, sizeof(uint64_t),
		                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
		if (it >= warmup)
			samples[it - warmup] = (double)(ts[1] - ts[0]) * ts_period / 1e6;
	}
	qsort(samples, (size_t)iters, sizeof(double), cmpd);
	printf("  update %4d tiles in %4d regions (%zu B/tile, %zu B total): min %.3f  p50 %.3f  p90 %.3f ms\n",
	       nTilesUpd, nregions, tileBytes, total, samples[0], samples[iters / 2], samples[(int)(iters * 0.9)]);
	free(samples);
	free(regions);
	vkDestroyBuffer(vkd, stage, NULL);
	vkFreeMemory(vkd, stageMem, NULL);
	vkDestroyImage(vkd, atlas, NULL);
	vkFreeMemory(vkd, atlasMem, NULL);
}

char g_spv_path[512] = "/data/local/tmp/nxtex/display.spv";
static int g_ntiles = 40;

int main(int argc, char ** argv)
{
	const char * mode = "formats";
	const char * only = NULL;
	const char * payload = NULL;
	int W = 2176, H = 1088, iters = 60, warmup = 20, nearest = 0;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--formats"))
			mode = "formats";
		else if (!strcmp(argv[i], "--bench"))
			mode = "bench";
		else if (!strcmp(argv[i], "--update"))
			mode = "update";
		else if (!strcmp(argv[i], "--strips"))
			g_strips = 1;
		else if (!strcmp(argv[i], "--ntiles") && i + 1 < argc)
			g_ntiles = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--format") && i + 1 < argc)
			only = argv[++i];
		else if (!strcmp(argv[i], "--payload") && i + 1 < argc)
			payload = argv[++i];
		else if (!strcmp(argv[i], "--spv") && i + 1 < argc)
			snprintf(g_spv_path, sizeof g_spv_path, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--size") && i + 2 < argc)
		{
			W = atoi(argv[++i]);
			H = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
			iters = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--warmup") && i + 1 < argc)
			warmup = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--nearest"))
			nearest = 1;
		else
		{
			fprintf(stderr,
			        "usage: nxtexnative [--formats | --bench]\n"
			        "  --format NAME    bench only this format (default: all supported)\n"
			        "  --payload FILE   atlas payload bytes (tiled if short)\n"
			        "  --spv FILE       display.spv path\n"
			        "  --size W H       atlas and output size (default 2176 1088)\n"
			        "  --iters N --warmup N --nearest\n");
			return 2;
		}
	}

	vk_init();

	if (!strcmp(mode, "formats"))
	{
		mode_formats();
		return 0;
	}

	if (!strcmp(mode, "update"))
	{
		printf("\natlas update: %dx%d atlas, %d refreshed 64x64 tiles per frame\n", W, H, g_ntiles);
		for (int i = 0; i < NFMT; i++)
		{
			if (only && strcmp(only, formats[i].name))
				continue;
			printf("\n== %s\n", formats[i].name);
			mode_update(&formats[i], W, H, g_ntiles, iters, warmup);
		}
		return 0;
	}

	printf("\nbench: %dx%d output (= %dx%d per eye, one displayed frame PAIR)\n", W, H, W / 2, H);
	for (int i = 0; i < NFMT; i++)
	{
		if (only && strcmp(only, formats[i].name))
			continue;
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(vkpd, formats[i].f, &fp);
		if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
		{
			printf("\n== %s: not sampleable, skipped\n", formats[i].name);
			continue;
		}
		if (!nearest && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
		{
			printf("\n== %s: no SAMPLED_IMAGE_FILTER_LINEAR, skipped\n", formats[i].name);
			continue;
		}
		printf("\n== %s\n", formats[i].name);
		double med, mn;
		bench_one(&formats[i], W, H, payload, iters, warmup, nearest, &med, &mn);
	}
	return 0;
}
