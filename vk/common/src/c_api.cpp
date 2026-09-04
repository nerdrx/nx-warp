// c_api.cpp - the C ABI in nxvc/vk/nxvc_vk.h, implemented over the C++ core.
//
// Every entry point catches: a C caller must never see an exception cross the
// boundary.  Failures become a status code plus a message retrievable with
// nxvc_vk_last_error().
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// The opaque feature chain the C ABI hands back for vkCreateDevice.  It has to
// be a complete type here and an opaque one in the header, which is why the
// header exposes nxvc_vk_feature_chain_size() rather than the layout.
struct nxvc_vk_feature_chain {
    VkPhysicalDeviceFeatures2 features2;
    VkPhysicalDevice16BitStorageFeatures storage16;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr;
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup_size_control;
};

struct nxvc_vk_context {
    std::unique_ptr<nxvc::vk::Context> impl;
    std::string last_error;
};

namespace {

std::mutex g_err_mutex;
std::string g_global_error;

void recordError(nxvc_vk_context* ctx, const std::string& msg) {
    if (ctx) {
        ctx->last_error = msg;
    } else {
        std::lock_guard lock(g_err_mutex);
        g_global_error = msg;
    }
}

template <class Fn>
nxvc_vk_status guard(nxvc_vk_context* ctx, Fn&& fn) {
    try {
        return fn();
    } catch (const nxvc::vk::Error& e) {
        recordError(ctx, e.what());
        return e.status();
    } catch (const std::bad_alloc&) {
        recordError(ctx, "out of host memory");
        return NXVC_VK_ERR_NOMEM;
    } catch (const std::exception& e) {
        recordError(ctx, e.what());
        return NXVC_VK_ERR_INTERNAL;
    } catch (...) {
        recordError(ctx, "unknown error");
        return NXVC_VK_ERR_INTERNAL;
    }
}

}  // namespace

