// nxvc-passB-test -- headless harness for the Pass B reconstruction kernel.
//
// Generates random-but-legal coefficient buffers and tile records, runs the
// CPU model (passB_model.cpp) and the GPU kernel (reconstruct.comp) over the
// same input, and compares the two images pixel for pixel.  Exit criterion is
// ZERO mismatching pixels.
//
// It also reports the RADV (or whatever ICD is selected) dispatch time for a
// 2048-tile frame and the coefficient SSBO traffic in MB per frame.
//
// Own minimal Vulkan boilerplate on purpose: vk/common does not exist yet and
// this must not block on it.  Everything here is plain Vulkan 1.1, no
// extensions, no window system, no validation layer requirement.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "passB_layout.h"
#include "passB_model.h"
#include "syntax_constants.h"
#include "reconstruct.spv.h"

using namespace nxvw;

// ============================================================ vulkan glue
#define VKCHECK(x)                                                          \
    do {                                                                    \
        VkResult _r = (x);                                                  \
        if (_r != VK_SUCCESS) {                                             \
            std::fprintf(stderr, "%s:%d: %s failed (VkResult %d)\n",        \
                         __FILE__, __LINE__, #x, (int)_r);                  \
            std::exit(1);                                                   \
        }                                                                   \
    } while (0)

namespace {

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
};

struct Ctx {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want)
                return i;
        std::fprintf(stderr, "no memory type for flags 0x%x\n", want);
        std::exit(1);
    }
};

