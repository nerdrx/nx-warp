// nxvc_vk_decoder: the two-dispatch Vulkan compute decoder.
//
//   upload  ->  Pass A (rANS entropy decode)  ->  Pass B (reconstruction)
//           ->  output images  ->  optional readback
//
// One command buffer per frame, one timeline-semaphore signal, timestamp
// queries around each dispatch.  Everything above the tile payload is parsed
// on the host by nxvc_vkdec_parse.cpp.
//
// The Vulkan boilerplate here is deliberately minimal and self-contained: it
// is the same shape the Pass A and Pass B harnesses already carry.  When
// vk/common's context / resources / pipeline helpers settle, instance and
// device creation, the buffer allocator and the pipeline cache in this file
// are the pieces that should be deleted in favour of nxvc_vk_context_create()
// and nxvc::vk::Buffer / Pipeline; the decode path itself does not change.
// See vk/decoder/README.md, "Relationship to vk/common".
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nxvc/nxvc_vk.h"
#include "nxvc_vkdec_parse.h"
#include "passA/syntax_constants.h"
#include "passB/syntax_constants.h"

#include "rans_decode.spv.h"
#include "reconstruct.spv.h"
#include "reconstruct_v1.spv.h"
#include "reconstruct_skip.spv.h"
#include "reconstruct_v1_x8.spv.h"
#include "reconstruct_x8.spv.h"
#include "warp_pred.spv.h"

namespace {

using nxvcvk::FrameParse;
using nxvcvk::LaneGroup;
using nxvcvk::StreamInfo;
using nxvcvk::InterCtx;

// Pass A's push constants: num_tiles, frame_nplanes, coef_stride, cbf_words,
// tools, [sparse] sparse.
constexpr uint32_t kPassAPushUints = 6;

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(
               clock::now().time_since_epoch())
        .count();
}

struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

struct Img {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
};

}  // namespace

// ---------------------------------------------------------------------------
struct nxvc_vk_decoder {
    // ---- device
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    bool own_instance = false, own_device = false;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    uint32_t subgroup_size = 0;
    bool has_size_control = false;
    // VK_KHR_pipeline_executable_properties: the driver's own account of what
    // it compiled -- registers, spill, shared memory, private memory.  PAPER
    // 3.2.3 asks for it where available.  It is opt-in
    // (NXVC_VKD_SHADER_STATS=1) because CAPTURE_STATISTICS is a pipeline
    // creation flag and a driver is allowed to compile differently with it
    // set, so it must not be on in a timed run.
    bool has_exec_props = false;
    bool want_shader_stats = false;
    PFN_vkGetPipelineExecutablePropertiesKHR fpExecProps = nullptr;
    PFN_vkGetPipelineExecutableStatisticsKHR fpExecStats = nullptr;
    bool have_timestamps = false;
    float ts_period = 0.f;

    // ---- config
    uint32_t want_output = NXVC_VKD_OUT_AUTO;
    uint32_t out_format = NXVC_VKD_OUT_RGBA8;  // resolved kOut* value
    uint32_t flags = 0;
    uint32_t read_ptr_mode = nxwarp_passA::kReadPtrBallot;
    // [v3] The directional-intra wavefront schedule Pass B is built with.  It
    // is a bitstream property (SYNTAX.md 7.6): 0 is the normative derivation
    // and the only one a conformant encoder emits; 1 and 3 exist so their
    // decode cost can be measured against the rate they cost.
    uint32_t dir_sched = 0;
    // Host-side reordering of Pass B's workgroup -> tile map.  Output is
    // identical either way; it only changes which tiles land in adjacent
    // workgroups.
    uint32_t tile_sort = 0;

    // ---- per-frame Vulkan objects
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t timeline_value = 0;
    // vkWaitSemaphores is core in 1.2 and an extension entry point on 1.1
    // (VK_KHR_timeline_semaphore).  Android's libvulkan.so stub for API 29
    // exports neither, so it is always resolved through the device rather
    // than linked -- which is also what an adopted device needs.
    PFN_vkWaitSemaphores fpWaitSemaphores = nullptr;
    // Same story, and used only to ask "is the frame done yet" without
    // blocking, so that nxvc_vk_decoder_stats() can take the timestamps for a
    // client that synchronises on the GPU and never waits on the host.
    PFN_vkGetSemaphoreCounterValue fpGetSemaphoreCounterValue = nullptr;
    // Fallback for a driver that advertises VK_KHR_timeline_semaphore and
    // its feature bit but refuses to create one -- which the Pico 4's Adreno
    // 650 driver (1.1.128, build 10/31/22) does, returning 5 from
    // vkCreateSemaphore for a timeline type while binary semaphores work.
    // The decode path only ever needs "has this frame finished", so a fence
    // answers it exactly.  nxvc_vk_decoder_timeline() then returns
    // VK_NULL_HANDLE and a compositor must wait through
    // nxvc_vk_decoder_wait() instead.
    VkFence fence = VK_NULL_HANDLE;
    bool fence_pending = false;
    // Signalled on request (NXVC_VKD_SUBMIT_SIGNAL_BINARY) when there is no
    // timeline, so a client on this driver can chain its own submission after
    // the decode on the queue instead of through the host.
    VkSemaphore binsem = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    // How many timestamps the frame in flight wrote, and whether they have
    // been read back into `stats` yet.  The readback used to sit only on the
    // synchronous path, so a client that submits with NXVC_VKD_SUBMIT_ASYNC
    // -- which is every real compositor, and the WiVRn client in particular
    // -- got pass_a_ms / pass_b_ms / gpu_ms of exactly zero and no way to see
    // where its frame time went.  It is now taken on the first wait or stats
    // read after the frame completes.
    uint32_t ts_count = 0;
    bool ts_pending = false;

    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslA = VK_NULL_HANDLE, dslB = VK_NULL_HANDLE;
    VkPipelineLayout plA = VK_NULL_HANDLE, plB = VK_NULL_HANDLE;
    VkShaderModule smA = VK_NULL_HANDLE;
    // Pass B's four build variants of one source, indexed
    // [intra_dir][xform_large]: the directional-intra wavefront and the 16x16
    // / 32x32 transform forms each exist or do not exist in the module rather
    // than behind a specialization constant, because on at least one of the
    // three ICDs the driver's own dead-code pass was measured and was not
    // enough.  passB/reconstruct.comp gives the numbers for both.  The first
    // index is chosen per DISPATCH -- the tiles that cannot enter the
    // wavefront are partitioned onto the module without it -- and the second
    // per frame, because xform_size is a tile-header field of a stream that
    // set the tool bit.
    // [inter] The WARP_SKIP module: one, not four.  A skip tile is never
    // INTRA and runs no transform, so neither build variant can reach it.
    VkShaderModule smBSkip = VK_NULL_HANDLE;
    VkShaderModule smB[2][2] = {{VK_NULL_HANDLE, VK_NULL_HANDLE},
                                {VK_NULL_HANDLE, VK_NULL_HANDLE}};
    // [inter] Pass W: the predictor.  Its own set layout, because it binds
    // three buffers and no image and has nothing to say about Pass B's
    // thirteen.  vk/decoder/inter/.
    VkDescriptorSetLayout dslW = VK_NULL_HANDLE;
    VkPipelineLayout plW = VK_NULL_HANDLE;
    VkShaderModule smW = VK_NULL_HANDLE;
    VkDescriptorSet dsetW = VK_NULL_HANDLE;
    VkPipeline pipeW = VK_NULL_HANDLE;
    VkDescriptorSet dsetA = VK_NULL_HANDLE, dsetB = VK_NULL_HANDLE;
    std::map<uint32_t, VkPipeline> pipesA;  // lanes | ctx_stride<<8 | xfl<<16
    // key: (format << 40) | (dirSched << 32) | storeWords
    std::map<uint64_t, VkPipeline> pipesB;

    // ---- buffers
    Buf staging, bBits, bDesc, bTables, bCoef, bCbf, bStatus, bRecs, bWgt,
        bModes, bOrder, bRead;
    // [inter] The four-slot reference ring, the predictor Pass W hands to
    // Pass B, and the parameter block that drives both.
    Buf bRing, bWPred, bWarp;
    // [sparse] Pass A's per-unit coefficient counts, and a host-visible mirror
    // that only exists when the caller asked for coefficient statistics.
    Buf bULen, bULenHost;
    std::vector<uint32_t> order;   // workgroup index -> tile index
    // [inter] Tiles that do NOT take the directional-intra wavefront come
    // first inside each eye's segment of `order`, and this is how many there
    // are.  See build_tile_order().
    uint32_t order_nodir[2] = {0, 0};
    // [inter] How many of each eye's tiles are WARP_SKIP, and therefore the
    // length of the leading range build_tile_order() puts them in.
    uint32_t order_nskip[2] = {0, 0};
    Img imgRgba, imgRgb10, imgLuma, imgCbCr;
    // [unorm] The same three 8-bit stores through normalised images.  Only
    // one group is ever real; the other is a 1x1 placeholder.
    Img imgRgbaN, imgLumaN, imgCbCrN;
    // 1 = Pass B writes the 8-bit stores through the UNORM images.  Off by
    // default on every platform; NXVC_VKD_UNORM=1 or --unorm 1 turns it on.
    // Exact either way -- the choice is performance only.
    uint32_t unorm_store = 0;
    // The image Pass B actually wrote, per store, whichever group is live.
    const Img &outRgba() const { return unorm_store ? imgRgbaN : imgRgba; }
    const Img &outLuma() const { return unorm_store ? imgLumaN : imgLuma; }
    const Img &outCbCr() const { return unorm_store ? imgCbCrN : imgCbCr; }

    // ---- stream state
    bool have_stream = false;
    StreamInfo si{};
    FrameParse fp{};
    bool resources_ready = false;
    // Byte layout of the staging buffer.
    VkDeviceSize offBits = 0, offDesc = 0, offTables = 0, offRecs = 0,
                 offWgt = 0, offOrder = 0;
    // Byte layout of the readback buffer.
    VkDeviceSize rbLuma = 0, rbCbCr = 0, rbRgba = 0, rbBytes = 0;
    bool need_alpha_pass = false;  // second Pass B dispatch for the A channel

    // ---- [inter] state
    InterCtx inter{};
    // The ring's layout, from nxvw_ring_layout().  Fixed for the stream.
    int ringOff[4] = {}, ringStride[4] = {}, ringPlaneW[4] = {};
    int ringSlotU16 = 0;
    int wpredStrideI16 = 0;
    // The staging buffer's inter blocks.
    VkDeviceSize offWarp = 0;
    // Host-side scratch for the parameter block, rebuilt per frame.
    std::vector<uint32_t> warp_words;

    nxvc_vkd_stats stats{};
    std::string err = "";
    std::string device_name = "";
    uint64_t tools_mask = 0;   // 0 until probe_device(); see tools_supported_for()
};

