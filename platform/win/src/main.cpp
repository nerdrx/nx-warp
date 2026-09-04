// nxvc-d3dinterop - NX Warp Windows interop probe.
//
// Answers, on a real Windows box, the two questions paper sections 3.7 and 3.8
// leave open before the encoder can be dropped into the WiVRn NX Windows
// helper:
//
//   1. Does the D3D11 -> Vulkan zero-copy path actually work here, and how
//      expensive is the per-frame handoff? The helper hands SteamVR frames over
//      as shared NT-handle D3D11 textures with a keyed-mutex staging ring; the
//      encoder wants them as VkImages with a shared timeline.
//   2. Which encoder profile does this adapter get (3.7 vendor table)?
//
// The probe creates a D3D11.4 device, a shared NT-handle RGBA8 texture, and a
// shared D3D11 fence; imports texture and fence into Vulkan through
// VK_KHR_external_memory_win32 and VK_KHR_external_semaphore_win32; runs a
// trivial checkerboard compute shader on the imported image; reads it back on
// the D3D11 side and verifies every byte; then times the signal/wait handoff.
//
// Build-time alternative: -DNXWARP_WIN_KEYED_MUTEX=ON swaps the shared fence
// for VK_KHR_win32_keyed_mutex (paper 3.8 fallback path).
//
// Everything goes to stdout as one JSON object. Human progress goes to stderr,
// so `nxvc-d3dinterop.exe > probe.json` is always a clean capture.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include "json.h"
#include "profile.h"
#include "vk_loader.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "checker_spv.h"

namespace nxwarp::win {
namespace {

#if NXWARP_WIN_KEYED_MUTEX
constexpr const char* kInteropMode = "keyed-mutex";
constexpr bool kUseFence = false;
#else
constexpr const char* kInteropMode = "shared-fence";
constexpr bool kUseFence = true;
#endif

constexpr const char* kProbeVersion = "0.1.0";

// Keys for the keyed-mutex path. 0 means "D3D11 owns it", 1 means "Vulkan owns
// it" - the same convention the WiVRn NX helper's staging ring uses.
constexpr UINT64 kKeyD3D = 0;
constexpr UINT64 kKeyVk = 1;

bool g_verbose = true;

void logf(const char* fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// errors
// ---------------------------------------------------------------------------

struct ProbeError : std::exception {
    std::string stage;
    std::string message;
    ProbeError(std::string s, std::string m) : stage(std::move(s)), message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

[[noreturn]] void fail(const char* stage, const std::string& msg)
{
    throw ProbeError(stage, msg);
}

std::string hresult_str(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

void check_hr(HRESULT hr, const char* stage, const char* what)
{
    if (FAILED(hr))
        fail(stage, std::string(what) + " failed, hr=" + hresult_str(hr));
}

const char* vk_result_str(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default: return "VK_ERROR_<other>";
    }
}

void check_vk(VkResult r, const char* stage, const char* what)
{
    if (r != VK_SUCCESS) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(r));
        fail(stage, std::string(what) + " -> " + vk_result_str(r) + " (" + buf + ")");
    }
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

std::string wide_to_utf8(const wchar_t* w)
{
    if (!w)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string umd_version_str(LARGE_INTEGER v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u",
                  static_cast<unsigned>(HIWORD(v.HighPart)),
                  static_cast<unsigned>(LOWORD(v.HighPart)),
                  static_cast<unsigned>(HIWORD(v.LowPart)),
                  static_cast<unsigned>(LOWORD(v.LowPart)));
    return buf;
}

std::string vk_version_str(uint32_t v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                  VK_API_VERSION_PATCH(v));
    return buf;
}

// NVIDIA and Intel pack driverVersion differently from the Vulkan convention.
std::string driver_version_str(uint32_t vendor_id, uint32_t v)
{
    char buf[64];
    if (vendor_id == 0x10DE) {
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (v >> 22) & 0x3ff, (v >> 14) & 0xff,
                      (v >> 6) & 0xff, v & 0x3f);
    } else if (vendor_id == 0x8086) {
        std::snprintf(buf, sizeof buf, "%u.%u", v >> 14, v & 0x3fff);
    } else {
        std::snprintf(buf, sizeof buf, "%u.%u.%u", VK_API_VERSION_MAJOR(v),
                      VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    }
    return buf;
}

template <typename T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER freq)
{
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

double percentile(std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size()));
    if (idx >= sorted.size())
        idx = sorted.size() - 1;
    return sorted[idx];
}

// ---------------------------------------------------------------------------
// AMF presence (informational only - NX Warp does not use AMF, but knowing
// whether the box has a working AMF runtime tells us if the hybrid HEVC base
// of paper 2.9 / 3.5 is available here)
// ---------------------------------------------------------------------------

struct AmfInfo {
    bool present = false;
    std::string version;
    std::string note;
};

AmfInfo probe_amf()
{
    AmfInfo info;
    HMODULE h = LoadLibraryA("amfrt64.dll");
    if (!h) {
        info.note = "amfrt64.dll not found";
        return info;
    }
    info.present = true;

    using AMFQueryVersion_Fn = long(__cdecl*)(unsigned long long*);
    auto query = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(h, "AMFQueryVersion"));
    if (!query) {
        info.note = "amfrt64.dll loaded but AMFQueryVersion is missing";
        FreeLibrary(h);
        return info;
    }
    unsigned long long v = 0;
    long res = query(&v);
    if (res != 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "AMFQueryVersion returned %ld", res);
        info.note = buf;
    } else {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%llu.%llu.%llu.%llu", (v >> 48) & 0xffff,
                      (v >> 32) & 0xffff, (v >> 16) & 0xffff, v & 0xffff);
        info.version = buf;
    }
    FreeLibrary(h);
    return info;
}

