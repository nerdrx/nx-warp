// nxvc/vk/context.hpp - device ownership and the capability probe, C++ side.
//
// Two ways in:
//
//   auto ctx = Context::create({.prefer_software = true});     // we own it
//   auto ctx = Context::adopt({inst, pd, dev, q, family, ...}); // host owns it
//
// An adopted Context destroys nothing on the way out.  That is the WiVRn /
// Monado case of docs/PAPER.md 3.6: the compositor's VkDevice is handed to us
// and the encoder runs on it, so there is no external memory at all, only
// barriers and (maybe) a queue-family ownership transfer.
#pragma once

#include <nxvc/vk/nxvc_vk.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nxvc::vk {

// ------------------------------------------------------------------- errors
class Error : public std::runtime_error {
public:
    Error(nxvc_vk_status s, std::string what)
        : std::runtime_error(std::move(what)), status_(s) {}
    [[nodiscard]] nxvc_vk_status status() const noexcept { return status_; }

private:
    nxvc_vk_status status_;
};

[[noreturn]] void throwVk(VkResult r, const char* expr, const char* file, int line);
const char* resultString(VkResult r);

inline void check(VkResult r, const char* expr, const char* file, int line) {
    if (r != VK_SUCCESS) throwVk(r, expr, file, line);
}

#define NXVC_VK_CHECK(x) ::nxvc::vk::check((x), #x, __FILE__, __LINE__)

// -------------------------------------------------------------------- probe
// Thin C++ view over nxvc_vk_probe with the string handling done.
struct Probe : nxvc_vk_probe {
    [[nodiscard]] bool has(nxvc_vk_cap_bit b) const noexcept {
        return (caps & static_cast<uint32_t>(b)) != 0;
    }
    [[nodiscard]] bool pureCompute() const noexcept {
        return profile >= NXVC_VK_PROFILE_LITE;
    }
    [[nodiscard]] std::string json() const;
    [[nodiscard]] std::string_view name() const noexcept { return device_name; }
    // The subgroup size we would pin with VK_EXT_subgroup_size_control, or 0
    // when the device offers no control and we take what it gives.
    [[nodiscard]] uint32_t pinnedSubgroupSize() const noexcept {
        return required_subgroup_size;
    }
};

Probe probeDevice(VkInstance instance, VkPhysicalDevice pd);
std::vector<Probe> probeAll();  // creates and destroys its own instance

// A device-independent list of the caps names, for reporting.
std::vector<std::string_view> capNames(uint32_t caps);
// The same, comma-joined, or "(none)".
std::string capListString(uint32_t caps);

// ------------------------------------------------------------------ context
struct ContextCreateInfo {
    bool validation = false;
    bool prefer_discrete = true;
    bool prefer_software = false;      // CI: pick lavapipe / SwiftShader
    bool dedicated_compute = false;    // async compute queue where one exists
    bool allow_hybrid = false;         // accept HYBRID_ONLY devices
    uint32_t device_index = UINT32_MAX;
    std::string app_name = "nxvc";
    // Extra device extensions to enable on top of what the probe asks for.
    std::vector<const char*> extra_device_extensions;
};

struct AdoptInfo {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    uint32_t api_version = VK_API_VERSION_1_1;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    std::vector<std::string> enabled_device_extensions;
};

class Context {
public:
    static std::unique_ptr<Context> create(const ContextCreateInfo& ci = {});
    static std::unique_ptr<Context> adopt(const AdoptInfo& ai);

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return pd_; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] VkQueue queue() const noexcept { return queue_; }
    [[nodiscard]] uint32_t queueFamily() const noexcept { return queue_family_; }
    [[nodiscard]] const Probe& probe() const noexcept { return probe_; }
    [[nodiscard]] bool adopted() const noexcept { return adopted_; }
    [[nodiscard]] uint32_t apiVersion() const noexcept { return api_version_; }
    [[nodiscard]] const VkPhysicalDeviceMemoryProperties& memoryProperties() const noexcept {
        return mem_props_;
    }
    [[nodiscard]] bool hasDeviceExtension(std::string_view name) const noexcept;

    // Pick a memory type index satisfying `type_bits` with all of `required`
    // and, where possible, `preferred`.  Throws when nothing matches required.
    [[nodiscard]] uint32_t findMemoryType(uint32_t type_bits,
                                          VkMemoryPropertyFlags required,
                                          VkMemoryPropertyFlags preferred = 0) const;

    // Device functions we resolve ourselves (external memory, timeline on 1.1
    // devices, host-image-copy...).  Null when the extension is absent.
    struct DeviceFns {
        PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
        PFN_vkWaitSemaphores waitSemaphores = nullptr;
        PFN_vkSignalSemaphore signalSemaphore = nullptr;
#if defined(VK_KHR_external_memory_fd)
        PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
        PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
#endif
#if defined(VK_KHR_external_semaphore_fd)
        PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;
        PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        PFN_vkGetMemoryWin32HandleKHR getMemoryWin32Handle = nullptr;
        PFN_vkGetMemoryWin32HandlePropertiesKHR getMemoryWin32HandleProperties = nullptr;
        PFN_vkGetSemaphoreWin32HandleKHR getSemaphoreWin32Handle = nullptr;
        PFN_vkImportSemaphoreWin32HandleKHR importSemaphoreWin32Handle = nullptr;
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties = nullptr;
        PFN_vkGetMemoryAndroidHardwareBufferANDROID getMemoryAhb = nullptr;
#endif
        PFN_vkCreateSamplerYcbcrConversion createYcbcrConversion = nullptr;
        PFN_vkDestroySamplerYcbcrConversion destroyYcbcrConversion = nullptr;
    };
    [[nodiscard]] const DeviceFns& fns() const noexcept { return fns_; }

    // Wait for the compute queue to drain.  Used by the one-shot helpers and
    // at teardown; never on the hot path.
    void waitIdle() const;

private:
    Context() = default;
    void finishInit();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = UINT32_MAX;
    uint32_t api_version_ = VK_API_VERSION_1_1;
    bool adopted_ = false;
    bool owns_instance_ = false;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    Probe probe_{};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    std::vector<std::string> device_extensions_;
    DeviceFns fns_{};
};

}  // namespace nxvc::vk