namespace {

using D = nxvc_vk_decoder;

nxvc_vkd_status seterr(D *d, nxvc_vkd_status st, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    d->err = b;
    return st;
}

#define VKTRY(d, expr)                                                    \
    do {                                                                  \
        VkResult _r = (expr);                                             \
        if (_r != VK_SUCCESS)                                             \
            return seterr((d), NXVC_VKD_ERR_VULKAN, "%s failed: VkResult %d", \
                          #expr, (int)_r);                                \
    } while (0)

// ------------------------------------------------------------------ memory
int find_memory(const D *d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (d->memProps.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

void destroy_buf(D *d, Buf &b) {
    if (b.mapped) vkUnmapMemory(d->dev, b.mem);
    if (b.buf) vkDestroyBuffer(d->dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(d->dev, b.mem, nullptr);
    b = Buf{};
}

// `host_cached` asks for a memory type the CPU can *read* at speed.  A plain
// HOST_VISIBLE|HOST_COHERENT allocation on a discrete GPU is write-combined:
// writing it is fast and reading it back is roughly 10 MB/s, which is fine for
// the one status word per tile and ruinous for anything larger.
nxvc_vkd_status make_buf(D *d, Buf &b, VkDeviceSize size,
                         VkBufferUsageFlags usage, bool host_visible,
                         bool host_cached = false) {
    if (b.buf && b.size >= size) return NXVC_VKD_OK;
    destroy_buf(d, b);
    if (size == 0) size = 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKTRY(d, vkCreateBuffer(d->dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(d->dev, b.buf, &mr);
    VkMemoryPropertyFlags want =
        host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int mt = -1;
    if (host_cached)
        mt = find_memory(d, mr.memoryTypeBits,
                         want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt < 0) mt = find_memory(d, mr.memoryTypeBits, want);
    if (mt < 0 && !host_visible) mt = find_memory(d, mr.memoryTypeBits, 0);
    if (mt < 0)
        return seterr(d, NXVC_VKD_ERR_NOMEM, "no memory type for a %llu B buffer",
                      (unsigned long long)size);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    VKTRY(d, vkAllocateMemory(d->dev, &ai, nullptr, &b.mem));
    VKTRY(d, vkBindBufferMemory(d->dev, b.buf, b.mem, 0));
    b.size = size;
    if (host_visible)
        VKTRY(d, vkMapMemory(d->dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return NXVC_VKD_OK;
}

void destroy_img(D *d, Img &i) {
    if (i.view) vkDestroyImageView(d->dev, i.view, nullptr);
    if (i.img) vkDestroyImage(d->dev, i.img, nullptr);
    if (i.mem) vkFreeMemory(d->dev, i.mem, nullptr);
    i = Img{};
}

nxvc_vkd_status make_img(D *d, Img &im, VkFormat fmt, uint32_t w, uint32_t h) {
    destroy_img(d, im);
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKTRY(d, vkCreateImage(d->dev, &ii, nullptr, &im.img));
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(d->dev, im.img, &mr);
    int mt = find_memory(d, mr.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memory(d, mr.memoryTypeBits, 0);
    if (mt < 0) return seterr(d, NXVC_VKD_ERR_NOMEM, "no memory type for image");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    VKTRY(d, vkAllocateMemory(d->dev, &ai, nullptr, &im.mem));
    VKTRY(d, vkBindImageMemory(d->dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKTRY(d, vkCreateImageView(d->dev, &vi, nullptr, &im.view));
    im.w = w;
    im.h = h;
    im.fmt = fmt;
    return NXVC_VKD_OK;
}

// ------------------------------------------------------------ device setup
bool name_matches(const char *name, const char *want) {
    if (!want || !*want) return true;
    std::string a(name), b(want);
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

bool has_device_ext(VkPhysicalDevice pd, const char *want) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> es(n);
    if (n) vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, es.data());
    for (const auto &e : es)
        if (std::strcmp(e.extensionName, want) == 0) return true;
    return false;
}

nxvc_vkd_status create_device(D *d, const nxvc_vkd_create_info *ci) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc_vk_decoder";
    // Ask for no more than the loader supports.  Android's 1.1 loader fails
    // vkCreateInstance outright on a 1.3 request, and the Pico 4's loader is
    // 1.1.  vkEnumerateInstanceVersion is itself 1.1; on a 1.0 loader the
    // symbol is absent and 1.0 is the answer.
    uint32_t loader = VK_API_VERSION_1_0;
    if (auto fp = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))
        fp(&loader);
    app.apiVersion = loader < VK_API_VERSION_1_3 ? loader : VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (ci->flags & NXVC_VKD_FLAG_VALIDATION) {
        ii.enabledLayerCount = 1;
        ii.ppEnabledLayerNames = &layer;
    }
    VkResult r = vkCreateInstance(&ii, nullptr, &d->inst);
    if (r != VK_SUCCESS)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "vkCreateInstance failed: VkResult %d", (int)r);
    d->own_instance = true;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d->inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(d->inst, &n, devs.data());
    for (VkPhysicalDevice pd : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        // Pass A stores int16 (core 1.1: shaderInt16 +
        // storageBuffer16BitAccess) and the decoder signals a timeline
        // semaphore, which is core in 1.2 and VK_KHR_timeline_semaphore on
        // 1.1.  The Pico 4's Adreno 650 driver is 1.1.128 and has the
        // extension, so 1.1 plus that extension is the floor.
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        if (p.apiVersion < VK_API_VERSION_1_2 &&
            !has_device_ext(pd, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
            continue;
        if (!name_matches(p.deviceName, ci->device_name)) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; ++q) {
            if (!(qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            d->phys = pd;
            d->qfam = q;
            break;
        }
        if (d->phys) break;
    }
    if (!d->phys)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "no Vulkan 1.1 device with timeline semaphores and a "
                      "compute queue%s%s",
                      ci->device_name ? " matching " : "",
                      ci->device_name ? ci->device_name : "");

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(d->phys, &props);
    const bool has13 = props.apiVersion >= VK_API_VERSION_1_3;
    const bool has12 = props.apiVersion >= VK_API_VERSION_1_2;

    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = has13 ? (void *)&f13 : nullptr;
    // The aggregate VkPhysicalDeviceVulkan1xFeatures structs are all 1.2
    // additions -- including the one named "Vulkan11" -- so a 1.1 device has
    // to be asked with the core-1.1 structs it actually knows: 16-bit storage
    // and timeline semaphores come from these two instead.
    VkPhysicalDeviceTimelineSemaphoreFeatures fts{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDevice16BitStorageFeatures f16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    f16.pNext = &fts;
    VkPhysicalDeviceVulkan11Features f11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = has12 ? (void *)&f11 : (void *)&f16;
    vkGetPhysicalDeviceFeatures2(d->phys, &f2);
    if (!f2.features.shaderInt16)
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "%s lacks shaderInt16", props.deviceName);
    if (!(has12 ? f11.storageBuffer16BitAccess : f16.storageBuffer16BitAccess))
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "%s lacks storageBuffer16BitAccess", props.deviceName);
    if (!(has12 ? f12.timelineSemaphore : fts.timelineSemaphore))
        return seterr(d, NXVC_VKD_ERR_NO_DEVICE,
                      "%s lacks timelineSemaphore", props.deviceName);
    d->has_size_control = has13 && f13.subgroupSizeControl &&
                          f13.computeFullSubgroups;

    float prio = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = d->qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features e13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.subgroupSizeControl = d->has_size_control;
    e13.computeFullSubgroups = d->has_size_control;
    VkPhysicalDeviceVulkan12Features e12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.timelineSemaphore = VK_TRUE;
    e12.pNext = has13 ? (void *)&e13 : nullptr;
    VkPhysicalDeviceTimelineSemaphoreFeatures ets{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    ets.timelineSemaphore = VK_TRUE;
    VkPhysicalDevice16BitStorageFeatures e16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    e16.storageBuffer16BitAccess = VK_TRUE;
    e16.pNext = &ets;
    VkPhysicalDeviceVulkan11Features e11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.storageBuffer16BitAccess = VK_TRUE;
    e11.pNext = &e12;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.features.shaderInt16 = VK_TRUE;
    e2.pNext = has12 ? (void *)&e11 : (void *)&e16;

    // The extension only has to be asked for on a 1.1 device; on 1.2+ it is
    // core and naming it is redundant (and refused by some loaders).
    std::vector<const char *> devExts;
    if (!has12) devExts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    // The driver's own shader statistics, opt-in.  The feature struct has to
    // be chained or the extension is enabled and unusable.
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR epf{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    if (d->want_shader_stats &&
        has_device_ext(d->phys,
                       VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)) {
        devExts.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        epf.pipelineExecutableInfo = VK_TRUE;
        epf.pNext = (void *)e2.pNext;
        e2.pNext = &epf;
        d->has_exec_props = true;
    }
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &e2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    if (!devExts.empty()) {
        di.enabledExtensionCount = (uint32_t)devExts.size();
        di.ppEnabledExtensionNames = devExts.data();
    }
    r = vkCreateDevice(d->phys, &di, nullptr, &d->dev);
    if (r != VK_SUCCESS)
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkCreateDevice failed: %d",
                      (int)r);
    d->own_device = true;
    vkGetDeviceQueue(d->dev, d->qfam, 0, &d->queue);
    if (d->has_exec_props) {
        d->fpExecProps = (PFN_vkGetPipelineExecutablePropertiesKHR)
            vkGetDeviceProcAddr(d->dev, "vkGetPipelineExecutablePropertiesKHR");
        d->fpExecStats = (PFN_vkGetPipelineExecutableStatisticsKHR)
            vkGetDeviceProcAddr(d->dev, "vkGetPipelineExecutableStatisticsKHR");
        if (!d->fpExecProps || !d->fpExecStats) d->has_exec_props = false;
    }
    return NXVC_VKD_OK;
}

// The driver's own account of a compiled pipeline, to stderr.  Registers,
// spill and private memory are the three numbers that decide whether a kernel
// this size fits an Adreno wave; nothing here is on any timed path.
void dump_shader_stats(D *d, VkPipeline p, const char *what) {
    if (!d->has_exec_props || !p) return;
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = p;
    uint32_t n = 0;
    if (d->fpExecProps(d->dev, &pi, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<VkPipelineExecutablePropertiesKHR> eps(
        n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    d->fpExecProps(d->dev, &pi, &n, eps.data());
    for (uint32_t e = 0; e < n; ++e) {
        VkPipelineExecutableInfoKHR ei{
            VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = p;
        ei.executableIndex = e;
        uint32_t sn = 0;
        if (d->fpExecStats(d->dev, &ei, &sn, nullptr) != VK_SUCCESS) continue;
        std::vector<VkPipelineExecutableStatisticKHR> ss(
            sn, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        d->fpExecStats(d->dev, &ei, &sn, ss.data());
        std::fprintf(stderr, "[shader-stats] %s / %s (%s), subgroup %u\n", what,
                     eps[e].name, eps[e].description, eps[e].subgroupSize);
        for (uint32_t i = 0; i < sn; ++i) {
            const auto &st = ss[i];
            switch (st.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                std::fprintf(stderr, "    %-40s %s\n", st.name,
                             st.value.b32 ? "true" : "false");
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                std::fprintf(stderr, "    %-40s %lld\n", st.name,
                             (long long)st.value.i64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                std::fprintf(stderr, "    %-40s %llu\n", st.name,
                             (unsigned long long)st.value.u64);
                break;
            default:
                std::fprintf(stderr, "    %-40s %f\n", st.name, st.value.f64);
                break;
            }
        }
    }
}

nxvc_vkd_status probe_device(D *d) {
    vkGetPhysicalDeviceProperties(d->phys, &d->props);
    vkGetPhysicalDeviceMemoryProperties(d->phys, &d->memProps);
    d->device_name = d->props.deviceName;
    // What this decoder will ACCEPT, which on some devices is less than what
    // it implements.  nxvc_vkdec_parse.cpp tools_supported_for() says why.
    d->tools_mask =
        nxvcvk::tools_supported_for(d->props.vendorID, d->props.deviceName);
    d->have_timestamps = d->props.limits.timestampComputeAndGraphics != 0 &&
                         d->props.limits.timestampPeriod > 0.f;
    d->ts_period = d->props.limits.timestampPeriod;

    VkPhysicalDeviceSubgroupProperties sg{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(d->phys, &p2);
    d->subgroup_size = sg.subgroupSize;
    const VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                        VK_SUBGROUP_FEATURE_BALLOT_BIT;
    if ((sg.supportedOperations & need) != need)
        d->read_ptr_mode = nxwarp_passA::kReadPtrLdsFallback;
    if (d->flags & NXVC_VKD_FLAG_LDS_FALLBACK)
        d->read_ptr_mode = nxwarp_passA::kReadPtrLdsFallback;
    // [unorm] Off by default everywhere, including Android.  The conversion
    // is exact on all three drivers (tests/vk-decoder/unorm), so this is
    // purely a performance question, and the performance did not survive
    // contact with the device: -7 % of Pass B at QP 24, +2 % at QP 36, and
    // nothing at all with INTRA_DIR on (vk/decoder/README.md, "The UNORM
    // store").  Against that, the switch changes the VkFormat that
    // nxvc_vk_decoder_images() hands out, which every consumer of the image
    // sees -- the WiVRn NX client samples it directly.  A format change
    // visible across the ABI needs more than a 7 % Pass B win on a pass that
    // is 30x over its frame budget either way, so it stays opt-in.
    if (const char *e = std::getenv("NXVC_VKD_UNORM"))
        d->unorm_store = (e[0] == '1') ? 1u : 0u;

    if (d->props.limits.maxComputeWorkGroupInvocations < 256)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "device allows only %u workgroup invocations, Pass B "
                      "needs 256",
                      d->props.limits.maxComputeWorkGroupInvocations);
    return NXVC_VKD_OK;
}

// --------------------------------------------------------------- pipelines
nxvc_vkd_status make_layouts(D *d) {
    auto set_layout = [&](int nbuf, int nimg, VkDescriptorSetLayout *out) {
        std::vector<VkDescriptorSetLayoutBinding> b((size_t)(nbuf + nimg));
        for (int i = 0; i < nbuf + nimg; ++i) {
            b[(size_t)i].binding = (uint32_t)i;
            b[(size_t)i].descriptorType =
                i < nbuf ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                         : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[(size_t)i].descriptorCount = 1;
            b[(size_t)i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = (uint32_t)b.size();
        ci.pBindings = b.data();
        return vkCreateDescriptorSetLayout(d->dev, &ci, nullptr, out);
    };
    // Pass A: 8 storage buffers (bitstream, descriptors, tables, coefficients,
    // CBF bits, status, [v3] intra modes, [sparse] unit lengths).
    VKTRY(d, set_layout(8, 0, &d->dslA));
    // Pass B: buffers 0-2, images 3-6, then [v3] buffers 7 (modes) and 8 (the
    // workgroup -> tile map) and [sparse] 9 (unit lengths).  The images keep
    // their bindings so nothing that already referenced them has to move.
    {
        // [unorm] and 10-12, the normalised twins of 3, 5 and 6.
        // [inter] and 13-15: the predictor Pass W wrote, the reference-ring
        // slot this frame writes, and the parameter block's ring geometry.
        VkDescriptorSetLayoutBinding b[16]{};
        for (int i = 0; i < 16; ++i) {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = ((i >= 3 && i <= 6) || (i >= 10 && i <= 12))
                                      ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 16;
        ci.pBindings = b;
        VKTRY(d, vkCreateDescriptorSetLayout(d->dev, &ci, nullptr, &d->dslB));
    }

    VkPushConstantRange pcA{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)sizeof(uint32_t) * kPassAPushUints};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &d->dslA;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcA;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plA));

    VkPushConstantRange pcB{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)sizeof(nxvw::NxvwPassBPush)};
    pl.pSetLayouts = &d->dslB;
    pl.pPushConstantRanges = &pcB;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plB));

    // [inter] Pass W: ring in, params in, predictor out, tile order in.
    VKTRY(d, set_layout(4, 0, &d->dslW));
    VkPushConstantRange pcW{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)sizeof(nxvw::NxvwWarpPush)};
    pl.pSetLayouts = &d->dslW;
    pl.pPushConstantRanges = &pcW;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plW));

    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(rans_decode_spv);
    sm.pCode = rans_decode_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smA));
    sm.codeSize = sizeof(reconstruct_spv);
    sm.pCode = reconstruct_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smB[1][1]));
    sm.codeSize = sizeof(reconstruct_v1_spv);
    sm.pCode = reconstruct_v1_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smB[0][1]));
    sm.codeSize = sizeof(reconstruct_x8_spv);
    sm.pCode = reconstruct_x8_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smB[1][0]));
    sm.codeSize = sizeof(reconstruct_v1_x8_spv);
    sm.pCode = reconstruct_v1_x8_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smB[0][0]));
    sm.codeSize = sizeof(reconstruct_skip_spv);
    sm.pCode = reconstruct_skip_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smBSkip));
    sm.codeSize = sizeof(warp_pred_spv);
    sm.pCode = warp_pred_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smW));

    // Pass A's 8 storage buffers plus Pass B's 6 (bindings 0-2, 7-9) is 14,
    // not 12; bindings 8 and 9 arrived with the tile map and the sparse unit
    // lengths and this count did not follow.  RADV and lavapipe hand out
    // descriptors past the declared pool size, so the shortfall was invisible
    // on both; the Adreno 650 driver returns VK_ERROR_OUT_OF_POOL_MEMORY,
    // which is the conformant answer.
    // Pass A's 8 storage buffers, Pass B's 9 (bindings 0-2, 7-9, 13-15) and
    // Pass W's 3 is 20.  RADV and lavapipe hand out descriptors past the
    // declared pool size, so a shortfall is invisible on both; the Adreno 650
    // driver returns VK_ERROR_OUT_OF_POOL_MEMORY, which is the conformant
    // answer, and is why this number is derived rather than guessed.
    VkDescriptorPoolSize sz[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 20},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7}};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 3;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sz;
    VKTRY(d, vkCreateDescriptorPool(d->dev, &dp, nullptr, &d->dpool));
    VkDescriptorSetLayout ls[3] = {d->dslA, d->dslB, d->dslW};
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo da{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = d->dpool;
    da.descriptorSetCount = 3;
    da.pSetLayouts = ls;
    VKTRY(d, vkAllocateDescriptorSets(d->dev, &da, sets));
    d->dsetA = sets[0];
    d->dsetB = sets[1];
    d->dsetW = sets[2];
    return NXVC_VKD_OK;
}