// ---------------------------------------------------------------------------
// D3D11 side
// ---------------------------------------------------------------------------

struct D3DState {
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11Device5* device5 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11DeviceContext4* ctx4 = nullptr;
    ID3D11Fence* fence = nullptr;
    HANDLE fence_handle = nullptr;

    // reported
    std::string adapter_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_vram = 0;
    std::string umd_version;
    LUID luid{};
    std::string feature_level;

    ~D3DState()
    {
        if (fence_handle)
            CloseHandle(fence_handle);
        release(fence);
        release(ctx4);
        release(ctx);
        release(device5);
        release(device);
        release(adapter);
        release(factory);
    }
};

void init_d3d(D3DState& d, int adapter_index)
{
    check_hr(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&d.factory)),
             "d3d.factory", "CreateDXGIFactory1");

    int seen = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (d.factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        a->GetDesc1(&desc);
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (software) {
            a->Release();
            continue;
        }
        if (adapter_index < 0 || seen == adapter_index) {
            d.adapter = a;
            d.adapter_name = wide_to_utf8(desc.Description);
            d.vendor_id = desc.VendorId;
            d.device_id = desc.DeviceId;
            d.dedicated_vram = desc.DedicatedVideoMemory;
            d.luid = desc.AdapterLuid;
            break;
        }
        ++seen;
        a->Release();
    }
    if (!d.adapter)
        fail("d3d.adapter", "no hardware DXGI adapter found");

    LARGE_INTEGER umd{};
    if (SUCCEEDED(d.adapter->CheckInterfaceSupport(IID_IDXGIDevice, &umd)))
        d.umd_version = umd_version_str(umd);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    UINT flags = 0;
#ifndef NDEBUG
    // Never request the debug layer: it is not installed on a stock box and
    // D3D11CreateDevice fails outright when it is missing.
#endif
    HRESULT hr = D3D11CreateDevice(d.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   &d.device, &got, &d.ctx);
    check_hr(hr, "d3d.device", "D3D11CreateDevice");
    d.feature_level = got == D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0";

    hr = d.device->QueryInterface(IID_ID3D11Device5, reinterpret_cast<void**>(&d.device5));
    if (FAILED(hr))
        fail("d3d.device5",
             "ID3D11Device5 unavailable (needs D3D11.4 / Windows 10 1703+), hr=" +
                 hresult_str(hr));
    hr = d.ctx->QueryInterface(IID_ID3D11DeviceContext4, reinterpret_cast<void**>(&d.ctx4));
    if (FAILED(hr))
        fail("d3d.context4", "ID3D11DeviceContext4 unavailable, hr=" + hresult_str(hr));

    logf("[d3d] %s (vendor 0x%04X device 0x%04X) FL%s driver %s", d.adapter_name.c_str(),
         d.vendor_id, d.device_id, d.feature_level.c_str(), d.umd_version.c_str());
}

// Split out of init_d3d so the adapter block is already in the JSON when this
// fails: "which box was this, and where did it stop" is the whole point of the
// probe on an unknown machine.
void create_shared_fence(D3DState& d)
{
    if (!kUseFence)
        return;
    HRESULT hr = d.device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence,
                                        reinterpret_cast<void**>(&d.fence));
    if (FAILED(hr))
        fail("d3d.fence", "ID3D11Device5::CreateFence(D3D11_FENCE_FLAG_SHARED) failed, hr=" +
                              hresult_str(hr));
    hr = d.fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &d.fence_handle);
    check_hr(hr, "d3d.fence", "ID3D11Fence::CreateSharedHandle");
    logf("[d3d] created a shared D3D11.4 fence");
}

struct SharedTexture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11Texture2D* staging = nullptr;
    IDXGIKeyedMutex* keyed = nullptr;
    HANDLE handle = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;

    ~SharedTexture()
    {
        if (handle)
            CloseHandle(handle);
        release(keyed);
        release(staging);
        release(tex);
    }
};

void create_shared_texture(D3DState& d, SharedTexture& t, uint32_t w, uint32_t h)
{
    t.width = w;
    t.height = h;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    desc.MiscFlags |= kUseFence ? D3D11_RESOURCE_MISC_SHARED
                                : D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    check_hr(d.device->CreateTexture2D(&desc, nullptr, &t.tex), "d3d.texture",
             "CreateTexture2D(shared)");

    IDXGIResource1* res = nullptr;
    check_hr(t.tex->QueryInterface(IID_IDXGIResource1, reinterpret_cast<void**>(&res)),
             "d3d.texture", "QueryInterface(IDXGIResource1)");
    HRESULT hr = res->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &t.handle);
    res->Release();
    check_hr(hr, "d3d.texture", "IDXGIResource1::CreateSharedHandle");

    if (!kUseFence) {
        check_hr(t.tex->QueryInterface(IID_IDXGIKeyedMutex, reinterpret_cast<void**>(&t.keyed)),
                 "d3d.texture", "QueryInterface(IDXGIKeyedMutex)");
    }

    D3D11_TEXTURE2D_DESC sdesc = desc;
    sdesc.BindFlags = 0;
    sdesc.MiscFlags = 0;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check_hr(d.device->CreateTexture2D(&sdesc, nullptr, &t.staging), "d3d.texture",
             "CreateTexture2D(staging)");

    logf("[d3d] shared texture %ux%u RGBA8, NT handle %p", w, h, t.handle);
}

// ---------------------------------------------------------------------------
// Vulkan side
// ---------------------------------------------------------------------------

struct VkState {
    HMODULE lib = nullptr;
    VkApi api{};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;

