// nxvc-vksubgroup - prove the cluster-of-8 emulation of docs/PAPER.md 3.2.6.
//
// The rule under test: "Cluster operations use subgroupBallot plus masks
// derived from gl_SubgroupInvocationID & ~7, never subgroupClustered*."  For
// that rule to be safe in a bit-exact codec, the emulation must produce
// identical results
//
//   (a) against a CPU reference,
//   (b) at every subgroup size the device can be pinned to (8, 16, 32, 64),
//   (c) and, where the device offers them, against the real clustered ops.
//
// This tool runs the same SPIR-V at every pinnable size and checks all three.
// It is the reason the cluster width is 8 and not 16: lavapipe's subgroup size
// is 8, and CI must exercise the narrowest case the codec claims to support.
//
// Exit codes:
//   0   all runs agree with the reference and with each other
//   1   a mismatch (the failing index and size are printed)
//   77  skipped: no Vulkan device, or the device cannot run the pure-compute
//       path at all (a hybrid-only device has nothing to prove here)
#include <nxvc/vk/vk_common.hpp>

#include <nxvc/vk/shaders/nxvc_subgroup_semantics.h>
#include <nxvc/vk/shaders/nxvc_subgroup_semantics_clustered.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitSkip = 77;
constexpr uint32_t kWorkgroupSize = 256;
// A multiple of the workgroup size, so no cluster of 8 is ever partially
// inactive and the CPU reference does not need a tail case.
constexpr uint32_t kElements = 256 * 64;  // 16384

struct Push {
    uint32_t element_count;
};

struct Result {
    uint32_t sum, max, popcount, lane0;
    bool operator==(const Result&) const = default;
};

// The shader's hash, repeated here rather than shared: the point of a
// reference is that it is written independently of the thing it checks.
uint32_t mix32(uint32_t x) {
    x *= 0x9E3779B9u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return x;
}

std::vector<Result> cpuReference(uint32_t n) {
    std::vector<Result> out(n);
    for (uint32_t base = 0; base < n; base += NXVC_VK_CLUSTER_WIDTH) {
        uint32_t sum = 0, mx = 0, pop = 0;
        for (uint32_t i = 0; i < NXVC_VK_CLUSTER_WIDTH; ++i) {
            const uint32_t v = mix32(base + i);
            sum += v;
            mx = std::max(mx, v);
            if (v & 1u) ++pop;
        }
        const uint32_t lane0 = mix32(base);
        for (uint32_t i = 0; i < NXVC_VK_CLUSTER_WIDTH; ++i)
            out[base + i] = Result{sum, mx, pop, lane0};
    }
    return out;
}

struct RunOutcome {
    uint32_t requested_size = 0;
    uint32_t observed_size = 0;
    uint32_t clustered_mismatches = 0;
    uint32_t first_bad_index = UINT32_MAX;
    bool matches_reference = false;
};

// One dispatch at one pinned subgroup size.
RunOutcome runOnce(const nxvc::vk::Context& ctx, uint32_t pin, bool use_clustered,
                   std::vector<Result>& out_results) {
    using namespace nxvc::vk;

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
        DescriptorSetLayout::b(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        DescriptorSetLayout::b(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
    };
    DescriptorSetLayout set_layout(ctx, bindings);

    const std::array<VkDescriptorPoolSize, 1> pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    DescriptorSet set(ctx, set_layout.handle(), pool_sizes);

    Buffer results(ctx, sizeof(Result) * kElements,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferKind::HostReadback,
                   "subgroup.results");
    Buffer status(ctx, 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  BufferKind::HostReadback, "subgroup.status");
    std::memset(results.mapped(), 0xCD, results.size());
    std::memset(status.mapped(), 0, status.size());
    results.flush();
    status.flush();

    set.write(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, results.descriptor())
        .write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, status.descriptor());
    set.flush();

    const std::array<VkDescriptorSetLayout, 1> set_layouts{set_layout.handle()};

    ComputePipelineDesc desc;
    desc.spirv = use_clustered
                     ? std::span<const uint32_t>(shaders::nxvc_subgroup_semantics_clustered_spv)
                     : std::span<const uint32_t>(shaders::nxvc_subgroup_semantics_spv);
    desc.set_layouts = set_layouts;
    desc.push_constant_size = sizeof(Push);
    desc.required_subgroup_size = pin;
    desc.require_full_subgroups = true;
    desc.spec.set(nxvc::vk::kSpecWorkgroupSize, kWorkgroupSize);
    desc.debug_name = "nxvc_subgroup_semantics";
    ComputePipeline pipe(ctx, desc);

    const VkDescriptorSet vk_set = set.handle();
    immediate(ctx, [&](VkCommandBuffer cmd) {
        pipe.bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout(), 0,
                                1, &vk_set, 0, nullptr);
        const Push p{kElements};
        pipe.push(cmd, p);
        vkCmdDispatch(cmd, divRoundUp(kElements, kWorkgroupSize), 1, 1);
    });

    results.invalidate();
    status.invalidate();

    out_results.assign(static_cast<const Result*>(results.mapped()),
                       static_cast<const Result*>(results.mapped()) + kElements);
    const auto* st = static_cast<const uint32_t*>(status.mapped());

    RunOutcome o;
    o.requested_size = pipe.subgroupSize();
    o.clustered_mismatches = st[0];
    o.first_bad_index = st[0] ? st[1] : UINT32_MAX;
    o.observed_size = st[2];
    return o;
}

