// nxvc-vkprobe - print the docs/PAPER.md 3.7 capability probe as JSON.
//
// Headless, no window, no surface: it creates a 1.1 instance, enumerates every
// physical device and prints one JSON object per device.  This is what CI runs
// on lavapipe and SwiftShader and what a developer runs on a new headset.
//
// Exit codes:
//   0   at least one device probed (and the --require constraint, if given, met)
//   1   a --require constraint was not met
//   77  no Vulkan at all: no loader, no ICD, or zero physical devices.
//       ctest treats 77 as "skipped", which is what a CI box without a
//       software rasteriser should report rather than a failure.
#include <nxvc/vk/vk_common.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// selftest.cpp
int nxvcVkSelfTest(bool prefer_software, uint32_t device_index);

namespace {

constexpr int kExitSkip = 77;

void usage() {
    std::puts(
        "usage: nxvc-vkprobe [options]\n"
        "  --json              JSON output (default)\n"
        "  --text              human-readable summary\n"
        "  --device N          probe only physical device N\n"
        "  --require PROFILE   fail with exit 1 unless some device reaches\n"
        "                      PROFILE (full | lite | hybrid)\n"
        "  --quiet             print nothing, use the exit code only\n"
        "  --selftest          create a device and exercise the runtime helpers\n"
        "  --software          prefer a software ICD when selecting a device\n"
        "  -h, --help          this text");
}

int profileRank(std::string_view s) {
    if (s == "full") return NXVC_VK_PROFILE_FULL;
    if (s == "lite") return NXVC_VK_PROFILE_LITE;
    if (s == "hybrid" || s == "hybrid-only") return NXVC_VK_PROFILE_HYBRID_ONLY;
    return -1;
}

void printText(const nxvc::vk::Probe& p, uint32_t index) {
    std::printf("device %u: %s\n", index, p.device_name);
    std::printf("  vendor          %s (0x%04x)  driver %s %s\n",
                nxvc_vk_vendor_string(p.vendor), p.vendor_id,
                p.driver_name[0] ? p.driver_name : "?",
                p.driver_info[0] ? p.driver_info : "");
    std::printf("  api             %u.%u.%u\n", VK_VERSION_MAJOR(p.api_version),
                VK_VERSION_MINOR(p.api_version), VK_VERSION_PATCH(p.api_version));
    std::printf("  profile         %s -- %s\n", nxvc_vk_profile_string(p.profile),
                p.reason);
    std::printf("  subgroup        size %u, range %u..%u, pin %u, cluster %u\n",
                p.subgroup_size, p.subgroup_size_min, p.subgroup_size_max,
                p.required_subgroup_size, NXVC_VK_CLUSTER_WIDTH);
    std::printf("  compute limits  %u B shared, %u invocations, wg %u x %u x %u\n",
                p.max_compute_shared_memory_size, p.max_compute_workgroup_invocations,
                p.max_compute_workgroup_size[0], p.max_compute_workgroup_size[1],
                p.max_compute_workgroup_size[2]);
    std::printf("  queue           family %u%s, timestamp bits %u, period %.3f ns\n",
                p.compute_queue_family,
                p.compute_queue_is_dedicated ? " (dedicated compute)" : "",
                p.timestamp_valid_bits, static_cast<double>(p.timestamp_period_ns));
    std::printf("  memory          %.1f GiB device-local, host-cached heap %s\n",
                static_cast<double>(p.device_local_bytes) / (1024.0 * 1024.0 * 1024.0),
                (p.caps & NXVC_VK_CAP_HOST_CACHED_HEAP)
                    ? (p.host_cached_is_device_local ? "yes (device-local)" : "yes")
                    : "NO");
    std::printf("  caps            %s\n", nxvc::vk::capListString(p.caps).c_str());
    if (p.caps_missing_for_pure)
        std::printf("  missing (pure)  %s\n",
                    nxvc::vk::capListString(p.caps_missing_for_pure).c_str());
    if (p.caps_missing_for_full)
        std::printf("  missing (full)  %s\n",
                    nxvc::vk::capListString(p.caps_missing_for_full).c_str());
    for (uint32_t i = 0; i < p.note_count; ++i)
        std::printf("  note            %s\n", p.notes[i]);
}

}  // namespace

int main(int argc, char** argv) {
    bool json = true;
    bool quiet = false;
    uint32_t only = UINT32_MAX;
    int require = -1;
    bool selftest = false;
    bool prefer_software = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json") {
            json = true;
        } else if (a == "--text") {
            json = false;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--selftest") {
            selftest = true;
        } else if (a == "--software") {
            prefer_software = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--device" && i + 1 < argc) {
            only = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--require" && i + 1 < argc) {
            require = profileRank(argv[++i]);
            if (require < 0) {
                std::fprintf(stderr, "unknown profile '%s'\n", argv[i]);
                return 2;
            }
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (selftest) return nxvcVkSelfTest(prefer_software, only);

    std::vector<nxvc::vk::Probe> probes;
    try {
        probes = nxvc::vk::probeAll();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nxvc-vkprobe: %s\n", e.what());
        return kExitSkip;
    }
    if (probes.empty()) {
        std::fprintf(stderr,
                     "nxvc-vkprobe: no Vulkan physical devices (no ICD?).  Set "
                     "VK_ICD_FILENAMES or VK_DRIVER_FILES to a driver manifest.\n");
        return kExitSkip;
    }

    int best = NXVC_VK_PROFILE_UNSUPPORTED;
    if (!quiet && json) {
        std::printf("{\n  \"abi_version\": %d,\n", NXVC_VK_ABI_VERSION);
        std::printf("  \"cluster_width\": %d,\n", NXVC_VK_CLUSTER_WIDTH);
        std::printf("  \"devices\": [\n");
    }
    bool first = true;
    for (uint32_t i = 0; i < probes.size(); ++i) {
        if (only != UINT32_MAX && i != only) continue;
        best = std::max<int>(best, probes[i].profile);
        if (quiet) continue;
        if (json) {
            if (!first) std::printf(",\n");
            std::fputs(probes[i].json().c_str(), stdout);
            first = false;
        } else {
            printText(probes[i], i);
        }
    }
    if (!quiet && json) std::printf("\n  ]\n}\n");

    if (only != UINT32_MAX && only >= probes.size()) {
        std::fprintf(stderr, "nxvc-vkprobe: no device %u (have %zu)\n", only,
                     probes.size());
        return 2;
    }
    if (require >= 0 && best < require) {
        std::fprintf(stderr,
                     "nxvc-vkprobe: best profile is %s, required %s\n",
                     nxvc_vk_profile_string(static_cast<nxvc_vk_profile>(best)),
                     nxvc_vk_profile_string(static_cast<nxvc_vk_profile>(require)));
        return 1;
    }
    return 0;
}
