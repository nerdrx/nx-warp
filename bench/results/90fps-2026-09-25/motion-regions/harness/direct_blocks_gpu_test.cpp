#include "nxwarp_codec.h"
#include "nxwarp_direct.h"
#include "nxwarp_direct_lz4.h"
#include "nxwarp_direct_native.h"
#include "nxwarp_direct_safety.h"
#include "nxwarp_direct_zstd.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

#ifdef NX_DIRECT_PHOTO_FIXTURE
#include "wivrn_ipc.h"
std::optional<wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets>> wivrn_ipc_socket_monado;
extern "C"
{
	int listen_socket = -1;
}
#endif
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

int main(int argc, char ** argv)
{
	if (argc == 2 || argc > 5)
	{
		std::fprintf(stderr, "usage: %s INPUT_RGBA8_2160x2160 OUTPUT_NXDF [horizontal_shift] [repeats]\n", argv[0]);
		return 2;
	}
	const int input_shift = argc >= 4 ? std::atoi(argv[3]) : 0;
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
	VkPhysicalDeviceFeatures features{};
	vkGetPhysicalDeviceFeatures(gpu, &features);
	dci.pEnabledFeatures = &features;
	VkDevice device{};
	ok(vkCreateDevice(gpu, &dci, nullptr, &device), "device");
	VkQueue queue{};
	vkGetDeviceQueue(device, family, 0, &queue);

	#ifndef NX_DIRECT_TEST_WIDTH
#define NX_DIRECT_TEST_WIDTH 64
#endif
	constexpr uint32_t w = NX_DIRECT_TEST_WIDTH, h = NX_DIRECT_TEST_WIDTH, layers = 2;
	constexpr VkDeviceSize yb = w * h, uvb = (w / 2) * (h / 2) * 2, lb = yb + uvb;
	std::vector<uint8_t> pixels(lb * layers);
	if (argc >= 3)
	{
		std::ifstream in(argv[1], std::ios::binary);
		std::vector<uint8_t> rgba(std::istreambuf_iterator<char>(in), {});
		constexpr uint32_t sw = 2160, sh = 2160;
		const size_t expected = size_t(sw) * sh * 4;
		if (rgba.size() != expected)
		{
			std::fprintf(stderr, "expected raw RGBA8 %ux%u (%zu bytes), got %zu bytes\n", sw, sh, expected, rgba.size());
			return 3;
		}
		for (uint32_t e = 0; e < layers; ++e)
		{
			auto * p = pixels.data() + e * lb;
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					const uint32_t sx = std::clamp<int>(int(x) + input_shift, 0, sw - 1), sy = std::min(y, sh - 1);
					const size_t at = (size_t(sy) * sw + sx) * 4;
					const float r = rgba[at], g = rgba[at + 1], b = rgba[at + 2];
					p[y * w + x] = uint8_t(std::clamp(0.2126f * r + 0.7152f * g + 0.0722f * b, 0.f, 255.f));
				}
			for (uint32_t y = 0; y < h; y += 2)
				for (uint32_t x = 0; x < w; x += 2)
				{
					float cb = 0, cr = 0;
					for (uint32_t dy = 0; dy < 2; ++dy)
						for (uint32_t dx = 0; dx < 2; ++dx)
						{
							const uint32_t sx = std::clamp<int>(int(x + dx) + input_shift, 0, sw - 1), sy = std::min(y + dy, sh - 1);
							const size_t at = (size_t(sy) * sw + sx) * 4;
							const float r = rgba[at], g = rgba[at + 1], b = rgba[at + 2];
							cb += -0.1146f * r - 0.3854f * g + 0.5f * b + 128.f;
							cr += 0.5f * r - 0.4542f * g - 0.0458f * b + 128.f;
						}
					p[yb + (y / 2) * (w / 2) * 2 + (x / 2) * 2] = uint8_t(std::clamp(cb / 4.f, 0.f, 255.f));
					p[yb + (y / 2) * (w / 2) * 2 + (x / 2) * 2 + 1] = uint8_t(std::clamp(cr / 4.f, 0.f, 255.f));
				}
		}
	}
	auto eye = [&](uint32_t n, uint8_t y, uint8_t cb, uint8_t cr) {
		auto * p = pixels.data() + n * lb;
		std::fill(p, p + yb, y);
		for (uint32_t i = 0; i < uvb; i += 2)
		{
			p[yb + i] = cb;
			p[yb + i + 1] = cr;
		}
	};
	if (argc < 3)
	{
		eye(0, 54, 98, 255);
		eye(1, 18, 255, 116);
	}
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
	if (argc >= 3)
	{
		wivrn::nxwarp_codec_config cfg{};
		cfg.width = w;
		cfg.height = h;
		cfg.eyes = 2;
		cfg.source_width = 2160;
		cfg.source_height = 2160;
		cfg.direct_native_center = true;
		cfg.safety = true;
		cfg.direct_lz4 = true;
		auto codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
		std::ifstream in(argv[1], std::ios::binary);
		std::vector<uint8_t> rgba(std::istreambuf_iterator<char>(in), {});
		std::vector<uint32_t> native(2 * 256 * 256);
		const uint32_t ox = std::clamp<int>((2160 - 256) / 2 + input_shift, 0, 2160 - 256), oy = (2160 - 256) / 2;
		for (uint32_t y = 0; y < 256; ++y)
			for (uint32_t x = 0; x < 256; ++x)
			{
				size_t a = (size_t(oy + y) * 2160 + ox + x) * 4;
				native[y * 256 + x] = native[256 * 256 + y * 256 + x] = uint32_t(rgba[a]) << 16 | uint32_t(rgba[a + 1]) << 8 | rgba[a + 2];
			}
		codec->set_native_center(native);
		codec->set_target_bitrate(500'000'000, 90);
		const int repeats = argc >= 5 ? std::max(1, std::atoi(argv[4])) : 1;
		std::vector<double> times;
		std::vector<uint8_t> reference;
		std::span<const uint8_t> wire;
		for (int i = 0; i < repeats; ++i)
		{
			const auto t = std::chrono::steady_clock::now();
			wire = codec->encode_image_pair(image, 0, 1, i + 1);
			const auto end = std::chrono::steady_clock::now();
			if (i == 0)
				reference.assign(wire.begin(), wire.end());
			else if (!std::equal(wire.begin(), wire.end(), reference.begin(), reference.end()))
				return 13;
			if (i >= 5)
				times.push_back(std::chrono::duration<double, std::milli>(end - t).count());
		}
		if (!times.empty())
		{
			std::sort(times.begin(), times.end());
			std::printf("encode GPU+CPU median %.6f ms p95 %.6f ms wire %zu, %d exact repeats\n", times[times.size() / 2], times[(times.size() * 95 + 99) / 100 - 1], wire.size(), repeats);
		}
		const auto full = wivrn::nxwarp_direct::parse_stream(codec->stream_header());
		if (!full)
			return 15;
		auto sh = wivrn::nxwarp_direct::parse_safety_header(*full, wire);
		if (!sh)
		{
			std::fprintf(stderr, "safety parse failure bytes=%zu", wire.size());
			for (size_t at = 0; at + 4 <= std::min<size_t>(wire.size(), 32); at += 4)
				std::fprintf(stderr, " %08x", wivrn::nxwarp_direct::read32(wire, at));
			std::fprintf(stderr, "\n");
			return 4;
		}
		auto safety = wire.subspan(32, sh->safety_bytes);
		std::vector<uint8_t> safety_raw;
		const auto low = sh->low;
		if (wivrn::nxwarp_direct::is_lz4(safety))
		{
			if (!wivrn::nxwarp_direct::decompress_lz4(low, safety, safety_raw))
				return 10;
			safety = safety_raw;
		}
		if (!wivrn::nxwarp_direct::parse_frame(low, safety))
			return 11;
		std::ofstream safety_out(std::string(argv[2]) + ".safety.nxdf", std::ios::binary);
		safety_out.write(reinterpret_cast<const char *>(safety.data()), safety.size());
		if (!safety_out)
			return 12;
		auto detail = wire.subspan(sh->prefix_bytes(), sh->detail_bytes);
		std::vector<uint8_t> raw;
		if (wivrn::nxwarp_direct::is_lz4(detail))
		{
			if (!wivrn::nxwarp_direct::decompress_lz4(*full, detail, raw))
				return 5;
			detail = raw;
		}
		else if (wivrn::nxwarp_direct::is_zstd(detail))
		{
			if (!wivrn::nxwarp_direct::decompress_zstd(*full, detail, raw))
				return 14;
			detail = raw;
		}

		const auto nl = *full;
		const auto parsed = wivrn::nxwarp_direct::parse_frame(nl, detail);
		if (!parsed || w != 2176)
			return 7;
		const unsigned origin = ((w - 256) / 2) & ~31u;
		for (unsigned e = 0; e < 2; ++e)
		{
			const unsigned x = 128, y = 128, px = origin + x, py = origin + y;
			const auto d = wivrn::nxwarp_direct::read32(parsed->descriptors, ((py / 32) * (w / 32 * 2) + e * (w / 32) + px / 32) * 4);
			if (!(d & (1u << 29)) || (d & (1u << 28)))
				return 8;
			const auto rgb = wivrn::nxwarp_direct::read32(parsed->blocks, ((d & 0x0fffffffu) + (py % 32) * 32 + px % 32) * 4);
			if (rgb != native[e * 256 * 256 + y * 256 + x])
				return 9;
		}
		std::printf("validated GPU NXDF %ux%u: %zu bytes, native centre exact\n", w, h, detail.size());
		std::ofstream out(argv[2], std::ios::binary);
		out.write(reinterpret_cast<const char *>(detail.data()), detail.size());
		if (!getenv("NX_DIRECT_BENCH_SEQUENCE")) return out ? 0 : 6;
	}

	wivrn::nxwarp_codec_config cfg{};
	cfg.width = w;
	cfg.height = h;
	cfg.eyes = 2;
	auto codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
	codec->set_target_bitrate(1, 90);
	auto low = codec->encode_image_pair(image, 0, 1, 1);
	auto parsed = wivrn::nxwarp_direct::parse_frame({w, h, 2}, low);
	assert(parsed);
	uint32_t left = wivrn::nxwarp_direct::read32(parsed->descriptors, 0), right = wivrn::nxwarp_direct::read32(parsed->descriptors, (w / 32) * 4);
	assert((left >> 30) == 3 && (right >> 30) == 3);
	assert(((left >> 16) & 255) > 180 && (left & 255) < 100);
	assert(((right >> 16) & 255) < 100 && (right & 255) > 150);
	codec->set_target_bitrate(800'000'000, 90);
	auto high = codec->encode_image_pair(image, 0, 1, 2);
	assert(wivrn::nxwarp_direct::parse_frame({w, h, 2}, high));
	assert(high.size() >= low.size());
	const std::vector<uint8_t> expected(high.begin(), high.end());
	cfg.direct_lz4 = true;
	auto packed_codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
	packed_codec->set_target_bitrate(800'000'000, 90);
	auto packed = packed_codec->encode_image_pair(image, 0, 1, 3);
	std::vector<uint8_t> unpacked;
	if (wivrn::nxwarp_direct::is_lz4(packed))
	{
		assert(wivrn::nxwarp_direct::decompress_lz4({w, h, 2}, packed, unpacked));
		assert(unpacked == expected);
	}
	else
		assert(std::vector<uint8_t>(packed.begin(), packed.end()) == expected);
	// A live HC switch must preserve decoded bytes in both directions.
	for (bool hc : {true, false, true, false})
	{
		packed_codec->set_lz4_hc(hc);
		auto wire = packed_codec->encode_image_pair(image, 0, 1, 4);
		if (wivrn::nxwarp_direct::is_lz4(wire)) {
			assert(wivrn::nxwarp_direct::decompress_lz4({w, h, 2}, wire, unpacked));
			assert(unpacked == expected);
		} else assert(std::vector<uint8_t>(wire.begin(), wire.end()) == expected);
	}
	// Safety envelope: verify the actual GPU output, source downsampling, and
	// independent raw/LZ4 validation at both requested rate points.
	for (uint32_t bps: {160'000'000u, 500'000'000u})
	{
		cfg.safety = true;
		cfg.direct_lz4 = true;
		auto safe_codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
		safe_codec->set_target_bitrate(bps, 90);
		auto t0 = std::chrono::steady_clock::now();
		auto safe = safe_codec->encode_image_pair(image, 0, 1, bps);
		auto encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		auto sh = wivrn::nxwarp_direct::parse_safety_header({w, h, 2}, safe);
		assert(sh && sh->total_bytes() == safe.size());
		auto part = [&](size_t off, size_t n, wivrn::nxwarp_direct::layout l) {
			std::span<const uint8_t> wire = safe.subspan(off, n);
			std::vector<uint8_t> raw;
			if (wivrn::nxwarp_direct::is_lz4(wire))
			{
				assert(wivrn::nxwarp_direct::decompress_lz4(l, wire, raw));
				wire = raw;
			}
			auto parsed = wivrn::nxwarp_direct::parse_frame(l, wire);
			assert(parsed);
			// Uniform red/blue source eyes must remain separated in both segments.
			for (unsigned eye = 0; eye < 2; ++eye)
			{
				const auto desc = wivrn::nxwarp_direct::read32(parsed->descriptors, eye * (l.width / 32) * 4);
				unsigned red, blue;
				if ((desc >> 30) == 3) { red = (desc >> 16) & 255; blue = desc & 255; }
				else {
					const auto rgb565 = wivrn::nxwarp_direct::read32(parsed->blocks, (desc & 0x3fffffff) * 4) & 65535;
					red = ((rgb565 >> 11) & 31) * 255 / 31; blue = (rgb565 & 31) * 255 / 31;
				}
				assert(eye == 0 ? red > 180 && blue < 100 : red < 100 && blue > 150);
			}
			return wire.size();
		};
		const auto low = wivrn::nxwarp_direct::layout{sh->low.width, sh->low.height, 2};
		const size_t safety_raw = part(32, sh->safety_bytes, low);
		const size_t detail_raw = part(32 + sh->safety_bytes, sh->detail_bytes, {w, h, 2});
		std::printf("safety %u Mbps: encode=%.3f ms wire=%zu safety=%zu/%u detail=%zu/%u\n", bps / 1'000'000,
		            encode_ms, safe.size(), safety_raw, sh->safety_bytes, detail_raw, sh->detail_bytes);
	}