    // reported
    uint32_t instance_version = 0;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    std::string device_name;
    std::string driver_name;
    std::string driver_info;
    std::vector<std::string> required_ext_present;
    std::vector<std::string> required_ext_missing;
    std::vector<std::string> optional_ext_present;
    DeviceCaps caps;
    bool luid_matched = false;
    bool fence_importable = false;
    bool keyed_mutex_ext = false;
    bool ext_semaphore_win32 = false;
    bool image_format_external_ok = false;
};

struct PushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t frame;
    uint32_t pad;
};

void load_vulkan(VkState& v)
{
    v.lib = LoadLibraryA("vulkan-1.dll");
    if (!v.lib)
        fail("vk.loader", "vulkan-1.dll not found (no Vulkan ICD installed?)");

    v.api.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(v.lib, "vkGetInstanceProcAddr")));
    if (!v.api.vkGetInstanceProcAddr)
        fail("vk.loader", "vulkan-1.dll has no vkGetInstanceProcAddr");

#define NXW_LOAD_GLOBAL(name)                                                                  \
    v.api.name = reinterpret_cast<PFN_##name>(v.api.vkGetInstanceProcAddr(nullptr, #name));
    NXW_VK_GLOBAL_FUNCS(NXW_LOAD_GLOBAL)
#undef NXW_LOAD_GLOBAL

    if (!v.api.vkCreateInstance)
        fail("vk.loader", "vkCreateInstance not resolvable");
}

void create_instance(VkState& v)
{
    v.instance_version = VK_API_VERSION_1_0;
    if (v.api.vkEnumerateInstanceVersion)
        v.api.vkEnumerateInstanceVersion(&v.instance_version);
    if (v.instance_version < VK_API_VERSION_1_2)
        fail("vk.instance", "Vulkan instance version " + vk_version_str(v.instance_version) +
                                " is below the 1.2 the probe needs (timeline semaphores)");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc-d3dinterop";
    app.applicationVersion = 1;
    app.pEngineName = "nx-warp";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    check_vk(v.api.vkCreateInstance(&ci, nullptr, &v.instance), "vk.instance", "vkCreateInstance");

#define NXW_LOAD_INSTANCE(name)                                                                \
    v.api.name = reinterpret_cast<PFN_##name>(v.api.vkGetInstanceProcAddr(v.instance, #name));
    NXW_VK_INSTANCE_FUNCS(NXW_LOAD_INSTANCE)
#undef NXW_LOAD_INSTANCE

    logf("[vk] instance %s", vk_version_str(v.instance_version).c_str());
}

