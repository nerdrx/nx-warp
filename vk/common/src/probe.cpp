// probe.cpp - the capability probe of docs/PAPER.md 3.7.
//
// The decision this file makes is the one that decides whether a headset runs
// the pure-compute decoder, the hybrid decoder, or nothing at all.  It is
// therefore written to be readable and to explain itself: every verdict comes
// with a `reason` string and a bitmask of exactly which requirements failed.
#include <nxvc/vk/context.hpp>

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nxvc::vk {
namespace {

// 3.7: we pin the subgroup size where VK_EXT_subgroup_size_control lets us.
// 32 is the common denominator across NVIDIA (fixed 32), Intel ANV (8/16/32,
// "force 32") and AMD RDNA (32 or 64, "never assume which").  Clusters of 8
// behave identically at any size >= 8, so the choice is about uniformity and
// occupancy, not correctness.  Devices without size control keep whatever
// they report (Adreno 6xx: 64).
constexpr uint32_t kPreferredSubgroupSize = 32;

// 3.2.3 / 3.2.5: Pass B is one workgroup of 256 per 64x64 tile and needs a
// 64x64 int16 tile plus scratch in LDS.
constexpr uint32_t kFullSharedMemoryBytes = 32u * 1024u;
constexpr uint32_t kFullWorkgroupInvocations = 256u;
constexpr uint32_t kLiteSharedMemoryBytes = 16u * 1024u;
constexpr uint32_t kLiteWorkgroupInvocations = 128u;

void setStr(char* dst, size_t cap, std::string_view src) {
    const size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void addNote(nxvc_vk_probe& p, std::string_view note) {
    if (p.note_count >= NXVC_VK_MAX_NOTES) return;
    setStr(p.notes[p.note_count], NXVC_VK_MAX_NOTE_LEN, note);
    ++p.note_count;
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

nxvc_vk_vendor classifyVendor(uint32_t vendor_id, uint32_t driver_id,
                              std::string_view name) {
    switch (driver_id) {
        case VK_DRIVER_ID_MESA_LLVMPIPE: return NXVC_VK_VENDOR_MESA_SOFTWARE;
        case VK_DRIVER_ID_GOOGLE_SWIFTSHADER: return NXVC_VK_VENDOR_SWIFTSHADER;
        case VK_DRIVER_ID_MOLTENVK: return NXVC_VK_VENDOR_APPLE;
        default: break;
    }
    switch (vendor_id) {
        case 0x1002: case 0x1022: return NXVC_VK_VENDOR_AMD;
        case 0x10DE: return NXVC_VK_VENDOR_NVIDIA;
        case 0x8086: return NXVC_VK_VENDOR_INTEL;
        case 0x5143: return NXVC_VK_VENDOR_QUALCOMM;
        case 0x13B5: return NXVC_VK_VENDOR_ARM;
        case 0x1010: return NXVC_VK_VENDOR_IMGTEC;
        case 0x106B: return NXVC_VK_VENDOR_APPLE;
        default: break;
    }
    if (name.find("llvmpipe") != std::string_view::npos)
        return NXVC_VK_VENDOR_MESA_SOFTWARE;
    if (name.find("SwiftShader") != std::string_view::npos)
        return NXVC_VK_VENDOR_SWIFTSHADER;
    return NXVC_VK_VENDOR_UNKNOWN;
}

// 3.7: "Mali Bifrost | 4 to 8 | partial | ... | unsupported for pure compute".
// The subgroup-width rule catches the 4-lane parts on its own; this catches
// the 8-lane Bifrost parts whose ballot support is the problem, and Midgard,
// which predates subgroups entirely.
bool isUnsupportedMali(std::string_view name) {
    static constexpr std::string_view kBifrost[] = {
        "Mali-G31", "Mali-G51", "Mali-G52", "Mali-G71", "Mali-G72", "Mali-G76",
    };
    if (name.find("Mali-T") != std::string_view::npos) return true;  // Midgard
    for (auto b : kBifrost)
        if (name.find(b) != std::string_view::npos) return true;
    return false;
}

struct ExtSet {
    std::vector<std::string> names;
    [[nodiscard]] bool has(std::string_view s) const { return contains(names, s); }
};

ExtSet enumerateExtensions(VkPhysicalDevice pd) {
    ExtSet e;
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr) != VK_SUCCESS)
        return e;
    std::vector<VkExtensionProperties> props(n);
    if (vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, props.data()) != VK_SUCCESS)
        return e;
    e.names.reserve(n);
    for (const auto& p : props) e.names.emplace_back(p.extensionName);
    return e;
}

}  // namespace

