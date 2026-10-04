#include "astc_gpu_timing.h"

#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr uint32_t element_count = 16'384;
constexpr VkDeviceSize buffer_bytes = VkDeviceSize(element_count) * sizeof(uint32_t);
constexpr uint32_t warmup_runs = 4;
constexpr uint32_t measured_runs = 24;

void check(VkResult result, const char * where)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error(std::string(where) + " failed: " + std::to_string(result));
}

std::vector<uint32_t> read_spirv(const char * path)
{
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input)
		throw std::runtime_error("cannot open SPIR-V file");
	const auto end = input.tellg();
	if (end <= 0)
		throw std::runtime_error("invalid SPIR-V size");
	const auto size = static_cast<std::streamsize>(end);
	if (size % sizeof(uint32_t) != 0)
		throw std::runtime_error("invalid SPIR-V size");
	std::vector<uint32_t> words(size_t(size) / sizeof(uint32_t));
	input.seekg(0);
	input.read(reinterpret_cast<char *>(words.data()), size);
	if (!input)
		throw std::runtime_error("cannot read SPIR-V file");
	return words;
}

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t bits, VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
		if ((bits & (1u << i)) &&
		    (properties.memoryTypes[i].propertyFlags & required) == required)
			return i;
	throw std::runtime_error("required Vulkan memory type unavailable");
}

struct buffer_t
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
};

buffer_t make_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                     VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
{
	buffer_t out;
	VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer_info.size = size;
	buffer_info.usage = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	check(vkCreateBuffer(device, &buffer_info, nullptr, &out.buffer), "vkCreateBuffer");
	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
	VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = find_memory_type(physical, requirements.memoryTypeBits, properties);
	check(vkAllocateMemory(device, &allocate_info, nullptr, &out.memory), "vkAllocateMemory");
	check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory");
	return out;
}

} // namespace

