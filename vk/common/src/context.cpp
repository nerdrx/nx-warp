// context.cpp - instance/device creation, host-device adoption, memory types.
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nxvc::vk {
namespace {

bool instanceLayerAvailable(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> props(n);
    if (n) vkEnumerateInstanceLayerProperties(&n, props.data());
    for (const auto& p : props)
        if (std::strcmp(p.layerName, name) == 0) return true;
    return false;
}

bool instanceExtensionAvailable(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    if (n) vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());
    for (const auto& p : props)
        if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}

std::vector<std::string> deviceExtensionNames(VkPhysicalDevice pd) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    if (n) vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, props.data());
    std::vector<std::string> out;
    out.reserve(n);
    for (const auto& p : props) out.emplace_back(p.extensionName);
    return out;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[nxvc-vk][validation] %s\n", data->pMessage);
    return VK_FALSE;
}

// A crude but sufficient ranking: profile first, then the caller's preference
// for discrete or software, then heap size.
int64_t score(const Probe& p, const ContextCreateInfo& ci) {
    if (p.profile == NXVC_VK_PROFILE_UNSUPPORTED) return -1;
    if (p.profile == NXVC_VK_PROFILE_HYBRID_ONLY && !ci.allow_hybrid) return -1;
    const bool software = p.vendor == NXVC_VK_VENDOR_MESA_SOFTWARE ||
                          p.vendor == NXVC_VK_VENDOR_SWIFTSHADER ||
                          p.device_type == VK_PHYSICAL_DEVICE_TYPE_CPU;
    int64_t s = static_cast<int64_t>(p.profile) * 1'000'000'000ll;
    if (ci.prefer_software) {
        if (software) s += 500'000'000ll;
    } else {
        if (software) s -= 400'000'000ll;
        if (ci.prefer_discrete && p.device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            s += 300'000'000ll;
    }
    s += static_cast<int64_t>(p.device_local_bytes >> 20);
    return s;
}

}  // namespace