// -------------------------------------------------------------------- probe
nxvc_vk_status probeInto(VkPhysicalDevice pd, nxvc_vk_probe& p) {
    p = nxvc_vk_probe{};
    p.compute_queue_family = UINT32_MAX;
    p.transfer_queue_family = UINT32_MAX;
    p.host_cached_type_index = UINT32_MAX;
    p.device_local_host_visible_type_index = UINT32_MAX;

    const ExtSet ext = enumerateExtensions(pd);

    // ---------------------------------------------------------- properties
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    const uint32_t api = props.apiVersion;

    setStr(p.device_name, NXVC_VK_MAX_NAME, props.deviceName);
    p.api_version = api;
    p.driver_version = props.driverVersion;
    p.vendor_id = props.vendorID;
    p.device_id = props.deviceID;
    p.device_type = props.deviceType;
    p.timestamp_period_ns = props.limits.timestampPeriod;
    p.max_compute_shared_memory_size = props.limits.maxComputeSharedMemorySize;
    p.max_compute_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
    for (int i = 0; i < 3; ++i) {
        p.max_compute_workgroup_count[i] = props.limits.maxComputeWorkGroupCount[i];
        p.max_compute_workgroup_size[i] = props.limits.maxComputeWorkGroupSize[i];
    }
    p.max_storage_buffer_range = props.limits.maxStorageBufferRange;
    p.max_push_constants_size = props.limits.maxPushConstantsSize;

    const bool have_1_1 = api >= VK_API_VERSION_1_1;
    if (!have_1_1) {
        p.profile = NXVC_VK_PROFILE_UNSUPPORTED;
        setStr(p.reason, NXVC_VK_MAX_NOTE_LEN,
               "device reports Vulkan " + std::to_string(VK_VERSION_MAJOR(api)) + "." +
                   std::to_string(VK_VERSION_MINOR(api)) + "; 1.1 is the floor");
        p.vendor = classifyVendor(props.vendorID, 0, p.device_name);
        return NXVC_VK_OK;
    }
    p.caps |= NXVC_VK_CAP_API_1_1;

    // properties2 chain.  Only chain a struct when its promotion level or its
    // extension is actually there; a driver is entitled to ignore unknown
    // pNext entries but several older ones do not.
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties sgsc{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};

    const bool have_driver_props =
        api >= VK_API_VERSION_1_2 || ext.has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    const bool have_sgsc =
        api >= VK_API_VERSION_1_3 || ext.has(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);

    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    void** tail = &props2.pNext;
    *tail = &subgroup; tail = &subgroup.pNext;
    if (have_driver_props) { *tail = &driver; tail = &driver.pNext; }
    if (have_sgsc) { *tail = &sgsc; tail = &sgsc.pNext; }
    vkGetPhysicalDeviceProperties2(pd, &props2);

    p.driver_id = have_driver_props ? static_cast<uint32_t>(driver.driverID) : 0u;
    if (have_driver_props) {
        setStr(p.driver_name, NXVC_VK_MAX_NAME, driver.driverName);
        setStr(p.driver_info, NXVC_VK_MAX_NAME, driver.driverInfo);
    }
    p.vendor = classifyVendor(props.vendorID, p.driver_id, p.device_name);

    p.subgroup_size = subgroup.subgroupSize;
    p.subgroup_supported_ops = subgroup.supportedOperations;
    p.subgroup_supported_stages = subgroup.supportedStages;
    p.quad_operations_in_all_stages = subgroup.quadOperationsInAllStages ? 1u : 0u;
    if (have_sgsc) {
        p.caps |= NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL;
        p.subgroup_size_min = sgsc.minSubgroupSize;
        p.subgroup_size_max = sgsc.maxSubgroupSize;
        p.max_compute_workgroup_subgroups = sgsc.maxComputeWorkgroupSubgroups;
    } else {
        p.subgroup_size_min = subgroup.subgroupSize;
        p.subgroup_size_max = subgroup.subgroupSize;
    }

    // The subgroup ops must be available in COMPUTE, not merely "supported".
    const bool compute_stage =
        (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    const auto op = [&](VkSubgroupFeatureFlagBits f) {
        return compute_stage && (subgroup.supportedOperations & f) != 0;
    };
    if (op(VK_SUBGROUP_FEATURE_BASIC_BIT))      p.caps |= NXVC_VK_CAP_SUBGROUP_BASIC;
    if (op(VK_SUBGROUP_FEATURE_VOTE_BIT))       p.caps |= NXVC_VK_CAP_SUBGROUP_VOTE;
    if (op(VK_SUBGROUP_FEATURE_BALLOT_BIT))     p.caps |= NXVC_VK_CAP_SUBGROUP_BALLOT;
    if (op(VK_SUBGROUP_FEATURE_SHUFFLE_BIT))    p.caps |= NXVC_VK_CAP_SUBGROUP_SHUFFLE;
    if (op(VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)) p.caps |= NXVC_VK_CAP_SUBGROUP_ARITHMETIC;
    if (op(VK_SUBGROUP_FEATURE_CLUSTERED_BIT))  p.caps |= NXVC_VK_CAP_SUBGROUP_CLUSTERED;

    // "refuse subgroups smaller than 8" (3.2.6).  With size control we can
    // *guarantee* >= 8 by pinning, so the test is on what we can pin to; the
    // minimum only matters when we cannot pin at all.
    const uint32_t guaranteed_min =
        (p.caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL) ? p.subgroup_size_max
                                                     : p.subgroup_size_min;
    if (guaranteed_min >= NXVC_VK_CLUSTER_WIDTH) p.caps |= NXVC_VK_CAP_SUBGROUP_WIDTH_8;

    // What we would ask a pipeline for.
    if (p.caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL) {
        if (p.subgroup_size_min == p.subgroup_size_max) {
            p.required_subgroup_size = 0;  // nothing to choose
        } else if (kPreferredSubgroupSize >= p.subgroup_size_min &&
                   kPreferredSubgroupSize <= p.subgroup_size_max) {
            p.required_subgroup_size = kPreferredSubgroupSize;
        } else if (p.subgroup_size_min > kPreferredSubgroupSize) {
            p.required_subgroup_size = p.subgroup_size_min;
        } else {
            p.required_subgroup_size = p.subgroup_size_max;
        }
        if (p.required_subgroup_size != 0 &&
            p.required_subgroup_size < NXVC_VK_CLUSTER_WIDTH) {
            // Never pin below the cluster width even if the range allows it.
            p.required_subgroup_size =
                std::min(p.subgroup_size_max, uint32_t{NXVC_VK_CLUSTER_WIDTH});
        }
    }

    // ------------------------------------------------------------ features
    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDevice16BitStorageFeatures f16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures tls{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features f16i8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};

    const bool have_timeline =
        api >= VK_API_VERSION_1_2 || ext.has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    const bool have_f16i8 =
        api >= VK_API_VERSION_1_2 || ext.has(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);

    void** ftail = &feat2.pNext;
    *ftail = &f16;  ftail = &f16.pNext;
    *ftail = &ycbcr; ftail = &ycbcr.pNext;
    if (have_timeline) { *ftail = &tls; ftail = &tls.pNext; }
    if (have_f16i8)    { *ftail = &f16i8; ftail = &f16i8.pNext; }
    vkGetPhysicalDeviceFeatures2(pd, &feat2);

    if (f16.storageBuffer16BitAccess) p.caps |= NXVC_VK_CAP_STORAGE_16BIT;
    if (have_timeline && tls.timelineSemaphore) p.caps |= NXVC_VK_CAP_TIMELINE_SEMAPHORE;
    if (ycbcr.samplerYcbcrConversion) p.caps |= NXVC_VK_CAP_YCBCR_CONVERSION;
    if (feat2.features.shaderInt16) p.caps |= NXVC_VK_CAP_SHADER_INT16;
    if (feat2.features.shaderInt64) p.caps |= NXVC_VK_CAP_SHADER_INT64;

    // ------------------------------------------------------------- queues
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    p.queue_family_count = qn;

    uint32_t any_compute = UINT32_MAX, dedicated_compute = UINT32_MAX;
    uint32_t dedicated_transfer = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i) {
        const VkQueueFlags f = qf[i].queueFlags;
        if (f & VK_QUEUE_COMPUTE_BIT) {
            if (any_compute == UINT32_MAX) any_compute = i;
            if (!(f & VK_QUEUE_GRAPHICS_BIT) && dedicated_compute == UINT32_MAX)
                dedicated_compute = i;
        }
        if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_COMPUTE_BIT) &&
            !(f & VK_QUEUE_GRAPHICS_BIT) && dedicated_transfer == UINT32_MAX)
            dedicated_transfer = i;
    }
    // Prefer the dedicated compute family: 3.6 wants the encoder off the
    // compositor's graphics queue where the hardware has an async engine.
    p.compute_queue_family =
        dedicated_compute != UINT32_MAX ? dedicated_compute : any_compute;
    p.compute_queue_is_dedicated = dedicated_compute != UINT32_MAX ? 1u : 0u;
    p.transfer_queue_family = dedicated_transfer;
    if (p.compute_queue_family != UINT32_MAX) {
        p.caps |= NXVC_VK_CAP_COMPUTE_QUEUE;
        p.timestamp_valid_bits = qf[p.compute_queue_family].timestampValidBits;
    }
    if (p.timestamp_valid_bits > 0 && p.timestamp_period_ns > 0.0f)
        p.caps |= NXVC_VK_CAP_TIMESTAMP_QUERY;

    // ------------------------------------------------------------- memory
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            p.device_local_bytes += mp.memoryHeaps[i].size;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        const bool hv = (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        const bool cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
        const bool coherent = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        const bool dl = (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (hv && cached && coherent && !dl && p.host_cached_type_index == UINT32_MAX) {
            p.host_cached_type_index = i;
            p.host_cached_bytes = mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size;
            p.caps |= NXVC_VK_CAP_HOST_CACHED_HEAP;
        }
        if (hv && dl && p.device_local_host_visible_type_index == UINT32_MAX)
            p.device_local_host_visible_type_index = i;
    }

    // ---------------------------------------------------------- resources
    if (p.max_compute_shared_memory_size >= kFullSharedMemoryBytes)
        p.caps |= NXVC_VK_CAP_SHARED_MEMORY_32K;
    if (p.max_compute_workgroup_invocations >= kFullWorkgroupInvocations &&
        p.max_compute_workgroup_size[0] >= kFullWorkgroupInvocations)
        p.caps |= NXVC_VK_CAP_WORKGROUP_256;

    // --------------------------------------------------------- interop
    if (ext.has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
        p.caps |= NXVC_VK_CAP_EXTERNAL_MEMORY_FD;
    if (ext.has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))
        p.caps |= NXVC_VK_CAP_EXTERNAL_SEMAPHORE_FD;
    if (ext.has("VK_KHR_external_memory_win32"))
        p.caps |= NXVC_VK_CAP_EXTERNAL_MEMORY_WIN32;
    if (ext.has("VK_KHR_external_semaphore_win32"))
        p.caps |= NXVC_VK_CAP_EXTERNAL_SEM_WIN32;
    if (ext.has("VK_ANDROID_external_memory_android_hardware_buffer"))
        p.caps |= NXVC_VK_CAP_ANDROID_HW_BUFFER;
    if (ext.has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME))
        p.caps |= NXVC_VK_CAP_PIPELINE_EXEC_PROPS;

    // ---------------------------------------------------------- verdict
    p.caps_missing_for_pure = NXVC_VK_CAPS_REQUIRED_PURE & ~p.caps;
    p.caps_missing_for_full = NXVC_VK_CAPS_REQUIRED_FULL & ~p.caps;

    const uint32_t missing_hybrid = NXVC_VK_CAPS_REQUIRED_HYBRID & ~p.caps;

    if (isUnsupportedMali(p.device_name)) {
        p.profile = NXVC_VK_PROFILE_UNSUPPORTED;
        setStr(p.reason, NXVC_VK_MAX_NOTE_LEN,
               "Mali Bifrost/Midgard: partial ballot and sub-8 subgroups, "
               "unsupported per 3.7");
        return NXVC_VK_OK;
    }
    if (missing_hybrid != 0) {
        p.profile = NXVC_VK_PROFILE_UNSUPPORTED;
        setStr(p.reason, NXVC_VK_MAX_NOTE_LEN,
               "missing for any path: " + capListString(missing_hybrid));
        return NXVC_VK_OK;
    }
    if (p.caps_missing_for_pure != 0) {
        p.profile = NXVC_VK_PROFILE_HYBRID_ONLY;
        setStr(p.reason, NXVC_VK_MAX_NOTE_LEN,
               "pure-compute path missing: " + capListString(p.caps_missing_for_pure));
    } else if (p.caps_missing_for_full != 0) {
        if (p.max_compute_shared_memory_size < kLiteSharedMemoryBytes ||
            p.max_compute_workgroup_invocations < kLiteWorkgroupInvocations) {
            p.profile = NXVC_VK_PROFILE_HYBRID_ONLY;
            setStr(p.reason, NXVC_VK_MAX_NOTE_LEN,
                   "compute resources below the Lite floor (16 KB LDS, 128 "
                   "invocations)");
        } else {
            p.profile = NXVC_VK_PROFILE_LITE;
            setStr(p.reason, NXVC_VK_MAX_NOTE_LEN,
                   "pure compute at reduced geometry; missing: " +
                       capListString(p.caps_missing_for_full));
        }
    } else {
        p.profile = NXVC_VK_PROFILE_FULL;
        setStr(p.reason, NXVC_VK_MAX_NOTE_LEN,
               "all pure-compute requirements of 3.7 met");
    }

    // ------------------------------------------------------------- notes
    if (!(p.caps & NXVC_VK_CAP_SUBGROUP_CLUSTERED))
        addNote(p, "no subgroupClustered*; normative shaders never use it "
                   "(3.2.6) but the conformance oracle does");
    if (!(p.caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL) &&
        p.subgroup_size_min != p.subgroup_size_max)
        addNote(p, "variable subgroup size and no size control: pipelines take "
                   "what the driver gives");
    if (p.vendor == NXVC_VK_VENDOR_AMD && p.subgroup_size_min != p.subgroup_size_max)
        addNote(p, "RDNA: 32 or 64 at the driver's discretion, never assumed");
    if (p.vendor == NXVC_VK_VENDOR_QUALCOMM)
        addNote(p, "Adreno: clustered ops and int64 avoided per 3.7");
    if (!(p.caps & NXVC_VK_CAP_HOST_CACHED_HEAP))
        addNote(p, "no HOST_VISIBLE|COHERENT|CACHED heap: the 3.6 send ring "
                   "must stage through device-local");
    if (!(p.caps & NXVC_VK_CAP_SHADER_INT64))
        addNote(p, "no shaderInt64; the normative path does not use it");
    if (p.profile >= NXVC_VK_PROFILE_LITE && p.required_subgroup_size != 0) {
        char b[NXVC_VK_MAX_NOTE_LEN];
        std::snprintf(b, sizeof b, "pipelines pin subgroup size %u (range %u..%u)",
                      p.required_subgroup_size, p.subgroup_size_min,
                      p.subgroup_size_max);
        addNote(p, b);
    }
    return NXVC_VK_OK;
}

Probe probeDevice(VkInstance instance, VkPhysicalDevice pd) {
    (void)instance;
    Probe p{};
    probeInto(pd, static_cast<nxvc_vk_probe&>(p));
    return p;
}

std::vector<Probe> probeAll() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc-probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) return {};

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    if (n) vkEnumeratePhysicalDevices(inst, &n, pds.data());

    std::vector<Probe> out;
    out.reserve(n);
    for (auto pd : pds) out.push_back(probeDevice(inst, pd));
    vkDestroyInstance(inst, nullptr);
    return out;
}

}  // namespace nxvc::vk
