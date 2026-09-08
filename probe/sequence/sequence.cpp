#include "context.h"
#include "renderer.h"

#include <nxvc/nxvc_vk.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <numeric>
#include <vector>

namespace nx_sequence {

bool VkContext::create() {
    const char* fdm_env = std::getenv("NX_SEQUENCE_FDM");
    const bool want_fdm = fdm_env && std::strcmp(fdm_env, "1") == 0;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nx-sequence-bench";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    if (vkCreateInstance(&ii, nullptr, &instance) != VK_SUCCESS)
        return false;

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || !count)
        return false;
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    for (VkPhysicalDevice candidate : devices) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> queues(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &nq, queues.data());
        for (uint32_t q = 0; q < nq; ++q) {
            if ((queues[q].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                physical = candidate;
                queue_family = q;
                break;
            }
        }
        if (physical)
            break;
    }
    if (!physical)
        return false;

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    VkPhysicalDevice16BitStorageFeatures supported16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT supported_fdm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFragmentDensityMapPropertiesEXT fdm_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
    VkPhysicalDeviceFeatures2 supported2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported2.pNext = &supported16;
    if (want_fdm)
        supported16.pNext = &supported_fdm;
    vkGetPhysicalDeviceFeatures2(physical, &supported2);
    if (!supported.shaderInt16 || !supported.shaderStorageImageExtendedFormats ||
        !supported16.storageBuffer16BitAccess)
        return false;
    if (want_fdm) {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> extensions(ne);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, extensions.data());
        bool available = false;
        for (const auto& extension : extensions)
            if (std::strcmp(extension.extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) ==
                0)
                available = true;
        if (!available || !supported_fdm.fragmentDensityMap ||
            !supported_fdm.fragmentDensityMapNonSubsampledImages)
            return false;
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &fdm_properties;
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        std::fprintf(stderr, "sequence: FDM texel limits %ux%u..%ux%u\n",
                     fdm_properties.minFragmentDensityTexelSize.width,
                     fdm_properties.minFragmentDensityTexelSize.height,
                     fdm_properties.maxFragmentDensityTexelSize.width,
                     fdm_properties.maxFragmentDensityTexelSize.height);
    }

    constexpr float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt16 = VK_TRUE;
    enabled.shaderStorageImageExtendedFormats = VK_TRUE;
    VkPhysicalDevice16BitStorageFeatures enabled16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    enabled16.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT enabled_fdm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    const char* device_extensions[] = {VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME};
    if (want_fdm) {
        enabled_fdm.fragmentDensityMap = VK_TRUE;
        enabled_fdm.fragmentDensityMapNonSubsampledImages = VK_TRUE;
        enabled16.pNext = &enabled_fdm;
    }
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pQueueCreateInfos = &qi;
    di.queueCreateInfoCount = 1;
    di.pEnabledFeatures = &enabled;
    di.pNext = &enabled16;
    if (want_fdm) {
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = device_extensions;
    }
    if (vkCreateDevice(physical, &di, nullptr, &device) != VK_SUCCESS)
        return false;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    fdm_enabled = want_fdm;
    return true;
}

void VkContext::destroy() {
    if (device)
        vkDestroyDevice(device, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
    *this = {};
}

}  // namespace nx_sequence