Buf createBuffer(const Ctx &c, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkMemoryPropertyFlags props) {
    Buf b;
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(c.dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = c.memType(req.memoryTypeBits, props);
    VKCHECK(vkAllocateMemory(c.dev, &ai, nullptr, &b.mem));
    VKCHECK(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VKCHECK(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

void destroyBuffer(const Ctx &c, Buf &b) {
    if (b.mapped) vkUnmapMemory(c.dev, b.mem);
    if (b.buf) vkDestroyBuffer(c.dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(c.dev, b.mem, nullptr);
    b = Buf{};
}

Img createStorageImage(const Ctx &c, VkFormat fmt, uint32_t w, uint32_t h) {
    Img im;
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
    VKCHECK(vkCreateImage(c.dev, &ii, nullptr, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.dev, im.img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = c.memType(req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(c.dev, &ai, nullptr, &im.mem));
    VKCHECK(vkBindImageMemory(c.dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKCHECK(vkCreateImageView(c.dev, &vi, nullptr, &im.view));
    return im;
}

void destroyImage(const Ctx &c, Img &im) {
    if (im.view) vkDestroyImageView(c.dev, im.view, nullptr);
    if (im.img) vkDestroyImage(c.dev, im.img, nullptr);
    if (im.mem) vkFreeMemory(c.dev, im.mem, nullptr);
    im = Img{};
}

VkCommandBuffer beginCmd(const Ctx &c) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = c.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(c.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

void endCmdAndWait(const Ctx &c, VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VKCHECK(vkCreateFence(c.dev, &fi, nullptr, &fence));
    VKCHECK(vkQueueSubmit(c.queue, 1, &si, fence));
    VKCHECK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, 30ull * 1000000000ull));
    vkDestroyFence(c.dev, fence, nullptr);
    vkFreeCommandBuffers(c.dev, c.pool, 1, &cb);
}

bool initVulkan(Ctx &c, int deviceIndex, const std::string &deviceName,
                bool listOnly) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc-passB-test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ii, nullptr, &c.inst);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateInstance failed (%d): no usable ICD\n", (int)r);
        return false;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c.inst, &n, nullptr);
    if (n == 0) {
        std::fprintf(stderr, "no Vulkan physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(c.inst, &n, devs.data());
    int pick = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (listOnly) std::printf("device %u: %s\n", i, p.deviceName);
        if (!deviceName.empty() && std::string(p.deviceName).find(deviceName) !=
                                       std::string::npos && pick < 0)
            pick = (int)i;
    }
    if (listOnly) return false;
    if (!deviceName.empty() && pick < 0) {
        std::fprintf(stderr, "no device matching \"%s\"\n", deviceName.c_str());
        return false;   // -> exit 77, i.e. skip, never a silent fallback
    }
    if (pick < 0) pick = deviceIndex;
    if (pick >= (int)n) {
        std::fprintf(stderr, "device index %d out of range (%u devices)\n", pick, n);
        return false;
    }
    c.phys = devs[pick];
    vkGetPhysicalDeviceProperties(c.phys, &c.props);
    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.memProps);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qn, qs.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i)
        if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { c.qfam = i; found = true; break; }
    if (!found) {
        std::fprintf(stderr, "no compute queue family\n");
        return false;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = c.qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VKCHECK(vkCreateDevice(c.phys, &di, nullptr, &c.dev));
    vkGetDeviceQueue(c.dev, c.qfam, 0, &c.queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = c.qfam;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKCHECK(vkCreateCommandPool(c.dev, &pi, nullptr, &c.pool));
    if (qs[c.qfam].timestampValidBits > 0 && c.props.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        VKCHECK(vkCreateQueryPool(c.dev, &qpi, nullptr, &c.queries));
    }
    return true;
}

// ======================================================= test case setup
struct Case {
    int tilesX = 4, tilesY = 4;
    int chroma420 = 0;      // stream chroma
    int alphaPresent = 0;
    int colorTransform = kCtYCoCgR;
    int baseQp = 24;
    int chromaQpOff = 0;
    int alphaQpOff = 0;
    int quantMatrix = 1;
    int outFormat = kOutRgba8;
    uint32_t seed = 1;
    // if >= 0, force these on every tile instead of randomizing
    int forceResLevel = -1;
    int forceTskip = -1;
    int forceAlphaMode = -1;
    int forceChroma444 = -1;
    int coefMagnitude = 400;   // random coefficient range
    // [sparse] 1 = build the scan-order layout plus per-unit lengths, which
    // is what Pass A ships; 0 = the dense raster-order one.
    int sparse = 1;
};

struct Scene {
    Case cs;
    NxvwPassBPush push{};
    std::vector<int16_t> coef;
    std::vector<NxvwTileRec> recs;
    std::vector<int> weights;
    // [v3] per-block intra modes, NXVW_MODE_WORDS_PER_TILE uints per tile.
    // Empty means "mode 0 everywhere", which is the v1 predictor.
    std::vector<uint32_t> modes;
    // [sparse] one byte per coding unit, NXVW_UNIT_LEN_WORDS_PER_TILE uints
    // per tile.  Empty when the case is dense.
    std::vector<uint32_t> unitLens;
    int dirSched = 0;
};

uint32_t packTileW1(int mode, int res_level, int chroma444, int alpha_mode,
                    int qp_delta, int tskip) {
    uint32_t w = (uint32_t)(mode & 7);
    w |= (uint32_t)(res_level & 3) << 3;
    w |= (uint32_t)(chroma444 & 1) << 5;
    w |= (uint32_t)(alpha_mode & 3) << 6;
    w |= (uint32_t)(qp_delta & 0x3f) << 8;
    w |= 3u << 17;                       // nsub_log2 = 3, informational here
    w |= (uint32_t)(tskip & 1) << 23;
    return w;
}

Scene buildScene(const Case &cs) {
    Scene sc;
    sc.cs = cs;
    std::mt19937 rng(cs.seed);
    const int ntiles = cs.tilesX * cs.tilesY;

    sc.push.imageW = cs.tilesX * 64;
    sc.push.imageH = cs.tilesY * 64;
    sc.push.tilesX = cs.tilesX;
    sc.push.baseQp = cs.baseQp;
    sc.push.chromaQpOff = cs.chromaQpOff;
    sc.push.alphaQpOff = cs.alphaQpOff;
    sc.push.colorTransform = cs.colorTransform;
    sc.push.chroma420 = cs.chroma420;
    sc.push.alphaPresent = cs.alphaPresent;
    sc.push.coefStrideI16 = nxvw_coef_stride_i16(cs.chroma420, cs.alphaPresent);
    sc.push.planeWords0 = nxvw_plane_store_words(0, cs.chroma420);
    sc.push.planeWords1 = nxvw_plane_store_words(1, cs.chroma420);
    sc.push.planeWords2 = nxvw_plane_store_words(2, cs.chroma420);
    sc.push.planeWords3 = cs.alphaPresent ? nxvw_plane_store_words(3, cs.chroma420) : 0;

    sc.weights.assign(128, 16);
    model_resolve_matrices(cs.quantMatrix, nullptr, sc.weights.data());

    sc.recs.resize(ntiles);
    sc.push.sparse = cs.sparse;
    // [sparse] Slots past a unit's LAST are never written by Pass A and must
    // never be read by Pass B; seeding them with a value no legal coefficient
    // path can produce is what proves it.
    sc.coef.assign((size_t)ntiles * sc.push.coefStrideI16,
                   cs.sparse ? int16_t(0x5555) : int16_t(0));
    if (cs.sparse)
        sc.unitLens.assign((size_t)ntiles * NXVW_UNIT_LEN_WORDS_PER_TILE, 0u);

    std::uniform_int_distribution<int> dRes(0, kMaxResLevel);
    std::uniform_int_distribution<int> dQpDelta(-8, 8);
    std::uniform_int_distribution<int> dBit(0, 1);
    std::uniform_int_distribution<int> dAlpha(0, 2);
    std::uniform_int_distribution<int> dCoef(-cs.coefMagnitude, cs.coefMagnitude);
    std::uniform_int_distribution<int> dZero(0, 3);
    std::uniform_int_distribution<int> dByte(0, 255);

    for (int t = 0; t < ntiles; ++t) {
        int res = cs.forceResLevel >= 0 ? cs.forceResLevel : dRes(rng);
        int tskip = cs.forceTskip >= 0 ? cs.forceTskip : dBit(rng);
        int c444 = cs.chroma420 ? 0
                   : (cs.forceChroma444 >= 0 ? cs.forceChroma444 : dBit(rng));
        int amode = 0;
        if (cs.alphaPresent)
            amode = cs.forceAlphaMode >= 0 ? cs.forceAlphaMode : dAlpha(rng);
        int qpd = dQpDelta(rng);

        NxvwTileRec &r = sc.recs[t];
        r.w0 = (uint32_t)(t & 0xfff) << 4;
        r.w1 = packTileW1(kModeIntra, res, c444, amode, qpd, tskip);
        r.w2 = (uint32_t)dByte(rng) | (1u << 8);
        r.w3 = 0xffffffffu;   // no warp record

        // Fill the coefficient slot exactly as Pass A would: DC plane then
        // blocks, per coded plane, contiguous.
        int ncoded = cs.alphaPresent ? 4 : 3;
        if (amode != kAlphaCoded) ncoded = std::min(ncoded, 3);
        int16_t *dst = sc.coef.data() + (size_t)t * sc.push.coefStrideI16;
        uint32_t *lens =
            cs.sparse ? sc.unitLens.data() +
                            (size_t)t * NXVW_UNIT_LEN_WORDS_PER_TILE
                      : nullptr;
        int unit = 0;
        // [sparse] The mode unit occupies one index per plane when the stream
        // carries modes, exactly as Pass A numbers them.
        const int unitsPerPlaneExtra = sc.push.intraDir ? 2 : 1;
        // Writes unit `u`'s coefficients: `ncoef` of them dense, or `len` of
        // them in scan order plus the published length.
        auto emit = [&](int u, int ncoef, int len) {
            if (!cs.sparse) {
                for (int i = 0; i < ncoef; ++i) {
                    int v = (dZero(rng) == 0) ? dCoef(rng) : 0;
                    if (i == 0) v = dCoef(rng);
                    dst[i] = (int16_t)v;
                }
                return;
            }
            for (int i = 0; i < len; ++i) {
                int v = (dZero(rng) == 0) ? dCoef(rng) : 0;
                if (i == 0 || i == len - 1) v = dCoef(rng);
                dst[i] = (int16_t)v;
            }
            lens[u / (int)NXVW_UNIT_LENS_PER_WORD] |=
                ((uint32_t)len & NXVW_UNIT_LEN_MASK)
                << ((uint32_t)(u % (int)NXVW_UNIT_LENS_PER_WORD) *
                    NXVW_UNIT_LEN_BITS);
        };
        for (int p = 0; p < ncoded; ++p) {
            int size = nxvw_plane_size(p, res, c444);
            int nb = size >> 3;
            int ndc = nb * nb;
            std::uniform_int_distribution<int> dDcLen(0, ndc);
            std::uniform_int_distribution<int> dBlkLen(0, 64);
            emit(unit, ndc, dDcLen(rng));
            dst += ndc;
            for (int b = 0; b < ndc; ++b) {
                emit(unit + unitsPerPlaneExtra + b, 64, dBlkLen(rng));
                dst += 64;
            }
            unit += unitsPerPlaneExtra + ndc;
        }
    }
    return sc;
}

// ------------------------------------------------------------- GPU run
struct GpuResult {
    std::vector<uint8_t> rgba8;
    std::vector<uint32_t> rgb10a2;
    std::vector<uint8_t> luma;
    std::vector<uint8_t> cbcr;
    double dispatchMs = -1.0;
    double wallMs = -1.0;
};

bool runGpu(Ctx &c, const Scene &sc, GpuResult &out, int repeats,
            std::string &err) {
    const int ntiles = sc.cs.tilesX * sc.cs.tilesY;
    const uint32_t W = (uint32_t)sc.push.imageW, H = (uint32_t)sc.push.imageH;

    int planeWords = sc.push.planeWords0 + sc.push.planeWords1 +
                     sc.push.planeWords2 + sc.push.planeWords3;
    size_t lds = (size_t)planeWords * 4 + 64 * 4 * 2;
    if (lds > c.props.limits.maxComputeSharedMemorySize) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "shared memory %zu B exceeds device limit %u B", lds,
                      c.props.limits.maxComputeSharedMemorySize);
        err = b;
        return false;
    }

    // Every binding must be filled even when only one output path is taken,
    // so all four output formats have to be storage-image capable.
    const VkFormat outFmts[4] = {VK_FORMAT_R8G8B8A8_UINT,
                                 VK_FORMAT_A2B10G10R10_UINT_PACK32,
                                 VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT};
    for (VkFormat f : outFmts) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(c.phys, f, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            char b[128];
            std::snprintf(b, sizeof b, "format %d not usable as a storage image",
                          (int)f);
            err = b;
            return false;
        }
    }

    // ---- buffers
    VkDeviceSize coefBytes = sc.coef.size() * sizeof(int16_t);
    VkDeviceSize recBytes = (VkDeviceSize)ntiles * sizeof(NxvwTileRec);
    VkDeviceSize wgtBytes = 128 * sizeof(int32_t);
    VkDeviceSize imgBytes = (VkDeviceSize)W * H * 4;
    VkDeviceSize stageBytes = std::max(std::max(coefBytes, recBytes),
                                       std::max(wgtBytes, imgBytes));
    stageBytes = std::max(
        stageBytes,
        (VkDeviceSize)ntiles *
            std::max<VkDeviceSize>(NXVW_MODE_REGION_UINTS, 1) *
            sizeof(uint32_t));

    Buf stage = createBuffer(c, stageBytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto devBuf = [&](VkDeviceSize n) {
        return createBuffer(c, n,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    };
    // [v3] binding 7: the per-block intra modes; binding 8: the workgroup ->
    // tile map, which this harness always fills with the identity.
    VkDeviceSize modeBytes =
        (VkDeviceSize)ntiles * NXVW_MODE_REGION_UINTS * sizeof(uint32_t);
    VkDeviceSize orderBytes = (VkDeviceSize)ntiles * sizeof(uint32_t);
    // [sparse] binding 9: the per-unit coefficient counts.
    VkDeviceSize lenBytes =
        (VkDeviceSize)ntiles * NXVW_UNIT_LEN_WORDS_PER_TILE * sizeof(uint32_t);
    stageBytes = std::max(stageBytes,
                          std::max(lenBytes, std::max(modeBytes, orderBytes)));
    Buf bCoef = devBuf(coefBytes);
    Buf bRec = devBuf(recBytes);
    Buf bWgt = devBuf(wgtBytes);
    Buf bModes = devBuf(modeBytes);
    Buf bOrder = devBuf(orderBytes);
    Buf bLens = devBuf(lenBytes);

    auto upload = [&](Buf &dst, const void *src, VkDeviceSize n) {
        std::memcpy(stage.mapped, src, (size_t)n);
        VkCommandBuffer cb = beginCmd(c);
        VkBufferCopy cp{0, 0, n};
        vkCmdCopyBuffer(cb, stage.buf, dst.buf, 1, &cp);
        endCmdAndWait(c, cb);
    };
    upload(bCoef, sc.coef.data(), coefBytes);
    upload(bRec, sc.recs.data(), recBytes);
    upload(bWgt, sc.weights.data(), wgtBytes);
    {
        std::vector<uint32_t> modes(sc.modes);
        modes.resize((size_t)ntiles * NXVW_MODE_REGION_UINTS, 0u);
        upload(bModes, modes.data(), modeBytes);
        std::vector<uint32_t> order((size_t)ntiles);
        for (int i = 0; i < ntiles; ++i) order[(size_t)i] = (uint32_t)i;
        upload(bOrder, order.data(), orderBytes);
        std::vector<uint32_t> lens(sc.unitLens);
        lens.resize((size_t)ntiles * NXVW_UNIT_LEN_WORDS_PER_TILE, 0u);
        upload(bLens, lens.data(), lenBytes);
    }

    const uint32_t CW = (W + 1) / 2, CH = (H + 1) / 2;
    Img imgA = createStorageImage(c, VK_FORMAT_R8G8B8A8_UINT, W, H);
    Img imgB = createStorageImage(c, VK_FORMAT_A2B10G10R10_UINT_PACK32, W, H);
    Img imgY = createStorageImage(c, VK_FORMAT_R8_UINT, W, H);
    Img imgC = createStorageImage(c, VK_FORMAT_R8G8_UINT, CW, CH);

    // ---- descriptors
    VkDescriptorSetLayoutBinding binds[10]{};
    for (int i = 0; i < 3; ++i) {
        binds[i].binding = (uint32_t)i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    for (int i = 3; i < 7; ++i) {
        binds[i].binding = (uint32_t)i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    for (int i = 7; i < 10; ++i) {
        binds[i].binding = (uint32_t)i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 10;
    dli.pBindings = binds;
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(c.dev, &dli, nullptr, &dsl));

    VkDescriptorPoolSize psz[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = psz;
    VkDescriptorPool dpool;
    VKCHECK(vkCreateDescriptorPool(c.dev, &dpi, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    VKCHECK(vkAllocateDescriptorSets(c.dev, &dai, &dset));

    VkDescriptorBufferInfo dbi[3] = {{bCoef.buf, 0, VK_WHOLE_SIZE},
                                     {bRec.buf, 0, VK_WHOLE_SIZE},
                                     {bWgt.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo dii[4] = {
        {VK_NULL_HANDLE, imgA.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, imgB.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, imgY.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, imgC.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo dbi2[3] = {{bModes.buf, 0, VK_WHOLE_SIZE},
                                      {bOrder.buf, 0, VK_WHOLE_SIZE},
                                      {bLens.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[10]{};
    for (int i = 0; i < 3; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = dset;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &dbi[i];
    }
    for (int i = 3; i < 7; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = dset;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &dii[i - 3];
    }
    for (int i = 7; i < 10; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = dset;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &dbi2[i - 7];
    }
    vkUpdateDescriptorSets(c.dev, 10, w, 0, nullptr);

    // ---- pipeline
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(reconstruct_spv);
    smi.pCode = reconstruct_spv;
    VkShaderModule sm;
    VKCHECK(vkCreateShaderModule(c.dev, &smi, nullptr, &sm));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)sizeof(NxvwPassBPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout plo;
    VKCHECK(vkCreatePipelineLayout(c.dev, &pli, nullptr, &plo));

    // 3 (the second store) is left at its kOutNone default: this harness
    // drives one format at a time.
    int32_t specData[4] = {(int32_t)sc.cs.outFormat, (int32_t)planeWords,
                           (int32_t)sc.dirSched, (int32_t)sc.push.sparse};
    VkSpecializationMapEntry sme[4] = {
        {0, 0, sizeof(int32_t)},
        {1, sizeof(int32_t), sizeof(int32_t)},
        {2, 2 * sizeof(int32_t), sizeof(int32_t)},
        {4, 3 * sizeof(int32_t), sizeof(int32_t)}};
    VkSpecializationInfo spec{4, sme, sizeof(specData), specData};

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.stage.pSpecializationInfo = &spec;
    cpi.layout = plo;
    VkPipeline pipe;
    VKCHECK(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));

    // ---- record and run
    auto layoutBarrier = [&](VkCommandBuffer cb, VkImage im, VkImageLayout from,
                             VkImageLayout to, VkAccessFlags srcA,
                             VkAccessFlags dstA, VkPipelineStageFlags srcS,
                             VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    auto t0 = std::chrono::steady_clock::now();
    VkCommandBuffer cb = beginCmd(c);
    layoutBarrier(cb, imgA.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    layoutBarrier(cb, imgB.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    layoutBarrier(cb, imgY.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    layoutBarrier(cb, imgC.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (c.queries) vkCmdResetQueryPool(cb, c.queries, 0, 2);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plo, 0, 1, &dset,
                            0, nullptr);
    vkCmdPushConstants(cb, plo, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(NxvwPassBPush), &sc.push);
    if (c.queries)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c.queries, 0);
    for (int i = 0; i < repeats; ++i) {
        vkCmdDispatch(cb, (uint32_t)ntiles, 1, 1);
        if (i + 1 < repeats) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                                 0, nullptr, 0, nullptr);
        }
    }
    if (c.queries)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c.queries, 1);
    endCmdAndWait(c, cb);
    auto t1 = std::chrono::steady_clock::now();
    out.wallMs = std::chrono::duration<double, std::milli>(t1 - t0).count() / repeats;

    if (c.queries) {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(c.dev, c.queries, 0, 2, sizeof(ts), ts,
                                  sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
            VK_SUCCESS)
            out.dispatchMs = (double)(ts[1] - ts[0]) *
                             (double)c.props.limits.timestampPeriod / 1e6 / repeats;
    }

    // ---- readback
    auto readback = [&](Img &im, void *dst, uint32_t iw, uint32_t ih,
                        uint32_t texel) {
        VkCommandBuffer rb = beginCmd(c);
        layoutBarrier(rb, im.img, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {iw, ih, 1};
        vkCmdCopyImageToBuffer(rb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               stage.buf, 1, &r);
        endCmdAndWait(c, rb);
        std::memcpy(dst, stage.mapped, (size_t)iw * ih * texel);
    };
    if (sc.cs.outFormat == kOutRgb10A2) {
        out.rgb10a2.resize((size_t)W * H);
        readback(imgB, out.rgb10a2.data(), W, H, 4);
    } else if (sc.cs.outFormat == kOutYcbcr420) {
        out.luma.resize((size_t)W * H);
        out.cbcr.resize((size_t)CW * CH * 2);
        readback(imgY, out.luma.data(), W, H, 1);
        readback(imgC, out.cbcr.data(), CW, CH, 2);
    } else {
        out.rgba8.resize((size_t)W * H * 4);
        readback(imgA, out.rgba8.data(), W, H, 4);
    }

    vkDestroyPipeline(c.dev, pipe, nullptr);
    vkDestroyPipelineLayout(c.dev, plo, nullptr);
    vkDestroyShaderModule(c.dev, sm, nullptr);
    vkDestroyDescriptorPool(c.dev, dpool, nullptr);
    vkDestroyDescriptorSetLayout(c.dev, dsl, nullptr);
    destroyImage(c, imgA);
    destroyImage(c, imgB);
    destroyImage(c, imgY);
    destroyImage(c, imgC);
    destroyBuffer(c, bCoef);
    destroyBuffer(c, bRec);
    destroyBuffer(c, bWgt);
    destroyBuffer(c, bModes);
    destroyBuffer(c, bOrder);
    destroyBuffer(c, bLens);
    destroyBuffer(c, stage);
    return true;
}

// ============================================================ comparison
size_t compareCase(Ctx &c, const Case &cs, bool verbose, int &ranOut) {
    Scene sc = buildScene(cs);
    PassBInput in;
    in.push = sc.push;
    in.tilesX = cs.tilesX;
    in.tilesY = cs.tilesY;
    in.coef = sc.coef.data();
    in.recs = sc.recs.data();
    in.weights = sc.weights.data();
    in.modes = sc.modes.empty() ? nullptr : sc.modes.data();
    in.unit_lens = sc.unitLens.empty() ? nullptr : sc.unitLens.data();
    in.dirSched = sc.dirSched;

    const size_t npix = (size_t)sc.push.imageW * sc.push.imageH;
    GpuResult gpu;
    std::string err;
    if (!runGpu(c, sc, gpu, 1, err)) {
        std::printf("  SKIP (%s)\n", err.c_str());
        return 0;
    }
    ranOut++;

    size_t bad = 0;
    if (cs.outFormat == kOutYcbcr420) {
        const int cw = (sc.push.imageW + 1) / 2, ch = (sc.push.imageH + 1) / 2;
        std::vector<uint8_t> cpuY(npix), cpuC((size_t)cw * ch * 2);
        passB_reconstruct_ycbcr420(in, cpuY.data(), cpuC.data());
        for (size_t i = 0; i < npix; ++i)
            if (cpuY[i] != gpu.luma[i]) {
                if (bad < 8 && verbose)
                    std::printf("    luma (%zu,%zu): cpu %u gpu %u\n",
                                i % sc.push.imageW, i / sc.push.imageW,
                                cpuY[i], gpu.luma[i]);
                ++bad;
            }
        for (size_t i = 0; i < cpuC.size(); ++i)
            if (cpuC[i] != gpu.cbcr[i]) {
                if (bad < 8 && verbose)
                    std::printf("    cbcr %zu: cpu %u gpu %u\n", i, cpuC[i],
                                gpu.cbcr[i]);
                ++bad;
            }
    } else if (cs.outFormat == kOutRgb10A2) {
        std::vector<uint32_t> cpu(npix);
        passB_reconstruct_rgb10a2(in, cpu.data());
        for (size_t i = 0; i < npix; ++i)
            if (cpu[i] != gpu.rgb10a2[i]) {
                if (bad < 8 && verbose)
                    std::printf("    px %zu (%zu,%zu): cpu %08x gpu %08x\n", i,
                                i % sc.push.imageW, i / sc.push.imageW, cpu[i],
                                gpu.rgb10a2[i]);
                ++bad;
            }
    } else {
        std::vector<uint8_t> cpu(npix * 4);
        passB_reconstruct_rgba8(in, cpu.data());
        for (size_t i = 0; i < npix; ++i)
            if (std::memcmp(&cpu[i * 4], &gpu.rgba8[i * 4], 4) != 0) {
                if (bad < 8 && verbose)
                    std::printf("    px (%zu,%zu): cpu %3u %3u %3u %3u  gpu %3u %3u %3u %3u\n",
                                i % sc.push.imageW, i / sc.push.imageW,
                                cpu[i * 4], cpu[i * 4 + 1], cpu[i * 4 + 2], cpu[i * 4 + 3],
                                gpu.rgba8[i * 4], gpu.rgba8[i * 4 + 1],
                                gpu.rgba8[i * 4 + 2], gpu.rgba8[i * 4 + 3]);
                ++bad;
            }
    }
    return bad;
}

const char *caseName(const Case &cs) {
    static char b[192];
    std::snprintf(b, sizeof b,
                  "%s%s ct=%s qp=%d res=%s tskip=%s alpha=%s out=%s seed=%u",
                  cs.chroma420 ? "4:2:0" : "4:4:4",
                  cs.alphaPresent ? "+A" : "",
                  cs.colorTransform == kCtYCoCgR ? "ycocgr" : "none", cs.baseQp,
                  cs.forceResLevel < 0 ? "rnd" : (cs.forceResLevel == 0 ? "0" :
                                                  cs.forceResLevel == 1 ? "1" : "2"),
                  cs.forceTskip < 0 ? "rnd" : (cs.forceTskip ? "1" : "0"),
                  cs.forceAlphaMode < 0 ? "rnd" : (cs.forceAlphaMode == 0 ? "opaque" :
                                                   cs.forceAlphaMode == 1 ? "const" : "coded"),
                  cs.outFormat == kOutRgb10A2 ? "rgb10a2"
                      : cs.outFormat == kOutYcbcr420 ? "ycbcr420" : "rgba8",
                  cs.seed);
    return b;
}

}  // namespace

// ==================================================================== main
int main(int argc, char **argv) {
    int deviceIndex = 0;
    std::string deviceName;
    bool listOnly = false, verbose = false, benchOnly = false, quick = false;
    int benchTiles = 2048;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--device") deviceIndex = std::atoi(val());
        else if (a == "--device-name") deviceName = val();
        else if (a == "--list") listOnly = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--bench") benchOnly = true;
        else if (a == "--quick") quick = true;
        else if (a == "--bench-tiles") benchTiles = std::atoi(val());
        else if (a == "-h" || a == "--help") {
            std::printf(
                "usage: nxvc-passB-test [--device N | --device-name SUBSTR] [--list]\n"
                "                       [--quick] [--bench] [--bench-tiles N] [--verbose]\n"
                "exit 0 = zero mismatches, 1 = mismatch, 77 = no usable ICD\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    Ctx c;
    if (!initVulkan(c, deviceIndex, deviceName, listOnly)) return listOnly ? 0 : 77;
    std::printf("device: %s (driver %u, api %u.%u.%u)\n", c.props.deviceName,
                c.props.driverVersion, VK_VERSION_MAJOR(c.props.apiVersion),
                VK_VERSION_MINOR(c.props.apiVersion),
                VK_VERSION_PATCH(c.props.apiVersion));
    std::printf("maxComputeSharedMemorySize: %u B\n",
                c.props.limits.maxComputeSharedMemorySize);

    size_t totalBad = 0;
    int ran = 0;

    if (!benchOnly) {
        std::vector<Case> cases;
        // A base sweep: both chroma formats, both colour paths, both output
        // formats, every res_level, transform-skip on and off, every alpha
        // mode, and a QP sweep that reaches both ends of the quantizer table.
        for (int chroma420 : {0, 1})
            for (int ct : {kCtYCoCgR, kCtNone})
                for (int qp : {0, 13, 24, 47, 63}) {
                    Case cs;
                    cs.chroma420 = chroma420;
                    cs.colorTransform = ct;
                    cs.baseQp = qp;
                    cs.seed = (uint32_t)(1000 + qp * 7 + chroma420 * 3 + ct);
                    cases.push_back(cs);
                    if (quick) break;
                }
        if (!quick) {
            for (int res = 0; res <= kMaxResLevel; ++res)
                for (int tskip : {0, 1})
                    for (int chroma420 : {0, 1}) {
                        Case cs;
                        cs.chroma420 = chroma420;
                        cs.forceResLevel = res;
                        cs.forceTskip = tskip;
                        cs.seed = (uint32_t)(2000 + res * 11 + tskip * 5 + chroma420);
                        cases.push_back(cs);
                    }
            for (int am = 0; am <= 2; ++am)
                for (int chroma420 : {0, 1}) {
                    Case cs;
                    cs.chroma420 = chroma420;
                    cs.alphaPresent = 1;
                    cs.forceAlphaMode = am;
                    cs.alphaQpOff = am == 2 ? -4 : 0;
                    cs.seed = (uint32_t)(3000 + am * 13 + chroma420);
                    cases.push_back(cs);
                }
            for (int of : {kOutRgba8, kOutRgb10A2})
                for (int chroma420 : {0, 1}) {
                    Case cs;
                    cs.chroma420 = chroma420;
                    cs.outFormat = of;
                    cs.chromaQpOff = 3;
                    cs.quantMatrix = 2;
                    cs.seed = (uint32_t)(4000 + of * 17 + chroma420);
                    cases.push_back(cs);
                }
            // Two-plane 4:2:0 YCbCr passthrough (the Android / WiVRn NX path):
            // 4:2:0 stream, no colour transform, every res_level.
            for (int res = -1; res <= kMaxResLevel; ++res)
                for (int tskip : {0, 1}) {
                    Case cs;
                    cs.chroma420 = 1;
                    cs.colorTransform = kCtNone;
                    cs.outFormat = kOutYcbcr420;
                    cs.forceResLevel = res;
                    cs.forceTskip = tskip;
                    cs.seed = (uint32_t)(4500 + (res + 1) * 19 + tskip);
                    cases.push_back(cs);
                }
            // Saturating coefficients, to exercise every clamp.
            for (int chroma420 : {0, 1}) {
                Case cs;
                cs.chroma420 = chroma420;
                cs.baseQp = 63;
                cs.coefMagnitude = 32767;
                cs.quantMatrix = 3;
                cs.seed = (uint32_t)(5000 + chroma420);
                cases.push_back(cs);
            }
            // Non-tile-multiple image: the last tile column/row is clipped.
            for (int chroma420 : {0, 1}) {
                Case cs;
                cs.chroma420 = chroma420;
                cs.tilesX = 3;
                cs.tilesY = 2;
                cs.seed = (uint32_t)(6000 + chroma420);
                cases.push_back(cs);
            }
        }

        for (const Case &cs : cases) {
            std::printf("case: %s\n", caseName(cs));
            size_t bad = compareCase(c, cs, verbose, ran);
            if (bad) std::printf("  MISMATCH: %zu pixels\n", bad);
            totalBad += bad;
        }
    }

    // ---------------------------------------------------------- benchmark
    {
        int tilesX = 64, tilesY = (benchTiles + 63) / 64;
        Case cs;
        cs.tilesX = tilesX;
        cs.tilesY = tilesY;
        cs.chroma420 = 1;
        cs.colorTransform = kCtNone;
        cs.forceResLevel = 0;
        cs.forceTskip = 0;
        cs.seed = 424242;
        Scene sc = buildScene(cs);
        GpuResult gpu;
        std::string err;
        const int reps = 20;
        if (runGpu(c, sc, gpu, reps, err)) {
            double coefMB = (double)sc.coef.size() * 2.0 / (1024.0 * 1024.0);
            double recMB = (double)sc.recs.size() * 16.0 / (1024.0 * 1024.0);
            double outMB = (double)sc.push.imageW * sc.push.imageH * 4.0 /
                           (1024.0 * 1024.0);
            std::printf(
                "\nbench: %d tiles (%dx%d), 4:2:0 res_level 0, %d dispatches\n",
                tilesX * tilesY, tilesX, tilesY, reps);
            std::printf("  dispatch time: %.3f ms (gpu timestamps), %.3f ms wall\n",
                        gpu.dispatchMs, gpu.wallMs);
            std::printf("  coefficient SSBO read: %.2f MB/frame (%d int16 per tile slot)\n",
                        coefMB, sc.push.coefStrideI16);
            std::printf("  tile records: %.3f MB/frame; output image write: %.2f MB/frame\n",
                        recMB, outMB);
            std::printf("  Pass B total traffic (coef + recs + out): %.2f MB/frame\n",
                        coefMB + recMB + outMB);
            if (gpu.dispatchMs > 0)
                std::printf("  effective bandwidth: %.1f GB/s\n",
                            (coefMB + recMB + outMB) / 1024.0 / (gpu.dispatchMs / 1000.0));
        } else {
            std::printf("\nbench: SKIP (%s)\n", err.c_str());
        }
    }

    std::printf("\n%s: %d cases run, %zu mismatching pixels\n",
                totalBad == 0 ? "PASS" : "FAIL", ran, totalBad);

    if (c.queries) vkDestroyQueryPool(c.dev, c.queries, nullptr);
    vkDestroyCommandPool(c.dev, c.pool, nullptr);
    vkDestroyDevice(c.dev, nullptr);
    vkDestroyInstance(c.inst, nullptr);
    return totalBad == 0 ? 0 : 1;
}