// ------------------------------------------------------------------ create
std::unique_ptr<Context> Context::create(const ContextCreateInfo& ci) {
    std::unique_ptr<Context> ctx(new Context());

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = ci.app_name.c_str();
    app.pEngineName = "nxvc";
    app.apiVersion = VK_API_VERSION_1_3;

    // Ask for 1.3, fall back to 1.1: the loader fails instance creation when
    // the *loader* is older than the requested version, and lavapipe builds
    // in the wild span both.
    uint32_t instance_version = VK_API_VERSION_1_1;
    if (vkEnumerateInstanceVersion(&instance_version) != VK_SUCCESS)
        instance_version = VK_API_VERSION_1_1;
    app.apiVersion = std::min<uint32_t>(instance_version, VK_API_VERSION_1_3);
    if (app.apiVersion < VK_API_VERSION_1_1)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "Vulkan loader is below 1.1");

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    const bool want_validation =
        ci.validation && instanceLayerAvailable("VK_LAYER_KHRONOS_validation");
    if (want_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        if (instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (instanceExtensionAvailable(
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) &&
        app.apiVersion < VK_API_VERSION_1_1)
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    NXVC_VK_CHECK(vkCreateInstance(&ici, nullptr, &ctx->instance_));
    ctx->owns_instance_ = true;
    ctx->api_version_ = app.apiVersion;

    if (want_validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx->instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT dci{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            create(ctx->instance_, &dci, nullptr, &ctx->debug_messenger_);
        }
    }

    // ------------------------------------------------------ device select
    uint32_t n = 0;
    NXVC_VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance_, &n, nullptr));
    if (n == 0) throw Error(NXVC_VK_ERR_NO_DEVICE, "no Vulkan physical devices");
    std::vector<VkPhysicalDevice> pds(n);
    NXVC_VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance_, &n, pds.data()));

    uint32_t chosen = UINT32_MAX;
    if (ci.device_index != UINT32_MAX) {
        if (ci.device_index >= n)
            throw Error(NXVC_VK_ERR_ARG, "device_index out of range");
        chosen = ci.device_index;
        probeInto(pds[chosen], ctx->probe_);
    } else {
        int64_t best = -1;
        for (uint32_t i = 0; i < n; ++i) {
            Probe p{};
            probeInto(pds[i], p);
            const int64_t s = score(p, ci);
            if (s > best) {
                best = s;
                chosen = i;
                ctx->probe_ = p;
            }
        }
        if (chosen == UINT32_MAX || best < 0) {
            std::string msg = "no device meets the 3.7 requirements:";
            for (uint32_t i = 0; i < n; ++i) {
                Probe p{};
                probeInto(pds[i], p);
                msg += "\n  ";
                msg += p.device_name;
                msg += ": ";
                msg += p.reason;
            }
            throw Error(NXVC_VK_ERR_UNSUPPORTED, msg);
        }
    }
    ctx->pd_ = pds[chosen];

    if (ctx->probe_.profile == NXVC_VK_PROFILE_UNSUPPORTED ||
        (ctx->probe_.profile == NXVC_VK_PROFILE_HYBRID_ONLY && !ci.allow_hybrid))
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    std::string(ctx->probe_.device_name) + ": " + ctx->probe_.reason);

    // ------------------------------------------------------ logical device
    ctx->queue_family_ = ci.dedicated_compute && ctx->probe_.compute_queue_is_dedicated
                             ? ctx->probe_.compute_queue_family
                             : ctx->probe_.compute_queue_family;
    if (ctx->queue_family_ == UINT32_MAX)
        throw Error(NXVC_VK_ERR_UNSUPPORTED, "no compute queue family");

    const auto available = deviceExtensionNames(ctx->pd_);
    const auto have = [&](const char* e) {
        return std::find(available.begin(), available.end(), e) != available.end();
    };

    std::vector<const char*> dev_ext;
    const auto add = [&](const char* e) {
        if (have(e) && std::find_if(dev_ext.begin(), dev_ext.end(), [&](const char* x) {
                           return std::strcmp(x, e) == 0;
                       }) == dev_ext.end())
            dev_ext.push_back(e);
    };
    if (ctx->api_version_ < VK_API_VERSION_1_2)
        add(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    if (ctx->api_version_ < VK_API_VERSION_1_3)
        add(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    if (ctx->api_version_ < VK_API_VERSION_1_1)
        add(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
#if !defined(_WIN32)
    add(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    add(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    add(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    add(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
#else
    add("VK_KHR_external_memory_win32");
    add("VK_KHR_external_semaphore_win32");
    add(VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
#endif
#if defined(__ANDROID__)
    add("VK_ANDROID_external_memory_android_hardware_buffer");
    add(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
#endif
    for (const char* e : ci.extra_device_extensions) add(e);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = ctx->queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDevice16BitStorageFeatures f16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures tls{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeatures sgsc{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};

    // Only ever request what the probe saw: asking for a feature the device
    // lacks is VK_ERROR_FEATURE_NOT_PRESENT, and a hybrid-only device (no
    // 16-bit storage -- SwiftShader is one) must still get a usable device.
    f16.storageBuffer16BitAccess =
        (ctx->probe_.caps & NXVC_VK_CAP_STORAGE_16BIT) ? VK_TRUE : VK_FALSE;
    f16.uniformAndStorageBuffer16BitAccess = f16.storageBuffer16BitAccess;
    tls.timelineSemaphore =
        (ctx->probe_.caps & NXVC_VK_CAP_TIMELINE_SEMAPHORE) ? VK_TRUE : VK_FALSE;
    ycbcr.samplerYcbcrConversion =
        (ctx->probe_.caps & NXVC_VK_CAP_YCBCR_CONVERSION) ? VK_TRUE : VK_FALSE;
    sgsc.subgroupSizeControl = VK_TRUE;
    sgsc.computeFullSubgroups = VK_TRUE;
    feat2.features.shaderInt16 =
        (ctx->probe_.caps & NXVC_VK_CAP_SHADER_INT16) ? VK_TRUE : VK_FALSE;

    void** tail = &feat2.pNext;
    if (f16.storageBuffer16BitAccess) { *tail = &f16; tail = &f16.pNext; }
    if (ctx->probe_.caps & NXVC_VK_CAP_TIMELINE_SEMAPHORE) { *tail = &tls; tail = &tls.pNext; }
    if (ctx->probe_.caps & NXVC_VK_CAP_YCBCR_CONVERSION) { *tail = &ycbcr; tail = &ycbcr.pNext; }
    if (ctx->probe_.caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL) { *tail = &sgsc; tail = &sgsc.pNext; }

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feat2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    NXVC_VK_CHECK(vkCreateDevice(ctx->pd_, &dci, nullptr, &ctx->device_));

    vkGetDeviceQueue(ctx->device_, ctx->queue_family_, 0, &ctx->queue_);
    for (const char* e : dev_ext) ctx->device_extensions_.emplace_back(e);
    ctx->finishInit();
    return ctx;
}

// ------------------------------------------------------------------- adopt
std::unique_ptr<Context> Context::adopt(const AdoptInfo& ai) {
    if (!ai.instance || !ai.physical_device || !ai.device || !ai.queue ||
        ai.queue_family == UINT32_MAX)
        throw Error(NXVC_VK_ERR_ARG, "adopt: null handle or missing queue family");

    std::unique_ptr<Context> ctx(new Context());
    ctx->instance_ = ai.instance;
    ctx->pd_ = ai.physical_device;
    ctx->device_ = ai.device;
    ctx->queue_ = ai.queue;
    ctx->queue_family_ = ai.queue_family;
    ctx->api_version_ = ai.api_version ? ai.api_version : VK_API_VERSION_1_1;
    ctx->adopted_ = true;
    ctx->owns_instance_ = false;
    ctx->device_extensions_ = ai.enabled_device_extensions;

    probeInto(ctx->pd_, ctx->probe_);

    // The host may have created the device on a *different* queue family from
    // the one we would have picked.  Trust the host's; it knows which queues
    // it created.  Re-derive the timestamp bits for that family so the timing
    // helper reports the truth.
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->pd_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    if (qn) vkGetPhysicalDeviceQueueFamilyProperties(ctx->pd_, &qn, qf.data());
    if (ai.queue_family >= qn)
        throw Error(NXVC_VK_ERR_ARG, "adopt: queue_family out of range");
    if (!(qf[ai.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        throw Error(NXVC_VK_ERR_ARG, "adopt: queue family has no COMPUTE bit");
    ctx->probe_.compute_queue_family = ai.queue_family;
    ctx->probe_.timestamp_valid_bits = qf[ai.queue_family].timestampValidBits;
    if (ctx->probe_.timestamp_valid_bits == 0)
        ctx->probe_.caps &= ~static_cast<uint32_t>(NXVC_VK_CAP_TIMESTAMP_QUERY);

    // The probe reports what the *device* can do; adoption is constrained by
    // what the host actually enabled.  Where the host gave us its list, mask
    // the interop bits down to it, so the encoder does not try to call a
    // function that was never enabled.
    if (!ctx->device_extensions_.empty()) {
        const auto en = [&](const char* e) { return ctx->hasDeviceExtension(e); };
        const auto clear = [&](nxvc_vk_cap_bit b) {
            ctx->probe_.caps &= ~static_cast<uint32_t>(b);
        };
        if (!en(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
            clear(NXVC_VK_CAP_EXTERNAL_MEMORY_FD);
        if (!en(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))
            clear(NXVC_VK_CAP_EXTERNAL_SEMAPHORE_FD);
        if (!en("VK_KHR_external_memory_win32")) clear(NXVC_VK_CAP_EXTERNAL_MEMORY_WIN32);
        if (!en("VK_KHR_external_semaphore_win32")) clear(NXVC_VK_CAP_EXTERNAL_SEM_WIN32);
        if (!en("VK_ANDROID_external_memory_android_hardware_buffer"))
            clear(NXVC_VK_CAP_ANDROID_HW_BUFFER);
        if (ctx->api_version_ < VK_API_VERSION_1_3 &&
            !en(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
            clear(NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL);
    }
    ctx->finishInit();
    return ctx;
}

void Context::finishInit() {
    vkGetPhysicalDeviceMemoryProperties(pd_, &mem_props_);

    const auto dev = [&](const char* n) {
        return vkGetDeviceProcAddr(device_, n);
    };
    if (api_version_ >= VK_API_VERSION_1_2) {
        fns_.getSemaphoreCounterValue =
            reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(dev("vkGetSemaphoreCounterValue"));
        fns_.waitSemaphores =
            reinterpret_cast<PFN_vkWaitSemaphores>(dev("vkWaitSemaphores"));
        fns_.signalSemaphore =
            reinterpret_cast<PFN_vkSignalSemaphore>(dev("vkSignalSemaphore"));
    }
    if (!fns_.getSemaphoreCounterValue)
        fns_.getSemaphoreCounterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
            dev("vkGetSemaphoreCounterValueKHR"));
    if (!fns_.waitSemaphores)
        fns_.waitSemaphores =
            reinterpret_cast<PFN_vkWaitSemaphores>(dev("vkWaitSemaphoresKHR"));
    if (!fns_.signalSemaphore)
        fns_.signalSemaphore =
            reinterpret_cast<PFN_vkSignalSemaphore>(dev("vkSignalSemaphoreKHR"));

#if defined(VK_KHR_external_memory_fd)
    fns_.getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(dev("vkGetMemoryFdKHR"));
    fns_.getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        dev("vkGetMemoryFdPropertiesKHR"));
#endif
#if defined(VK_KHR_external_semaphore_fd)
    fns_.getSemaphoreFd =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(dev("vkGetSemaphoreFdKHR"));
    fns_.importSemaphoreFd =
        reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(dev("vkImportSemaphoreFdKHR"));
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    fns_.getMemoryWin32Handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        dev("vkGetMemoryWin32HandleKHR"));
    fns_.getMemoryWin32HandleProperties =
        reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            dev("vkGetMemoryWin32HandlePropertiesKHR"));
    fns_.getSemaphoreWin32Handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        dev("vkGetSemaphoreWin32HandleKHR"));
    fns_.importSemaphoreWin32Handle =
        reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            dev("vkImportSemaphoreWin32HandleKHR"));
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    fns_.getAhbProperties =
        reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            dev("vkGetAndroidHardwareBufferPropertiesANDROID"));
    fns_.getMemoryAhb = reinterpret_cast<PFN_vkGetMemoryAndroidHardwareBufferANDROID>(
        dev("vkGetMemoryAndroidHardwareBufferANDROID"));
#endif
    fns_.createYcbcrConversion = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
        dev("vkCreateSamplerYcbcrConversion"));
    if (!fns_.createYcbcrConversion)
        fns_.createYcbcrConversion = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
            dev("vkCreateSamplerYcbcrConversionKHR"));
    fns_.destroyYcbcrConversion = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
        dev("vkDestroySamplerYcbcrConversion"));
    if (!fns_.destroyYcbcrConversion)
        fns_.destroyYcbcrConversion =
            reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
                dev("vkDestroySamplerYcbcrConversionKHR"));
}

Context::~Context() {
    if (!adopted_) {
        if (device_) {
            vkDeviceWaitIdle(device_);
            vkDestroyDevice(device_, nullptr);
        }
        if (debug_messenger_ && instance_) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance_, debug_messenger_, nullptr);
        }
        if (owns_instance_ && instance_) vkDestroyInstance(instance_, nullptr);
    }
}

bool Context::hasDeviceExtension(std::string_view name) const noexcept {
    for (const auto& e : device_extensions_)
        if (e == name) return true;
    return false;
}

uint32_t Context::findMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred) const {
    // Two passes: required + preferred first, then required alone.
    for (int pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags want = required | (pass == 0 ? preferred : 0);
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if (!(type_bits & (1u << i))) continue;
            if ((mem_props_.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        if (preferred == 0) break;
    }
    throw Error(NXVC_VK_ERR_NOMEM, "no memory type matches the required flags");
}

void Context::waitIdle() const {
    if (device_) NXVC_VK_CHECK(vkDeviceWaitIdle(device_));
}

}  // namespace nxvc::vk
