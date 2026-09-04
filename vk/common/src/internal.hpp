// internal.hpp - shared between the nxvc_vk_common translation units only.
// Not installed, not part of the ABI.
#pragma once

#include <nxvc/vk/context.hpp>

#include <string>

namespace nxvc::vk {

// probe.cpp
nxvc_vk_status probeInto(VkPhysicalDevice pd, nxvc_vk_probe& out);

// util.cpp
std::string apiVersionString(uint32_t v);   // "1.3.290"
std::string jsonEscape(std::string_view s);

// Records the last error on a context (or the process-wide slot when ctx is
// null) for nxvc_vk_last_error().
void setLastError(const void* ctx, std::string msg);

}  // namespace nxvc::vk