// [minor 6] The pipeline cache key carries the context-table stride as well as
// the lane count: the stride sizes Pass A's shared table, so a v1/v2 frame and
// a v3 frame want different kernels and a frame must not inherit the other's.
nxvc_vkd_status pipeline_a(D *d, uint32_t lanes, uint32_t ctx_stride,
                           uint32_t xform_large, uint32_t entropy_mode,
                           VkPipeline *out) {
    const uint32_t key =
        lanes | (ctx_stride << 8) | (xform_large << 16) | (entropy_mode << 17);
    auto it = d->pipesA.find(key);
    if (it != d->pipesA.end()) {
        *out = it->second;
        return NXVC_VKD_OK;
    }
    // The ballot path needs a subgroup at least as wide as one tile's lane
    // cluster; otherwise the cluster straddles subgroups and the prefix count
    // is wrong.  The LDS fallback produces identical offsets with no subgroup
    // op at all (vk/decoder/passA/README.md).
    uint32_t mode = d->read_ptr_mode;
    if (d->subgroup_size < lanes) mode = nxwarp_passA::kReadPtrLdsFallback;
    // [entropy-lite] One tile per workgroup, and the read-pointer mode and
    // lane count are unread: the Lite path has no rANS lanes and no shared
    // read pointer.  The workgroup is the same 256 threads either way.
    const bool lite = entropy_mode == nxwarp_passA::kEntropyLiteFixed;
    const uint32_t tpg =
        lite ? 1u : nxwarp_passA::nxs_tiles_per_group(lanes);
    const uint32_t data[6] = {mode,        tpg,        lanes,
                              entropy_mode, ctx_stride, xform_large};
    VkSpecializationMapEntry me[6] = {{0, 0, 4},  {1, 4, 4},  {2, 8, 4},
                                      {3, 12, 4}, {4, 16, 4}, {5, 20, 4}};
    VkSpecializationInfo spec{6, me, sizeof(data), data};

    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    // DISPATCH_BASE lets one dispatch cover a contiguous slice of the tile
    // descriptor array, which is how the frame's tiles are grouped by lane
    // count without an extra push constant.
    ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    if (d->has_exec_props)
        ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = d->smA;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = &spec;
    // The lane cluster must not straddle a subgroup; a partial trailing
    // subgroup would break the ballot prefix (passA/README.md).
    if (d->has_size_control && mode == nxwarp_passA::kReadPtrBallot)
        ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    ci.layout = d->plA;
    VkPipeline p = VK_NULL_HANDLE;
    VKTRY(d, vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &p));
    d->pipesA[key] = p;
    *out = p;
    if (d->has_exec_props) {
        char tag[64];
        std::snprintf(tag, sizeof tag,
                      "passA lanes=%u mode=%u tpg=%u ctx=%u xfl=%u", lanes,
                      mode, tpg, ctx_stride, xform_large);
        dump_shader_stats(d, p, tag);
    }
    return NXVC_VKD_OK;
}

// [inter] Two ablations, and they produce WRONG PICTURES on purpose.
//
// The inter path's share of Pass B is not obvious from the outside: the
// predictor hook and the reference-ring store are both per sample and both
// compiled in frame-wide, and neither can be timed by turning the tool off,
// because turning the tool off changes the frame.  These turn off one half of
// the kernel while decoding the same stream, which is the only way to price
// them against each other.
//
// NXVC_VKD_ABL_NORING drops the ring store, so every frame after the first
// predicts from a stale slot.  NXVC_VKD_ABL_NOWPRED drops the predictor hook,
// so every inter tile reconstructs its residual over nothing.  Both are for
// `--stats` on a stream you already know the timing shape of, and nothing
// else.  On a 7900 XTX with the 1088x1088 head-turn fixture, 289 tiles,
// 13.4 KB a frame, 82 % WARP_SKIP:
//
//   baseline          passB 0.110 ms
//   no ring store     passB 0.069 ms   -- the ring store is 37 %
//   no wpred hook     passB 0.078 ms   -- the predictor hook is 29 %
//   neither           passB 0.068 ms
static int32_t inter_pred_on(const D *, bool inter) {
    return (inter && !std::getenv("NXVC_VKD_ABL_NOWPRED")) ? 1 : 0;
}
static int32_t ring_store_on(const D *, bool inter) {
    return (inter && !std::getenv("NXVC_VKD_ABL_NORING")) ? 1 : 0;
}

nxvc_vkd_status pipeline_b(D *d, uint32_t fmt, int32_t fmt2, int32_t sparse,
                           uint32_t store_words, int32_t intra_dir,
                           int32_t split_tool, int32_t xform_large,
                           int32_t inter_pred, int32_t ring_store,
                           VkPipeline *out, bool skip_only = false) {
    const uint32_t sched = d->dir_sched;
    uint64_t key = ((uint64_t)(uint32_t)skip_only << 59) |
                   ((uint64_t)(uint32_t)ring_store << 63) |
                   ((uint64_t)(uint32_t)inter_pred << 62) |
                   ((uint64_t)(uint32_t)xform_large << 61) |
                   ((uint64_t)(uint32_t)split_tool << 60) |
                   ((uint64_t)(uint32_t)intra_dir << 56) |
                   ((uint64_t)d->unorm_store << 52) |
                   ((uint64_t)(uint32_t)sparse << 48) |
                   ((uint64_t)(uint32_t)(fmt2 + 1) << 44) |
                   ((uint64_t)fmt << 40) | ((uint64_t)sched << 32) | store_words;
    auto it = d->pipesB.find(key);
    if (it != d->pipesB.end()) {
        *out = it->second;
        return NXVC_VKD_OK;
    }
    size_t lds = (size_t)store_words * 4 + 512;
    if (lds > d->props.limits.maxComputeSharedMemorySize)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "Pass B needs %zu B of shared memory, device offers %u B",
                      lds, d->props.limits.maxComputeSharedMemorySize);
    const int32_t data[9] = {(int32_t)fmt,  (int32_t)store_words,
                             (int32_t)sched, fmt2,
                             sparse,         (int32_t)d->unorm_store,
                             split_tool,     inter_pred,
                             ring_store};
    VkSpecializationMapEntry me[9] = {{0, 0, 4},  {1, 4, 4},  {2, 8, 4},
                                      {3, 12, 4}, {4, 16, 4}, {5, 20, 4},
                                      {6, 24, 4}, {7, 28, 4}, {8, 32, 4}};
    VkSpecializationInfo spec{9, me, sizeof(data), data};
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    // [inter] A frame that carries a STEREO tile runs Pass B once per eye,
    // with a barrier between, because a STEREO tile reads the first eye of
    // THIS frame's ring slot and a dispatch has no ordering inside it.  The
    // second dispatch covers workgroups [n0, ntiles), which is what
    // vkCmdDispatchBase expresses.
    ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    if (d->has_exec_props)
        ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module =
        skip_only ? d->smBSkip
                  : d->smB[intra_dir != 0 ? 1 : 0][xform_large != 0 ? 1 : 0];
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = &spec;
    ci.layout = d->plB;
    VkPipeline p = VK_NULL_HANDLE;
    VKTRY(d, vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &p));
    d->pipesB[key] = p;
    *out = p;
    if (d->has_exec_props) {
        char tag[96];
        std::snprintf(tag, sizeof tag,
                      "passB fmt=%u fmt2=%d sched=%u storeWords=%u lds=%zuB "
                      "intraDir=%d xformLarge=%d",
                      fmt, fmt2, sched, store_words, lds, intra_dir,
                      xform_large);
        dump_shader_stats(d, p, tag);
    }
    return NXVC_VKD_OK;
}

// [inter] Pass W has no specialization constants at all: everything it needs
// is per frame and lives in the push block or the parameter buffer, and one
// pipeline for every stream shape is what keeps the third dispatch free of the
// pipeline-cache key the other two need.
nxvc_vkd_status pipeline_w(D *d, VkPipeline *out) {
    if (d->pipeW != VK_NULL_HANDLE) {
        *out = d->pipeW;
        return NXVC_VKD_OK;
    }
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    if (d->has_exec_props)
        ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = d->smW;
    ci.stage.pName = "main";
    ci.layout = d->plW;
    VKTRY(d, vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &d->pipeW));
    *out = d->pipeW;
    if (d->has_exec_props) dump_shader_stats(d, d->pipeW, "passW");
    return NXVC_VKD_OK;
}

// ------------------------------------------------------------- resources
VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) / a * a;
}

nxvc_vkd_status ensure_bits(D *d, VkDeviceSize bytes);

nxvc_vkd_status check_storage_format(D *d, VkFormat f) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(d->phys, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "VkFormat %d is not usable as a storage image", (int)f);
    return NXVC_VKD_OK;
}