#ifdef NX_DIRECT_TEST_NATIVE
	setenv("NX_DIRECT_NATIVE_RGB888", "1", 1);
	unsetenv("NX_DIRECT_NATIVE_RGB565");
	setenv("NX_DIRECT_ZSTD", "1", 1);
	setenv("NX_DIRECT_PREDICTOR", "1", 1);
	setenv("NX_DIRECT_MOTION", "1", 1);
#ifdef NX_DIRECT_TEST_REGIONS
	setenv("NX_DIRECT_MOTION_REGIONS", "1", 1);
#else
	unsetenv("NX_DIRECT_MOTION_REGIONS");
#endif
	cfg.direct_native_center = true;
	cfg.direct_lz4 = true;
	cfg.safety = true;
	auto motion_codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
	motion_codec->set_target_bitrate(700'000'000u, 90);
	auto independent_cfg = cfg;
	auto independent = [&]() {
		unsetenv("NX_DIRECT_MOTION");
		auto p = wivrn::nxwarp_codec::make_direct(independent_cfg, instance, gpu, device, queue, family);
		setenv("NX_DIRECT_MOTION", "1", 1);
		return p;
	}();
	independent->set_target_bitrate(700'000'000u, 90);
	auto stream = wivrn::nxwarp_direct::parse_stream(motion_codec->stream_header()); assert(stream);
	const auto nl = *stream;
	const auto plain_layout = *wivrn::nxwarp_direct::parse_stream(independent->stream_header());
