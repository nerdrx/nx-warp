#include "profile.h"

#include <algorithm>
#include <cctype>

namespace nxwarp::win {

namespace {

constexpr uint32_t kVendorAMD = 0x1002;
constexpr uint32_t kVendorNVIDIA = 0x10DE;
constexpr uint32_t kVendorIntel = 0x8086;
constexpr uint32_t kVendorQualcomm = 0x5143;
constexpr uint32_t kVendorARM = 0x13B5;
constexpr uint32_t kVendorMesa = 0x10005; // lavapipe / llvmpipe report VK_VENDOR_ID_MESA

bool contains_ci(const std::string& hay, const char* needle)
{
    std::string h = hay, n = needle;
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    lower(h);
    lower(n);
    return h.find(n) != std::string::npos;
}

// GCN4 (Polaris) is the reference PC encoder platform in 3.7. Windows drivers
// report the marketing name, so match the Polaris board families.
bool looks_like_gcn4(const std::string& name)
{
    static const char* kPolaris[] = {"RX 460", "RX 470", "RX 480", "RX 550", "RX 560",
                                     "RX 570", "RX 580", "RX 590", "Polaris"};
    for (const char* p : kPolaris)
        if (contains_ci(name, p))
            return true;
    return false;
}

} // namespace

const char* verdict_name(Verdict v)
{
    switch (v) {
    case Verdict::Supported: return "supported";
    case Verdict::HybridOnly: return "hybrid-only";
    default: return "unsupported";
    }
}

ProfileDecision decide_profile(const DeviceCaps& c)
{
    ProfileDecision d;

    // --- hard requirements of the normative compute path (3.7 bit-exactness
    //     rules plus 3.2.6 subgroup portability rules) ---------------------
    if (!c.op_basic)
        d.blockers.push_back("no VK_SUBGROUP_FEATURE_BASIC_BIT");
    if (!c.op_ballot)
        d.blockers.push_back("no subgroup ballot: pass A cannot share a read pointer");
    if (!c.storage_16bit)
        d.blockers.push_back("no storageBuffer16BitAccess: int16 coefficient storage is normative");

    const uint32_t eff_min = c.subgroup_size_control && c.min_subgroup_size
                                 ? c.min_subgroup_size
                                 : c.subgroup_size;
    if (eff_min < d.cluster_size)
        d.blockers.push_back("subgroup size below the normative cluster size of 8");

    // int64 is never used by the normative path; report it, do not require it.
    if (!c.shader_int64)
        d.notes.push_back("shaderInt64 absent; the normative path never uses it");

    // --- per-vendor profile ------------------------------------------------
    switch (c.vendor_id) {
    case kVendorAMD:
        if (c.subgroup_size_control && c.min_subgroup_size != c.max_subgroup_size) {
            // RDNA: the driver picks 32 or 64. 3.7: never assume which.
            d.profile = "amd-wave-dynamic";
            d.required_subgroup_size = c.max_subgroup_size >= 64 ? 64u : c.max_subgroup_size;
            d.notes.push_back("RDNA-class: subgroup size is driver-chosen, pinned with "
                              "VK_EXT_subgroup_size_control");
        } else if (c.subgroup_size == 64) {
            d.profile = looks_like_gcn4(c.device_name) ? "amd-wave64-gcn4" : "amd-wave64";
            if (looks_like_gcn4(c.device_name))
                d.notes.push_back("GCN4 (Polaris): the reference PC encoder platform of 3.7; "
                                  "same SPIR-V binaries as on RADV/Linux");
        } else {
            d.profile = "amd-wave32";
            d.notes.push_back("AMD reporting wave32 without size control; shaders stay "
                              "cluster-8 portable");
        }
        break;

    case kVendorNVIDIA:
        d.profile = "nvidia-wave32";
        d.required_subgroup_size = 0; // NVIDIA is always 32
        break;

    case kVendorIntel:
        // ANV/Windows picks 8, 16 or 32 per shader. 3.7 says force 32.
        if (c.subgroup_size_control && c.max_subgroup_size >= 32) {
            d.profile = "intel-force32";
            d.required_subgroup_size = 32;
            d.notes.push_back("subgroup size pinned to 32 with VK_EXT_subgroup_size_control");
        } else {
            d.profile = "intel-cluster8";
            d.notes.push_back("no subgroup size control: relying on cluster-of-8 portability only");
        }
        d.notes.push_back("Intel needs the shared-fence interop path; keyed mutex is not "
                          "sufficient (3.8)");
        break;

    case kVendorQualcomm:
        d.profile = "adreno-wave64";
        d.notes.push_back("proprietary compiler: avoid clustered subgroup ops (3.7)");
        if (c.shader_int64)
            d.notes.push_back("int64 is reported but unreliable on Adreno; unused anyway");
        break;

    case kVendorARM:
        if (c.subgroup_size >= 16) {
            d.profile = "mali-valhall";
            d.blockers.push_back("Mali Valhall is hybrid-only until a Phase 0 style bench passes");
        } else {
            d.profile = "mali-bifrost";
            d.blockers.push_back("Mali Bifrost: subgroup width too small for pure compute");
        }
        break;

    case kVendorMesa:
        d.profile = "mesa-software";
        d.notes.push_back("software rasterizer (lavapipe/llvmpipe): conformance target, "
                          "not a performance target");
        break;

    default:
        d.profile = "generic-cluster8";
        d.notes.push_back("unrecognised vendor: assuming only the cluster-of-8 portable subset");
        break;
    }

    // --- verdict -----------------------------------------------------------
    if (d.blockers.empty()) {
        d.verdict = Verdict::Supported;
    } else {
        const bool only_hybrid_note =
            std::all_of(d.blockers.begin(), d.blockers.end(), [](const std::string& b) {
                return b.find("hybrid-only") != std::string::npos;
            });
        d.verdict = only_hybrid_note ? Verdict::HybridOnly : Verdict::Unsupported;
    }

    if (!c.op_clustered && d.verdict == Verdict::Supported)
        d.notes.push_back("clustered subgroup ops absent; the portable ballot-only path is used");

    return d;
}

} // namespace nxwarp::win
