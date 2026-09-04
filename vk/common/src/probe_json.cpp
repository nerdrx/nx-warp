// probe_json.cpp - the JSON form of a probe.  This is what nxvc-vkprobe
// prints and what CI diffs, so the key set is treated as a stable contract:
// add keys freely, never rename or remove one without bumping
// NXVC_VK_ABI_VERSION.
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nxvc::vk {
namespace {

void kv(std::string& o, const char* k, std::string_view v, bool comma = true) {
    o += "    \"";
    o += k;
    o += "\": \"";
    o += jsonEscape(v);
    o += comma ? "\",\n" : "\"\n";
}

void kn(std::string& o, const char* k, unsigned long long v, bool comma = true) {
    char b[32];
    std::snprintf(b, sizeof b, "%llu", v);
    o += "    \"";
    o += k;
    o += "\": ";
    o += b;
    o += comma ? ",\n" : "\n";
}

void kf(std::string& o, const char* k, double v, bool comma = true) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    o += "    \"";
    o += k;
    o += "\": ";
    o += b;
    o += comma ? ",\n" : "\n";
}

void kb(std::string& o, const char* k, bool v, bool comma = true) {
    o += "    \"";
    o += k;
    o += "\": ";
    o += v ? "true" : "false";
    o += comma ? ",\n" : "\n";
}

void karr3(std::string& o, const char* k, const uint32_t v[3]) {
    char b[96];
    std::snprintf(b, sizeof b, "[%u, %u, %u]", v[0], v[1], v[2]);
    o += "    \"";
    o += k;
    o += "\": ";
    o += b;
    o += ",\n";
}

void kstrlist(std::string& o, const char* k, const std::vector<std::string_view>& v,
              bool comma = true) {
    o += "    \"";
    o += k;
    o += "\": [";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ", ";
        o += '"';
        o += v[i];
        o += '"';
    }
    o += comma ? "],\n" : "]\n";
}

const char* deviceTypeString(uint32_t t) {
    switch (static_cast<VkPhysicalDeviceType>(t)) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

}  // namespace

std::string Probe::json() const {
    std::string o;
    o.reserve(4096);
    o += "  {\n";
    kv(o, "device_name", device_name);
    kv(o, "device_type", deviceTypeString(device_type));
    kv(o, "vendor", nxvc_vk_vendor_string(vendor));
    kv(o, "driver_name", driver_name);
    kv(o, "driver_info", driver_info);
    kn(o, "driver_id", driver_id);
    kv(o, "api_version", apiVersionString(api_version));
    kn(o, "driver_version", driver_version);
    kn(o, "vendor_id", vendor_id);
    kn(o, "device_id", device_id);

    kv(o, "profile", nxvc_vk_profile_string(profile));
    kv(o, "reason", reason);
    kb(o, "pure_compute", profile >= NXVC_VK_PROFILE_LITE);

    kn(o, "subgroup_size", subgroup_size);
    kn(o, "subgroup_size_min", subgroup_size_min);
    kn(o, "subgroup_size_max", subgroup_size_max);
    kn(o, "required_subgroup_size", required_subgroup_size);
    kn(o, "cluster_width", NXVC_VK_CLUSTER_WIDTH);
    kb(o, "quad_operations_in_all_stages", quad_operations_in_all_stages != 0);

    kn(o, "max_compute_shared_memory_size", max_compute_shared_memory_size);
    kn(o, "max_compute_workgroup_invocations", max_compute_workgroup_invocations);
    karr3(o, "max_compute_workgroup_size", max_compute_workgroup_size);
    karr3(o, "max_compute_workgroup_count", max_compute_workgroup_count);
    kn(o, "max_storage_buffer_range", max_storage_buffer_range);
    kn(o, "max_push_constants_size", max_push_constants_size);

    kn(o, "queue_family_count", queue_family_count);
    kn(o, "compute_queue_family",
       compute_queue_family == UINT32_MAX ? 0xFFFFFFFFull : compute_queue_family);
    kb(o, "compute_queue_is_dedicated", compute_queue_is_dedicated != 0);
    kn(o, "timestamp_valid_bits", timestamp_valid_bits);
    kf(o, "timestamp_period_ns", timestamp_period_ns);

    kn(o, "device_local_bytes", device_local_bytes);
    kn(o, "host_cached_bytes", host_cached_bytes);
    kb(o, "has_host_cached_heap", (caps & NXVC_VK_CAP_HOST_CACHED_HEAP) != 0);
    kb(o, "host_cached_is_device_local", host_cached_is_device_local != 0);
    kb(o, "has_device_local_host_visible",
       device_local_host_visible_type_index != UINT32_MAX);

    kn(o, "caps_mask", caps);
    kstrlist(o, "caps", capNames(caps));
    kstrlist(o, "missing_for_pure_compute", capNames(caps_missing_for_pure));
    kstrlist(o, "missing_for_full_profile", capNames(caps_missing_for_full));

    o += "    \"notes\": [";
    for (uint32_t i = 0; i < note_count; ++i) {
        if (i) o += ", ";
        o += '"';
        o += jsonEscape(notes[i]);
        o += '"';
    }
    o += "]\n";
    o += "  }";
    return o;
}

}  // namespace nxvc::vk

extern "C" int nxvc_vk_probe_to_json(const nxvc_vk_probe* p, char* buf, size_t cap) {
    if (!p) return -1;
    nxvc::vk::Probe pr{};
    static_cast<nxvc_vk_probe&>(pr) = *p;
    const std::string s = pr.json();
    if (buf && cap) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
    return static_cast<int>(s.size());
}