#ifdef NX_DIRECT_TEST_REGIONS
	assert(nl.motion_regions);
#else
	assert(!std::getenv("NX_DIRECT_TEST_REGIONS"));
#endif
	auto forest = [&]() { std::ifstream f(std::getenv("NX_BENCH_FOREST"), std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}); }();
	auto dark = [&]() { std::ifstream f(std::getenv("NX_BENCH_DARK"), std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}); }();
	assert(forest.size() == size_t(2160)*2160*4 && dark.size() == forest.size());
	auto source_native = [&](bool is_dark, int mode, int j) {
		const auto & source = is_dark ? dark : forest;
		std::vector<uint32_t> px(2u*256u*256u);
		for (unsigned y=0; y<256; ++y) for (unsigned x=0; x<256; ++x) {
			int dx=0,dy=0;
			if (mode==0) { const int d=j*2; if (x<128 && y<128) dx=d; else if(x>=128 && y<128) dx=-d; else if(x<128) dy=d; else dy=-d; }
			else if (mode==1) dx=j*2;
			const size_t at=(size_t(952+y+dy)*2160u+size_t(952+x+dx))*4u;
			const uint32_t c=uint32_t(source[at])<<16 | uint32_t(source[at+1])<<8 | source[at+2];
			px[y*256+x]=px[256u*256u+y*256+x]=c;
		}
		return px;
	};
	auto unpack = [](std::span<const uint8_t> body, wivrn::nxwarp_direct::layout l) {
		std::vector<uint8_t> raw;
		if (wivrn::nxwarp_direct::is_zstd(body)) assert(wivrn::nxwarp_direct::decompress_zstd(l, body, raw));
		else if (wivrn::nxwarp_direct::is_lz4(body)) assert(wivrn::nxwarp_direct::decompress_lz4(l, body, raw));
		else raw.assign(body.begin(), body.end());
		return raw;
	};
	auto detail = [](auto wire, auto l) {
		auto h=wivrn::nxwarp_direct::parse_safety_header(l,wire); assert(h && h->total_bytes()==wire.size());
		return std::pair{wire.subspan(h->prefix_bytes(),h->detail_bytes),size_t(h->detail_bytes)};
	};
	auto checksum = [](std::span<const uint8_t> b) { uint64_t h=1469598103934665603ull; for(uint8_t x:b){h^=x;h*=1099511628211ull;} return h; };
	std::printf("scenario,phase,id,detail_mode,bytes,wire_bytes,encode_ms,raw_fnv\n");
	auto run = [&](const char * name, int mode, bool ack, bool cut) {
		motion_codec->reset_direct_references();
		std::vector<uint8_t> previous;
		wivrn::nxwarp_direct::motion_native_info previous_info;
		for (int j=0;j<30;++j) {
			const uint16_t id=uint16_t(100+j);
			const bool is_dark=cut && j>=6;
			auto px=source_native(is_dark,mode,j);
			motion_codec->set_native_center(px); independent->set_native_center(px);
			motion_codec->set_wire_frame_id(id);
			motion_codec->set_direct_held_ack(uint16_t(id-1), ack && j>0 ? 1u : 0u);
			const auto t0=std::chrono::steady_clock::now();
			auto cw=motion_codec->encode_image_pair(image,0,1,id);
			const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
			std::vector<uint8_t> ccopy(cw.begin(),cw.end());
			auto iw=independent->encode_image_pair(image,0,1,id);
			auto [cd,cb]=detail(std::span<const uint8_t>(ccopy),nl);
			auto [idetail,ib]=detail(iw,plain_layout);
			auto oracle=unpack(idetail,plain_layout); std::vector<uint8_t> decoded;
			std::string tag="independent";
#ifdef NX_DIRECT_TEST_REGIONS
			if (auto h=wivrn::nxwarp_direct::parse_motion_regions_wire(cd)) {
				assert(h->reference==uint16_t(id-1));
				decoded=unpack(h->body,nl);
				assert(wivrn::nxwarp_direct::build_motion_native(nl,previous,previous_info));
				assert(wivrn::nxwarp_direct::restore_motion_regions(nl,previous,previous_info,decoded,h->vectors)); tag="regional-v2";
			} else
#endif
			if (wivrn::nxwarp_direct::is_motion(cd)) {
				auto h=wivrn::nxwarp_direct::parse_motion_wire(cd); assert(h && h->reference==uint16_t(id-1));
				decoded=unpack(h->body,nl);
				assert(wivrn::nxwarp_direct::build_motion_native(nl,previous,previous_info));
				assert(wivrn::nxwarp_direct::restore_motion(nl,previous,previous_info,decoded,h->dx,h->dy)); tag="global-v1";
			}
			else decoded=unpack(cd,nl);
			assert(decoded==oracle && wivrn::nxwarp_direct::parse_frame(nl,decoded));
			if (j>=6) std::printf("%s,measured,%u,%s,%zu,%zu,%.5f,%016llx\n",name,id,tag.c_str(),cb,ccopy.size(),ms,(unsigned long long)checksum(decoded));
			previous=std::move(oracle);
			assert(wivrn::nxwarp_direct::build_motion_native(nl,previous,previous_info));
		}
	};
	run("opposing_source_motion",0,true,false);
	run("continuous_pan_source",1,true,false);
	run("photo_scene_cut",1,true,true);
	run("opposing_no_ack",0,false,false);
	independent.reset(); motion_codec.reset();
#endif
	packed_codec.reset();
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
