// Encoder profile decision, paper 3.7 "Vendor differences".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nxwarp::win {

// Everything 3.7 keys off, gathered from Vulkan in one struct so the decision
// itself stays pure and testable.
struct DeviceCaps {
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    std::string device_name;
    uint32_t subgroup_size = 0;          // VkPhysicalDeviceSubgroupProperties
    uint32_t min_subgroup_size = 0;      // VK_EXT_subgroup_size_control (0 if absent)
    uint32_t max_subgroup_size = 0;
    bool subgroup_size_control = false;
    bool subgroup_compute_full = false;  // computeFullSubgroups
    bool op_basic = false;
    bool op_vote = false;
    bool op_ballot = false;
    bool op_arithmetic = false;
    bool op_shuffle = false;
    bool op_clustered = false;
    bool shader_int64 = false;
    bool shader_int16 = false;
    bool storage_16bit = false;          // storageBuffer16BitAccess
    bool storage_image_write_without_format = false;
};

enum class Verdict {
    Supported,   // normative compute path runs here
    HybridOnly,  // only paper 2.9 / 3.5 hybrid (hardware HEVC base) is viable
    Unsupported,
};

struct ProfileDecision {
    std::string profile;          // short id, e.g. "amd-wave64-gcn4"
    Verdict verdict = Verdict::Unsupported;
    uint32_t required_subgroup_size = 0;  // 0 = do not pin
    uint32_t cluster_size = 8;            // paper 3.2.6: clusters of 8
    std::vector<std::string> notes;
    std::vector<std::string> blockers;
};

const char* verdict_name(Verdict v);
ProfileDecision decide_profile(const DeviceCaps& caps);

} // namespace nxwarp::win