nxvc_vkd_status make_resources(D *d) {
    const StreamInfo &si = d->si;
    const uint32_t ntiles = si.tile_count;
    const bool chroma420 = si.chroma == 0;
    const uint32_t coef_stride = (uint32_t)nxvw::nxvw_coef_stride_i16(
        chroma420 ? 1 : 0, si.alpha ? 1 : 0);
    const uint32_t cbf_words = nxwarp_passA::kCbfWordsPerTile;

    // Resolve the output format.
    uint32_t want = d->want_output;
    if (want == NXVC_VKD_OUT_AUTO)
        want = (chroma420 && si.color_transform == 0) ? NXVC_VKD_OUT_YCBCR420
                                                      : NXVC_VKD_OUT_RGBA8;
    if (want == NXVC_VKD_OUT_YCBCR420 &&
        !(chroma420 && si.color_transform == 0))
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "the two-plane 4:2:0 output needs a 4:2:0 stream with no "
                      "colour transform");
    d->out_format = want == NXVC_VKD_OUT_RGBA8      ? (uint32_t)nxvw::kOutRgba8
                    : want == NXVC_VKD_OUT_RGB10A2 ? (uint32_t)nxvw::kOutRgb10A2
                                                   : (uint32_t)nxvw::kOutYcbcr420;
    // The two-plane path writes no alpha.  A stream that carries one gets a
    // second Pass B dispatch in the RGBA8 format, whose A channel is exactly
    // the alpha plane at its full 64x64-per-tile extent -- the same value the
    // reference decoder writes into plane 3.
    d->need_alpha_pass =
        (d->out_format == (uint32_t)nxvw::kOutYcbcr420) && si.alpha != 0;

    // [inter] The output image spans the eye pair: a stereo frame is `eyes`
    // pictures ([SYN] 3.3), and the merged raster is exact because
    // parse_stream_header() refuses eyes == 2 with a width that is not a
    // multiple of 64.  The chroma image follows the same rule -- CW is the
    // pair's chroma width, not one eye's.
    const uint32_t W = si.width * si.eyes, H = si.height;
    const uint32_t CW = (si.cw * si.eyes), CH = si.ch;

    // ---- images
    nxvc_vkd_status st;
    // Pass A's binding 0 must point at a real buffer before the descriptor
    // writes below; ensure_bits() grows it again when a frame needs more.
    if ((st = ensure_bits(d, 1u << 16))) return st;
    const bool needRgba = d->out_format == (uint32_t)nxvw::kOutRgba8 ||
                          d->need_alpha_pass;
    const bool needRgb10 = d->out_format == (uint32_t)nxvw::kOutRgb10A2;
    const bool needYuv = d->out_format == (uint32_t)nxvw::kOutYcbcr420;
    // [unorm] Exactly one of the two 8-bit store groups is real.
    const bool uI = d->unorm_store == 0;
    if (uI && needRgba && (st = check_storage_format(d, VK_FORMAT_R8G8B8A8_UINT)))
        return st;
    if (needRgb10 &&
        (st = check_storage_format(d, VK_FORMAT_A2B10G10R10_UINT_PACK32)))
        return st;
    if (uI && needYuv) {
        if ((st = check_storage_format(d, VK_FORMAT_R8_UINT))) return st;
        if ((st = check_storage_format(d, VK_FORMAT_R8G8_UINT))) return st;
    }
    // Unused bindings still have to point at a real storage image, so the
    // formats the frame does not write get a 1x1 placeholder.
    if ((st = make_img(d, d->imgRgba, VK_FORMAT_R8G8B8A8_UINT,
                       (uI && needRgba) ? W : 1, (uI && needRgba) ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgRgb10, VK_FORMAT_A2B10G10R10_UINT_PACK32,
                       needRgb10 ? W : 1, needRgb10 ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgLuma, VK_FORMAT_R8_UINT,
                       (uI && needYuv) ? W : 1, (uI && needYuv) ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgCbCr, VK_FORMAT_R8G8_UINT,
                       (uI && needYuv) ? CW : 1, (uI && needYuv) ? CH : 1)))
        return st;
    // [unorm] The normalised twins.  Whichever group Pass B is not compiled
    // for shrinks to a 1x1 placeholder, so exactly one full-size copy of each
    // plane exists at any time and the memory cost is unchanged.
    const bool uN = d->unorm_store != 0;
    if (uN && needRgba && (st = check_storage_format(d, VK_FORMAT_R8G8B8A8_UNORM)))
        return st;
    if (uN && needYuv) {
        if ((st = check_storage_format(d, VK_FORMAT_R8_UNORM))) return st;
        if ((st = check_storage_format(d, VK_FORMAT_R8G8_UNORM))) return st;
    }
    if ((st = make_img(d, d->imgRgbaN, VK_FORMAT_R8G8B8A8_UNORM,
                       (uN && needRgba) ? W : 1, (uN && needRgba) ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgLumaN, VK_FORMAT_R8_UNORM,
                       (uN && needYuv) ? W : 1, (uN && needYuv) ? H : 1)))
        return st;
    if ((st = make_img(d, d->imgCbCrN, VK_FORMAT_R8G8_UNORM,
                       (uN && needYuv) ? CW : 1, (uN && needYuv) ? CH : 1)))
        return st;

    // ---- buffers
    const VkBufferUsageFlags kSsbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkDeviceSize coefBytes = (VkDeviceSize)ntiles * coef_stride * 2;
    if ((st = make_buf(d, d->bCoef, coefBytes, kSsbo, false))) return st;
    if ((st = make_buf(d, d->bCbf, (VkDeviceSize)ntiles * cbf_words * 4, kSsbo,
                       false)))
        return st;
    // Host-visible: one uint per tile, written once by Pass A and read on the
    // CPU right after the wait, so it costs nothing to keep it mappable.
    // One uint per *descriptor slot*, which is the padded, lane-grouped array
    // Pass A dispatches over -- not the tile count.  The slack is derived from
    // the workgroup shape (nxs_desc_slots), because it is exactly the sum of
    // the six groups' alignment padding and therefore doubles when the shape
    // does; a fixed 64 was already under the 76 that 16 tiles per group can
    // need and badly under 32's 152.
    const VkDeviceSize descSlots =
        (VkDeviceSize)nxwarp_passA::nxs_desc_slots(ntiles);
    if ((st = make_buf(d, d->bStatus, descSlots * 4,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)))
        return st;
    if ((st = make_buf(d, d->bDesc,
                       descSlots * nxwarp_passA::kTileDescUints * 4,
                       kSsbo, false)))
        return st;
    if ((st = make_buf(d, d->bTables,
                       (VkDeviceSize)nxwarp_passA::kNumTableSets *
                           nxwarp_passA::kNumCtx * nxwarp_passA::kNumSym * 4,
                       kSsbo, false)))
        return st;
    if ((st = make_buf(d, d->bRecs, (VkDeviceSize)ntiles * 16, kSsbo, false)))
        return st;
    if ((st = make_buf(d, d->bWgt, 512 * 4, kSsbo, false))) return st;
    // [v3] Pass A writes the per-block intra modes here and Pass B reads them:
    // kModeRegionUints uints per tile, 160 B, against the coefficient slot's
    // 12.5 KB.
    if ((st = make_buf(d, d->bModes,
                       (VkDeviceSize)(ntiles + 64) *
                           nxwarp_passA::kModeRegionUints * 4,
                       kSsbo, false)))
        return st;
    if ((st = make_buf(d, d->bOrder, (VkDeviceSize)(ntiles + 1) * 4, kSsbo,
                       false)))
        return st;
    // [sparse] One byte per coding unit, 264 B per tile against the
    // coefficient slot's 12.5 KB.  Pass A writes it, Pass B reads it.
    const VkDeviceSize ulenBytes =
        (VkDeviceSize)(ntiles + 64) * nxwarp_passA::kUnitLenWordsPerTile * 4;
    if ((st = make_buf(d, d->bULen, ulenBytes, kSsbo, false))) return st;
    // [inter] The four-slot reference ring, the predictor, and the parameter
    // block.  All three exist whatever the stream's tools say, because an
    // unbound descriptor is not legal and a 1-uint placeholder costs nothing;
    // the ring is the only one that is ever large, and it is only allocated
    // at full size for a stream that sets INTER.
    {
        const bool want_inter = (si.tools & (1ull << 10)) != 0;
        int off[4], stride[4], planeW[4], slot = 0;
        nxvw::nxvw_ring_layout((int)si.width, (int)si.height, (int)si.cw,
                               (int)si.ch, (int)si.eyes, si.nplanes(), off,
                               stride, planeW, &slot);
        for (int i = 0; i < 4; ++i) {
            d->ringOff[i] = off[i];
            d->ringStride[i] = stride[i];
            d->ringPlaneW[i] = planeW[i];
        }
        d->ringSlotU16 = slot;
        d->wpredStrideI16 =
            nxvw::nxvw_wpred_stride_i16(chroma420 ? 1 : 0, si.alpha ? 1 : 0);
        const VkDeviceSize ringBytes =
            want_inter ? (VkDeviceSize)slot * 4 * 2 : 4;
        const VkDeviceSize wpredBytes =
            want_inter ? (VkDeviceSize)ntiles * d->wpredStrideI16 * 2 : 4;
        const VkDeviceSize warpBytes =
            (VkDeviceSize)(NXVW_WARP_HDR_UINTS +
                           (size_t)ntiles * NXVW_WARP_TILE_UINTS) * 4;
        if ((st = make_buf(d, d->bRing, ringBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bWPred, wpredBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bWarp, warpBytes, kSsbo, false))) return st;
        d->inter.resize(ntiles);
    }
    // Only when the caller asked for the exact coefficient traffic: a
    // host-visible copy of the same buffer, filled after Pass A.
    if (d->flags & NXVC_VKD_FLAG_COEF_STATS) {
        if ((st = make_buf(d, d->bULenHost, ulenBytes,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true)))
            return st;
    }

    // ---- readback
    if (d->flags & NXVC_VKD_FLAG_READBACK) {
        VkDeviceSize o = 0;
        d->rbLuma = o;
        if (needYuv) o += align_up((VkDeviceSize)W * H, 256);
        d->rbCbCr = o;
        if (needYuv) o += align_up((VkDeviceSize)CW * CH * 2, 256);
        d->rbRgba = o;
        if (needRgba || needRgb10) o += align_up((VkDeviceSize)W * H * 4, 256);
        d->rbBytes = o ? o : 4;
        if ((st = make_buf(d, d->bRead, d->rbBytes,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)))
            return st;
    }

    // ---- descriptor writes
    VkDescriptorBufferInfo a[8] = {{d->bBits.buf, 0, VK_WHOLE_SIZE},
                                   {d->bDesc.buf, 0, VK_WHOLE_SIZE},
                                   {d->bTables.buf, 0, VK_WHOLE_SIZE},
                                   {d->bCoef.buf, 0, VK_WHOLE_SIZE},
                                   {d->bCbf.buf, 0, VK_WHOLE_SIZE},
                                   {d->bStatus.buf, 0, VK_WHOLE_SIZE},
                                   {d->bModes.buf, 0, VK_WHOLE_SIZE},
                                   {d->bULen.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorBufferInfo b[3] = {{d->bCoef.buf, 0, VK_WHOLE_SIZE},
                                   {d->bRecs.buf, 0, VK_WHOLE_SIZE},
                                   {d->bWgt.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorBufferInfo b2[3] = {{d->bModes.buf, 0, VK_WHOLE_SIZE},
                                    {d->bOrder.buf, 0, VK_WHOLE_SIZE},
                                    {d->bULen.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo im[4] = {
        {VK_NULL_HANDLE, d->imgRgba.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgRgb10.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgLuma.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgCbCr.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorImageInfo imN[3] = {
        {VK_NULL_HANDLE, d->imgRgbaN.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgLumaN.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgCbCrN.view, VK_IMAGE_LAYOUT_GENERAL}};
    // [inter] Pass B's 13-15 and Pass W's 0-2.
    VkDescriptorBufferInfo b3[3] = {{d->bWPred.buf, 0, VK_WHOLE_SIZE},
                                    {d->bRing.buf, 0, VK_WHOLE_SIZE},
                                    {d->bWarp.buf, 0, VK_WHOLE_SIZE}};
    // [inter] Binding 3 is the tile order, the same buffer Pass B reads.  Pass
    // W used to index tiles by gl_WorkGroupID.x directly, which is fine while
    // it dispatches over every tile in raster order and impossible the moment
    // it does not -- and the WARP_SKIP bypass needs it to cover exactly the
    // range build_tile_order() partitioned.  Going through the order buffer
    // costs one uint load per workgroup and changes no output address.
    VkDescriptorBufferInfo wI[4] = {{d->bRing.buf, 0, VK_WHOLE_SIZE},
                                    {d->bWarp.buf, 0, VK_WHOLE_SIZE},
                                    {d->bWPred.buf, 0, VK_WHOLE_SIZE},
                                    {d->bOrder.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[28]{};
    uint32_t nw = 0;
    for (int i = 0; i < 8; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetA;
        w[nw].dstBinding = (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &a[i];
        ++nw;
    }
    for (int i = 0; i < 3; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &b[i];
        ++nw;
    }
    for (int i = 0; i < 4; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = (uint32_t)(3 + i);
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[nw].pImageInfo = &im[i];
        ++nw;
    }
    for (int i = 0; i < 3; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = (uint32_t)(7 + i);
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &b2[i];
        ++nw;
    }
    for (int i = 0; i < 3; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = (uint32_t)(10 + i);
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[nw].pImageInfo = &imN[i];
        ++nw;
    }
    for (int i = 0; i < 3; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = (uint32_t)(13 + i);
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &b3[i];
        ++nw;
    }
    for (int i = 0; i < 4; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetW;
        w[nw].dstBinding = (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &wI[i];
        ++nw;
    }
    vkUpdateDescriptorSets(d->dev, nw, w, 0, nullptr);
    d->resources_ready = true;
    return NXVC_VKD_OK;
}

// The bitstream buffer follows the frame size, so it is (re)made per frame
// when a frame is bigger than anything seen before.  Growing it invalidates
// descriptor binding 0 of Pass A, which is rewritten here.
nxvc_vkd_status ensure_bits(D *d, VkDeviceSize bytes) {
    // Pass A reads the bitstream as uints and may touch up to 16 bytes past
    // the last tile byte (vk/decoder/passA/README.md).
    VkDeviceSize want = align_up(bytes + 16, 4096);
    if (d->bBits.buf && d->bBits.size >= want) return NXVC_VKD_OK;
    nxvc_vkd_status st =
        make_buf(d, d->bBits, want,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 false);
    if (st) return st;
    if (d->dsetA) {
        VkDescriptorBufferInfo bi{d->bBits.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = d->dsetA;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d->dev, 1, &w, 0, nullptr);
    }
    return NXVC_VKD_OK;
}

// Pass B's workgroup -> tile map.  The identity is the natural order; with
// `tile_sort` the tiles are grouped by the fields that decide which branches a
// workgroup takes -- mode, res_level, chroma444, tskip and the intra-mode
// presence -- so that neighbouring workgroups, which a GPU schedules together,
// run the same path.  The sort is stable, so within a group the tiles stay in
// raster order, and no output address depends on the order: every write is
// addressed from the tile index the map yields.
// [inter] The Pass W parameter block: NXVW_WARP_HDR_UINTS of header -- four
// conjugated matrices and the ring geometry -- then one NxvwWarpTile per tile,
// exactly as inter_layout.h lays it out.
//
// The conjugation of [SYN] 13.3 step 1 is done here, on the host, once per
// frame: it is four integers and a rounding rule, and doing it per tile in the
// shader would be four divides of arithmetic to save four uploads of 48 bytes.
void build_warp_params(D *d, const FrameParse &fp, uint32_t ntiles) {
    const StreamInfo &si = d->si;
    d->warp_words.assign((size_t)NXVW_WARP_HDR_UINTS +
                             (size_t)ntiles * NXVW_WARP_TILE_UINTS,
                         0u);
    uint32_t *w = d->warp_words.data();
    for (uint32_t eye = 0; eye < 2; ++eye)
        for (int sub = 1; sub <= 2; ++sub) {
            const int pw = sub == 2 ? (int)si.cw : (int)si.width;
            const int ph = sub == 2 ? (int)si.ch : (int)si.height;
            const nxvcvk::PlaneMatrix H =
                nxvcvk::plane_homography(fp.warp[eye], pw, ph, sub);
            uint32_t *m = w + (size_t)(eye * 2 + (sub - 1)) * NXVW_WARP_MAT_UINTS;
            for (int i = 0; i < 9; ++i) m[i] = (uint32_t)H.h[i];
            m[9] = (uint32_t)H.ox;
            m[10] = (uint32_t)H.oy;
        }
    uint32_t *h = w + NXVW_WARP_HDR_RING;
    h[0] = (uint32_t)d->ringSlotU16;
    h[1] = si.eyes;
    h[2] = si.tiles_x;             // cols_per_eye
    h[3] = fp.cur_slot;
    for (int p = 0; p < 4; ++p) {
        h[4 + p] = (uint32_t)d->ringOff[p];
        h[8 + p] = (uint32_t)d->ringStride[p];
        h[12 + p] = (uint32_t)d->ringPlaneW[p];
    }
    // The parse left `refBase` as a ring SLOT INDEX because the slot stride is
    // a property of the allocation and not of the bitstream; this is where the
    // two meet.
    nxvw::NxvwWarpTile *t = (nxvw::NxvwWarpTile *)(w + NXVW_WARP_HDR_UINTS);
    for (uint32_t i = 0; i < ntiles && i < fp.warp_tiles.size(); ++i) {
        t[i] = fp.warp_tiles[i];
        if (t[i].refBase != 0xffffffffu)
            t[i].refBase = t[i].refBase * (uint32_t)d->ringSlotU16;
    }
}

void build_tile_order(D *d, const FrameParse &fp, uint32_t ntiles) {
    d->order.resize(ntiles);
    d->order_nodir[0] = d->order_nodir[1] = 0;
    d->order_nskip[0] = d->order_nskip[1] = 0;
    // [inter] A frame with a STEREO tile is dispatched one eye at a time, so
    // the map has to make each eye a contiguous range of workgroups.  A tile
    // index is `row * cols + eye * cols_per_eye + col` ([SYN] 3.3), which
    // interleaves the eyes; this walks eye-major instead.  The decoded image
    // is bit-identical either way -- every write address comes from the tile
    // index the map yields -- and the order is what makes the second
    // dispatch's `vkCmdDispatchBase` cover exactly eye 1.
    const uint32_t passes = fp.any_stereo_tile ? d->si.eyes : 1u;
    if (fp.any_stereo_tile) {
        const uint32_t cpe = d->si.tiles_x, cols = d->si.cols;
        uint32_t n = 0;
        for (uint32_t eye = 0; eye < d->si.eyes; ++eye)
            for (uint32_t row = 0; row < d->si.tiles_y; ++row)
                for (uint32_t c = 0; c < cpe; ++c)
                    d->order[n++] = row * cols + eye * cpe + c;
    } else {
        for (uint32_t i = 0; i < ntiles; ++i) d->order[i] = i;
    }
    // [inter] Partition each eye's segment into the tiles that cannot enter
    // the directional-intra wavefront -- everything whose mode is not INTRA --
    // and the tiles that can.  Pass B is then dispatched twice, once with each
    // MODULE: the wavefront is a build variant, and its register footprint is
    // paid by every workgroup of a dispatch that uses it, whether or not that
    // workgroup ever reaches the wavefront.
    //
    // On the Adreno 650 that footprint is 328 words against 16, and it was
    // most of Pass B on an inter frame: 42.9 ms with one module, 21.8 ms with
    // the split, and 6.4 ms for the same sequence encoded with the tool off
    // entirely -- so the split recovers about half and the rest is the
    // rolling intra refresh's own tiles, which really do want the wavefront.
    // That refresh is also why the module cannot be chosen per FRAME: it puts
    // at least one INTRA tile in nearly every frame, so a per-frame test would
    // nearly never fire.  The output is bit-identical -- the two modules
    // differ only in whether a branch no inter tile takes is present.
    const uint32_t per = ntiles / passes;
    auto key = [&](uint32_t t) {
        const uint32_t w1 = fp.recs[t].w1;
        const uint32_t mode = w1 & 7u;
        const uint32_t res = (w1 >> 3) & 3u;
        const uint32_t c444 = (w1 >> 5) & 1u;
        const uint32_t amode = (w1 >> 6) & 3u;
        const uint32_t tskip = (w1 >> 23) & 1u;
        return (mode << 6) | (res << 4) | (c444 << 3) | (amode << 1) | tskip;
    };
    auto sort_range = [&](std::vector<uint32_t>::iterator b,
                          std::vector<uint32_t>::iterator e) {
        if (!d->tile_sort) return;
        std::stable_sort(b, e,
                         [&](uint32_t a, uint32_t c) { return key(a) < key(c); });
    };
    for (uint32_t pass = 0; pass < passes; ++pass) {
        auto beg = d->order.begin() + (size_t)pass * per;
        auto end = beg + per;
        // [inter] Three groups, not two, and the WARP_SKIP one comes first.
        //
        //   [beg, skipMid)  WARP_SKIP -- the module that computes clamp(W) and
        //                   nothing else
        //   [skipMid, mid)  every other non-INTRA mode -- the module with no
        //                   directional wavefront
        //   [mid, end)      INTRA -- the wavefront module, when the stream has
        //                   the tool
        //
        // The skip partition is taken FIRST so that the second one, which is
        // the pre-existing INTRA_DIR split, sees exactly the range it always
        // saw minus the skips -- which are not INTRA and so were always on its
        // first side anyway.  Both are stable, so tile_sort still composes
        // inside each group.
        auto skipMid = beg;
        if (fp.any_inter) {
            skipMid = std::stable_partition(beg, end, [&](uint32_t t) {
                return (fp.recs[t].w1 & 7u) == 0u;   // WARP_SKIP
            });
            d->order_nskip[pass] = (uint32_t)(skipMid - beg);
        }
        auto mid = end;
        if (fp.push.intraDir != 0) {
            mid = std::stable_partition(skipMid, end, [&](uint32_t t) {
                return (fp.recs[t].w1 & 7u) != 3u;   // not INTRA
            });
            d->order_nodir[pass] = (uint32_t)(mid - beg);
        }
        // `tile_sort` still applies, INSIDE each group: the two orders compose
        // because the partition is what a dispatch boundary needs and the sort
        // is what a warp scheduler wants, and neither cares about the other.
        sort_range(beg, skipMid);
        sort_range(skipMid, mid);
        sort_range(mid, end);
    }
}

void buffer_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                    VkPipelineStageFlags dst, VkAccessFlags sa,
                    VkAccessFlags da) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = sa;
    mb.dstAccessMask = da;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void image_to_general(VkCommandBuffer cmd, VkImage img) {
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &ib);
}

}  // namespace

// ===========================================================================
// C ABI
// ===========================================================================
extern "C" const char *nxvc_vk_decoder_status_string(nxvc_vkd_status s) {
    switch (s) {
        case NXVC_VKD_OK: return "ok";
        case NXVC_VKD_ERR_ARG: return "bad argument";
        case NXVC_VKD_ERR_UNSUPPORTED: return "unsupported";
        case NXVC_VKD_ERR_VULKAN: return "vulkan error";
        case NXVC_VKD_ERR_NOMEM: return "out of memory";
        case NXVC_VKD_ERR_NO_DEVICE: return "no usable device";
        case NXVC_VKD_ERR_INTERNAL: return "internal error";
        case NXVC_VKD_ERR_BITSTREAM: return "malformed bitstream";
        case NXVC_VKD_ERR_TRUNCATED: return "truncated bitstream";
        case NXVC_VKD_ERR_VERSION: return "unsupported version or tool";
    }
    return "unknown";
}

extern "C" void nxvc_vk_decoder_create_info_default(nxvc_vkd_create_info *ci) {
    if (!ci) return;
    std::memset(ci, 0, sizeof *ci);
    ci->output_format = NXVC_VKD_OUT_AUTO;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_create(
    const nxvc_vkd_create_info *ci, nxvc_vk_decoder **out) {
    if (!ci || !out) return NXVC_VKD_ERR_ARG;
    *out = nullptr;
    D *d = new (std::nothrow) D();
    if (!d) return NXVC_VKD_ERR_NOMEM;
    d->want_output = ci->output_format;
    d->flags = ci->flags;
    // Opt-in, and only on a device this library creates: the statistics
    // require a device extension and a pipeline creation flag, and the flag
    // may change what the driver compiles, so it must never be on in a run
    // whose numbers are quoted.
    if (const char *e = std::getenv("NXVC_VKD_SHADER_STATS"))
        d->want_shader_stats = (e[0] == '1');

    nxvc_vkd_status st;
    if (ci->device) {
        d->inst = ci->instance;
        d->phys = ci->physical_device;
        d->dev = ci->device;
        d->queue = ci->queue;
        d->qfam = ci->queue_family;
        if (!d->phys || !d->queue) {
            nxvc_vk_decoder_destroy(d);
            return NXVC_VKD_ERR_ARG;
        }
    } else if ((st = create_device(d, ci))) {
        *out = d;  // hand back the decoder so the caller can read last_error
        return st;
    }
    if ((st = probe_device(d))) {
        *out = d;
        return st;
    }

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = d->qfam;
    if (vkCreateCommandPool(d->dev, &cp, nullptr, &d->pool) != VK_SUCCESS) {
        *out = d;
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkCreateCommandPool failed");
    }
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = d->pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d->dev, &cb, &d->cmd) != VK_SUCCESS) {
        *out = d;
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkAllocateCommandBuffers failed");
    }

    // Core name first, then the KHR alias a 1.1 device exposes.
    d->fpWaitSemaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(
        d->dev, "vkWaitSemaphores");
    if (!d->fpWaitSemaphores)
        d->fpWaitSemaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(
            d->dev, "vkWaitSemaphoresKHR");
    d->fpGetSemaphoreCounterValue =
        (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(
            d->dev, "vkGetSemaphoreCounterValue");
    if (!d->fpGetSemaphoreCounterValue)
        d->fpGetSemaphoreCounterValue =
            (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(
                d->dev, "vkGetSemaphoreCounterValueKHR");

    VkSemaphoreTypeCreateInfo sti{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sti.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &sti;
    if (!d->fpWaitSemaphores ||
        vkCreateSemaphore(d->dev, &sci, nullptr, &d->timeline) != VK_SUCCESS) {
        d->timeline = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo bsi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(d->dev, &bsi, nullptr, &d->binsem) != VK_SUCCESS)
            d->binsem = VK_NULL_HANDLE;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(d->dev, &fi, nullptr, &d->fence) != VK_SUCCESS) {
            *out = d;
            return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                          "neither a timeline semaphore nor a fence could be "
                          "created on %s",
                          d->props.deviceName);
        }
    }

    if (d->have_timestamps) {
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 6;   // [inter] +2 for Pass W
        if (vkCreateQueryPool(d->dev, &qp, nullptr, &d->queries) != VK_SUCCESS)
            d->have_timestamps = false;
    }

    if ((st = make_layouts(d))) {
        *out = d;
        return st;
    }
    *out = d;
    return NXVC_VKD_OK;
}

extern "C" void nxvc_vk_decoder_destroy(nxvc_vk_decoder *d) {
    if (!d) return;
    if (d->dev) {
        vkDeviceWaitIdle(d->dev);
        if (d->smBSkip) vkDestroyShaderModule(d->dev, d->smBSkip, nullptr);
        for (auto &kv : d->pipesA) vkDestroyPipeline(d->dev, kv.second, nullptr);
        for (auto &kv : d->pipesB) vkDestroyPipeline(d->dev, kv.second, nullptr);
        if (d->smA) vkDestroyShaderModule(d->dev, d->smA, nullptr);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                if (d->smB[i][j])
                    vkDestroyShaderModule(d->dev, d->smB[i][j], nullptr);
        if (d->pipeW) vkDestroyPipeline(d->dev, d->pipeW, nullptr);
        if (d->smW) vkDestroyShaderModule(d->dev, d->smW, nullptr);
        if (d->plA) vkDestroyPipelineLayout(d->dev, d->plA, nullptr);
        if (d->plB) vkDestroyPipelineLayout(d->dev, d->plB, nullptr);
        if (d->plW) vkDestroyPipelineLayout(d->dev, d->plW, nullptr);
        if (d->dpool) vkDestroyDescriptorPool(d->dev, d->dpool, nullptr);
        if (d->dslA) vkDestroyDescriptorSetLayout(d->dev, d->dslA, nullptr);
        if (d->dslB) vkDestroyDescriptorSetLayout(d->dev, d->dslB, nullptr);
        if (d->dslW) vkDestroyDescriptorSetLayout(d->dev, d->dslW, nullptr);
        if (d->binsem) vkDestroySemaphore(d->dev, d->binsem, nullptr);
        if (d->queries) vkDestroyQueryPool(d->dev, d->queries, nullptr);
        if (d->timeline) vkDestroySemaphore(d->dev, d->timeline, nullptr);
        if (d->fence) vkDestroyFence(d->dev, d->fence, nullptr);
        if (d->pool) vkDestroyCommandPool(d->dev, d->pool, nullptr);
        for (Buf *b : {&d->staging, &d->bBits, &d->bDesc, &d->bTables,
                       &d->bCoef, &d->bCbf, &d->bStatus, &d->bRecs, &d->bWgt,
                       &d->bModes, &d->bOrder, &d->bRead, &d->bULen,
                       &d->bULenHost, &d->bRing, &d->bWPred, &d->bWarp})
            destroy_buf(d, *b);
        for (Img *i : {&d->imgRgba, &d->imgRgb10, &d->imgLuma, &d->imgCbCr,
                       &d->imgRgbaN, &d->imgLumaN, &d->imgCbCrN})
            destroy_img(d, *i);
        if (d->own_device) vkDestroyDevice(d->dev, nullptr);
    }
    if (d->own_instance && d->inst) vkDestroyInstance(d->inst, nullptr);
    delete d;
}

extern "C" uint64_t nxvc_vk_decoder_tools_supported(void) {
    return nxvcvk::tools_supported();
}

extern "C" uint64_t nxvc_vk_decoder_tools(const nxvc_vk_decoder *d) {
    // The build-wide mask when there is no decoder to ask; a device may accept
    // less, and this is the number a handshake must use.
    return d && d->tools_mask ? d->tools_mask : nxvcvk::tools_supported();
}

extern "C" const char *nxvc_vk_decoder_last_error(const nxvc_vk_decoder *d) {
    return d ? d->err.c_str() : "null decoder";
}

extern "C" const char *nxvc_vk_decoder_device_name(const nxvc_vk_decoder *d) {
    return d ? d->device_name.c_str() : "";
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_parse_stream_header(
    nxvc_vk_decoder *d, const uint8_t *buf, size_t len, size_t *consumed) {
    if (!d || !buf) return NXVC_VKD_ERR_ARG;
    nxvc_vkd_status st =
        nxvcvk::parse_stream_header(buf, len, d->si, consumed, d->tools_mask);
    if (st) return seterr(d, st, "stream header: %s",
                          nxvc_vk_decoder_status_string(st));
    d->have_stream = true;
    d->resources_ready = false;
    st = make_resources(d);
    if (st) return st;
    // [inter] A new stream is a new reference ring and a new prediction
    // history.  make_resources() sized them; this is where they start empty.
    d->inter.resize(d->si.tile_count);
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_mark_missing(nxvc_vk_decoder *d,
                                                        const uint32_t *ids,
                                                        uint32_t count) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (!d->have_stream) return NXVC_VKD_ERR_BITSTREAM;
    if (count && !ids) return NXVC_VKD_ERR_ARG;
    const uint32_t n = (uint32_t)d->inter.missing.size();
    for (uint32_t i = 0; i < count; ++i)
        if (ids[i] >= n)
            return seterr(d, NXVC_VKD_ERR_ARG,
                          "mark_missing: tile %u is past the stream's %u tiles",
                          ids[i], n);
    for (auto &m : d->inter.missing) m = 0;
    for (uint32_t i = 0; i < count; ++i) d->inter.missing[ids[i]] = 1;
    d->inter.have_missing = count != 0;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_stream_info(
    const nxvc_vk_decoder *d, nxvc_vkd_stream_info *o) {
    if (!d || !o) return NXVC_VKD_ERR_ARG;
    if (!d->have_stream) return NXVC_VKD_ERR_BITSTREAM;
    const StreamInfo &s = d->si;
    std::memset(o, 0, sizeof *o);
    o->width = s.width;
    o->height = s.height;
    o->chroma = s.chroma;
    o->color_transform = s.color_transform;
    o->color_space = s.color_space;
    o->alpha = s.alpha;
    o->bit_depth = s.bit_depth;
    o->eyes = s.eyes;
    o->num_layers = s.num_layers;
    o->profile = s.profile;
    o->level = s.level;
    o->tools = s.tools;
    o->tiles_x = s.tiles_x;
    o->tiles_y = s.tiles_y;
    o->tile_count = s.tile_count;
    o->chroma_width = s.cw;
    o->chroma_height = s.ch;
    o->ext_len = s.ext_len;
    o->output_format = d->out_format == (uint32_t)nxvw::kOutRgba8
                           ? NXVC_VKD_OUT_RGBA8
                       : d->out_format == (uint32_t)nxvw::kOutRgb10A2
                           ? NXVC_VKD_OUT_RGB10A2
                           : NXVC_VKD_OUT_YCBCR420;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_plane_size(const nxvc_vk_decoder *d,
                                                      int plane, uint32_t *w,
                                                      uint32_t *h) {
    if (!d || !w || !h || plane < 0 || plane > 3) return NXVC_VKD_ERR_ARG;
    if (!d->have_stream) return NXVC_VKD_ERR_BITSTREAM;
    // [inter] The reference decoder writes planes of `width * eyes`
    // (codec_impl.inc: `*w = d->g.width * d->g.eyes`), so a stereo readback
    // is byte-comparable with nxv-dec's.
    if (plane == 1 || plane == 2) {
        *w = d->si.cw * d->si.eyes;
        *h = d->si.ch;
    } else {
        *w = d->si.width * d->si.eyes;
        *h = d->si.height;
    }
    return NXVC_VKD_OK;
}

extern "C" VkSemaphore nxvc_vk_decoder_timeline(const nxvc_vk_decoder *d) {
    return d ? d->timeline : VK_NULL_HANDLE;
}
extern "C" VkSemaphore nxvc_vk_decoder_binary_semaphore(
    const nxvc_vk_decoder *d) {
    return d && d->timeline == VK_NULL_HANDLE ? d->binsem : VK_NULL_HANDLE;
}

extern "C" uint64_t nxvc_vk_decoder_timeline_value(const nxvc_vk_decoder *d) {
    return d ? d->timeline_value : 0;
}

// Drain the frame's timestamp queries into `stats`.  Only ever called once
// the frame is known complete, so VK_QUERY_RESULT_WAIT_BIT never blocks.
static void collect_timestamps(D *d) {
    if (!d->ts_pending) return;
    d->ts_pending = false;
    if (!d->have_timestamps || !d->queries) return;
    uint64_t ts[6] = {};
    const uint32_t nq = d->ts_count;
    if (nq < 4) return;
    if (vkGetQueryPoolResults(d->dev, d->queries, 0, nq, sizeof ts, ts,
                              sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return;
    const double k = (double)d->ts_period / 1e6;
    d->stats.pass_a_ms = (double)(ts[1] - ts[0]) * k;
    // [inter] Pass W sits inside the Pass A -> Pass B window, so the
    // reported Pass B is the predictor plus the reconstruction; the
    // predictor's own share is broken out rather than hidden.
    d->stats.pass_b_ms = (double)(ts[2] - ts[1]) * k;
    d->stats.gpu_ms = (double)(ts[3] - ts[0]) * k;
    if (nq >= 6) d->stats.pass_w_ms = (double)(ts[5] - ts[4]) * k;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_wait(nxvc_vk_decoder *d,
                                                uint64_t timeout_ns) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (d->timeline_value == 0) return NXVC_VKD_OK;
    if (d->timeline == VK_NULL_HANDLE) {
        if (!d->fence_pending) { collect_timestamps(d); return NXVC_VKD_OK; }
        VkResult fr =
            vkWaitForFences(d->dev, 1, &d->fence, VK_TRUE, timeout_ns);
        if (fr == VK_TIMEOUT) return NXVC_VKD_ERR_INTERNAL;
        if (fr != VK_SUCCESS)
            return seterr(d, NXVC_VKD_ERR_VULKAN, "vkWaitForFences: %d",
                          (int)fr);
        d->fence_pending = false;
        collect_timestamps(d);
        return NXVC_VKD_OK;
    }
    uint64_t v = d->timeline_value;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &d->timeline;
    wi.pValues = &v;
    if (!d->fpWaitSemaphores)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "vkWaitSemaphores is not available on this device");
    VkResult r = d->fpWaitSemaphores(d->dev, &wi, timeout_ns);
    if (r == VK_TIMEOUT) return NXVC_VKD_ERR_INTERNAL;
    if (r != VK_SUCCESS)
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkWaitSemaphores: %d", (int)r);
    collect_timestamps(d);
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_images(const nxvc_vk_decoder *d,
                                                  nxvc_vkd_images *o) {
    if (!d || !o) return NXVC_VKD_ERR_ARG;
    std::memset(o, 0, sizeof *o);
    auto add = [&](const Img &i) {
        o->image[o->count] = i.img;
        o->view[o->count] = i.view;
        o->format[o->count] = i.fmt;
        o->width[o->count] = i.w;
        o->height[o->count] = i.h;
        ++o->count;
    };
    if (d->out_format == (uint32_t)nxvw::kOutYcbcr420) {
        add(d->outLuma());
        add(d->outCbCr());
        if (d->need_alpha_pass) add(d->outRgba());
    } else if (d->out_format == (uint32_t)nxvw::kOutRgb10A2) {
        add(d->imgRgb10);
    } else {
        add(d->outRgba());
    }
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_set_dir_sched(nxvc_vk_decoder *d,
                                                         uint32_t sched) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (sched > 3) return NXVC_VKD_ERR_ARG;
    d->dir_sched = sched;
    return NXVC_VKD_OK;
}

extern "C" uint32_t nxvc_vk_decoder_dir_sched(const nxvc_vk_decoder *d) {
    return d ? d->dir_sched : 0u;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_set_tile_sort(nxvc_vk_decoder *d,
                                                         uint32_t on) {
    if (!d) return NXVC_VKD_ERR_ARG;
    d->tile_sort = on ? 1u : 0u;
    return NXVC_VKD_OK;
}

// Has the frame in flight finished, without blocking to find out?
static bool frame_complete(const D *d) {
    if (d->timeline_value == 0) return false;
    if (d->timeline == VK_NULL_HANDLE)
        return !d->fence_pending ||
               vkGetFenceStatus(d->dev, d->fence) == VK_SUCCESS;
    if (!d->fpGetSemaphoreCounterValue) return false;
    uint64_t v = 0;
    if (d->fpGetSemaphoreCounterValue(d->dev, d->timeline, &v) != VK_SUCCESS)
        return false;
    return v >= d->timeline_value;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_stats(const nxvc_vk_decoder *d,
                                                 nxvc_vkd_stats *o) {
    if (!d || !o) return NXVC_VKD_ERR_ARG;
    // Take the timestamps here too, when the frame has already finished and
    // nobody has waited on it.  An async client that synchronises entirely on
    // the GPU -- the binary-semaphore path -- never calls
    // nxvc_vk_decoder_wait() for the frame it is about to report on, so
    // without this its numbers would always be one frame stale.  The check is
    // non-blocking: a frame still running leaves the previous frame's figures
    // in place rather than stalling a caller who only wanted to read a
    // counter.
    D *m = const_cast<D *>(d);
    if (m->ts_pending && frame_complete(m)) collect_timestamps(m);
    *o = d->stats;
    return NXVC_VKD_OK;
}

// --------------------------------------------------------------- decode
extern "C" nxvc_vkd_status nxvc_vk_decode_frame_ex(nxvc_vk_decoder *d,
                                                   const uint8_t *bytes,
                                                   size_t len,
                                                   uint32_t submit_flags,
                                                   size_t *consumed) {
    if (!d || !bytes) return NXVC_VKD_ERR_ARG;
    if (!d->have_stream || !d->resources_ready)
        return seterr(d, NXVC_VKD_ERR_BITSTREAM,
                      "no stream header parsed yet");
    const double t0 = now_ms();

    // ---- 1. host parse ------------------------------------------------
    FrameParse &fp = d->fp;
    nxvc_vkd_status st = nxvcvk::parse_frame(
        d->si, bytes, len, (d->flags & NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES) != 0,
        fp, &d->inter);
    // [inter] The missing-tile map covers exactly ONE frame, whether or not
    // the parse got far enough to use it, so a refused frame does not leave
    // it armed for the next one.  [REF] codec_impl.inc, which clears
    // `d->lost` at the top of every decode.
    d->inter.consume_missing();
    if (st) return seterr(d, st, "frame: %s",
                          nxvc_vk_decoder_status_string(st));
    if (consumed) *consumed = fp.frame_bytes;
    fp.push.sparse = (d->flags & NXVC_VKD_FLAG_DENSE_COEF) ? 0 : 1;
    const double t_parse = now_ms();

    const uint32_t ntiles = d->si.tile_count;
    if ((st = ensure_bits(d, fp.frame_bytes))) return st;

    // ---- 2. staging --------------------------------------------------
    const VkDeviceSize descBytes =
        (VkDeviceSize)fp.desc.size() * sizeof(nxvcvk::TileDesc);
    const VkDeviceSize tabBytes = (VkDeviceSize)fp.cum.size() * 4;
    const VkDeviceSize recBytes = (VkDeviceSize)ntiles * 16;
    VkDeviceSize o = 0;
    d->offBits = o;
    o = align_up(o + fp.frame_bytes, 256);
    d->offDesc = o;
    o = align_up(o + descBytes, 256);
    d->offTables = o;
    o = align_up(o + tabBytes, 256);
    d->offRecs = o;
    o = align_up(o + recBytes, 256);
    d->offWgt = o;
    o = align_up(o + sizeof fp.weights, 256);
    d->offOrder = o;
    o = align_up(o + (VkDeviceSize)ntiles * 4, 256);
    // [inter] The Pass W parameter block: the ring geometry and the four
    // conjugated matrices, then one record per tile.
    build_warp_params(d, fp, ntiles);
    const VkDeviceSize warpBytes = (VkDeviceSize)d->warp_words.size() * 4;
    d->offWarp = o;
    o = align_up(o + warpBytes, 256);
    if ((st = make_buf(d, d->staging, o, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       true)))
        return st;
    uint8_t *sp = (uint8_t *)d->staging.mapped;
    std::memcpy(sp + d->offBits, bytes, fp.frame_bytes);
    if (descBytes) std::memcpy(sp + d->offDesc, fp.desc.data(), descBytes);
    std::memcpy(sp + d->offTables, fp.cum.data(), tabBytes);
    std::memcpy(sp + d->offRecs, fp.recs.data(), recBytes);
    std::memcpy(sp + d->offWgt, fp.weights, sizeof fp.weights);
    build_tile_order(d, fp, ntiles);
    std::memcpy(sp + d->offOrder, d->order.data(), (size_t)ntiles * 4);
    if (warpBytes)
        std::memcpy(sp + d->offWarp, d->warp_words.data(), (size_t)warpBytes);

    // ---- 3. record ----------------------------------------------------
    VKTRY(d, vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKTRY(d, vkBeginCommandBuffer(d->cmd, &bi));
    if (d->have_timestamps) {
        vkCmdResetQueryPool(d->cmd, d->queries, 0, 6);
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            d->queries, 0);
    }

    auto copy = [&](Buf &dst, VkDeviceSize src_off, VkDeviceSize n) {
        if (!n) return;
        VkBufferCopy c{src_off, 0, n};
        vkCmdCopyBuffer(d->cmd, d->staging.buf, dst.buf, 1, &c);
    };
    copy(d->bBits, d->offBits, fp.frame_bytes);
    copy(d->bDesc, d->offDesc, descBytes);
    copy(d->bTables, d->offTables, tabBytes);
    copy(d->bRecs, d->offRecs, recBytes);
    copy(d->bWgt, d->offWgt, sizeof fp.weights);
    copy(d->bOrder, d->offOrder, (VkDeviceSize)ntiles * 4);
    copy(d->bWarp, d->offWarp, warpBytes);
    // A skipped tile gets no Pass A descriptor, so nothing would zero its
    // coefficient slot.  Zero it here; Pass B then reconstructs it as
    // "no coefficients" over the WARP_SKIP record.
    // [sparse] ... which under the sparse layout means zeroing its 264 B of
    // unit lengths rather than its 12.5 KB of coefficient slots: a length of
    // zero already says "this unit coded nothing".
    for (uint32_t t : fp.zero_tiles) {
        if (fp.push.sparse)
            vkCmdFillBuffer(
                d->cmd, d->bULen.buf,
                (VkDeviceSize)t * nxwarp_passA::kUnitLenWordsPerTile * 4,
                (VkDeviceSize)nxwarp_passA::kUnitLenWordsPerTile * 4, 0);
        else
            vkCmdFillBuffer(d->cmd, d->bCoef.buf,
                            (VkDeviceSize)t * fp.coef_stride * 2,
                            (VkDeviceSize)fp.coef_stride * 2, 0);
    }
    buffer_barrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    image_to_general(d->cmd, d->imgRgba.img);
    image_to_general(d->cmd, d->imgRgb10.img);
    image_to_general(d->cmd, d->imgLuma.img);
    image_to_general(d->cmd, d->imgCbCr.img);
    image_to_general(d->cmd, d->imgRgbaN.img);
    image_to_general(d->cmd, d->imgLumaN.img);
    image_to_general(d->cmd, d->imgCbCrN.img);

    // ---- Pass A: one dispatch per distinct lane count -----------------
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->plA, 0,
                            1, &d->dsetA, 0, nullptr);
    uint32_t dispatches = 0;
    for (const LaneGroup &g : fp.groups) {
        VkPipeline p;
        const uint32_t emode = fp.entropy_lite
                                   ? nxwarp_passA::kEntropyLiteFixed
                                   : nxwarp_passA::kEntropyRans;
        if ((st = pipeline_a(d, g.lanes, (uint32_t)fp.ctx_stride,
                             (uint32_t)fp.xform_large, emode, &p)))
            return st;
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        const uint32_t push[kPassAPushUints] = {
            g.limit,       fp.frame_nplanes,       fp.coef_stride,
            fp.cbf_words,  fp.tools,               (uint32_t)fp.push.sparse};
        vkCmdPushConstants(d->cmd, d->plA, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof push, push);
        const uint32_t tpg =
            fp.entropy_lite ? 1u : nxwarp_passA::nxs_tiles_per_group(g.lanes);
        vkCmdDispatchBase(d->cmd, g.first / tpg, 0, 0, g.groups, 1, 1);
        ++dispatches;
    }
    if (d->have_timestamps)
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            d->queries, 1);
    buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // ---- Pass W and Pass B --------------------------------------------
    // [inter] A frame with a STEREO tile runs the two passes ONCE PER EYE
    // with a barrier between: a STEREO tile predicts from the first eye of
    // THIS frame's ring slot, and [SYN] 3.3's row order -- which is what
    // makes that dependency satisfiable on a serial decoder -- has no
    // counterpart inside a single dispatch.  Every other frame runs each pass
    // once, so the extra dispatch is paid only by the frames that need it.
    const uint32_t storeWords =
        (uint32_t)(fp.push.planeWords0 + fp.push.planeWords1 +
                   fp.push.planeWords2 + fp.push.planeWords3);
    const bool interStream = (d->si.tools & (1ull << 10)) != 0;
    const uint32_t eyePasses = fp.any_stereo_tile ? d->si.eyes : 1u;
    const uint32_t tilesPerEye = ntiles / (fp.any_stereo_tile ? d->si.eyes : 1u);

    nxvw::NxvwWarpPush wpush{};
    wpush.eyeW = (int)d->si.width;
    wpush.eyeH = (int)d->si.height;
    wpush.chromaW = (int)d->si.cw;
    wpush.chromaH = (int)d->si.ch;
    wpush.eyes = (int)d->si.eyes;
    wpush.colsPerEye = (int)d->si.tiles_x;
    wpush.chroma420 = fp.push.chroma420;
    wpush.alphaPresent = fp.push.alphaPresent;
    wpush.colorTransform = fp.push.colorTransform;
    wpush.chromaQpOff = fp.push.chromaQpOff;
    wpush.alphaQpOff = fp.push.alphaQpOff;
    wpush.wpredStrideI16 = d->wpredStrideI16;
    wpush.ringSlotU16 = d->ringSlotU16;
    wpush.tileCount = (int)ntiles;

    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->plB, 0,
                            1, &d->dsetB, 0, nullptr);
    vkCmdPushConstants(d->cmd, d->plB, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       (uint32_t)sizeof(nxvw::NxvwPassBPush), &fp.push);
    // The two-plane 4:2:0 store has nowhere to put alpha, so a 4:2:0 stream
    // that carries one also needs the RGBA8 store, whose A channel is the
    // alpha plane.  Two stores of one reconstruction is the same shape the
    // reference ring slot will have when the inter path lands, and the kernel
    // does both from one dispatch rather than transforming the frame twice.
    const bool fuse = d->need_alpha_pass && !(d->flags & NXVC_VKD_FLAG_SPLIT_STORES);
    // [inter] Two Pass B pipelines, one per module: the tiles that cannot
    // enter the directional-intra wavefront take the one that does not
    // contain it, over the range build_tile_order() partitioned for them.
    // On a frame whose tiles are all one kind the other dispatch is empty and
    // is not issued.
    VkPipeline pipeB[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline pipeBa[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (int dir = 0; dir < 2; ++dir) {
        if (dir == 1 && fp.push.intraDir == 0) break;
        if ((st = pipeline_b(d, d->out_format,
                             fuse ? (int32_t)nxvw::kOutRgba8
                                  : (int32_t)nxvw::kOutNone,
                             fp.push.sparse, storeWords, dir,
                             (int32_t)fp.split4, (int32_t)fp.xform_large,
                             inter_pred_on(d, interStream),
                             ring_store_on(d, interStream),
                             &pipeB[dir])))
            return st;
        if (d->need_alpha_pass && !fuse) {
            if ((st = pipeline_b(d, (uint32_t)nxvw::kOutRgba8,
                                 (int32_t)nxvw::kOutNone, fp.push.sparse,
                                 storeWords, dir, (int32_t)fp.split4,
                                 (int32_t)fp.xform_large,
                                 interStream ? 1 : 0, 0, &pipeBa[dir])))
                return st;
        }
    }
    // [inter] The WARP_SKIP module, built only when the frame has skip tiles
    // to give it.  It carries no alpha companion: the alpha second store is
    // for a 4:2:0 stream that also codes an alpha plane, and a WARP_SKIP tile
    // codes no plane at all.
    VkPipeline pipeBSkip = VK_NULL_HANDLE;
    const bool anySkip = d->order_nskip[0] != 0 || d->order_nskip[1] != 0;
    if (anySkip && !d->need_alpha_pass &&
        (st = pipeline_b(d, d->out_format,
                         fuse ? (int32_t)nxvw::kOutRgba8
                              : (int32_t)nxvw::kOutNone,
                         fp.push.sparse, storeWords, 0, (int32_t)fp.split4, 0,
                         inter_pred_on(d, interStream),
                         ring_store_on(d, interStream), &pipeBSkip, true)))
        return st;

    VkPipeline pipeWp = VK_NULL_HANDLE;
    if (fp.any_inter && (st = pipeline_w(d, &pipeWp))) return st;

    for (uint32_t pass = 0; pass < eyePasses; ++pass) {
        if (fp.any_inter) {
            if (d->have_timestamps && pass == 0)
                vkCmdWriteTimestamp(d->cmd,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    d->queries, 4);
            wpush.eyeFilter = fp.any_stereo_tile ? (int)pass : -1;
            vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d->plW, 0, 1, &d->dsetW, 0, nullptr);
            vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeWp);
            vkCmdPushConstants(d->cmd, d->plW, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               (uint32_t)sizeof(nxvw::NxvwWarpPush), &wpush);
            vkCmdDispatchBase(d->cmd, 0, 0, 0, ntiles, 1, 1);
            ++dispatches;
            if (d->have_timestamps && pass == 0)
                vkCmdWriteTimestamp(d->cmd,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    d->queries, 5);
            buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
            // Pass W is not the only pipeline bound to set 0: rebind Pass B's.
            vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d->plB, 0, 1, &d->dsetB, 0, nullptr);
            vkCmdPushConstants(d->cmd, d->plB, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               (uint32_t)sizeof(nxvw::NxvwPassBPush), &fp.push);
        }
        const uint32_t base = pass * tilesPerEye;
        // Three contiguous ranges, in build_tile_order()'s order:
        //   [0, nskip)        WARP_SKIP        -> the skip module
        //   [nskip, nodir)    other non-INTRA  -> the module with no wavefront
        //   [nodir, perEye)   INTRA            -> the wavefront module
        // `order_nskip` is 0 on a frame with no inter tiles and `order_nodir`
        // is 0 without INTRA_DIR, which collapses this to what it was: one
        // range on the one module that exists.
        // A frame that has skip tiles but no skip PIPELINE -- the alpha
        // second-store configuration, which the skip module deliberately does
        // not carry -- must not lose them: they are non-INTRA, so they are
        // already inside [0, nodir), and folding the range back to zero puts
        // them on the module that would have had them before this split.
        const uint32_t nskip = pipeBSkip != VK_NULL_HANDLE
                                   ? d->order_nskip[pass]
                                   : 0u;
        const uint32_t nodir =
            fp.push.intraDir != 0 ? d->order_nodir[pass] : tilesPerEye;
        const uint32_t seg[3] = {nskip, nodir - nskip, tilesPerEye - nodir};
        const uint32_t segBase[3] = {base, base + nskip, base + nodir};
        VkPipeline segPipe[3] = {pipeBSkip, pipeB[0], pipeB[1]};
        VkPipeline segPipeA[3] = {VK_NULL_HANDLE, pipeBa[0], pipeBa[1]};
        for (int g = 0; g < 3; ++g) {
            if (seg[g] == 0) continue;
            vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              segPipe[g]);
            vkCmdDispatchBase(d->cmd, segBase[g], 0, 0, seg[g], 1, 1);
            ++dispatches;
            if (segPipeA[g] != VK_NULL_HANDLE) {
                vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  segPipeA[g]);
                vkCmdDispatchBase(d->cmd, segBase[g], 0, 0, seg[g], 1, 1);
                ++dispatches;
            }
        }
        if (pass + 1 < eyePasses)
            buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
    }
    if (d->have_timestamps)
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            d->queries, 2);

    // ---- readback -----------------------------------------------------
    if (d->flags & NXVC_VKD_FLAG_READBACK) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        auto grab = [&](const Img &im, VkDeviceSize off) {
            VkBufferImageCopy c{};
            c.bufferOffset = off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {im.w, im.h, 1};
            vkCmdCopyImageToBuffer(d->cmd, im.img, VK_IMAGE_LAYOUT_GENERAL,
                                   d->bRead.buf, 1, &c);
        };
        if (d->out_format == (uint32_t)nxvw::kOutYcbcr420) {
            grab(d->outLuma(), d->rbLuma);
            grab(d->outCbCr(), d->rbCbCr);
            if (d->need_alpha_pass) grab(d->outRgba(), d->rbRgba);
        } else if (d->out_format == (uint32_t)nxvw::kOutRgb10A2) {
            grab(d->imgRgb10, d->rbRgba);
        } else {
            grab(d->outRgba(), d->rbRgba);
        }
    }
    // Pass A's status words are read back with the same submission.
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }
    if (d->have_timestamps)
        vkCmdWriteTimestamp(d->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            d->queries, 3);
    // [sparse] The exact coefficient traffic, on request only.  It is copied
    // after the last timestamp so it never lands inside a reported pass time.
    if (d->bULenHost.buf != VK_NULL_HANDLE) {
        VkBufferCopy c{0, 0, d->bULenHost.size};
        vkCmdCopyBuffer(d->cmd, d->bULen.buf, d->bULenHost.buf, 1, &c);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }
    VKTRY(d, vkEndCommandBuffer(d->cmd));

    // ---- 4. submit ----------------------------------------------------
    const uint64_t signal = ++d->timeline_value;
    VkTimelineSemaphoreSubmitInfo tsi{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.signalSemaphoreValueCount = 1;
    tsi.pSignalSemaphoreValues = &signal;
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.pNext = &tsi;
    su.commandBufferCount = 1;
    su.pCommandBuffers = &d->cmd;
    su.signalSemaphoreCount = 1;
    su.pSignalSemaphores = &d->timeline;
    if (d->timeline == VK_NULL_HANDLE) {
        su.pNext = nullptr;
        su.signalSemaphoreCount = 0;
        su.pSignalSemaphores = nullptr;
        // Only on the async path: the synchronous one waits before it returns
        // and would leave a single-use semaphore signalled with no waiter.
        if ((submit_flags & NXVC_VKD_SUBMIT_SIGNAL_BINARY) &&
            (submit_flags & NXVC_VKD_SUBMIT_ASYNC) &&
            d->binsem != VK_NULL_HANDLE) {
            su.signalSemaphoreCount = 1;
            su.pSignalSemaphores = &d->binsem;
        }
        vkResetFences(d->dev, 1, &d->fence);
        d->fence_pending = true;
    }
    VKTRY(d, vkQueueSubmit(d->queue, 1, &su,
                           d->timeline == VK_NULL_HANDLE ? d->fence
                                                         : VK_NULL_HANDLE));
    const double t_submit = now_ms();

    d->stats.parse_ms = t_parse - t0;
    d->stats.submit_ms = t_submit - t_parse;
    d->stats.frame_bytes = fp.frame_bytes;
    d->stats.payload_bytes = fp.payload_bytes;
    d->stats.coef_slot_bytes = (uint64_t)ntiles * fp.coef_stride * 2;
    d->stats.coef_bytes = d->stats.coef_slot_bytes;
    d->stats.tiles = ntiles;
    d->stats.tiles_skipped = fp.tiles_skipped;
    d->stats.tiles_concealed = fp.tiles_concealed;
    d->stats.tiles_tskip = fp.tiles_tskip;
    d->stats.lane_groups = (uint32_t)fp.groups.size();
    d->stats.dispatches = dispatches;
    // NOT zeroed here.  The per-pass times describe the most recently
    // COMPLETED frame, and on the async path this frame has not started: a
    // client that reads the stats at the end of its own frame loop would get
    // zeros for every frame if the submit wiped them.  That is exactly what
    // happened to the WiVRn client once it stopped host-waiting after the
    // copy -- the collection moved to the NEXT frame's pre-decode wait, and
    // the decode submit that followed it zeroed the numbers again before
    // anyone could read them.  collect_timestamps() overwrites all four.
    ++d->stats.frames;

    d->ts_count = d->have_timestamps ? (fp.any_inter ? 6u : 4u) : 0u;
    d->ts_pending = d->have_timestamps;
    if (submit_flags & NXVC_VKD_SUBMIT_ASYNC) {
        d->stats.total_ms = now_ms() - t0;
        return NXVC_VKD_OK;
    }
    if ((st = nxvc_vk_decoder_wait(d, UINT64_MAX))) return st;
    // [sparse] The exact number of int16 slots that crossed between the two
    // passes: the sum of every unit's LAST + 1, plus the length words
    // themselves, which Pass A writes and Pass B reads like any other input.
    if (fp.push.sparse && d->bULenHost.mapped) {
        const uint32_t *lw = (const uint32_t *)d->bULenHost.mapped;
        uint64_t coefs = 0;
        const size_t nwords =
            (size_t)ntiles * nxwarp_passA::kUnitLenWordsPerTile;
        for (size_t i = 0; i < nwords; ++i) {
            uint32_t w = lw[i];
            while (w) {
                coefs += w & nxwarp_passA::kUnitLenMask;
                w >>= nxwarp_passA::kUnitLenBits;
            }
        }
        d->stats.coef_bytes =
            coefs * 2 + (uint64_t)nwords * 4;
    }

    // Pass A reports per tile.  A non-zero status means the entropy decoder
    // refused that tile's payload; the frame's pixels are not conformant, so
    // the call fails rather than handing back a plausible-looking image.
    if (d->bStatus.mapped) {
        const uint32_t *sw = (const uint32_t *)d->bStatus.mapped;
        for (const LaneGroup &g : fp.groups)
            for (uint32_t i = g.first; i < g.limit; ++i) {
                if (sw[i] == nxwarp_passA::kStatusOk) continue;
                static const char *kNames[5] = {
                    "ok", "truncated payload", "illegal symbol",
                    "bad tile header", "scheduling round overflow"};
                uint32_t c = sw[i] < 5 ? sw[i] : 0;
                return seterr(d, NXVC_VKD_ERR_BITSTREAM,
                              "Pass A refused tile %u (%u lanes): %s",
                              fp.desc_tile[i], g.lanes, kNames[c]);
            }
    }

    d->stats.total_ms = now_ms() - t0;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decode_frame(nxvc_vk_decoder *d,
                                                const uint8_t *bytes,
                                                size_t len, size_t *consumed) {
    return nxvc_vk_decode_frame_ex(d, bytes, len, 0, consumed);
}

// ------------------------------------------------------------- readback
extern "C" nxvc_vkd_status nxvc_vk_decoder_read_planes(
    nxvc_vk_decoder *d, uint8_t *const plane[4], const int32_t stride[4]) {
    if (!d || !plane || !stride) return NXVC_VKD_ERR_ARG;
    if (!(d->flags & NXVC_VKD_FLAG_READBACK))
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "the decoder was created without NXVC_VKD_FLAG_READBACK");
    nxvc_vkd_status st = nxvc_vk_decoder_wait(d, UINT64_MAX);
    if (st) return st;
    const StreamInfo &si = d->si;
    const uint8_t *rb = (const uint8_t *)d->bRead.mapped;
    // [inter] The image spans the eye pair, and so does the planar layout the
    // reference decoder writes (codec_impl.inc: `*w = d->g.width *
    // d->g.eyes`).  Both eyes are one raster here for exactly the reason
    // parse_stream_header() gives.
    const uint32_t W = si.width * si.eyes, H = si.height;
    const uint32_t CW = si.cw * si.eyes, CH = si.ch;

    if (d->out_format == (uint32_t)nxvw::kOutYcbcr420) {
        const uint8_t *Y = rb + d->rbLuma;
        const uint8_t *C = rb + d->rbCbCr;
        if (plane[0])
            for (uint32_t y = 0; y < H; ++y)
                std::memcpy(plane[0] + (size_t)y * stride[0], Y + (size_t)y * W,
                            W);
        for (uint32_t y = 0; y < CH; ++y)
            for (uint32_t x = 0; x < CW; ++x) {
                const uint8_t *s = C + ((size_t)y * CW + x) * 2;
                if (plane[1]) plane[1][(size_t)y * stride[1] + x] = s[0];
                if (plane[2]) plane[2][(size_t)y * stride[2] + x] = s[1];
            }
        if (si.alpha && plane[3]) {
            const uint8_t *A = rb + d->rbRgba;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    plane[3][(size_t)y * stride[3] + x] =
                        A[((size_t)y * W + x) * 4 + 3];
        }
        return NXVC_VKD_OK;
    }

    if (d->out_format == (uint32_t)nxvw::kOutRgb10A2) {
        const uint32_t *px = (const uint32_t *)(rb + d->rbRgba);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                uint32_t v = px[(size_t)y * W + x];
                uint32_t c[3] = {v & 1023u, (v >> 10) & 1023u,
                                 (v >> 20) & 1023u};
                for (int p = 0; p < 3; ++p)
                    if (plane[p])
                        plane[p][(size_t)y * stride[p] + x] =
                            (uint8_t)(c[p] >> 2);
                if (si.alpha && plane[3])
                    plane[3][(size_t)y * stride[3] + x] =
                        (uint8_t)(((v >> 30) & 3u) * 85u);
            }
        return NXVC_VKD_OK;
    }

    const uint8_t *px = rb + d->rbRgba;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t *s = px + ((size_t)y * W + x) * 4;
            for (int p = 0; p < 3; ++p)
                if (plane[p]) plane[p][(size_t)y * stride[p] + x] = s[p];
            if (si.alpha && plane[3])
                plane[3][(size_t)y * stride[3] + x] = s[3];
        }
    (void)CW;
    (void)CH;
    return NXVC_VKD_OK;
}