std::vector<uint32_t> candidateSizes(const nxvc::vk::Probe& p) {
    std::vector<uint32_t> sizes;
    if (!(p.caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL) ||
        p.subgroup_size_min == p.subgroup_size_max) {
        sizes.push_back(0);  // 0 == "whatever the driver gives"
        return sizes;
    }
    for (uint32_t s = 8; s <= 128; s *= 2)
        if (s >= p.subgroup_size_min && s <= p.subgroup_size_max &&
            kWorkgroupSize % s == 0)
            sizes.push_back(s);
    if (sizes.empty()) sizes.push_back(0);
    return sizes;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace nxvc::vk;

    bool prefer_software = false;
    bool verbose = false;
    uint32_t device_index = UINT32_MAX;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--software") {
            prefer_software = true;
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else if (a == "--device" && i + 1 < argc) {
            device_index = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "-h" || a == "--help") {
            std::puts(
                "usage: nxvc-vksubgroup [--software] [--device N] [--verbose]\n"
                "Verifies the cluster-of-8 ballot/shuffle emulation of 3.2.6 at "
                "every\npinnable subgroup size, against a CPU reference and (where "
                "available)\nagainst the real subgroupClustered* ops.");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    std::unique_ptr<Context> ctx;
    try {
        ContextCreateInfo ci;
        ci.app_name = "nxvc-vksubgroup";
        ci.prefer_software = prefer_software;
        ci.device_index = device_index;
        ci.allow_hybrid = true;  // decide below, with a better message
        ctx = Context::create(ci);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nxvc-vksubgroup: skipping: %s\n", e.what());
        return kExitSkip;
    }

    const Probe& p = ctx->probe();
    if (!p.pureCompute()) {
        std::fprintf(stderr,
                     "nxvc-vksubgroup: skipping: %s is %s (%s); the cluster "
                     "emulation only applies to the pure-compute path\n",
                     p.device_name, nxvc_vk_profile_string(p.profile), p.reason);
        return kExitSkip;
    }

    const bool use_clustered = (p.caps & NXVC_VK_CAP_SUBGROUP_CLUSTERED) != 0;
    const auto sizes = candidateSizes(p);
    const auto reference = cpuReference(kElements);

    std::printf("{\n");
    std::printf("  \"device\": \"%s\",\n", p.device_name);
    std::printf("  \"driver\": \"%s\",\n", p.driver_name);
    std::printf("  \"profile\": \"%s\",\n", nxvc_vk_profile_string(p.profile));
    std::printf("  \"cluster_width\": %d,\n", NXVC_VK_CLUSTER_WIDTH);
    std::printf("  \"elements\": %u,\n", kElements);
    std::printf("  \"clustered_oracle\": %s,\n", use_clustered ? "true" : "false");
    std::printf("  \"runs\": [\n");

    bool ok = true;
    std::vector<Result> first_results;
    uint32_t first_size = 0;

    for (size_t i = 0; i < sizes.size(); ++i) {
        std::vector<Result> got;
        RunOutcome o;
        try {
            o = runOnce(*ctx, sizes[i], use_clustered, got);
        } catch (const std::exception& e) {
            std::printf("%s    {\"requested_subgroup_size\": %u, \"error\": \"%s\"}",
                        i ? ",\n" : "", sizes[i], e.what());
            ok = false;
            continue;
        }

        uint32_t bad_index = UINT32_MAX;
        for (uint32_t k = 0; k < kElements; ++k) {
            if (!(got[k] == reference[k])) {
                bad_index = k;
                break;
            }
        }
        o.matches_reference = bad_index == UINT32_MAX;

        uint32_t cross_bad = UINT32_MAX;
        if (first_results.empty()) {
            first_results = got;
            first_size = o.observed_size;
        } else {
            for (uint32_t k = 0; k < kElements; ++k) {
                if (!(got[k] == first_results[k])) {
                    cross_bad = k;
                    break;
                }
            }
        }

        const bool run_ok = o.matches_reference && cross_bad == UINT32_MAX &&
                            o.clustered_mismatches == 0;
        ok = ok && run_ok;

        std::printf("%s    {\"requested_subgroup_size\": %u, "
                    "\"pipeline_subgroup_size\": %u, \"observed_subgroup_size\": %u, "
                    "\"matches_reference\": %s, \"clustered_mismatches\": %u, "
                    "\"cross_size_mismatch_index\": %d, \"first_bad_index\": %d, "
                    "\"pass\": %s}",
                    i ? ",\n" : "", sizes[i], o.requested_size, o.observed_size,
                    o.matches_reference ? "true" : "false", o.clustered_mismatches,
                    cross_bad == UINT32_MAX ? -1 : static_cast<int>(cross_bad),
                    bad_index == UINT32_MAX ? -1 : static_cast<int>(bad_index),
                    run_ok ? "true" : "false");

        if (verbose && !run_ok && bad_index != UINT32_MAX) {
            std::fprintf(stderr,
                         "  index %u: got (%u,%u,%u,%u) want (%u,%u,%u,%u)\n",
                         bad_index, got[bad_index].sum, got[bad_index].max,
                         got[bad_index].popcount, got[bad_index].lane0,
                         reference[bad_index].sum, reference[bad_index].max,
                         reference[bad_index].popcount, reference[bad_index].lane0);
        }
    }

    std::printf("\n  ],\n");
    std::printf("  \"reference_subgroup_size\": %u,\n", first_size);
    std::printf("  \"pass\": %s\n}\n", ok ? "true" : "false");
    return ok ? 0 : 1;
}