void pick_physical_device(VkState& v, const D3DState& d)
{
    uint32_t count = 0;
    check_vk(v.api.vkEnumeratePhysicalDevices(v.instance, &count, nullptr), "vk.phys",
             "vkEnumeratePhysicalDevices");
    if (count == 0)
        fail("vk.phys", "no Vulkan physical devices");
    std::vector<VkPhysicalDevice> devs(count);
    v.api.vkEnumeratePhysicalDevices(v.instance, &count, devs.data());

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (VkPhysicalDevice pd : devs) {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
        v.api.vkGetPhysicalDeviceProperties2(pd, &p2);
        if (!fallback && p2.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
            fallback = pd;
        if (idp.deviceLUIDValid && std::memcmp(idp.deviceLUID, &d.luid, sizeof(LUID)) == 0) {
            v.phys = pd;
            v.luid_matched = true;
            break;
        }
    }
    if (!v.phys) {
        v.phys = fallback ? fallback : devs[0];
        logf("[vk] warning: no device LUID matched the DXGI adapter, falling back to device 0");
    }
}

void query_caps(VkState& v)
{
    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties ssc{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    sub.pNext = &drv;
    drv.pNext = &ssc;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sub};
    v.api.vkGetPhysicalDeviceProperties2(v.phys, &p2);

    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    v.api.vkGetPhysicalDeviceFeatures2(v.phys, &f2);

    v.api_version = p2.properties.apiVersion;
    v.driver_version = p2.properties.driverVersion;
    v.device_name = p2.properties.deviceName;
    v.driver_name = drv.driverName;
    v.driver_info = drv.driverInfo;

    DeviceCaps& c = v.caps;
    c.vendor_id = p2.properties.vendorID;
    c.device_id = p2.properties.deviceID;
    c.device_name = v.device_name;
    c.subgroup_size = sub.subgroupSize;
    c.min_subgroup_size = ssc.minSubgroupSize;
    c.max_subgroup_size = ssc.maxSubgroupSize;
    c.op_basic = (sub.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
    c.op_vote = (sub.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) != 0;
    c.op_ballot = (sub.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
    c.op_arithmetic = (sub.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0;
    c.op_shuffle = (sub.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0;
    c.op_clustered = (sub.supportedOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) != 0;
    c.shader_int64 = f2.features.shaderInt64 == VK_TRUE;
    c.shader_int16 = f2.features.shaderInt16 == VK_TRUE;
    c.storage_16bit = f11.storageBuffer16BitAccess == VK_TRUE;
    c.storage_image_write_without_format =
        f2.features.shaderStorageImageWriteWithoutFormat == VK_TRUE;

    logf("[vk] %s, driver %s (%s), subgroup %u", v.device_name.c_str(), v.driver_name.c_str(),
         driver_version_str(c.vendor_id, v.driver_version).c_str(), c.subgroup_size);
}

void survey_extensions(VkState& v)
{
    uint32_t n = 0;
    v.api.vkEnumerateDeviceExtensionProperties(v.phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    if (n)
        v.api.vkEnumerateDeviceExtensionProperties(v.phys, nullptr, &n, ext.data());

    auto has = [&](const char* name) {
        for (const auto& e : ext)
            if (std::strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };

    // What paper 3.8 needs on the encoder side.
    const char* required[] = {
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        kUseFence ? VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
                  : VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
    };
    for (const char* r : required) {
        if (has(r))
            v.required_ext_present.push_back(r);
        else
            v.required_ext_missing.push_back(r);
    }

    const char* optional[] = {
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
        VK_KHR_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_WIN32_EXTENSION_NAME,
    };
    for (const char* o : optional)
        if (has(o))
            v.optional_ext_present.push_back(o);

    v.ext_semaphore_win32 = has(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    v.keyed_mutex_ext = has(VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
    v.caps.subgroup_size_control = has(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    if (!v.caps.subgroup_size_control && v.api_version >= VK_API_VERSION_1_3)
        v.caps.subgroup_size_control = true; // core in 1.3

    if (!v.required_ext_missing.empty())
        fail("vk.extensions", "missing required device extension(s): " +
                                  v.required_ext_missing.front());
}

void check_external_semaphore(VkState& v)
{
    if (!kUseFence)
        return;
    VkPhysicalDeviceExternalSemaphoreInfo info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    info.pNext = &type;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

    VkExternalSemaphoreProperties props{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    v.api.vkGetPhysicalDeviceExternalSemaphoreProperties(v.phys, &info, &props);
    v.fence_importable =
        (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
    if (!v.fence_importable)
        fail("vk.semaphore",
             "driver cannot import D3D12_FENCE handles as timeline semaphores; rebuild with "
             "-DNXWARP_WIN_KEYED_MUTEX=ON for the paper 3.8 fallback path");
}

void check_external_image_format(VkState& v)
{
    VkPhysicalDeviceExternalImageFormatInfo ext{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                          &ext};
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;

    VkExternalImageFormatProperties efp{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 fp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &efp};

    VkResult r = v.api.vkGetPhysicalDeviceImageFormatProperties2(v.phys, &info, &fp);
    if (r != VK_SUCCESS)
        fail("vk.format", std::string("RGBA8 storage image from a D3D11 shared texture is not "
                                      "supported: ") +
                              vk_result_str(r));
    if (!(efp.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        fail("vk.format", "D3D11_TEXTURE handles are not importable for this image config");
    v.image_format_external_ok = true;
}

void create_device(VkState& v)
{
    uint32_t n = 0;
    v.api.vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    v.api.vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &n, qf.data());

    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            v.queue_family = i;
            found = true;
            if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                break; // prefer an async compute family, like the real encoder
        }
    }
    if (!found)
        fail("vk.device", "no compute-capable queue family");

    std::vector<const char*> exts;
    for (const auto& e : v.required_ext_present)
        exts.push_back(e.c_str());
    if (v.caps.subgroup_size_control && v.api_version < VK_API_VERSION_1_3)
        exts.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = v.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                         &f12};
    if (v.caps.storage_16bit)
        f11.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    f2.features.shaderInt16 = v.caps.shader_int16 ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();

    check_vk(v.api.vkCreateDevice(v.phys, &dci, nullptr, &v.device), "vk.device", "vkCreateDevice");

#define NXW_LOAD_DEVICE(name)                                                                  \
    v.api.name = reinterpret_cast<PFN_##name>(v.api.vkGetDeviceProcAddr(v.device, #name));
    NXW_VK_DEVICE_FUNCS(NXW_LOAD_DEVICE)
#undef NXW_LOAD_DEVICE

    if (kUseFence && !v.api.vkImportSemaphoreWin32HandleKHR)
        fail("vk.device", "vkImportSemaphoreWin32HandleKHR not resolvable");
    if (!v.api.vkGetMemoryWin32HandlePropertiesKHR)
        fail("vk.device", "vkGetMemoryWin32HandlePropertiesKHR not resolvable");

    v.api.vkGetDeviceQueue(v.device, v.queue_family, 0, &v.queue);
}

void import_fence(VkState& v, const D3DState& d)
{
    if (!kUseFence)
        return;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    check_vk(v.api.vkCreateSemaphore(v.device, &sci, nullptr, &v.timeline), "vk.semaphore",
             "vkCreateSemaphore(timeline)");

    VkImportSemaphoreWin32HandleInfoKHR imp{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    imp.semaphore = v.timeline;
    imp.flags = 0;
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    imp.handle = d.fence_handle;
    check_vk(v.api.vkImportSemaphoreWin32HandleKHR(v.device, &imp), "vk.semaphore",
             "vkImportSemaphoreWin32HandleKHR(D3D12_FENCE)");
    logf("[vk] imported the D3D11 shared fence as a timeline semaphore");
}

void create_pipeline(VkState& v)
{
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kCheckerSpv);
    smci.pCode = kCheckerSpv;
    check_vk(v.api.vkCreateShaderModule(v.device, &smci, nullptr, &v.shader), "vk.pipeline",
             "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &b;
    check_vk(v.api.vkCreateDescriptorSetLayout(v.device, &dslci, nullptr, &v.dsl), "vk.pipeline",
             "vkCreateDescriptorSetLayout");

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &v.dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    check_vk(v.api.vkCreatePipelineLayout(v.device, &plci, nullptr, &v.playout), "vk.pipeline",
             "vkCreatePipelineLayout");

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = v.shader;
    cpci.stage.pName = "main";
    cpci.layout = v.playout;
    check_vk(v.api.vkCreateComputePipelines(v.device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                            &v.pipeline),
             "vk.pipeline", "vkCreateComputePipelines");

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 8;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    check_vk(v.api.vkCreateDescriptorPool(v.device, &dpci, nullptr, &v.dpool), "vk.pipeline",
             "vkCreateDescriptorPool");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = v.queue_family;
    check_vk(v.api.vkCreateCommandPool(v.device, &cpi, nullptr, &v.cpool), "vk.pipeline",
             "vkCreateCommandPool");
}

// One imported texture, on the Vulkan side.
struct ImportedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

void import_texture(VkState& v, const SharedTexture& t, ImportedImage& out)
{
    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {t.width, t.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check_vk(v.api.vkCreateImage(v.device, &ici, nullptr, &out.image), "vk.import",
             "vkCreateImage(external)");

    VkMemoryWin32HandlePropertiesKHR mhp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    check_vk(v.api.vkGetMemoryWin32HandlePropertiesKHR(
                 v.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, t.handle, &mhp),
             "vk.import", "vkGetMemoryWin32HandlePropertiesKHR");

    VkImageMemoryRequirementsInfo2 mri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    mri.image = out.image;
    VkMemoryDedicatedRequirements ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded};
    v.api.vkGetImageMemoryRequirements2(v.device, &mri, &req);

    VkPhysicalDeviceMemoryProperties mp{};
    v.api.vkGetPhysicalDeviceMemoryProperties(v.phys, &mp);
    const uint32_t bits = req.memoryRequirements.memoryTypeBits & mhp.memoryTypeBits;
    uint32_t type_index = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i)))
            continue;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            type_index = i;
            break;
        }
        if (type_index == UINT32_MAX)
            type_index = i;
    }
    if (type_index == UINT32_MAX)
        fail("vk.import", "no Vulkan memory type accepts this D3D11 shared texture");

    // The dedicated allocation is not optional for imported D3D11 textures.
    VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dai.image = out.image;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                         &dai};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    imp.handle = t.handle;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    mai.allocationSize = req.memoryRequirements.size;
    mai.memoryTypeIndex = type_index;
    check_vk(v.api.vkAllocateMemory(v.device, &mai, nullptr, &out.memory), "vk.import",
             "vkAllocateMemory(import D3D11 texture)");

    VkBindImageMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    bind.image = out.image;
    bind.memory = out.memory;
    bind.memoryOffset = 0;
    check_vk(v.api.vkBindImageMemory2(v.device, 1, &bind), "vk.import", "vkBindImageMemory2");

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = out.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check_vk(v.api.vkCreateImageView(v.device, &ivci, nullptr, &out.view), "vk.import",
             "vkCreateImageView");

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = v.dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &v.dsl;
    check_vk(v.api.vkAllocateDescriptorSets(v.device, &dsai, &out.set), "vk.import",
             "vkAllocateDescriptorSets");

    VkDescriptorImageInfo dii{};
    dii.imageView = out.view;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = out.set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &dii;
    v.api.vkUpdateDescriptorSets(v.device, 1, &w, 0, nullptr);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = v.cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    check_vk(v.api.vkAllocateCommandBuffers(v.device, &cbai, &out.cmd), "vk.import",
             "vkAllocateCommandBuffers");

    // The command buffer is identical every iteration: barrier, dispatch,
    // barrier back out to the external consumer.
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check_vk(v.api.vkBeginCommandBuffer(out.cmd, &bi), "vk.record", "vkBeginCommandBuffer");

    VkImageMemoryBarrier in{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    in.srcAccessMask = 0;
    in.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    in.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    in.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    in.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    in.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    in.image = out.image;
    in.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    v.api.vkCmdPipelineBarrier(out.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &in);

    v.api.vkCmdBindPipeline(out.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v.pipeline);
    v.api.vkCmdBindDescriptorSets(out.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v.playout, 0, 1,
                                  &out.set, 0, nullptr);
    PushConstants pc{t.width, t.height, 0, 0};
    v.api.vkCmdPushConstants(out.cmd, v.playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    v.api.vkCmdDispatch(out.cmd, (t.width + 15) / 16, (t.height + 15) / 16, 1);

    VkImageMemoryBarrier back = in;
    back.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    v.api.vkCmdPipelineBarrier(out.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &back);

    check_vk(v.api.vkEndCommandBuffer(out.cmd), "vk.record", "vkEndCommandBuffer");

    logf("[vk] imported %ux%u D3D11 texture, memory type %u, dedicated=%d", t.width, t.height,
         type_index, static_cast<int>(ded.prefersDedicatedAllocation));
}

void destroy_imported(VkState& v, ImportedImage& i)
{
    if (i.view)
        v.api.vkDestroyImageView(v.device, i.view, nullptr);
    if (i.image)
        v.api.vkDestroyImage(v.device, i.image, nullptr);
    if (i.memory)
        v.api.vkFreeMemory(v.device, i.memory, nullptr);
    i = {};
}

// One Vulkan submit that waits for the D3D11 producer and signals it back.
void submit_handoff(VkState& v, const ImportedImage& img, const ImportedImage* mem_owner,
                    uint64_t wait_value, uint64_t signal_value)
{
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &img.cmd;

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    if (kUseFence) {
        VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tsi.waitSemaphoreValueCount = 1;
        tsi.pWaitSemaphoreValues = &wait_value;
        tsi.signalSemaphoreValueCount = 1;
        tsi.pSignalSemaphoreValues = &signal_value;
        si.pNext = &tsi;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &v.timeline;
        si.pWaitDstStageMask = &stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &v.timeline;
        check_vk(v.api.vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE), "vk.submit",
                 "vkQueueSubmit(timeline)");
    } else {
        const VkDeviceMemory mem = mem_owner->memory;
        const uint64_t acquire_key = kKeyVk;
        const uint64_t release_key = kKeyD3D;
        const uint32_t timeout_ms = 5000;
        VkWin32KeyedMutexAcquireReleaseInfoKHR km{
            VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
        km.acquireCount = 1;
        km.pAcquireSyncs = &mem;
        km.pAcquireKeys = &acquire_key;
        km.pAcquireTimeouts = &timeout_ms;
        km.releaseCount = 1;
        km.pReleaseSyncs = &mem;
        km.pReleaseKeys = &release_key;
        si.pNext = &km;
        check_vk(v.api.vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE), "vk.submit",
                 "vkQueueSubmit(keyed mutex)");
    }
}

// ---------------------------------------------------------------------------
// verification
// ---------------------------------------------------------------------------

struct VerifyResult {
    bool ok = false;
    uint64_t mismatches = 0;
    uint32_t first_x = 0;
    uint32_t first_y = 0;
    uint32_t got = 0;
    uint32_t want = 0;
};

VerifyResult verify_readback(D3DState& d, SharedTexture& t)
{
    VerifyResult vr;
    d.ctx->CopyResource(t.staging, t.tex);

    D3D11_MAPPED_SUBRESOURCE map{};
    HRESULT hr = d.ctx->Map(t.staging, 0, D3D11_MAP_READ, 0, &map);
    check_hr(hr, "verify", "ID3D11DeviceContext::Map(staging)");

    const auto* base = static_cast<const uint8_t*>(map.pData);
    for (uint32_t y = 0; y < t.height; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * map.RowPitch;
        const uint32_t cy = y >> 6;
        for (uint32_t x = 0; x < t.width; ++x) {
            const uint32_t cx = x >> 6;
            const uint8_t want_r = ((cx + cy) & 1u) ? 255 : 32;
            const uint8_t want_g = static_cast<uint8_t>((cx & 15u) * 16u);
            const uint8_t want_b = static_cast<uint8_t>((cy & 15u) * 16u);
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            if (p[0] != want_r || p[1] != want_g || p[2] != want_b || p[3] != 255) {
                if (vr.mismatches == 0) {
                    vr.first_x = x;
                    vr.first_y = y;
                    vr.got = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                             (uint32_t(p[2]) << 8) | p[3];
                    vr.want = (uint32_t(want_r) << 24) | (uint32_t(want_g) << 16) |
                              (uint32_t(want_b) << 8) | 255u;
                }
                ++vr.mismatches;
            }
        }
    }
    d.ctx->Unmap(t.staging, 0);
    vr.ok = vr.mismatches == 0;
    return vr;
}

// ---------------------------------------------------------------------------
// one size: import, one verified handoff, then N timed handoffs
// ---------------------------------------------------------------------------

struct SizeResult {
    uint32_t width = 0;
    uint32_t height = 0;
    VerifyResult verify;
    uint32_t iterations = 0;
    double p50_ms = 0;
    double p99_ms = 0;
    double min_ms = 0;
    double max_ms = 0;
    double mean_ms = 0;
};

SizeResult run_size(D3DState& d, VkState& v, uint32_t w, uint32_t h, int iterations)
{
    SizeResult r;
    r.width = w;
    r.height = h;

    SharedTexture tex;
    create_shared_texture(d, tex, w, h);

    ImportedImage img;
    import_texture(v, tex, img);

    HANDLE ev = nullptr;
    if (kUseFence) {
        ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!ev)
            fail("timing", "CreateEvent failed");
    }

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    uint64_t counter = 0;
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));

    auto one_handoff = [&](bool timed) {
        LARGE_INTEGER t0{}, t1{};
        if (kUseFence) {
            const uint64_t wait_value = ++counter;   // D3D says "yours"
            const uint64_t signal_value = ++counter; // Vulkan says "done"
            check_hr(d.ctx4->Signal(d.fence, wait_value), "timing", "ID3D11DeviceContext4::Signal");
            d.ctx->Flush();
            QueryPerformanceCounter(&t0);
            submit_handoff(v, img, &img, wait_value, signal_value);
            check_hr(d.fence->SetEventOnCompletion(signal_value, ev), "timing",
                     "ID3D11Fence::SetEventOnCompletion");
            if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0)
                fail("timing", "timed out waiting for the shared fence (GPU hang or a driver "
                               "that silently drops D3D12_FENCE imports)");
            QueryPerformanceCounter(&t1);
        } else {
            check_hr(tex.keyed->AcquireSync(kKeyD3D, 5000), "timing",
                     "IDXGIKeyedMutex::AcquireSync(D3D)");
            check_hr(tex.keyed->ReleaseSync(kKeyVk), "timing",
                     "IDXGIKeyedMutex::ReleaseSync(to Vulkan)");
            QueryPerformanceCounter(&t0);
            submit_handoff(v, img, &img, 0, 0);
            check_hr(tex.keyed->AcquireSync(kKeyD3D, 10000), "timing",
                     "IDXGIKeyedMutex::AcquireSync(back from Vulkan)");
            QueryPerformanceCounter(&t1);
            check_hr(tex.keyed->ReleaseSync(kKeyD3D), "timing",
                     "IDXGIKeyedMutex::ReleaseSync(keep D3D ownership)");
        }
        if (timed)
            samples.push_back(qpc_ms(t0, t1, freq));
    };

    // Warm-up plus the correctness pass: the readback must see the checker the
    // Vulkan dispatch wrote through the shared allocation.
    one_handoff(false);
    if (kUseFence) {
        // Make the D3D11 copy engine wait for the Vulkan signal before reading.
        check_hr(d.ctx4->Wait(d.fence, counter), "verify", "ID3D11DeviceContext4::Wait");
    } else {
        check_hr(tex.keyed->AcquireSync(kKeyD3D, 5000), "verify",
                 "IDXGIKeyedMutex::AcquireSync(for readback)");
    }
    r.verify = verify_readback(d, tex);
    if (!kUseFence)
        check_hr(tex.keyed->ReleaseSync(kKeyD3D), "verify", "IDXGIKeyedMutex::ReleaseSync");
    logf("[verify] %ux%u readback %s (%llu mismatching pixels)", w, h,
         r.verify.ok ? "OK" : "FAILED", static_cast<unsigned long long>(r.verify.mismatches));

    for (int i = 0; i < 8; ++i)
        one_handoff(false); // settle clocks before measuring
    for (int i = 0; i < iterations; ++i)
        one_handoff(true);

    r.iterations = static_cast<uint32_t>(samples.size());
    if (!samples.empty()) {
        double sum = 0;
        for (double s : samples)
            sum += s;
        r.mean_ms = sum / static_cast<double>(samples.size());
        std::sort(samples.begin(), samples.end());
        r.min_ms = samples.front();
        r.max_ms = samples.back();
        r.p50_ms = percentile(samples, 0.50);
        r.p99_ms = percentile(samples, 0.99);
    }
    logf("[timing] %ux%u  p50 %.3f ms  p99 %.3f ms  (n=%u)", w, h, r.p50_ms, r.p99_ms,
         r.iterations);

    if (ev)
        CloseHandle(ev);
    v.api.vkDeviceWaitIdle(v.device);
    destroy_imported(v, img);
    return r;
}

// ---------------------------------------------------------------------------
// JSON emission
// ---------------------------------------------------------------------------

void emit_caps(Json& j, const VkState& v)
{
    const DeviceCaps& c = v.caps;
    j.begin_object("subgroup");
    j.num("size", static_cast<unsigned long long>(c.subgroup_size));
    if (c.subgroup_size_control) {
        j.num("min_size", static_cast<unsigned long long>(c.min_subgroup_size));
        j.num("max_size", static_cast<unsigned long long>(c.max_subgroup_size));
    } else {
        j.null("min_size");
        j.null("max_size");
    }
    j.boolean("size_control", c.subgroup_size_control);
    j.begin_object("operations");
    j.boolean("basic", c.op_basic);
    j.boolean("vote", c.op_vote);
    j.boolean("ballot", c.op_ballot);
    j.boolean("arithmetic", c.op_arithmetic);
    j.boolean("shuffle", c.op_shuffle);
    j.boolean("clustered", c.op_clustered);
    j.end_object();
    j.end_object();

    j.begin_object("features");
    j.boolean("shaderInt64", c.shader_int64);
    j.boolean("shaderInt16", c.shader_int16);
    j.boolean("storageBuffer16BitAccess", c.storage_16bit);
    j.boolean("shaderStorageImageWriteWithoutFormat", c.storage_image_write_without_format);
    j.end_object();
}

void emit_profile(Json& j, const ProfileDecision& p)
{
    j.begin_object("profile");
    j.str("id", p.profile);
    j.str("verdict", verdict_name(p.verdict));
    if (p.required_subgroup_size)
        j.num("required_subgroup_size", static_cast<unsigned long long>(p.required_subgroup_size));
    else
        j.null("required_subgroup_size");
    j.num("cluster_size", static_cast<unsigned long long>(p.cluster_size));
    j.strings("notes", p.notes);
    j.strings("blockers", p.blockers);
    j.str("reference", "paper 3.7");
    j.end_object();
}

void emit_size(Json& j, const SizeResult& s)
{
    j.begin_object();
    j.num("width", static_cast<unsigned long long>(s.width));
    j.num("height", static_cast<unsigned long long>(s.height));
    j.begin_object("verify");
    j.boolean("pass", s.verify.ok);
    j.num("mismatching_pixels", static_cast<unsigned long long>(s.verify.mismatches));
    if (!s.verify.ok) {
        j.num("first_x", static_cast<unsigned long long>(s.verify.first_x));
        j.num("first_y", static_cast<unsigned long long>(s.verify.first_y));
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", s.verify.got);
        j.str("got_rgba", buf);
        std::snprintf(buf, sizeof buf, "%08X", s.verify.want);
        j.str("want_rgba", buf);
    }
    j.end_object();
    j.begin_object("handoff_ms");
    j.num("iterations", static_cast<unsigned long long>(s.iterations));
    j.num("p50", s.p50_ms);
    j.num("p99", s.p99_ms);
    j.num("min", s.min_ms);
    j.num("max", s.max_ms);
    j.num("mean", s.mean_ms);
    j.end_object();
    j.end_object();
}

// ---------------------------------------------------------------------------

struct Options {
    int iterations = 600;
    int adapter = -1;
    bool quiet = false;
    std::string out_path;
};

void usage()
{
    std::fprintf(stderr,
                 "nxvc-d3dinterop " NXWARP_PROBE_BUILD_ID "\n"
                 "  --iterations N   timed handoffs per texture size (default 600)\n"
                 "  --adapter N      hardware DXGI adapter index (default: first)\n"
                 "  --out FILE       also write the JSON to FILE\n"
                 "  --quiet          suppress the stderr progress log\n"
                 "  --help\n");
}

int run(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--iterations")
            o.iterations = std::atoi(next("--iterations").c_str());
        else if (a == "--adapter")
            o.adapter = std::atoi(next("--adapter").c_str());
        else if (a == "--out")
            o.out_path = next("--out");
        else if (a == "--quiet")
            o.quiet = true;
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (o.iterations < 1)
        o.iterations = 1;
    g_verbose = !o.quiet;

    Json j;
    j.begin_object();
    j.str("probe", "nxvc-d3dinterop");
    j.str("probe_version", kProbeVersion);
    j.str("build", NXWARP_PROBE_BUILD_ID);
    j.str("interop_mode", kInteropMode);
    j.str("paper_sections", "3.7, 3.8");

    const AmfInfo amf = probe_amf();
    j.begin_object("amf");
    j.boolean("present", amf.present);
    if (amf.version.empty())
        j.null("version");
    else
        j.str("version", amf.version);
    j.str("note", amf.note.empty() ? std::string("informational only; NX Warp does not use AMF")
                                   : amf.note);
    j.end_object();

    bool pass = false;
    std::string fail_stage, fail_message;
    std::vector<SizeResult> sizes;

    D3DState d;
    VkState v;
    bool adapter_emitted = false;
    auto emit_adapter = [&] {
        if (adapter_emitted)
            return;
        adapter_emitted = true;
        j.begin_object("adapter");
        j.str("name", d.adapter_name.empty() ? std::string("unknown") : d.adapter_name);
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%04X", d.vendor_id);
        j.str("vendor_id", buf);
        std::snprintf(buf, sizeof buf, "0x%04X", d.device_id);
        j.str("device_id", buf);
        j.num("dedicated_vram_bytes", static_cast<unsigned long long>(d.dedicated_vram));
        j.str("d3d_feature_level", d.feature_level.empty() ? std::string("unknown")
                                                           : d.feature_level);
        j.str("driver_version_umd", d.umd_version.empty() ? std::string("unknown") : d.umd_version);
        j.end_object();
    };

    try {
        init_d3d(d, o.adapter);
        emit_adapter();
        create_shared_fence(d);

        load_vulkan(v);
        create_instance(v);
        pick_physical_device(v, d);
        query_caps(v);
        survey_extensions(v);
        check_external_semaphore(v);
        check_external_image_format(v);
        create_device(v);
        import_fence(v, d);
        create_pipeline(v);

        j.begin_object("vulkan");
        j.str("instance_version", vk_version_str(v.instance_version));
        j.str("device_api_version", vk_version_str(v.api_version));
        j.str("device_name", v.device_name);
        j.str("driver_name", v.driver_name);
        j.str("driver_info", v.driver_info);
        j.str("driver_version", driver_version_str(v.caps.vendor_id, v.driver_version));
        j.boolean("luid_matched_dxgi_adapter", v.luid_matched);
        j.num("queue_family", static_cast<unsigned long long>(v.queue_family));
        j.begin_object("extensions");
        j.strings("required_present", v.required_ext_present);
        j.strings("required_missing", v.required_ext_missing);
        j.strings("optional_present", v.optional_ext_present);
        j.end_object();
        j.boolean("d3d12_fence_importable", v.fence_importable);
        j.boolean("keyed_mutex_available", v.keyed_mutex_ext);
        j.boolean("external_image_format_ok", v.image_format_external_ok);
        emit_caps(j, v);
        j.end_object();

        emit_profile(j, decide_profile(v.caps));

        // 2048x2048 is the per-eye working size of paper 3.1; 2160x2160 is the
        // Pico 4 native eye buffer, and it is deliberately not a power of two.
        sizes.push_back(run_size(d, v, 2048, 2048, o.iterations));
        sizes.push_back(run_size(d, v, 2160, 2160, o.iterations));

        pass = true;
        for (const auto& s : sizes)
            if (!s.verify.ok)
                pass = false;
    } catch (const ProbeError& e) {
        fail_stage = e.stage;
        fail_message = e.message;
        logf("[error] %s: %s", e.stage.c_str(), e.message.c_str());
    } catch (const std::exception& e) {
        fail_stage = "unknown";
        fail_message = e.what();
        logf("[error] %s", e.what());
    }
    emit_adapter();

    j.begin_array("sizes");
    for (const auto& s : sizes)
        emit_size(j, s);
    j.end_array();

    j.boolean("pass", pass);
    if (fail_stage.empty()) {
        j.null("error");
    } else {
        j.begin_object("error");
        j.str("stage", fail_stage);
        j.str("message", fail_message);
        j.end_object();
    }
    j.end_object();

    // Vulkan teardown (D3DState has a destructor; VkState is torn down here so
    // it always happens before the DLL unloads).
    if (v.device) {
        v.api.vkDeviceWaitIdle(v.device);
        if (v.cpool)
            v.api.vkDestroyCommandPool(v.device, v.cpool, nullptr);
        if (v.dpool)
            v.api.vkDestroyDescriptorPool(v.device, v.dpool, nullptr);
        if (v.pipeline)
            v.api.vkDestroyPipeline(v.device, v.pipeline, nullptr);
        if (v.playout)
            v.api.vkDestroyPipelineLayout(v.device, v.playout, nullptr);
        if (v.dsl)
            v.api.vkDestroyDescriptorSetLayout(v.device, v.dsl, nullptr);
        if (v.shader)
            v.api.vkDestroyShaderModule(v.device, v.shader, nullptr);
        if (v.timeline)
            v.api.vkDestroySemaphore(v.device, v.timeline, nullptr);
        v.api.vkDestroyDevice(v.device, nullptr);
    }
    if (v.instance && v.api.vkDestroyInstance)
        v.api.vkDestroyInstance(v.instance, nullptr);
    if (v.lib)
        FreeLibrary(v.lib);

    const std::string& text = j.text();
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);

    if (!o.out_path.empty()) {
        if (FILE* f = std::fopen(o.out_path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fputc('\n', f);
            std::fclose(f);
        } else {
            std::fprintf(stderr, "could not write %s\n", o.out_path.c_str());
        }
    }

    return pass ? 0 : 1;
}

} // namespace
} // namespace nxwarp::win

int main(int argc, char** argv)
{
    // A probe must never pop a dialog: it runs headless over SSH.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return nxwarp::win::run(argc, argv);
}
