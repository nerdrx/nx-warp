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
#include <cstdint>
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

static uint64_t hash_bytes(std::span<const uint8_t> bytes)
{
	uint64_t hash = 14695981039346656037ull;
	for (uint8_t b: bytes) hash = (hash ^ b) * 1099511628211ull;
	return hash;
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
		return out ? 0 : 6;
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
	#ifdef NX_DIRECT_TEST_MOTION
	// Exercise the production codec's opt-in motion envelope through the same
	// Vulkan offscreen fixture. Keep this behind a separate translation-unit
	// wrapper so the ordinary native-centre fixture stays unchanged.
	setenv("NX_DIRECT_NATIVE_RGB888", "1", 1);
	unsetenv("NX_DIRECT_NATIVE_RGB565");
	setenv("NX_DIRECT_ZSTD", "1", 1);
	setenv("NX_DIRECT_PREDICTOR", "1", 1);
	setenv("NX_DIRECT_MOTION", "1", 1);
	setenv("NX_DIRECT_COMPRESSION_CACHE", "0", 1);
	cfg.direct_native_center = true;
	cfg.direct_lz4 = true;
	cfg.safety = true;
	auto native_codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
	native_codec->set_target_bitrate(700'000'000u, 90);
	const bool regions_enabled = std::getenv("NX_DIRECT_MOTION_REGIONS") && std::strcmp(std::getenv("NX_DIRECT_MOTION_REGIONS"), "1") == 0;
	const bool row_enabled = std::getenv("NX_DIRECT_ROW_PREDICTOR") && std::strcmp(std::getenv("NX_DIRECT_ROW_PREDICTOR"), "1") == 0;
	const wivrn::nxwarp_direct::layout nl{w, h, 2, true, 256, false, true, true, false, true, regions_enabled, row_enabled};
	// Build a separate independent encoder as the byte oracle; keep its codec
	// work outside the timed motion/row encode call.
	unsetenv("NX_DIRECT_MOTION");
	unsetenv("NX_DIRECT_MOTION_REGIONS");
	unsetenv("NX_DIRECT_ROW_PREDICTOR");
	auto oracle_codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
	oracle_codec->set_target_bitrate(700'000'000u, 90);
	setenv("NX_DIRECT_MOTION", "1", 1);
	setenv("NX_DIRECT_MOTION_REGIONS", regions_enabled ? "1" : "0", 1);
	setenv("NX_DIRECT_ROW_PREDICTOR", row_enabled ? "1" : "0", 1);
	auto shifted_pixels = [](int shift) {
		std::vector<uint32_t> pixels(2 * 256 * 256);
		const char * fixture_env = std::getenv(shift == 0 ? "NX_DIRECT_FIXTURE_BASE" : "NX_DIRECT_FIXTURE_NEXT");
		if (fixture_env && *fixture_env)
		{
			std::ifstream in(fixture_env, std::ios::binary);
			in.read(reinterpret_cast<char *>(pixels.data()), std::streamsize(pixels.size() * sizeof(uint32_t)));
			if (!in || in.gcount() != std::streamsize(pixels.size() * sizeof(uint32_t)))
				throw std::runtime_error("native-centre fixture must contain 2*256*256 RGBA words");
			return pixels;
		}
		for (unsigned eye = 0; eye < 2; ++eye)
			for (unsigned y = 0; y < 256; ++y)
				for (unsigned x = 0; x < 256; ++x)
				{
					const unsigned sx = unsigned(std::clamp(int(x) - shift, 0, 255));
					uint32_t v = sx * 0x9e3779b9u ^ y * 0x85ebca6bu ^ (sx * y + 0x27d4eb2du);
					v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15;
					pixels[eye * 256 * 256 + y * 256 + x] = v & 0xffffffu;
				}
		return pixels;
	};
	double last_encode_ms = 0.0;
	auto encode = [&](uint16_t id, int shift) {
		auto pixels_now = shifted_pixels(shift);
		native_codec->set_native_center(pixels_now);
		native_codec->set_wire_frame_id(id);
		auto started = std::chrono::steady_clock::now();
		auto wire_now = native_codec->encode_image_pair(image, 0, 1, 0);
		last_encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		return std::vector<uint8_t>(wire_now.begin(), wire_now.end());
	};
	auto encode_oracle = [&](int shift) {
		auto pixels_now = shifted_pixels(shift);
		oracle_codec->set_native_center(pixels_now);
		auto wire_now = oracle_codec->encode_image_pair(image, 0, 1, 0);
		return std::vector<uint8_t>(wire_now.begin(), wire_now.end());
	};
	auto detail_of = [&](const std::vector<uint8_t> & wire_now) {
		auto safe = std::span<const uint8_t>(wire_now);
		auto sh = wivrn::nxwarp_direct::parse_safety_header(nl, safe); assert(sh && sh->total_bytes() == safe.size());
		return safe.subspan(sh->prefix_bytes(), sh->detail_bytes);
	};
	auto unpack = [&](std::span<const uint8_t> detail) {
		std::vector<uint8_t> raw_now;
		if (wivrn::nxwarp_direct::is_zstd(detail))
			assert(wivrn::nxwarp_direct::decompress_zstd(nl, detail, raw_now));
		else if (wivrn::nxwarp_direct::is_lz4(detail))
			assert(wivrn::nxwarp_direct::decompress_lz4(nl, detail, raw_now));
		else
			raw_now.assign(detail.begin(), detail.end());
		assert(wivrn::nxwarp_direct::parse_frame(nl, raw_now));
		return raw_now;
	};
	auto restore_detail = [&](std::span<const uint8_t> detail, const std::vector<uint8_t> & reference_raw) {
		const auto global = wivrn::nxwarp_direct::parse_motion_wire(detail);
		const auto regional = wivrn::nxwarp_direct::parse_motion_regions_wire(detail);
		auto decoded = unpack(global ? global->body : regional ? regional->body : detail);
		if (global || regional)
		{
			wivrn::nxwarp_direct::motion_native_info reference_info;
			assert(wivrn::nxwarp_direct::build_motion_native(nl, reference_raw, reference_info));
			if (global) assert(wivrn::nxwarp_direct::restore_motion(nl, reference_raw, reference_info, decoded, global->dx, global->dy));
			else assert(wivrn::nxwarp_direct::restore_motion_regions(nl, reference_raw, reference_info, decoded, regional->vectors));
		}
		return decoded;
	};
	auto report = [&](const char * label, const std::vector<uint8_t> & wire_now,
	                  std::span<const uint8_t> decoded = {}, bool oracle_match = false) {
		const auto detail = detail_of(wire_now);
		const auto global = wivrn::nxwarp_direct::parse_motion_wire(detail);
		const auto regional = wivrn::nxwarp_direct::parse_motion_regions_wire(detail);
		const auto body = global ? global->body : regional ? regional->body : detail;
		const char * mode = regional ? "regional" : global ? "global" :
		                    wivrn::nxwarp_direct::is_zstd(detail) && wivrn::nxwarp_direct::read32(detail, 4) == 3 ? "row-v3" : "independent";
		const uint32_t body_version = wivrn::nxwarp_direct::is_zstd(body) ? wivrn::nxwarp_direct::read32(body, 4) : 0u;
		std::printf("FRAME,%s,%s,%s,%u,%zu,%zu,%.5f,%016llx,%d\n",
		            std::getenv("NX_DIRECT_CASE") ? std::getenv("NX_DIRECT_CASE") : "case", label, mode, body_version,
		            wire_now.size(), detail.size(), last_encode_ms,
		            (unsigned long long)(decoded.empty() ? 0 : hash_bytes(decoded)), int(oracle_match));
	};
	const auto no_ack = encode(1, 0);
	const auto reference_raw = unpack(detail_of(no_ack));
	const auto oracle_base = unpack(detail_of(encode_oracle(0)));
	assert(reference_raw == oracle_base);
	report("anchor", no_ack, reference_raw, true);
	assert(!wivrn::nxwarp_direct::is_motion(detail_of(no_ack)));
	const auto no_ack_next = encode(2, 8);
	const auto oracle_next = unpack(detail_of(encode_oracle(8)));
	const auto no_ack_raw = restore_detail(detail_of(no_ack_next), reference_raw);
	assert(no_ack_raw == oracle_next);
	report("no-ack", no_ack_next, no_ack_raw, true);
	assert(!wivrn::nxwarp_direct::is_motion(detail_of(no_ack_next)));
	const bool acked = !std::getenv("NX_DIRECT_TEST_NO_ACK") || std::strcmp(std::getenv("NX_DIRECT_TEST_NO_ACK"), "1") != 0;
	if (acked) native_codec->set_direct_held_ack(1, 1u);
	const auto motion_wire = encode(3, 8);
	const auto motion_detail = detail_of(motion_wire);
	const auto mh = wivrn::nxwarp_direct::parse_motion_wire(motion_detail);
	const auto rh = wivrn::nxwarp_direct::parse_motion_regions_wire(motion_detail);
	assert(!acked || (mh && mh->reference == 1) || (rh && rh->reference == 1) ||
	       wivrn::nxwarp_direct::parse_frame(nl, unpack(motion_detail)).has_value());
	auto restored = restore_detail(motion_detail, reference_raw);
	assert(restored == oracle_next);
	report("ack-pair", motion_wire, restored, true);
	native_codec->forget_direct_frame(1);
	const auto rejected_ref = encode(4, 8);
	assert(!wivrn::nxwarp_direct::is_motion(detail_of(rejected_ref)));
	const auto rejected_raw = unpack(detail_of(rejected_ref));
	assert(restored == rejected_raw && rejected_raw == oracle_next);
	report("forgot-ref", rejected_ref, rejected_raw, true);
	const auto anchor = encode(8, 8);
	const auto anchor_raw = unpack(detail_of(anchor));
	assert(anchor_raw == oracle_next);
	report("anchor-id8", anchor, anchor_raw, true);
	assert(!wivrn::nxwarp_direct::is_motion(detail_of(anchor)));
	native_codec->reset_direct_references();
	const auto after_reset = encode(9, 8);
	const auto post_reset_raw = unpack(detail_of(after_reset));
	assert(post_reset_raw == oracle_next);
	report("post-reset", after_reset, post_reset_raw, true);
	assert(!wivrn::nxwarp_direct::is_motion(detail_of(after_reset)));
	// Repeat paired production encodes: six warmups then 24 timed samples.
	for (unsigned sample = 0; sample < 30; ++sample)
	{
		native_codec->reset_direct_references();
		const auto anchor_now = encode(1, 0);
		const auto reference_now = unpack(detail_of(anchor_now));
		assert(reference_now == unpack(detail_of(encode_oracle(0))));
		if (acked) native_codec->set_direct_held_ack(1, 1u);
		const auto current = encode(2, 8);
		const auto decoded_current = restore_detail(detail_of(current), reference_now);
		const auto oracle_current = unpack(detail_of(encode_oracle(8)));
		assert(decoded_current == oracle_current);
		if (sample >= 6) report("measure", current, decoded_current, true);
	}
	std::printf("native motion GPU codec: no-ack, ACKed-reference exact decode, anchor and reset ok\n");
	oracle_codec.reset();
	native_codec.reset();
	#else
	// Deliberately different native RGB source: one-pixel red/green checks.
	// NV12 source above is solid red/blue, so reconstructed chroma cannot pass this.
	cfg.direct_native_center = true;
	cfg.direct_lz4 = true;
	cfg.safety = true;
	auto native_codec = wivrn::nxwarp_codec::make_direct(cfg, instance, gpu, device, queue, family);
	std::vector<uint32_t> native_pixels(2 * 256 * 256);
	for (unsigned eye=0; eye<2; ++eye)
		for (unsigned y=0; y<256; ++y) for (unsigned x=0; x<256; ++x)
			native_pixels[eye*256*256+y*256+x]=((x+y+eye)&1) ? 0xff0000u : 0x00ff00u;
	native_codec->set_native_center(native_pixels);
	native_codec->set_target_bitrate(700'000'000u,90);
	const auto wire=native_codec->encode_image_pair(image,0,1,99);
	const wivrn::nxwarp_direct::layout nl{w,h,2,true};
	const auto nh=wivrn::nxwarp_direct::parse_safety_header(nl,wire);assert(nh);
	auto detail=wire.subspan(nh->prefix_bytes(),nh->detail_bytes);
	std::vector<uint8_t> raw;
	if(wivrn::nxwarp_direct::is_zstd(detail)) {
        assert(wivrn::nxwarp_direct::decompress_zstd(nl,detail,raw));detail=raw;
    } else if(wivrn::nxwarp_direct::is_lz4(detail)) {
		assert(wivrn::nxwarp_direct::decompress_lz4(nl,detail,raw));detail=raw;
	}
	const auto nfview=wivrn::nxwarp_direct::parse_frame(nl,detail);assert(nfview);
	unsigned origin=((w-256)/2)&~31u;
	for(unsigned eye=0;eye<2;++eye) for(unsigned y=0;y<256;++y) for(unsigned x=0;x<256;++x) {
		unsigned px=origin+x,py=origin+y;
		auto d=wivrn::nxwarp_direct::read32(nfview->descriptors,((py/32)*(w/32*2)+eye*(w/32)+px/32)*4);
		if (!(d&(1u<<29))) { assert(wivrn::nxwarp_direct::native_center_weight(x,y)==0.f); continue; }
		unsigned index=(py%32)*32+px%32;
        auto pair=wivrn::nxwarp_direct::read32(nfview->blocks,((d&0x0fffffffu)+index/2)*4);
        auto v=(pair>>(16*(index%2)))&65535;
        unsigned r=(v>>11)&31,g=(v>>5)&63,b=v&31;
        auto rgb=((r<<3)|(r>>2))<<16 | ((g<<2)|(g>>4))<<8 | (b<<3)|(b>>2);
        if (!(d&0x10000000u)) rgb=wivrn::nxwarp_direct::read32(nfview->blocks,((d&0xfffffff)+index)*4);
		if (wivrn::nxwarp_direct::native_center_weight(x, y) == 1.f)
			assert(rgb==native_pixels[eye*256*256+y*256+x]);
	}
	std::printf("native centre: circular 64px-radius core exact, envelope %zu bytes\n",wire.size());
	native_codec.reset();
	#endif
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
