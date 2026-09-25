/* SPDX-License-Identifier: GPL-3.0-or-later
 * Direct RGB blocks: GPU-only source processing, independent frames.
 */
#include "nxwarp_codec.h"
#include "nxwarp_compression_credit.h"
#include "nxwarp_direct_admission.h"
#include "nxwarp_direct_checkerboard.h"
#include "nxwarp_direct_compression_cache.h"
#include "nxwarp_direct_layout.h"
#include "nxwarp_direct_lz4.h"
#include "nxwarp_direct_motion.h"
#include "nxwarp_direct_motion_regions.h"
#include "nxwarp_direct_native.h"
#include "nxwarp_direct_zstd.h"
#include "util/u_logging.h"
#include "wivrn-server_shaders.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace wivrn
{
namespace
{
void check(VkResult r, const char * what)
{
	if (r != VK_SUCCESS)
		throw std::runtime_error(std::string("NX direct: ") + what + " (" + std::to_string(r) + ")");
}
nxwarp_direct::layout direct_geometry(const nxwarp_codec_config & c)
{
	nxwarp_direct::layout l{c.width, c.height, c.eyes, c.direct_native_center};
	l.checkerboard = c.direct_checkerboard;
	auto enabled = [](const char * name) {
		const auto v = std::getenv(name);
		return v && std::strcmp(v, "1") == 0;
	};
	l.packed_native = enabled("NX_DIRECT_NATIVE_RGB565") && !enabled("NX_DIRECT_NATIVE_RGB888");
	l.zstd = l.native_center && enabled("NX_DIRECT_ZSTD");
	l.predictor = l.zstd && enabled("NX_DIRECT_PREDICTOR");
	l.motion = l.native_center && !l.packed_native && l.zstd && l.predictor && !l.checkerboard && enabled("NX_DIRECT_MOTION");
	l.motion_regions = l.motion && enabled("NX_DIRECT_MOTION_REGIONS");
	return l;
}
class direct_codec final : public nxwarp_codec
{
	nxwarp_direct::layout geometry;
	uint32_t source_width, source_height;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	struct buffer
	{
		VkBuffer handle{};
		VkDeviceMemory memory{};
		void * mapped{};
		VkDeviceSize size{};
	} jobs, output;
	VkDescriptorSetLayout set_layout{};
	VkDescriptorPool descriptor_pool{};
	VkDescriptorSet descriptor{};
	VkPipelineLayout pipeline_layout{};
	VkPipeline pipeline{};
	VkCommandPool command_pool{};
	VkCommandBuffer command{};
	VkFence fence{};
	VkShaderModule module{};
	std::map<std::pair<VkImage, uint32_t>, std::array<VkImageView, 2>> views;
	std::vector<uint8_t> header;
	bool lz4_enabled = false;
	std::atomic_bool lz4_hc = false;
	std::vector<uint8_t> compressed, cached_raw, native_frame, zstd_compressed, zstd_predicted, predictor_scratch;
	// One workspace per encoder; each call still produces an independent frame.
	std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> zstd_context{nullptr, ZSTD_freeCCtx};
	std::vector<uint8_t> motion_baseline;
	std::vector<uint8_t> checker_frame;
	uint64_t checker_sequence = 0;
	nxwarp_direct::exact_compression_cache compression_cache;
	nxwarp_direct::motion_reference_cache motion_references;
	std::vector<uint8_t> motion_wire;
	uint16_t wire_frame_id = 0, held_ack_base = 0;
	uint32_t held_ack_mask = 0;
	bool wire_frame_id_valid = false, held_ack_valid = false;
	uint64_t motion_attempts = 0, motion_selected = 0, motion_saved_bytes = 0;
	uint64_t motion_regions_selected = 0;
	bool compression_cache_enabled = true;
	std::span<const uint32_t> native_pixels;
	float native_radius = 0;
	bool credit_enabled = false, plan_dirty = false;
	nxwarp_direct::compression_credit credit;
	std::span<const uint8_t> pack_detail(std::span<const uint8_t> raw, std::vector<uint8_t> & out, bool hc)
	{
		const nxwarp_direct::compression_cache_policy policy{
			hc, geometry.zstd, geometry.width, geometry.height, geometry.eyes, geometry.predictor};
		if (compression_cache_enabled && compression_cache.lookup(raw, policy, out))
			return out;
		auto fast = hc ? nxwarp_direct::compress_lz4_hc(raw, out) : nxwarp_direct::compress_lz4(raw, out);
		if (geometry.zstd)
		{
			auto dense = nxwarp_direct::compress_zstd(raw, zstd_compressed, zstd_context.get());
			if (geometry.predictor)
			{
				auto predicted = nxwarp_direct::compress_zstd_predicted(raw, zstd_predicted, predictor_scratch, zstd_context.get());
				// Spend PC work only when it removes at least 5% more wire bytes.
				if (predicted.size() * 100 <= dense.size() * 95)
					dense = predicted;
			}
			if (dense.size() * 100 <= fast.size() * 90)
			{
				if (compression_cache_enabled)
					compression_cache.store(raw, policy, dense);
				return dense;
			}
		}
		if (compression_cache_enabled)
			compression_cache.store(raw, policy, fast);
		return fast;
	}
	std::span<const uint8_t> pack_detail_motion(std::span<const uint8_t> raw, bool hc)
	{
		auto baseline = pack_detail(raw, compressed, hc);
		if (!geometry.motion)
			return baseline;

		// Cache only the exact, decoder-ready NXDF bytes; failed sends are harmless
		// because a reference is usable only after the matching positive ACK.
		const bool checker = nxwarp_direct::checker_frame(raw);
		const bool current_cached = !checker && wire_frame_id_valid &&
		                            motion_references.put(geometry, wire_frame_id, raw);
		if (checker || !wire_frame_id_valid || (wire_frame_id & 7u) == 0 || !held_ack_valid)
			return baseline;
		const auto * current = motion_references.find(wire_frame_id);
		if (!current_cached || !current)
			return baseline;

		const nxwarp_direct::motion_reference * reference = nullptr;
		uint16_t reference_id = 0;
		uint16_t age_best = UINT16_MAX;
		for (uint32_t k = 0; k < 32; ++k)
		{
			if (!(held_ack_mask & (1u << k)))
				continue;
			const uint16_t id = uint16_t(held_ack_base - k);
			const uint16_t age = uint16_t(wire_frame_id - id);
			if (!age || age > 8 || age >= age_best)
				continue;
			if (const auto * candidate = motion_references.find(id))
			{
				reference = candidate;
				reference_id = id;
				age_best = age;
			}
		}
		if (!reference)
			return baseline;

		// Preserve the independent incumbent before any predicted compression
		// reuses its buffers. Keep a winning global candidate exactly as before;
		// regional search is useful when one shift cannot explain the centre.
		motion_baseline.assign(baseline.begin(), baseline.end());
		bool selected_regions = false;
		auto try_residual = [&](std::vector<uint8_t> residual,
		                        const nxwarp_direct::motion_region_vectors & vectors, bool regions) {
			if (residual.empty()) return false;
			const auto body = nxwarp_direct::compress_zstd_predicted(residual, zstd_predicted, predictor_scratch, zstd_context.get());
			motion_wire = regions ? nxwarp_direct::make_motion_regions_wire(vectors, reference_id, body) :
			                        nxwarp_direct::make_motion_wire(vectors[0].dx, vectors[0].dy, reference_id, body);
			return !motion_wire.empty() && motion_wire.size() * 100 <= motion_baseline.size() * 90;
		};
		const auto vector = nxwarp_direct::estimate_motion(geometry, reference->raw, reference->native, raw, current->native);
		const bool global_valid = vector.hits >= 256 && std::isfinite(vector.sad);
		nxwarp_direct::motion_region_vectors global_vectors{};
		global_vectors.fill(vector);
		bool selected = global_valid && try_residual(nxwarp_direct::motion_residual(geometry, reference->raw, reference->native,
		                                                                          raw, current->native, vector.dx, vector.dy),
		                                            global_vectors, false);
		if (!selected && geometry.motion_regions)
		{
			const auto vectors = nxwarp_direct::estimate_motion_regions(geometry, reference->raw, reference->native, raw, current->native);
			const bool valid = std::all_of(vectors.begin(), vectors.end(), [](const auto & v) {
				return v.hits >= 32 && std::isfinite(v.sad);
			});
			const bool diverse = std::any_of(vectors.begin() + 1, vectors.end(), [&](const auto & v) {
				return v.dx != vectors[0].dx || v.dy != vectors[0].dy;
			});
			// No second compression of an identical prediction.
			const bool same_global = global_valid && !diverse && vectors[0].dx == vector.dx && vectors[0].dy == vector.dy;
			if (valid && !same_global)
			{
				auto residual = diverse ? nxwarp_direct::motion_regions_residual(geometry, reference->raw, reference->native, raw, current->native, vectors) :
				                          nxwarp_direct::motion_residual(geometry, reference->raw, reference->native, raw, current->native, vectors[0].dx, vectors[0].dy);
				selected = try_residual(std::move(residual), vectors, diverse);
				selected_regions = selected && diverse;
			}
		}
		++motion_attempts;
		if (selected)
		{
			++motion_selected;
			motion_regions_selected += selected_regions;
			motion_saved_bytes += motion_baseline.size() - motion_wire.size();
		}
		if ((motion_attempts & 255u) == 0)
			U_LOG_I("nxwarp: direct motion selected=%llu/%llu saved=%llu bytes regional=%llu",
			        (unsigned long long)motion_selected,
			        (unsigned long long)motion_attempts,
			        (unsigned long long)motion_saved_bytes,
			        (unsigned long long)motion_regions_selected);
		return selected ? std::span<const uint8_t>(motion_wire) : std::span<const uint8_t>(motion_baseline);
	}
	std::span<const uint8_t> finish_frame(std::span<const uint8_t> wire)
	{
		if (wire_admission)
			admission.settle(int64_t(std::ceil(wire.size() * 8.0 * 1.25 * 1e9 / total_target)));
		return wire;
	}
	size_t native_extra() const
	{
		return geometry.native_center ? nxwarp_direct::native_center_extra(native_radius, geometry.packed_native) : 0u;
	}
	std::vector<uint8_t> frame;
	std::unique_ptr<direct_codec> safety_codec;
	bool safety_enabled = false;
	nxwarp_direct::plan plan;
	std::vector<nxwarp_tile_desc> tile_info;
	uint32_t target = 0;
	uint32_t total_target = 0;
	float refresh = 90;
	nxwarp_direct::frame_admission admission;
	bool wire_admission = false;
	bool submitted = false;

	void allocate(buffer & b, VkDeviceSize bytes)
	{
		b.size = bytes;
		VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		                      .size = bytes,
		                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
		check(vkCreateBuffer(device, &ci, nullptr, &b.handle), "create buffer");
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, b.handle, &req);
		VkPhysicalDeviceMemoryProperties props;
		vkGetPhysicalDeviceMemoryProperties(physical, &props);
		uint32_t type = UINT32_MAX;
		constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t i = 0; i < props.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
			{
				type = i;
				break;
			}
		if (type == UINT32_MAX)
			throw std::runtime_error("NX direct: no coherent host-visible storage memory");
		VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
		check(vkAllocateMemory(device, &ai, nullptr, &b.memory), "allocate buffer");
		check(vkBindBufferMemory(device, b.handle, b.memory, 0), "bind buffer");
		check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map buffer");
	}
	void release(buffer & b)
	{
		if (b.mapped)
			vkUnmapMemory(device, b.memory);
		if (b.handle)
			vkDestroyBuffer(device, b.handle, nullptr);
		if (b.memory)
			vkFreeMemory(device, b.memory, nullptr);
	}
	void update_plan()
	{
		const uint32_t quality_target = uint32_t(double(target) * (credit_enabled ? credit.value() : 1.0));
		native_radius = geometry.native_center ? nxwarp_direct::native_center_radius(quality_target, refresh) : 0.f;
		const auto reserved = uint32_t(std::ceil(native_extra() * 8.0 * 1.25 * refresh));
		plan = nxwarp_direct::select_plan(geometry, quality_target > reserved ? quality_target - reserved : 1u, refresh);
		tile_info.resize(geometry.tile_count());
		for (uint32_t i = 0; i < tile_info.size(); i++)
			tile_info[i] = {.index = i, .qp = 0, .mode = 0, .res_level = uint8_t(plan.descriptors[i] >> 30), .ref_delta = 3};
		std::memcpy(jobs.mapped, plan.jobs.data(), plan.jobs.size() * sizeof(plan.jobs[0]));
	}
	size_t planned_bytes() const
	{
		if (geometry.checkerboard && checker_sequence > 0)
			return 16 + geometry.tile_count() * 4 + (plan.words / 5) * 12 + native_extra() / 2;
		return plan.bytes() + native_extra();
	}
	std::array<VkImageView, 2> image_views(VkImage image, uint32_t layers)
	{
		auto key = std::make_pair(image, layers);
		if (auto it = views.find(key); it != views.end())
			return it->second;
		std::array<VkImageView, 2> result{};
		try
		{
			for (uint32_t i = 0; i < 2; i++)
			{
				VkImageViewUsageCreateInfo usage{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_STORAGE_BIT};
				VkImageViewCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = i ? VK_FORMAT_R8G8_UINT : VK_FORMAT_R8_UINT, .subresourceRange = {VkImageAspectFlags(i ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT), 0, 1, 0, layers}};
				check(vkCreateImageView(device, &ci, nullptr, &result[i]), "source plane view");
			}
		}
		catch (...)
		{
			for (auto v: result)
				if (v)
					vkDestroyImageView(device, v, nullptr);
			throw;
		}
		views.emplace(key, result);
		return result;
	}

public:
	direct_codec(const nxwarp_codec_config & c, VkPhysicalDevice p, VkDevice d, VkQueue q, uint32_t f) :
	        geometry{direct_geometry(c)}, source_width(c.source_width ? c.source_width : c.width), source_height(c.source_height ? c.source_height : c.height), physical(p), device(d), queue(q), family(f), header(nxwarp_direct::stream_header(geometry, c.trusted_lan, c.direct_lz4, c.safety)), lz4_enabled(c.direct_lz4), safety_enabled(c.safety)
	{
		lz4_hc = false;
		if (geometry.zstd)
			zstd_context.reset(ZSTD_createCCtx()); // Allocation failure retains the one-shot fallback.
		const char * cache_env = std::getenv("NX_DIRECT_COMPRESSION_CACHE");
		compression_cache_enabled = !cache_env || std::strcmp(cache_env, "0") != 0;
		const char * wire_env = std::getenv("NX_DIRECT_WIRE_ADMISSION");
		wire_admission = lz4_enabled && (!wire_env || std::strcmp(wire_env, "0") != 0);
		const char * credit_env = std::getenv("NX_DIRECT_COMPRESSION_CREDIT");
		credit_enabled = geometry.native_center && geometry.zstd && credit_env && std::strcmp(credit_env, "1") == 0;
		if (c.direct_native_center && (!c.safety || !c.direct_lz4 || c.eyes != 2 || c.width < 256 || c.height < 256))
			throw std::runtime_error("NX direct native centre requires paired LZ4 safety stream >=256 pixels");
		if (header.empty())
			throw std::runtime_error("NX direct: eye geometry must be multiples of 32, <=4096");
		if (geometry.motion)
			U_LOG_I("nxwarp: direct motion residual enabled (NX_DIRECT_MOTION=1; native RGB888 predictor stream)");
		if (geometry.motion_regions)
			U_LOG_I("nxwarp: regional motion residual enabled (NX_DIRECT_MOTION_REGIONS=1; four fixed native-centre regions)");
		if (safety_enabled)
		{
			const uint32_t sw = ((source_width / 4 + 31) / 32) * 32;
			const uint32_t sh = ((source_height / 4 + 31) / 32) * 32;
			if (!sw || !sh || sw > geometry.width || sh > geometry.height)
				throw std::runtime_error("NX direct: invalid safety geometry");
			nxwarp_codec_config sc = c;
			sc.width = sw;
			sc.height = sh;
			sc.safety = false;
			sc.direct_native_center = false;
			sc.direct_checkerboard = false;
			sc.direct_lz4 = c.direct_lz4;
			sc.source_width = source_width;
			sc.source_height = source_height;
			safety_codec = std::make_unique<direct_codec>(sc, p, d, q, f);
		}
	}
	void set_native_center(std::span<const uint32_t> pixels) override
	{
		native_pixels = pixels;
	}
	void set_lz4_hc(bool enabled) override
	{
		lz4_hc = enabled;
		if (safety_codec)
			safety_codec->set_lz4_hc(enabled);
	}
	void initialize()
	{
		allocate(jobs, VkDeviceSize(geometry.tile_count()) * 16 * 16);
		allocate(output, geometry.max_frame_bytes());
		std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
		for (uint32_t i = 0; i < 4; i++)
			bindings[i] = {i, i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo sl{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings.data()};
		check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &set_layout), "descriptor layout");
		VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28};
		VkPipelineLayoutCreateInfo pl{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
		check(vkCreatePipelineLayout(device, &pl, nullptr, &pipeline_layout), "pipeline layout");
		const auto & code = ::shaders.at("direct_blocks_encode");
		VkShaderModuleCreateInfo sm{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code.size() * 4, .pCode = code.data()};
		check(vkCreateShaderModule(device, &sm, nullptr, &module), "shader module");
		VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		                               .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
		                               .layout = pipeline_layout};
		check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline), "compute pipeline");
		std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
		VkDescriptorPoolCreateInfo dp{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes.data()};
		check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptor_pool), "descriptor pool");
		VkDescriptorSetAllocateInfo ds{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
		check(vkAllocateDescriptorSets(device, &ds, &descriptor), "descriptor set");
		VkCommandPoolCreateInfo cp{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
		check(vkCreateCommandPool(device, &cp, nullptr, &command_pool), "command pool");
		VkCommandBufferAllocateInfo ca{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
		check(vkAllocateCommandBuffers(device, &ca, &command), "command buffer");
		VkFenceCreateInfo fc{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		check(vkCreateFence(device, &fc, nullptr, &fence), "fence");
		update_plan();
		if (safety_codec)
			safety_codec->initialize();
		set_target_bitrate(500'000'000u, 90.f);
	}
	~direct_codec() override
	{
		if (submitted)
			vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		for (auto & [key, v]: views)
			for (auto view: v)
				vkDestroyImageView(device, view, nullptr);
		if (fence)
			vkDestroyFence(device, fence, nullptr);
		if (command_pool)
			vkDestroyCommandPool(device, command_pool, nullptr);
		if (pipeline)
			vkDestroyPipeline(device, pipeline, nullptr);
		if (module)
			vkDestroyShaderModule(device, module, nullptr);
		if (descriptor_pool)
			vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
		if (pipeline_layout)
			vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
		if (set_layout)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		release(output);
		release(jobs);
	}
	bool direct_blocks() const override
	{
		return true;
	}
	void set_wire_frame_id(uint16_t id) override
	{
		if (!geometry.motion)
			return;
		wire_frame_id = id;
		wire_frame_id_valid = true;
		// An encode that later fails to send may be retried with this same wire id.
		// Discard any uncommitted bytes left under that id before rebuilding it.
		motion_references.erase(id);
	}
	void set_direct_held_ack(uint16_t base, uint32_t mask) override
	{
		held_ack_base = base;
		held_ack_mask = mask;
		held_ack_valid = true;
	}
	void forget_direct_frame(uint16_t id) override
	{
		motion_references.erase(id);
	}
	void reset_direct_references() override
	{
		motion_references.clear();
		held_ack_base = 0;
		held_ack_mask = 0;
		held_ack_valid = false;
		wire_frame_id_valid = false;
	}
	bool accepts_image() const override
	{
		return true;
	}
	void set_target_bitrate(uint32_t bps, float fps) override
	{
		bps = std::clamp(bps, 1u, 1'000'000'000u);
		fps = std::isfinite(fps) ? std::clamp(fps, 1.f, 240.f) : 90.f;
		if (bps == total_target && fps == refresh)
			return;
		const bool refresh_changed = fps != refresh;
		total_target = bps;
		refresh = fps;
		if (safety_codec)
		{
			const uint32_t safety_bps = std::min(20'000'000u, bps / 4u);
			safety_codec->set_target_bitrate(safety_bps, fps);
			bps -= safety_bps;
		}
		target = bps;
		if (refresh_changed)
			credit.reset();
		else
			credit.budget_changed();
		plan_dirty = false;
		update_plan();
		admission.reset();
	}
	bool admit_frame(int64_t now) override
	{
		if (plan_dirty)
		{
			update_plan();
			plan_dirty = false;
		}
		const auto lz4_overhead = [](size_t n) {
			return size_t(16) + ((n + nxwarp_direct::lz4_chunk_bytes - 1) / nxwarp_direct::lz4_chunk_bytes) * 12;
		};
		const size_t safety_bytes = safety_codec ? safety_codec->planned_bytes() : 0;
		const size_t raw_bytes = size_t(std::ceil(planned_bytes() / (credit_enabled ? credit.value() : 1.0))) + safety_bytes + (safety_codec ? 32 : 0);
		const size_t wire_overhead = lz4_enabled ? lz4_overhead(planned_bytes()) + (safety_codec ? lz4_overhead(safety_bytes) : 0) : 0;
		int64_t interval = int64_t(std::ceil((raw_bytes + wire_overhead) * 8.0 * 1.25 * 1e9 / total_target));
		return admission.admit(now, interval);
	}
	std::span<const uint8_t> stream_header() const override
	{
		return header;
	}
	void tile_grid(uint32_t & cols, uint32_t & rows) const override
	{
		cols = geometry.width / 32 * geometry.eyes;
		rows = geometry.height / 32;
	}
	void set_view(const nxwarp_codec_view &) override {}
	bool set_qp(uint32_t) override
	{
		return false;
	}
	void set_received_tiles(std::span<const uint8_t>) override {}
	std::span<const uint8_t> encode(const uint8_t *, size_t, const uint8_t *, const uint8_t *, size_t) override
	{
		return {};
	}
	std::span<const nxwarp_tile_desc> tiles() const override
	{
		return tile_info;
	}
	std::string description() const override
	{
		if (geometry.checkerboard)
			return "NX direct checkerboard half-refresh (experimental)";
		if (geometry.zstd)
			return "NX direct RGB blocks + adaptive LZ4/Zstd (independent frames)";
		return lz4_enabled ? "NX direct RGB blocks + LZ4 (64 KiB chunks, 5% minimum saving)" : "NX direct RGB blocks v1 (GPU source, independent frames)";
	}
	std::span<const uint8_t> encode_image(VkImage image, uint32_t layer) override
	{
		return encode_image_pair(image, layer, layer, 0);
	}
	std::span<const uint8_t> encode_image_pair(VkImage image, uint32_t left, uint32_t right, uint64_t) override
	{
		if (plan_dirty)
		{
			update_plan();
			plan_dirty = false;
		}
		const bool use_hc = lz4_hc.load();
		if (submitted)
			throw std::runtime_error("NX direct: unfinished GPU work");
		std::span<const uint8_t> safety_raw;
		if (safety_codec)
			safety_raw = safety_codec->encode_image_pair(image, left, right, 0);
		auto source = image_views(image, std::max(left, right) + 1);
		auto * words = static_cast<uint32_t *>(output.mapped);
		words[0] = nxwarp_direct::frame_magic;
		words[1] = nxwarp_direct::version;
		words[2] = geometry.tile_count();
		words[3] = plan.words;
		std::memcpy(words + 4, plan.descriptors.data(), plan.descriptors.size() * 4);
		std::array<VkDescriptorImageInfo, 2> images{{{VK_NULL_HANDLE, source[0], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, source[1], VK_IMAGE_LAYOUT_GENERAL}}};
		std::array<VkDescriptorBufferInfo, 2> buffers{{{jobs.handle, 0, jobs.size}, {output.handle, 0, output.size}}};
		std::array<VkWriteDescriptorSet, 4> writes{};
		for (uint32_t i = 0; i < 4; i++)
			writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor, .dstBinding = i, .descriptorCount = 1, .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pImageInfo = i < 2 ? &images[i] : nullptr, .pBufferInfo = i >= 2 ? &buffers[i - 2] : nullptr};
		vkUpdateDescriptorSets(device, 4, writes.data(), 0, nullptr);
		check(vkResetCommandBuffer(command, 0), "reset commands");
		VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		check(vkBeginCommandBuffer(command, &begin), "begin commands");
		VkMemoryBarrier input{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &input, 0, nullptr, 0, nullptr);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor, 0, nullptr);
		uint32_t push[7] = {geometry.width, geometry.height, uint32_t(plan.jobs.size()), left, right, source_width, source_height};
		vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(command, (push[2] + 63) / 64, 1, 1);
		VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
		check(vkEndCommandBuffer(command), "end commands");
		check(vkResetFences(device, 1, &fence), "reset fence");
		VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
		check(vkQueueSubmit(queue, 1, &submit, fence), "submit");
		submitted = true;
		check(vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000), "encode timeout");
		submitted = false;
		std::span<const uint8_t> raw{static_cast<const uint8_t *>(output.mapped), plan.bytes()};
		if (lz4_enabled || geometry.native_center)
		{
			// Coherent mapped output is not necessarily CPU-cached. Stage it once
			// before compression and native sampling revisit its bytes.
			cached_raw.assign(raw.begin(), raw.end());
			raw = cached_raw;
		}
		if (geometry.native_center)
		{
			if (native_pixels.size() != 2u * 256u * 256u)
				throw std::runtime_error("NX direct native centre source missing");
			raw = nxwarp_direct::native_center_frame(geometry, raw, native_pixels, native_frame, native_radius);
		}
		if (geometry.checkerboard && checker_sequence++ > 0)
		{
			raw = nxwarp_direct::checkerboard_frame(geometry, raw, checker_frame, uint32_t((checker_sequence - 1) & 1u));
			if (raw.empty())
				throw std::runtime_error("NX direct: invalid checkerboard source frame");
		}
		if (!safety_codec)
		{
			if (!lz4_enabled)
				return finish_frame(raw);
			return finish_frame(geometry.motion ? pack_detail_motion(raw, use_hc) : pack_detail(raw, compressed, use_hc));
		}
		std::span<const uint8_t> safety_wire = safety_raw;
		std::span<const uint8_t> detail_wire = raw;
		if (lz4_enabled)
		{
			// The child owns and caches its optional LZ4 result, so do not
			// revisit its mapped Vulkan memory or wrap an existing NXDL envelope.
			safety_wire = safety_raw;
			detail_wire = geometry.motion ? pack_detail_motion(raw, use_hc) : pack_detail(raw, compressed, use_hc);
		}
		frame.clear();
		frame.reserve(32 + safety_wire.size() + detail_wire.size());
		for (uint32_t v: {uint32_t(0x5344584e), 1u, safety_codec->geometry.width, safety_codec->geometry.height, geometry.eyes, uint32_t(safety_wire.size()), uint32_t(detail_wire.size()), 0u})
			nxwarp_direct::append32(frame, v);
		frame.insert(frame.end(), safety_wire.begin(), safety_wire.end());
		frame.insert(frame.end(), detail_wire.begin(), detail_wire.end());
		if (credit_enabled && credit.observe(raw.size(), detail_wire.size(), frame.size(), double(total_target) / (8.0 * 1.25 * refresh)))
		{
			// Keep this frame's tile metadata intact until the next admission/encode.
			plan_dirty = true;
		}
		return finish_frame(frame);
	}
};
} // namespace
std::unique_ptr<nxwarp_codec> nxwarp_codec::make_direct(const nxwarp_codec_config & c, VkInstance, VkPhysicalDevice p, VkDevice d, VkQueue q, uint32_t f)
{
	auto result = std::make_unique<direct_codec>(c, p, d, q, f);
	result->initialize();
	return result;
}
} // namespace wivrn
