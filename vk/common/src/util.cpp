// util.cpp - strings, error plumbing, JSON.
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxvc::vk {
namespace {

struct CapName {
    nxvc_vk_cap_bit bit;
    const char* name;
};

constexpr std::array kCapNames{
    CapName{NXVC_VK_CAP_API_1_1, "api_1_1"},
    CapName{NXVC_VK_CAP_COMPUTE_QUEUE, "compute_queue"},
    CapName{NXVC_VK_CAP_SUBGROUP_BASIC, "subgroup_basic"},
    CapName{NXVC_VK_CAP_SUBGROUP_BALLOT, "subgroup_ballot"},
    CapName{NXVC_VK_CAP_SUBGROUP_SHUFFLE, "subgroup_shuffle"},
    CapName{NXVC_VK_CAP_SUBGROUP_ARITHMETIC, "subgroup_arithmetic"},
    CapName{NXVC_VK_CAP_SUBGROUP_CLUSTERED, "subgroup_clustered"},
    CapName{NXVC_VK_CAP_SUBGROUP_VOTE, "subgroup_vote"},
    CapName{NXVC_VK_CAP_SUBGROUP_WIDTH_8, "subgroup_width_8"},
    CapName{NXVC_VK_CAP_STORAGE_16BIT, "storage_16bit"},
    CapName{NXVC_VK_CAP_TIMELINE_SEMAPHORE, "timeline_semaphore"},
    CapName{NXVC_VK_CAP_TIMESTAMP_QUERY, "timestamp_query"},
    CapName{NXVC_VK_CAP_SHARED_MEMORY_32K, "shared_memory_32k"},
    CapName{NXVC_VK_CAP_WORKGROUP_256, "workgroup_256"},
    CapName{NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL, "subgroup_size_control"},
    CapName{NXVC_VK_CAP_SHADER_INT16, "shader_int16"},
    CapName{NXVC_VK_CAP_SHADER_INT64, "shader_int64"},
    CapName{NXVC_VK_CAP_HOST_CACHED_HEAP, "host_cached_heap"},
    CapName{NXVC_VK_CAP_EXTERNAL_MEMORY_FD, "external_memory_fd"},
    CapName{NXVC_VK_CAP_EXTERNAL_SEMAPHORE_FD, "external_semaphore_fd"},
    CapName{NXVC_VK_CAP_EXTERNAL_MEMORY_WIN32, "external_memory_win32"},
    CapName{NXVC_VK_CAP_EXTERNAL_SEM_WIN32, "external_semaphore_win32"},
    CapName{NXVC_VK_CAP_ANDROID_HW_BUFFER, "android_hardware_buffer"},
    CapName{NXVC_VK_CAP_YCBCR_CONVERSION, "ycbcr_conversion"},
    CapName{NXVC_VK_CAP_PIPELINE_EXEC_PROPS, "pipeline_executable_properties"},
};

std::mutex g_err_mutex;
std::unordered_map<const void*, std::string> g_errors;

}  // namespace

std::vector<std::string_view> capNames(uint32_t caps) {
    std::vector<std::string_view> out;
    for (const auto& c : kCapNames)
        if (caps & static_cast<uint32_t>(c.bit)) out.emplace_back(c.name);
    return out;
}

std::string capListString(uint32_t caps) {
    std::string s;
    for (auto n : capNames(caps)) {
        if (!s.empty()) s += ", ";
        s += n;
    }
    return s.empty() ? std::string("(none)") : s;
}

std::string apiVersionString(uint32_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v),
                  VK_VERSION_PATCH(v));
    return b;
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void setLastError(const void* ctx, std::string msg) {
    std::lock_guard lock(g_err_mutex);
    g_errors[ctx] = std::move(msg);
}

const char* resultString(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        default: return "VkResult(other)";
    }
}

void throwVk(VkResult r, const char* expr, const char* file, int line) {
    char b[512];
    std::snprintf(b, sizeof b, "%s failed with %s (%d) at %s:%d", expr,
                  resultString(r), static_cast<int>(r), file, line);
    setLastError(nullptr, b);
    const nxvc_vk_status s = (r == VK_ERROR_OUT_OF_HOST_MEMORY ||
                              r == VK_ERROR_OUT_OF_DEVICE_MEMORY)
                                 ? NXVC_VK_ERR_NOMEM
                                 : NXVC_VK_ERR_VULKAN;
    throw Error(s, b);
}

void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

BuildInfo buildInfo() {
    static char hdr[16];
    std::snprintf(hdr, sizeof hdr, "%d", VK_HEADER_VERSION);
    return BuildInfo{
        /*version=*/"0.1.0",
        /*vulkan_header_version=*/hdr,
#if defined(__ANDROID__)
        /*android_ahb=*/true,
#else
        /*android_ahb=*/false,
#endif
#if defined(_WIN32)
        /*posix_fd=*/false,
        /*win32_handles=*/true,
#else
        /*posix_fd=*/true,
        /*win32_handles=*/false,
#endif
    };
}

}  // namespace nxvc::vk

// ------------------------------------------------------------------- C ABI
extern "C" {

const char* nxvc_vk_status_string(nxvc_vk_status s) {
    switch (s) {
        case NXVC_VK_OK: return "ok";
        case NXVC_VK_ERR_ARG: return "bad argument";
        case NXVC_VK_ERR_UNSUPPORTED: return "device unsupported";
        case NXVC_VK_ERR_VULKAN: return "vulkan error";
        case NXVC_VK_ERR_NOMEM: return "out of memory";
        case NXVC_VK_ERR_NO_DEVICE: return "no matching device";
        case NXVC_VK_ERR_INTERNAL: return "internal error";
    }
    return "unknown";
}

const char* nxvc_vk_profile_string(nxvc_vk_profile p) {
    switch (p) {
        case NXVC_VK_PROFILE_UNSUPPORTED: return "unsupported";
        case NXVC_VK_PROFILE_HYBRID_ONLY: return "hybrid-only";
        case NXVC_VK_PROFILE_LITE: return "lite";
        case NXVC_VK_PROFILE_FULL: return "full";
    }
    return "unknown";
}

const char* nxvc_vk_vendor_string(nxvc_vk_vendor v) {
    switch (v) {
        case NXVC_VK_VENDOR_UNKNOWN: return "unknown";
        case NXVC_VK_VENDOR_AMD: return "amd";
        case NXVC_VK_VENDOR_NVIDIA: return "nvidia";
        case NXVC_VK_VENDOR_INTEL: return "intel";
        case NXVC_VK_VENDOR_QUALCOMM: return "qualcomm";
        case NXVC_VK_VENDOR_ARM: return "arm";
        case NXVC_VK_VENDOR_IMGTEC: return "imgtec";
        case NXVC_VK_VENDOR_APPLE: return "apple";
        case NXVC_VK_VENDOR_MESA_SOFTWARE: return "mesa-software";
        case NXVC_VK_VENDOR_SWIFTSHADER: return "swiftshader";
    }
    return "unknown";
}

}  // extern "C"
