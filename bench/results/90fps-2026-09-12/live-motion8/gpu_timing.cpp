// SPDX-License-Identifier: GPL-3.0-or-later
// Headless GPU readback regression fixture; not a timing benchmark.
#include "nxb_vk.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <vulkan/vulkan.h>
using namespace nxb;
#ifndef MOTION_TRUTH_SIZE
#define MOTION_TRUTH_SIZE 256
#endif
#ifndef MOTION_TRUTH_BLOCK
#define MOTION_TRUTH_BLOCK 64
#endif
constexpr uint32_t W = MOTION_TRUTH_SIZE, H = W, E = W / 4, L = 3, G = W / MOTION_TRUTH_BLOCK;
static_assert(MOTION_TRUTH_BLOCK >= 1 && W % MOTION_TRUTH_BLOCK == 0);
static_assert(W >= 256 && W % 64 == 0);
uint32_t DX = 8, DY = 4;
struct Img
{
	VkImage im{};
	VkDeviceMemory mem{};
	VkImageView view{};
	std::vector<VkImageView> levels;
	uint32_t mips = 1, layers = 1;
};
static uint32_t mt(VkCtx & c, uint32_t bits, VkMemoryPropertyFlags want)
{
	return c.findMem(bits, want);
}
static Img image(VkCtx & c, uint32_t w, uint32_t h, uint32_t layers, uint32_t mips, VkFormat f, VkImageUsageFlags u)
{
	Img x;
	x.layers = layers;
	x.mips = mips;
	VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	i.imageType = VK_IMAGE_TYPE_2D;
	i.format = f;
	i.extent = {w, h, 1};
	i.mipLevels = mips;
	i.arrayLayers = layers;
	i.samples = VK_SAMPLE_COUNT_1_BIT;
	i.tiling = VK_IMAGE_TILING_OPTIMAL;
	i.usage = u;
	i.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	NXB_VK(vkCreateImage(c.dev, &i, nullptr, &x.im));
	VkMemoryRequirements r;
	vkGetImageMemoryRequirements(c.dev, x.im, &r);
	VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	a.allocationSize = r.size;
	a.memoryTypeIndex = mt(c, r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	NXB_VK(vkAllocateMemory(c.dev, &a, nullptr, &x.mem));
	NXB_VK(vkBindImageMemory(c.dev, x.im, x.mem, 0));
	auto view = [&](uint32_t mip, uint32_t count, VkImageViewType t) {VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=x.im;v.viewType=t;v.format=f;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,count,0,layers};VkImageView q;NXB_VK(vkCreateImageView(c.dev,&v,nullptr,&q));return q; };
	x.view = view(0, mips, layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	for (uint32_t m = 0; m < mips; m++)
		x.levels.push_back(view(m, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY));
	return x;
}
static void destroy(VkCtx & c, Img & x)
{
	for (auto v: x.levels)
		vkDestroyImageView(c.dev, v, nullptr);
	if (x.view)
		vkDestroyImageView(c.dev, x.view, nullptr);
	vkDestroyImage(c.dev, x.im, nullptr);
	vkFreeMemory(c.dev, x.mem, nullptr);
}
static void barrier(VkCommandBuffer cmd, VkImage im, VkImageLayout old, VkImageLayout now, uint32_t layers, uint32_t mips, VkAccessFlags src, VkAccessFlags dst)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = old;
	b.newLayout = now;
	b.srcAccessMask = src;
	b.dstAccessMask = dst;
	b.image = im;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
static void upload(VkCtx & c, Img & x, const std::vector<uint8_t> & p, uint32_t w, uint32_t h)
{
	Buffer s = c.createBuffer(p.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	memcpy(s.mapped, p.data(), p.size());
	c.oneShot([&](VkCommandBuffer cmd) {barrier(cmd,x.im,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,x.layers,x.mips,0,VK_ACCESS_TRANSFER_WRITE_BIT);VkBufferImageCopy q{};q.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,x.layers};q.imageExtent={w,h,1};vkCmdCopyBufferToImage(cmd,s.buf,x.im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&q);barrier(cmd,x.im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,x.layers,x.mips,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT); });
	c.destroyBuffer(s);
}
static std::vector<uint32_t> code(const char * n)
{
	std::ifstream f(std::string(SPV_DIR) + "/" + n, std::ios::binary | std::ios::ate);
	auto z = f.tellg();
	f.seekg(0);
	std::vector<uint32_t> b(size_t(z) / 4);
	f.read((char *)b.data(), z);
	return b;
}
static VkPipeline pipe(VkCtx & c, const char * n, VkDescriptorSetLayout d, VkPipelineLayout * l, uint32_t pc)
{
	auto b = code(n);
	VkShaderModule sm = c.shader(b.data(), b.size() * 4);
	VkPushConstantRange r{VK_SHADER_STAGE_COMPUTE_BIT, 0, pc};
	VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	li.setLayoutCount = 1;
	li.pSetLayouts = &d;
	li.pushConstantRangeCount = pc ? 1 : 0;
	li.pPushConstantRanges = &r;
	NXB_VK(vkCreatePipelineLayout(c.dev, &li, nullptr, l));
	VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	st.module = sm;
	st.pName = "main";
	VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	ci.stage = st;
	ci.layout = *l;
	VkPipeline p;
	NXB_VK(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
	vkDestroyShaderModule(c.dev, sm, nullptr);
	return p;
}
int main(int argc, char ** argv)
{
	float step = argc > 1 ? std::stof(argv[1]) : 1.f;
	if (argc > 3)
	{
		DX = std::stoul(argv[2]);
		DY = std::stoul(argv[3]);
	}
	if (DX >= 96 || DY >= 96 || !std::isfinite(step)) return 2;
	VkCtx c;
	if (!c.create({}, {VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME}, false))
		return 2;
	const auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	Img prev = image(c, W, H, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, usage), cur = image(c, W, H, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, usage), py0 = image(c, E, E, 2, L, VK_FORMAT_R32_SFLOAT, usage), py1 = image(c, E, E, 2, L, VK_FORMAT_R32_SFLOAT, usage), retained = image(c, W, H, 2, 1, VK_FORMAT_R8G8B8A8_UNORM, usage), out = image(c, W, H, 2, 1, VK_FORMAT_R8G8B8A8_UNORM, usage);
	std::vector<uint8_t> a(W * H * 4), b(W * H * 4);
	if (argc <= 4)
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++)
		{
			auto v = [&](uint32_t xx, uint32_t yy) {double z=35; for(int n=0;n<35;n++){double cx=(n*73+19)%300,cy=(n*137+47)%300,dx=double(xx)-cx,dy=double(yy)-cy;z+=70*exp(-(dx*dx+dy*dy)/180.);}return uint8_t(std::clamp(z,0.,255.)); };
			size_t o = (y * W + x) * 4;
			a[o] = v(x, y);
			a[o + 1] = v(x + 19, y + 31);
			a[o + 2] = v(x * 3, y * 2);
			a[o + 3] = 255;
			uint32_t sx = uint32_t(std::max(0, int(x) - int(DX))), sy = uint32_t(std::max(0, int(y) - int(DY)));
			size_t q = (y * W + x) * 4;
			b[q] = v(sx, sy);
			b[q + 1] = v(sx + 19, sy + 31);
			b[q + 2] = v(sx * 3, sy * 2);
			b[q + 3] = 255;
		}
	// Optional tightly packed linear RGBA8 source; previous/current/truth retain
	// exactly the same translation convention as the synthetic fixture.
	if (argc > 4)
	{
	 std::ifstream f(argv[4], std::ios::binary | std::ios::ate);
	 if (!f || f.tellg() != std::streamoff(a.size())) return 2;
	 f.seekg(0); f.read(reinterpret_cast<char *>(a.data()), a.size());
	 for (unsigned y=0; y<H; ++y) for(unsigned x=0; x<W; ++x)
	  for(unsigned k=0; k<4; ++k)
	   b[(y*W+x)*4+k]=a[(std::max(0,int(y)-int(DY))*W+std::max(0,int(x)-int(DX)))*4+k];
	}
	std::vector<uint8_t> explicit_truth;
	if (argc > 6)
	{
	 auto load_frame = [&](const char *path, std::vector<uint8_t> &data) {
	  std::ifstream f(path, std::ios::binary | std::ios::ate);
	  if (!f || f.tellg() != std::streamoff(a.size())) return false;
	  data.resize(a.size()); f.seekg(0);
	  return bool(f.read(reinterpret_cast<char *>(data.data()), data.size()));
	 };
	 if (!load_frame(argv[5], b) || !load_frame(argv[6], explicit_truth)) return 2;
	}
	upload(c, prev, a, W, H);
	upload(c, cur, b, W, H);
	auto stereo = b;
	stereo.insert(stereo.end(), b.begin(), b.end());
	upload(c, retained, stereo, W, H);
	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
	VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pi.maxSets = 4;
	pi.poolSizeCount = 3;
	pi.pPoolSizes = ps;
	VkDescriptorPool pool;
	NXB_VK(vkCreateDescriptorPool(c.dev, &pi, nullptr, &pool));
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler samp;
	NXB_VK(vkCreateSampler(c.dev, &si, nullptr, &samp));
	auto layout = [&](std::vector<VkDescriptorSetLayoutBinding> x) {VkDescriptorSetLayout d;VkDescriptorSetLayoutCreateInfo q{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};q.bindingCount=x.size();q.pBindings=x.data();NXB_VK(vkCreateDescriptorSetLayout(c.dev,&q,nullptr,&d));return d; };
	auto alloc = [&](VkDescriptorSetLayout d) {VkDescriptorSet s;VkDescriptorSetAllocateInfo q{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};q.descriptorPool=pool;q.descriptorSetCount=1;q.pSetLayouts=&d;NXB_VK(vkAllocateDescriptorSets(c.dev,&q,&s));return s; };
	VkDescriptorSetLayout dl = layout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, L, VK_SHADER_STAGE_COMPUTE_BIT}}), el = layout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}}), wl = layout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}});
	VkDescriptorSet ds = alloc(dl), ds2 = alloc(dl), es = alloc(el), ws = alloc(wl);
	VkPipelineLayout dpl, epl, wpl;
	VkPipeline dp = pipe(c, "downsample.spv", dl, &dpl, 40), ep = pipe(c, "estimate.spv", el, &epl, 16), wp = pipe(c, "warp.spv", wl, &wpl, 20);
	Buffer field = c.createBuffer(G * G * 2 * 2 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDescriptorImageInfo src[2] = {{samp, prev.view, VK_IMAGE_LAYOUT_GENERAL}, {samp, prev.view, VK_IMAGE_LAYOUT_GENERAL}};
	std::vector<VkDescriptorImageInfo> lev(L);
	for (uint32_t i = 0; i < L; i++)
		lev[i] = {VK_NULL_HANDLE, py0.levels[i], VK_IMAGE_LAYOUT_GENERAL};
	VkWriteDescriptorSet dw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, nullptr, nullptr}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 1, 0, L, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, lev.data(), nullptr, nullptr}};
	dw[0].pImageInfo = src;
	vkUpdateDescriptorSets(c.dev, 2, dw, 0, nullptr);
	for (uint32_t i = 0; i < L; i++)
		lev[i].imageView = py1.levels[i];
	dw[0].dstSet = ds2;
	dw[1].dstSet = ds2;
	src[0].imageView = cur.view;
	src[1].imageView = cur.view;
	vkUpdateDescriptorSets(c.dev, 2, dw, 0, nullptr);
	VkDescriptorImageInfo pi0{samp, py0.view, VK_IMAGE_LAYOUT_GENERAL}, pi1{samp, py1.view, VK_IMAGE_LAYOUT_GENERAL};
	VkDescriptorBufferInfo fi{field.buf, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet ew[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, es, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &pi1, nullptr, nullptr}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, es, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &pi0, nullptr, nullptr}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, es, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fi, nullptr}};
	vkUpdateDescriptorSets(c.dev, 3, ew, 0, nullptr);
	VkDescriptorImageInfo ri{samp, retained.view, VK_IMAGE_LAYOUT_GENERAL}, oi{VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
	VkWriteDescriptorSet ww[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ws, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ri, nullptr, nullptr}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ws, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fi, nullptr}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ws, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr}};
	vkUpdateDescriptorSets(c.dev, 3, ww, 0, nullptr);
	struct DPC
	{
		float m[2][4];
		int32_t sz[2];
	} dpc{{{0, 0, 4, 4}, {0, 0, 4, 4}}, {int32_t(E), int32_t(E)}};
	struct EPC
	{
		int32_t g[2], sz[2];
	} epc{{G, G}, {int32_t(E), int32_t(E)}};
	struct WPC
	{
		int32_t sz[2], g[2];
		float t;
	} wpc{{int32_t(W), int32_t(H)}, {G, G}, step};
	c.oneShot([&](VkCommandBuffer cmd) {for(auto* im:{&py0,&py1,&out})barrier(cmd,im->im,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,im->layers,im->mips,0,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,dp);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,dpl,0,1,&ds,0,nullptr);vkCmdPushConstants(cmd,dpl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof dpc,&dpc);vkCmdDispatch(cmd,E/8,E/8,2);fullBarrier(cmd);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,dpl,0,1,&ds2,0,nullptr);vkCmdDispatch(cmd,E/8,E/8,2);fullBarrier(cmd);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,ep);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,epl,0,1,&es,0,nullptr);vkCmdPushConstants(cmd,epl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof epc,&epc);if (!std::getenv("NX_MOTION_FIELD_OVERRIDE")) vkCmdDispatch(cmd,G,G,2);fullBarrier(cmd);
	 VkMemoryBarrier readback{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	 readback.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; readback.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
	 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,nullptr,0,nullptr);
	 });
	constexpr uint32_t warmup = 30, repeat = 100, query_count = 2 * (warmup + repeat);
	VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qi.queryCount = query_count;
	VkQueryPool queries{};
	NXB_VK(vkCreateQueryPool(c.dev, &qi, nullptr, &queries));
	c.oneShot([&](VkCommandBuffer cmd) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ep);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, epl, 0, 1, &es, 0, nullptr);
		vkCmdPushConstants(cmd, epl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof epc, &epc);
		for (uint32_t i = 0; i < warmup + repeat; ++i)
		{
			const uint32_t q = 2 * i;
			vkCmdResetQueryPool(cmd, queries, q, 2);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, q);
			vkCmdDispatch(cmd, G, G, 2);
			fullBarrier(cmd);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, q + 1);
		}
	});
	std::vector<uint64_t> query_values(query_count);
	NXB_VK(vkGetQueryPoolResults(c.dev, queries, 0, query_count,
	                             query_values.size() * sizeof(uint64_t), query_values.data(),
	                             sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
	std::vector<double> gpu_ms;
	const uint32_t valid_bits = c.info.timestampValidBits;
	const uint64_t timestamp_mask = valid_bits > 0 and valid_bits < 64 ? (uint64_t(1) << valid_bits) - 1 : ~uint64_t(0);
	for (uint32_t i = warmup; i < warmup + repeat; ++i)
		gpu_ms.push_back(double((query_values[2 * i + 1] - query_values[2 * i]) & timestamp_mask) * c.props.limits.timestampPeriod / 1e6);
	std::sort(gpu_ms.begin(), gpu_ms.end());
	double sum = 0;
	for (double ms : gpu_ms) sum += ms;
	printf("estimate_gpu_ms device=%s timestamp_period_ns=%.6f valid_bits=%u warmup=%u repeat=%u mean=%.6f median=%.6f p95=%.6f min=%.6f max=%.6f\n",
	       c.info.name.c_str(), c.props.limits.timestampPeriod, valid_bits, warmup, repeat,
	       sum / gpu_ms.size(), gpu_ms[gpu_ms.size() / 2], gpu_ms[(gpu_ms.size() * 95) / 100], gpu_ms.front(), gpu_ms.back());
	printf("estimate_gpu_ms_raw=");
	for (double ms : gpu_ms) printf("%.6f,", ms);
	puts("");
	vkDestroyQueryPool(c.dev, queries, nullptr);
	if (!std::getenv("NX_MOTION_FIELD_OVERRIDE")) { std::ofstream f("field.f32", std::ios::binary); f.write(static_cast<const char *>(field.mapped), G*G*2*2*sizeof(float)); }
	if (const char *path = std::getenv("NX_MOTION_FIELD_OVERRIDE"))
	{
	 std::ifstream f(path, std::ios::binary | std::ios::ate);
	 if (!f || f.tellg() != std::streamoff(G*G*2*2*sizeof(float))) return 2;
	 f.seekg(0); f.read(static_cast<char *>(field.mapped), G*G*2*2*sizeof(float));
	}
	c.oneShot([&](VkCommandBuffer cmd) {
	 VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	 host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT; host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
	 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
	vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,wp);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,wpl,0,1,&ws,0,nullptr);vkCmdPushConstants(cmd,wpl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof wpc,&wpc);vkCmdDispatch(cmd,W/8,H/8,2);fullBarrier(cmd); });
	float * fv = (float *)field.mapped;
	if (G <= 64)
	for (unsigned j = 0; j < G; j++)
	{
		for (unsigned i = 0; i < G; i++)
			printf("(%.3f,%.3f) ", fv[2 * (j * G + i)], fv[2 * (j * G + i) + 1]);
		puts("");
	}
	Buffer pyrread = c.createBuffer(E * E * 4 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	c.oneShot([&](VkCommandBuffer cmd) {unsigned index=0;for(auto* im:{&py0,&py1}){barrier(cmd,im->im,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,2,L,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);VkBufferImageCopy q{};q.bufferOffset=index++*E*E*4*2;q.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2};q.imageExtent={E,E,1};vkCmdCopyImageToBuffer(cmd,im->im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pyrread.buf,1,&q);} });
	auto pp = (float *)pyrread.mapped;
	double pe = 0;
	int pn = 0;
	for (unsigned y = std::max(8u, DY / 4); y < E - 8; y++)
		for (unsigned x = std::max(8u, DX / 4); x < E - 8; x++)
		{
			double d = pp[2 * E * E + y * E + x] - pp[(y - DY / 4) * E + x - DX / 4];
			pe += d * d;
			pn++;
		}
	if (explicit_truth.empty()) printf("pyramid l0 shifted RMSE=%g\n", sqrt(pe / pn));
	c.destroyBuffer(pyrread);
	Buffer read = c.createBuffer(W * H * 4 * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	c.oneShot([&](VkCommandBuffer cmd) {barrier(cmd,out.im,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,2,1,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);VkBufferImageCopy q{};q.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2};q.imageExtent={W,H,1};vkCmdCopyImageToBuffer(cmd,out.im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,read.buf,1,&q);VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr); });
	auto encode = [](uint8_t v) {double x=v/255.;return uint8_t(std::lround(255.*(x<.0031308?12.92*x:1.055*pow(x,1/2.4)-.055))); };
	auto * warped = (uint8_t *)read.mapped;
	std::vector<uint8_t> truth(W * H * 4), held = b;
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++)
			for (int k = 0; k < 4; k++)
			{
				size_t o = (y * W + x) * 4 + k;
				int sx = std::max(0, int(x) - 2 * int(DX)), sy = std::max(0, int(y) - 2 * int(DY));
				truth[o] = k == 3 ? 255 : encode(a[(sy * W + sx) * 4 + k]);
				held[o] = k == 3 ? 255 : encode(held[o]);
			}
	if (!explicit_truth.empty())
	 for (size_t i=0; i<truth.size(); ++i)
	  truth[i] = i % 4 == 3 ? explicit_truth[i] : encode(explicit_truth[i]);
	double err = 0, old = 0;
	size_t n = 0;
	for (unsigned eye = 0; eye < 2; eye++)
		for (unsigned y = std::max(64u, 2 * DY); y < H - 64; y++)
			for (unsigned x = std::max(64u, 2 * DX); x < W - 64; x++)
				for (int k = 0; k < 3; k++)
				{
					size_t o = (y * W + x) * 4 + k;
					double d = double(warped[eye * W * H * 4 + o]) - truth[o], h = double(held[o]) - truth[o];
					err += d * d;
					old += h * h;
					n++;
				}
	auto ppm = [](const char * name, const uint8_t * data) {std::ofstream f(name,std::ios::binary);f<<"P6\n"<<W<<" "<<H<<"\n255\n";for(size_t i=0;i<W*H;i++)f.write((const char*)data+i*4,3); };
	ppm("held.ppm", held.data());
	ppm("warped.ppm", warped);
	ppm("truth.ppm", truth.data());
	printf("device=%s field0=(%.6f,%.6f) truth=(%.6f,%.6f) held_rmse=%.6f warped_rmse=%.6f pixels=%zu\n", c.info.name.c_str(), fv[0], fv[1], DX / float(W), DY / float(H), sqrt(old / n), sqrt(err / n), n / 3);
	c.destroyBuffer(read);
	destroy(c, prev);
	destroy(c, cur);
	destroy(c, retained);
	destroy(c, py0);
	destroy(c, py1);
	destroy(c, out);
	c.destroyBuffer(field);
	for (auto p: {dp, ep, wp})
		vkDestroyPipeline(c.dev, p, nullptr);
	for (auto p: {dpl, epl, wpl})
		vkDestroyPipelineLayout(c.dev, p, nullptr);
	vkDestroyDescriptorPool(c.dev, pool, nullptr);
	for (auto p: {dl, el, wl})
		vkDestroyDescriptorSetLayout(c.dev, p, nullptr);
	vkDestroySampler(c.dev, samp, nullptr);
	c.destroy();
	return 0;
}