int main(int argc, char ** argv)
try
{
	if (argc != 2)
		throw std::runtime_error("usage: gpu_timestamp_scope <scope.comp.spv>");
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "gpu timestamp scope check";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance_info.pApplicationInfo = &app;
	VkInstance instance = VK_NULL_HANDLE;
	check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");

	uint32_t device_count = 0;
	check(vkEnumeratePhysicalDevices(instance, &device_count, nullptr), "vkEnumeratePhysicalDevices");
	if (!device_count)
		throw std::runtime_error("no Vulkan physical device");
	std::vector<VkPhysicalDevice> devices(device_count);
	check(vkEnumeratePhysicalDevices(instance, &device_count, devices.data()), "vkEnumeratePhysicalDevices");

	VkPhysicalDevice physical = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	VkPhysicalDeviceProperties properties{};
	VkQueueFamilyProperties selected_queue{};
	for (VkPhysicalDevice candidate: devices)
	{
		VkPhysicalDeviceProperties candidate_properties{};
		vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
		if (VK_API_VERSION_MAJOR(candidate_properties.apiVersion) < 1 ||
		    (VK_API_VERSION_MAJOR(candidate_properties.apiVersion) == 1 &&
		     VK_API_VERSION_MINOR(candidate_properties.apiVersion) < 3))
			continue;
		uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
		for (uint32_t i = 0; i < count; ++i)
			if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && families[i].timestampValidBits)
			{
				physical = candidate;
				queue_family = i;
				properties = candidate_properties;
				selected_queue = families[i];
				break;
			}
		if (physical)
			break;
	}
	if (!physical)
		throw std::runtime_error("no Vulkan 1.3 compute queue with timestamp support");
	if (!(properties.limits.timestampPeriod > 0) || !std::isfinite(properties.limits.timestampPeriod))
		throw std::runtime_error("invalid device timestampPeriod");

	VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	supported13.pNext = &supported12;
	VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	supported.pNext = &supported13;
	vkGetPhysicalDeviceFeatures2(physical, &supported);
	if (!supported12.timelineSemaphore || !supported13.synchronization2)
		throw std::runtime_error("timeline semaphore or synchronization2 unsupported");

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue_info.queueFamilyIndex = queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	enabled12.timelineSemaphore = VK_TRUE;
	VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	enabled13.synchronization2 = VK_TRUE;
	enabled13.pNext = &enabled12;
	VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	device_info.pNext = &enabled13;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	VkDevice device = VK_NULL_HANDLE;
	check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(device, queue_family, 0, &queue);
	std::cerr << "device=" << properties.deviceName << " queue_family=" << queue_family
	          << " timestampValidBits=" << selected_queue.timestampValidBits
	          << " timestampPeriod_ns=" << properties.limits.timestampPeriod << '\n';

	const auto source = make_buffer(physical, device, buffer_bytes,
	                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	const auto readback = make_buffer(physical, device, buffer_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	void * mapped = nullptr;
	check(vkMapMemory(device, readback.memory, 0, buffer_bytes, 0, &mapped), "vkMapMemory");

	auto spirv = read_spirv(argv[1]);
	VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	shader_info.codeSize = spirv.size() * sizeof(uint32_t);
	shader_info.pCode = spirv.data();
	VkShaderModule shader = VK_NULL_HANDLE;
	check(vkCreateShaderModule(device, &shader_info, nullptr, &shader), "vkCreateShaderModule");

	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo set_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	set_layout_info.bindingCount = 1;
	set_layout_info.pBindings = &binding;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	check(vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
	VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(uint32_t)};
	VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts = &set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges = &push_range;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout), "vkCreatePipelineLayout");
	VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = shader;
	stage.pName = "main";
	VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	pipeline_info.stage = stage;
	pipeline_info.layout = pipeline_layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "vkCreateComputePipelines");

	VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
	VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	check(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool), "vkCreateDescriptorPool");
	VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	set_info.descriptorPool = descriptor_pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts = &set_layout;
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	check(vkAllocateDescriptorSets(device, &set_info, &descriptor_set), "vkAllocateDescriptorSets");
	VkDescriptorBufferInfo descriptor_buffer{source.buffer, 0, buffer_bytes};
	VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = descriptor_set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &descriptor_buffer;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

	VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	query_info.queryCount = 2;
	VkQueryPool query_pool = VK_NULL_HANDLE;
	check(vkCreateQueryPool(device, &query_info, nullptr, &query_pool), "vkCreateQueryPool");
	VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	semaphore_info.pNext = &timeline_type;
	VkSemaphore semaphore = VK_NULL_HANDLE;
	check(vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore), "vkCreateSemaphore");
	VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VkFence fence = VK_NULL_HANDLE;
	check(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
	VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool_info.queueFamilyIndex = queue_family;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	check(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	command_info.commandPool = command_pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	check(vkAllocateCommandBuffers(device, &command_info, &command_buffer), "vkAllocateCommandBuffers");

	std::cout << "run,delay_ms,warmup,cpu_fence_wait_ms,gpu_compute_through_readback_ms,begin_ticks,end_ticks,readback_ok\n";
	for (uint32_t run = 0; run < warmup_runs + measured_runs; ++run)
	{
		const uint32_t delay_ms = (run & 1) ? 10 : 0;
		const bool warmup = run < warmup_runs;
		check(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer");
		VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		check(vkBeginCommandBuffer(command_buffer, &begin_info), "vkBeginCommandBuffer");
		vkCmdResetQueryPool(command_buffer, query_pool, 0, 2);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
		const std::array<uint32_t, 2> push_constants{element_count, 0x13579bdfu ^ (run * 0x9e3779b9u)};
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), push_constants.data());
		vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, query_pool, 0);
		vkCmdDispatch(command_buffer, (element_count + 63) / 64, 1, 1);
		VkBufferMemoryBarrier2 compute_to_copy{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
		compute_to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		compute_to_copy.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
		compute_to_copy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		compute_to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		compute_to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		compute_to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		compute_to_copy.buffer = source.buffer;
		compute_to_copy.size = buffer_bytes;
		VkDependencyInfo compute_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		compute_dependency.bufferMemoryBarrierCount = 1;
		compute_dependency.pBufferMemoryBarriers = &compute_to_copy;
		vkCmdPipelineBarrier2(command_buffer, &compute_dependency);
		VkBufferCopy copy{0, 0, buffer_bytes};
		vkCmdCopyBuffer(command_buffer, source.buffer, readback.buffer, 1, &copy);
		VkBufferMemoryBarrier2 copy_to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
		copy_to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		copy_to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		copy_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		copy_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		copy_to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		copy_to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		copy_to_host.buffer = readback.buffer;
		copy_to_host.size = buffer_bytes;
		VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		host_dependency.bufferMemoryBarrierCount = 1;
		host_dependency.pBufferMemoryBarriers = &copy_to_host;
		vkCmdPipelineBarrier2(command_buffer, &host_dependency);
		vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, query_pool, 1);
		check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

		check(vkResetFences(device, 1, &fence), "vkResetFences");
		const uint64_t timeline_value = uint64_t(run) + 1;
		VkSemaphoreSubmitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
		wait_info.semaphore = semaphore;
		wait_info.value = timeline_value;
		wait_info.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		VkCommandBufferSubmitInfo command_submit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
		command_submit.commandBuffer = command_buffer;
		VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
		submit.waitSemaphoreInfoCount = 1;
		submit.pWaitSemaphoreInfos = &wait_info;
		submit.commandBufferInfoCount = 1;
		submit.pCommandBufferInfos = &command_submit;

		std::mutex signal_mutex;
		std::condition_variable signal_cv;
		bool release_signal = false;
		VkResult signal_result = VK_SUCCESS;
		std::thread signaler([&] {
			{
				std::unique_lock lock(signal_mutex);
				signal_cv.wait(lock, [&] { return release_signal; });
			}
			if (delay_ms)
				std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
			VkSemaphoreSignalInfo signal_info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
			signal_info.semaphore = semaphore;
			signal_info.value = timeline_value;
			signal_result = vkSignalSemaphore(device, &signal_info);
		});
		const VkResult submit_result = vkQueueSubmit2(queue, 1, &submit, fence);
		const auto wait_begin = std::chrono::steady_clock::now();
		{
			std::lock_guard lock(signal_mutex);
			release_signal = true;
		}
		signal_cv.notify_one();
		const VkResult fence_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
		const auto wait_end = std::chrono::steady_clock::now();
		signaler.join();
		check(submit_result, "vkQueueSubmit2");
		check(signal_result, "vkSignalSemaphore");
		check(fence_result, "vkWaitForFences");

		std::array<uint64_t, 2> timestamps{};
		const VkResult query_result = vkGetQueryPoolResults(device, query_pool, 0, 2, sizeof(timestamps),
		                                                    timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
		check(query_result, "vkGetQueryPoolResults without WAIT");
		auto gpu_ms = wivrn::astc_gpu_timing::elapsed_ms(timestamps[0], timestamps[1],
		                                                selected_queue.timestampValidBits,
		                                                properties.limits.timestampPeriod);
		if (!gpu_ms)
			throw std::runtime_error("production elapsed_ms helper rejected GPU timestamps");
		bool readback_ok = true;
		const auto * values = static_cast<const uint32_t *>(mapped);
		for (uint32_t i = 0; i < element_count; ++i)
			if (values[i] != i * 17u + push_constants[1])
			{
				readback_ok = false;
				break;
			}
		if (!readback_ok)
			throw std::runtime_error("readback pattern mismatch");
		const double cpu_wait_ms = std::chrono::duration<double, std::milli>(wait_end - wait_begin).count();
		std::cout << run << ',' << delay_ms << ',' << (warmup ? 1 : 0) << ','
		          << cpu_wait_ms << ',' << *gpu_ms << ',' << timestamps[0] << ',' << timestamps[1] << ",1\n";
	}

	check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
	vkUnmapMemory(device, readback.memory);
	vkDestroyCommandPool(device, command_pool, nullptr);
	vkDestroyFence(device, fence, nullptr);
	vkDestroySemaphore(device, semaphore, nullptr);
	vkDestroyQueryPool(device, query_pool, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
	vkDestroyShaderModule(device, shader, nullptr);
	vkDestroyBuffer(device, readback.buffer, nullptr);
	vkFreeMemory(device, readback.memory, nullptr);
	vkDestroyBuffer(device, source.buffer, nullptr);
	vkFreeMemory(device, source.memory, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	return 0;
}
catch (const std::exception & error)
{
	std::cerr << "ERROR: " << error.what() << '\n';
	return 1;
}
