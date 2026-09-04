// nxvc/vk/vk_common.hpp - umbrella header for nxvc_vk_common.
//
// The shared Vulkan runtime of NX Warp: capability probe (docs/PAPER.md 3.7),
// device creation *or* adoption of a host's device (3.6), resource and
// command helpers, timeline semaphores, timestamp timing (3.4), compute
// pipelines from embedded SPIR-V with specialization constants (3.2.6), and
// external memory interop for Linux, Android (3.5) and Windows (3.8).
//
// The encoder (vk/encoder) and decoder (vk/decoder) link this and nothing
// else; WiVRn NX links it through the C ABI in nxvc/vk/nxvc_vk.h.
#pragma once

#include <nxvc/vk/nxvc_vk.h>

#include <nxvc/vk/commands.hpp>
#include <nxvc/vk/context.hpp>
#include <nxvc/vk/external.hpp>
#include <nxvc/vk/pipeline.hpp>
#include <nxvc/vk/resources.hpp>

namespace nxvc::vk {

// Build-time identification, so a host can log which runtime it linked.
struct BuildInfo {
    const char* version;
    const char* vulkan_header_version;
    bool android_ahb;      // AHardwareBuffer import compiled in
    bool posix_fd;         // dma-buf / opaque-fd interop compiled in
    bool win32_handles;    // Win32 handle interop compiled in
};
BuildInfo buildInfo();

// Ceil-divide, the one arithmetic helper worth sharing: dispatch sizes are
// full of it and getting it wrong is a class of bug.
constexpr uint32_t divRoundUp(uint32_t n, uint32_t d) noexcept {
    return (n + d - 1u) / d;
}

// A full compute-to-compute barrier.  The codec's passes are chained with
// these; the encoder's per-pass barriers are narrower and stated at the
// dispatch site.
void computeBarrier(VkCommandBuffer cmd);

}  // namespace nxvc::vk