int main(int argc, char** argv) {
    const char* input = argc > 1 ? argv[1] : "fixture.nxv";
    const size_t requested = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1200;
    const size_t warmup = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 120;
    const char* async_env = std::getenv("NX_SEQUENCE_ASYNC");
    const bool async_submit = async_env && std::strcmp(async_env, "1") == 0;
    const char* repeat_env = std::getenv("NX_SEQUENCE_RENDER_REPEATS");
    char* repeat_end = nullptr;
    const size_t render_repeats = repeat_env ? std::strtoull(repeat_env, &repeat_end, 10) : 1;
    if (render_repeats < 1 || render_repeats > 8 ||
        (repeat_env && (repeat_end == repeat_env || *repeat_end || std::strchr(repeat_env, '-')))) {
        std::fprintf(stderr, "sequence: NX_SEQUENCE_RENDER_REPEATS must be 1..8\n");
        return 2;
    }
    if (!requested || warmup >= requested) {
        std::fprintf(stderr, "sequence: warmup_frames must be less than requested frames\n");
        return 2;
    }
    std::ifstream file(input, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "sequence: cannot open %s\n", input);
        return 2;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    nx_sequence::VkContext vk;
    if (!vk.create()) {
        std::fprintf(stderr, "sequence: Vulkan context unavailable\n");
        vk.destroy();
        return 2;
    }
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.flags = 0;  // GPU display consumer: no per-frame CPU pixel readback.
    ci.instance = vk.instance;
    ci.physical_device = vk.physical;
    ci.device = vk.device;
    ci.queue = vk.queue;
    ci.queue_family = vk.queue_family;
    ci.output_format = NXVC_VKD_OUT_AUTO;
    nxvc_vk_decoder* decoder = nullptr;
    if (nxvc_vk_decoder_create(&ci, &decoder) != NXVC_VKD_OK) {
        std::fprintf(stderr, "sequence: decoder create failed\n");
        vk.destroy();
        return 2;
    }
    size_t header_bytes = 0;
    if (nxvc_vk_decoder_parse_stream_header(decoder, bytes.data(), bytes.size(), &header_bytes) !=
            NXVC_VKD_OK ||
        nxvc_vk_decoder_set_atlas_view(decoder, NXVC_VKD_ATLAS_VIEW_R8) != NXVC_VKD_OK) {
        std::fprintf(stderr, "sequence: input is not an R8 atlas stream\n");
        nxvc_vk_decoder_destroy(decoder);
        vk.destroy();
        return 2;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vk.physical, &properties);
    std::printf("device %s submit_mode %s\n", properties.deviceName,
                async_submit ? "async" : "sync");
    if (async_submit)
        std::printf(
            "async timing: decode_ms is CPU submission; render_ms is fence completion remainder\n");
    const std::filesystem::path shader_dir = argc > 4 ? argv[4] : ".";
    auto renderer = std::make_unique<nx_probe::Renderer>(
        vk.physical, vk.device, vk.queue, vk.queue_family, shader_dir / "atlas.vert.spv",
        shader_dir / "atlas.frag.spv");

    std::vector<double> milliseconds;
    std::vector<double> decode_milliseconds;
    std::vector<double> render_milliseconds;
    std::vector<double> steady_milliseconds;
    std::vector<double> render_milliseconds_all;
    std::vector<double> steady_render_milliseconds;
    const auto outer_begin = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point steady_begin = outer_begin;
    bool failed = false;
    size_t offset = header_bytes;
    for (size_t frame = 0; frame < requested && offset < bytes.size(); ++frame) {
        const auto begin = std::chrono::steady_clock::now();
        size_t consumed = 0;
        const uint32_t submit_flags = async_submit ? NXVC_VKD_SUBMIT_ASYNC : 0u;
        if (nxvc_vk_decode_frame_ex(decoder, bytes.data() + offset, bytes.size() - offset,
                                    submit_flags, &consumed) != NXVC_VKD_OK ||
            !consumed) {
            std::fprintf(stderr, "sequence: decode failed at frame %zu: %s\n", frame,
                         nxvc_vk_decoder_last_error(decoder));
            failed = true;
            break;
        }
        const double decode_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        nxvc_vkd_atlas_images images{};
        VkBuffer table = VK_NULL_HANDLE;
        VkDeviceSize table_bytes = 0;
        if (nxvc_vk_decoder_atlas_images(decoder, &images) != NXVC_VKD_OK ||
            nxvc_vk_decoder_atlas_table_buffer(decoder, &table, &table_bytes) != NXVC_VKD_OK) {
            std::fprintf(stderr, "sequence: atlas export failed: %s\n",
                         nxvc_vk_decoder_last_error(decoder));
            failed = true;
            break;
        }
        if (!images.image[0] || !images.image[1] || images.format[0] != VK_FORMAT_R8_UNORM ||
            images.format[1] != VK_FORMAT_R8G8_UNORM || !images.view[0] || !images.view[1]) {
            std::fprintf(stderr, "sequence: unsupported atlas image format/count\n");
            failed = true;
            break;
        }
        for (size_t render_index = 0; render_index < render_repeats; ++render_index) {
            const auto render_begin = std::chrono::steady_clock::now();
            VkFence fence = VK_NULL_HANDLE;
            const bool draw_ok = renderer->draw(images, table, table_bytes, &fence);
            const VkResult render_status =
                draw_ok ? vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX)
                        : VK_ERROR_UNKNOWN;
            if (!draw_ok || render_status != VK_SUCCESS) {
                std::fprintf(stderr, "sequence: renderer failed at frame %zu render %zu\n", frame,
                             render_index);
                if (draw_ok && fence)
                    vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
                vkDeviceWaitIdle(vk.device);
                if (fence)
                    vkDestroyFence(vk.device, fence, nullptr);
                failed = true;
                break;
            }
            vkDestroyFence(vk.device, fence, nullptr);
            const double render_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          (render_index ? render_begin : begin))
                    .count();
            render_milliseconds_all.push_back(render_ms);
            if (frame + 1 > warmup)
                steady_render_milliseconds.push_back(render_ms);
        }
        if (failed)
            break;
        offset += consumed;
        const double pair_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        milliseconds.push_back(pair_ms);
        decode_milliseconds.push_back(decode_ms);
        render_milliseconds.push_back(pair_ms - decode_ms);
        if (frame + 1 == warmup)
            steady_begin = std::chrono::steady_clock::now();
        else if (frame + 1 > warmup)
            steady_milliseconds.push_back(pair_ms);
    }
    const auto capture_end = std::chrono::steady_clock::now();
    if (failed)
        vkDeviceWaitIdle(vk.device);
    const bool readback_ok =
        !failed && renderer->readback_ppm(0, "left.ppm") && renderer->readback_ppm(1, "right.ppm");
    renderer.reset();
    nxvc_vk_decoder_destroy(decoder);
    vk.destroy();
    const double outer_seconds = std::chrono::duration<double>(capture_end - outer_begin).count();
    {
        std::ofstream csv("timings.csv");
        csv << "frame,render_index,decode_ms,render_ms,total_ms\n";
        for (size_t i = 0; i < milliseconds.size(); ++i)
            for (size_t r = 0; r < render_repeats; ++r) {
                const size_t ri = i * render_repeats + r;
                csv << i << ',' << r << ',' << (r == 0 ? decode_milliseconds[i] : 0.0) << ','
                    << (render_milliseconds_all[ri] - (r == 0 ? decode_milliseconds[i] : 0.0))
                    << ',' << render_milliseconds_all[ri] << '\n';
            }
        if (failed || milliseconds.size() != requested)
            csv << "# incomplete\n";
    }
    if (failed || !readback_ok || milliseconds.size() != requested || steady_milliseconds.empty() ||
        render_milliseconds_all.size() != requested * render_repeats)
        return 1;
    const double steady_seconds = std::chrono::duration<double>(capture_end - steady_begin).count();
    std::sort(steady_milliseconds.begin(), steady_milliseconds.end());
    auto percentile = [&](double p) {
        const size_t i = static_cast<size_t>(std::ceil(p * steady_milliseconds.size())) - 1;
        return steady_milliseconds[i];
    };
    std::sort(steady_render_milliseconds.begin(), steady_render_milliseconds.end());
    auto render_percentile = [&](double p) {
        const size_t i = static_cast<size_t>(std::ceil(p * steady_render_milliseconds.size())) - 1;
        return steady_render_milliseconds[i];
    };
    const double sum = std::accumulate(steady_milliseconds.begin(), steady_milliseconds.end(), 0.0);
    const size_t deadline =
        static_cast<size_t>(std::count_if(steady_milliseconds.begin(), steady_milliseconds.end(),
                                          [](double x) { return x <= (1000.0 / 240.0); }));
    const size_t render_deadline = static_cast<size_t>(
        std::count_if(steady_render_milliseconds.begin(), steady_render_milliseconds.end(),
                      [](double x) { return x <= (1000.0 / 240.0); }));
    std::printf("completed_pairs %zu warmup_frames %zu render_repeats %zu completed_renders %zu "
                "elapsed_wall_s %.6f actual_fps %.3f render_steady_fps %.3f render_p50_ms %.3f "
                "render_p95_ms %.3f render_p99_ms %.3f render_deadline_count %zu "
                "steady_completed %zu steady_elapsed_s %.6f steady_fps %.3f steady_mean_pair_ms "
                "%.3f steady_p50_ms %.3f steady_p95_ms %.3f steady_p99_ms %.3f "
                "deadline_1000_over_240_ms %.6f steady_deadline_count %zu\n",
                milliseconds.size(), warmup, render_repeats, render_milliseconds_all.size(),
                outer_seconds, milliseconds.size() / outer_seconds,
                steady_render_milliseconds.size() / steady_seconds, render_percentile(.50),
                render_percentile(.95), render_percentile(.99), render_deadline,
                steady_milliseconds.size(), steady_seconds,
                steady_milliseconds.size() / steady_seconds, sum / steady_milliseconds.size(),
                percentile(.50), percentile(.95), percentile(.99), 1000.0 / 240.0, deadline);
    return 0;
}