extern "C" {

// -------------------------------------------------------------------- probe
nxvc_vk_status nxvc_vk_probe_physical_device(VkInstance instance, VkPhysicalDevice pd,
                                             nxvc_vk_probe* out) {
    if (!pd || !out) return NXVC_VK_ERR_ARG;
    (void)instance;
    return guard(nullptr, [&] { return nxvc::vk::probeInto(pd, *out); });
}

nxvc_vk_status nxvc_vk_probe_all(nxvc_vk_probe* out, uint32_t capacity,
                                 uint32_t* out_count) {
    return guard(nullptr, [&]() -> nxvc_vk_status {
        const auto probes = nxvc::vk::probeAll();
        if (out_count) *out_count = static_cast<uint32_t>(probes.size());
        if (out) {
            const uint32_t n = std::min<uint32_t>(capacity,
                                                  static_cast<uint32_t>(probes.size()));
            for (uint32_t i = 0; i < n; ++i)
                out[i] = static_cast<const nxvc_vk_probe&>(probes[i]);
        }
        return NXVC_VK_OK;
    });
}

uint32_t nxvc_vk_required_device_extensions(const nxvc_vk_probe* p, const char** out,
                                            uint32_t capacity) {
    if (!p) return 0;
    std::vector<const char*> ext;
    if (p->api_version < VK_API_VERSION_1_2)
        ext.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    if (p->api_version < VK_API_VERSION_1_3 &&
        (p->caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL))
        ext.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    if (p->api_version < VK_API_VERSION_1_1)
        ext.push_back(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    if (p->caps & NXVC_VK_CAP_EXTERNAL_MEMORY_FD)
        ext.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (p->caps & NXVC_VK_CAP_EXTERNAL_SEMAPHORE_FD)
        ext.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    if (p->caps & NXVC_VK_CAP_EXTERNAL_MEMORY_WIN32)
        ext.push_back("VK_KHR_external_memory_win32");
    if (p->caps & NXVC_VK_CAP_EXTERNAL_SEM_WIN32)
        ext.push_back("VK_KHR_external_semaphore_win32");
    if (p->caps & NXVC_VK_CAP_ANDROID_HW_BUFFER)
        ext.push_back("VK_ANDROID_external_memory_android_hardware_buffer");
    for (uint32_t i = 0; i < capacity && i < ext.size(); ++i) out[i] = ext[i];
    return static_cast<uint32_t>(ext.size());
}

size_t nxvc_vk_feature_chain_size(void) { return sizeof(nxvc_vk_feature_chain); }

nxvc_vk_status nxvc_vk_fill_feature_chain(const nxvc_vk_probe* p,
                                          nxvc_vk_feature_chain* chain,
                                          void** out_pnext) {
    if (!p || !chain || !out_pnext) return NXVC_VK_ERR_ARG;
    *chain = nxvc_vk_feature_chain{};
    chain->features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    chain->storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    chain->timeline.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    chain->ycbcr.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    chain->subgroup_size_control.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;

    chain->storage16.storageBuffer16BitAccess =
        (p->caps & NXVC_VK_CAP_STORAGE_16BIT) ? VK_TRUE : VK_FALSE;
    chain->storage16.uniformAndStorageBuffer16BitAccess =
        chain->storage16.storageBuffer16BitAccess;
    chain->timeline.timelineSemaphore =
        (p->caps & NXVC_VK_CAP_TIMELINE_SEMAPHORE) ? VK_TRUE : VK_FALSE;
    chain->ycbcr.samplerYcbcrConversion =
        (p->caps & NXVC_VK_CAP_YCBCR_CONVERSION) ? VK_TRUE : VK_FALSE;
    chain->subgroup_size_control.subgroupSizeControl =
        (p->caps & NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL) ? VK_TRUE : VK_FALSE;
    chain->subgroup_size_control.computeFullSubgroups =
        chain->subgroup_size_control.subgroupSizeControl;
    chain->features2.features.shaderInt16 =
        (p->caps & NXVC_VK_CAP_SHADER_INT16) ? VK_TRUE : VK_FALSE;

    // The host splices features2 into VkDeviceCreateInfo::pNext, or -- when it
    // already has its own VkPhysicalDeviceFeatures2 -- splices storage16 and
    // walks from there.  Chain everything the device actually supports.
    void** tail = &chain->features2.pNext;
    *tail = &chain->storage16;
    tail = &chain->storage16.pNext;
    if (chain->timeline.timelineSemaphore) {
        *tail = &chain->timeline;
        tail = &chain->timeline.pNext;
    }
    if (chain->ycbcr.samplerYcbcrConversion) {
        *tail = &chain->ycbcr;
        tail = &chain->ycbcr.pNext;
    }
    if (chain->subgroup_size_control.subgroupSizeControl) {
        *tail = &chain->subgroup_size_control;
        tail = &chain->subgroup_size_control.pNext;
    }
    *out_pnext = &chain->features2;
    return NXVC_VK_OK;
}

// ------------------------------------------------------------------ context
nxvc_vk_status nxvc_vk_context_create(const nxvc_vk_context_create_info* ci,
                                      nxvc_vk_context** out) {
    if (!ci || !out) return NXVC_VK_ERR_ARG;
    if (ci->abi_version != NXVC_VK_ABI_VERSION) return NXVC_VK_ERR_ARG;
    *out = nullptr;
    auto* c = new (std::nothrow) nxvc_vk_context();
    if (!c) return NXVC_VK_ERR_NOMEM;
    const nxvc_vk_status s = guard(c, [&]() -> nxvc_vk_status {
        nxvc::vk::ContextCreateInfo cci;
        cci.validation = (ci->flags & NXVC_VK_CTX_VALIDATION) != 0;
        cci.prefer_discrete = (ci->flags & NXVC_VK_CTX_PREFER_DISCRETE) != 0;
        cci.prefer_software = (ci->flags & NXVC_VK_CTX_PREFER_SOFTWARE) != 0;
        cci.dedicated_compute = (ci->flags & NXVC_VK_CTX_DEDICATED_COMPUTE) != 0;
        cci.allow_hybrid = (ci->flags & NXVC_VK_CTX_ALLOW_HYBRID) != 0;
        cci.device_index = ci->device_index;
        if (ci->app_name) cci.app_name = ci->app_name;
        c->impl = nxvc::vk::Context::create(cci);
        return NXVC_VK_OK;
    });
    if (s != NXVC_VK_OK) {
        // Keep the message reachable: copy it to the global slot and drop the
        // half-built context.
        recordError(nullptr, c->last_error);
        delete c;
        return s;
    }
    *out = c;
    return NXVC_VK_OK;
}

nxvc_vk_status nxvc_vk_context_adopt(const nxvc_vk_adopt_info* ai,
                                     nxvc_vk_context** out) {
    if (!ai || !out) return NXVC_VK_ERR_ARG;
    if (ai->abi_version != NXVC_VK_ABI_VERSION) return NXVC_VK_ERR_ARG;
    *out = nullptr;
    auto* c = new (std::nothrow) nxvc_vk_context();
    if (!c) return NXVC_VK_ERR_NOMEM;
    const nxvc_vk_status s = guard(c, [&]() -> nxvc_vk_status {
        nxvc::vk::AdoptInfo a;
        a.instance = ai->instance;
        a.physical_device = ai->physical_device;
        a.device = ai->device;
        a.queue = ai->queue;
        a.queue_family = ai->queue_family;
        a.api_version = ai->api_version ? ai->api_version : VK_API_VERSION_1_1;
        a.get_instance_proc_addr = ai->get_instance_proc_addr;
        for (uint32_t i = 0; i < ai->enabled_device_extension_count; ++i)
            if (ai->enabled_device_extensions && ai->enabled_device_extensions[i])
                a.enabled_device_extensions.emplace_back(ai->enabled_device_extensions[i]);
        c->impl = nxvc::vk::Context::adopt(a);
        return NXVC_VK_OK;
    });
    if (s != NXVC_VK_OK) {
        recordError(nullptr, c->last_error);
        delete c;
        return s;
    }
    *out = c;
    return NXVC_VK_OK;
}

void nxvc_vk_context_destroy(nxvc_vk_context* ctx) { delete ctx; }

VkInstance nxvc_vk_context_instance(const nxvc_vk_context* ctx) {
    return ctx && ctx->impl ? ctx->impl->instance() : VK_NULL_HANDLE;
}
VkPhysicalDevice nxvc_vk_context_physical_device(const nxvc_vk_context* ctx) {
    return ctx && ctx->impl ? ctx->impl->physicalDevice() : VK_NULL_HANDLE;
}
VkDevice nxvc_vk_context_device(const nxvc_vk_context* ctx) {
    return ctx && ctx->impl ? ctx->impl->device() : VK_NULL_HANDLE;
}
VkQueue nxvc_vk_context_queue(const nxvc_vk_context* ctx) {
    return ctx && ctx->impl ? ctx->impl->queue() : VK_NULL_HANDLE;
}
uint32_t nxvc_vk_context_queue_family(const nxvc_vk_context* ctx) {
    return ctx && ctx->impl ? ctx->impl->queueFamily() : UINT32_MAX;
}
int nxvc_vk_context_is_adopted(const nxvc_vk_context* ctx) {
    return ctx && ctx->impl && ctx->impl->adopted() ? 1 : 0;
}
const nxvc_vk_probe* nxvc_vk_context_probe(const nxvc_vk_context* ctx) {
    return ctx && ctx->impl ? &static_cast<const nxvc_vk_probe&>(ctx->impl->probe())
                            : nullptr;
}

const char* nxvc_vk_last_error(const nxvc_vk_context* ctx) {
    if (ctx) return ctx->last_error.c_str();
    std::lock_guard lock(g_err_mutex);
    static std::string copy;
    copy = g_global_error;
    return copy.c_str();
}

}  // extern "C"
