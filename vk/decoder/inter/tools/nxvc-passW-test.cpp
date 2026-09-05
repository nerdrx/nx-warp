// nxvc-passW-test -- headless harness for the Pass W inter predictor.
//
// Generates random-but-legal reference rings and Pass W parameter blocks, runs
// the CPU model (inter_model.cpp) and the GPU kernel (warp_pred.comp) over the
// same input, and compares the two predictors sample for sample.  Exit
// criterion is ZERO mismatching samples.
//
// The two are NOT two copies of the same arithmetic.  The kernel's corner
// derivation, corner interpolation and bilinear tap are a line-for-line copy
// of warp/glsl/warp_tile.comp; the model calls nxvc_warp_ref's
// `warp_tile_quad()` directly, exactly as ref/src/inter.cpp does.  So this
// harness compares the GPU's emulated 64-bit restoring divide against the
// normative library's, over whatever matrices and vectors the sweep produces,
// which is the property the end-to-end vectors can only test at the sixteen
// points they pin.
//
// Own minimal Vulkan boilerplate on purpose, the same shape passA/ and passB/
// already carry: plain Vulkan 1.1, no extensions, no window system.
//
// Exit 0 = conformant, 1 = a mismatch, 77 = no usable ICD (a ctest skip).
//
// SPDX-License-Identifier: Apache-2.0

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "inter_layout.h"
#include "inter_model.h"
#include "passB/passB_layout.h"
#include "passB/syntax_constants.h"
#include "warp_pred.spv.h"

using namespace nxvw;

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

// ============================================================ vulkan glue
struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
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

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want)
                return i;
        std::fprintf(stderr, "no memory type for flags 0x%x\n", want);
        std::exit(1);
    }
};

Buf createBuffer(const Ctx &c, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buf b;
    b.size = size ? size : 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = b.size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(c.dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        c.memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(c.dev, &ai, nullptr, &b.mem));
    VKCHECK(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    VKCHECK(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

void destroyBuffer(const Ctx &c, Buf &b) {
    if (b.mapped) vkUnmapMemory(c.dev, b.mem);
    if (b.buf) vkDestroyBuffer(c.dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(c.dev, b.mem, nullptr);
    b = Buf{};
}

bool initVulkan(Ctx &c, const std::string &deviceName) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc-passW-test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    if (vkCreateInstance(&ii, nullptr, &c.inst) != VK_SUCCESS) {
        std::fprintf(stderr, "no usable Vulkan ICD\n");
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
        if (!deviceName.empty() &&
            std::string(p.deviceName).find(deviceName) != std::string::npos &&
            pick < 0)
            pick = (int)i;
    }
    // A named device that is not present is a SKIP, never a silent fallback.
    if (!deviceName.empty() && pick < 0) {
        std::fprintf(stderr, "no device matching \"%s\"\n", deviceName.c_str());
        return false;
    }
    if (pick < 0) pick = 0;
    c.phys = devs[pick];
    vkGetPhysicalDeviceProperties(c.phys, &c.props);
    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.memProps);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qn, qs.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i)
        if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            c.qfam = i;
            found = true;
            break;
        }
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
    return true;
}

// ======================================================== the test scene
struct Case {
    const char *name;
    int w, h;            // per eye
    int eyes;
    int chroma420;
    int alpha;
    int color_transform;
    int mode_mix;        // 0 all WARP_MV, 1 mixed, 2 all STATIC_MV
    int res_pattern;     // 0 none, 1 cycling
    int quad;            // 1 = every tile carries quadrant vectors
    int near_skip;       // 1 = every skipped tile carries a correction
    int warp_kind;       // 0 identity, 1 mild, 2 near the envelope edge
};

struct Scene {
    NxvwWarpPush push{};
    std::vector<uint32_t> params;
    std::vector<uint32_t> ring;
    int ntiles = 0;
    int colsPerEye = 0, rows = 0;
};

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int lo, int hi) {
        return lo + (int)(next() % (uint32_t)(hi - lo + 1));
    }
};

// A homography inside the envelope docs/SYNTAX.md 3.1.1 requires: h22 exactly
// 2^29, every entry inside +-2^30, and `den` in [2^28, 2^30) at all four
// picture corners.  `kind` 2 pushes the perspective row until the denominator
// is close to one end of that range without leaving it, which is what makes
// the corner divide's magnitude term large.
void make_matrix(int kind, int w, int h, Rng &rng, int32_t hh[9]) {
    const int32_t one = 1 << kWarpQNum;
    hh[0] = one; hh[1] = 0; hh[2] = 0;
    hh[3] = 0;   hh[4] = one; hh[5] = 0;
    hh[6] = 0;   hh[7] = 0;   hh[8] = kWarpH22;
    if (kind == 0) return;
    // A small rotation-and-scale plus a translation of a few pel.
    const int amp = kind == 1 ? 12000 : 60000;
    hh[0] = one + rng.range(-amp, amp);
    hh[1] = rng.range(-amp, amp);
    hh[3] = rng.range(-amp, amp);
    hh[4] = one + rng.range(-amp, amp);
    hh[2] = rng.range(-8, 8) * one;
    hh[5] = rng.range(-8, 8) * one;
    // The perspective row, bounded so `den` stays inside [2^28, 2^30) at
    // every corner: |h20| * (w/2) + |h21| * (h/2) < 2^29 - 2^28 leaves a
    // whole octave of headroom either side of h22 = 2^29.
    const int32_t room = (1 << 28) - 4;
    const int32_t lim_x = room / (w / 2 + 1) / 2;
    const int32_t lim_y = room / (h / 2 + 1) / 2;
    hh[6] = rng.range(-(int)lim_x, (int)lim_x);
    hh[7] = rng.range(-(int)lim_y, (int)lim_y);
}

void build_scene(const Case &cs, uint32_t seed, Scene &sc) {
    Rng rng(seed);
    const int cw = cs.chroma420 ? (cs.w + 1) / 2 : cs.w;
    const int ch = cs.chroma420 ? (cs.h + 1) / 2 : cs.h;
    const int nplanes = cs.alpha ? 4 : 3;
    sc.colsPerEye = (cs.w + 63) / 64;
    sc.rows = (cs.h + 63) / 64;
    sc.ntiles = sc.colsPerEye * sc.rows * cs.eyes;

    int off[4], stride[4], planeW[4], slotU16 = 0;
    nxvw_ring_layout(cs.w, cs.h, cw, ch, cs.eyes, nplanes, off, stride, planeW,
                     &slotU16);

    NxvwWarpPush &p = sc.push;
    p.eyeW = cs.w;
    p.eyeH = cs.h;
    p.chromaW = cw;
    p.chromaH = ch;
    p.eyes = cs.eyes;
    p.colsPerEye = sc.colsPerEye;
    p.chroma420 = cs.chroma420;
    p.alphaPresent = cs.alpha;
    p.colorTransform = cs.color_transform;
    p.chromaQpOff = -2;
    p.alphaQpOff = 0;
    p.wpredStrideI16 = nxvw_wpred_stride_i16(cs.chroma420, cs.alpha);
    p.ringSlotU16 = slotU16;
    p.tileCount = sc.ntiles;
    p.eyeFilter = -1;

    // --- the ring: four slots of random samples inside each plane's domain.
    sc.ring.assign((size_t)slotU16 * 4 / 2 + 4, 0u);
    auto put = [&](int slot, int plane, int x, int y, int v) {
        const uint32_t e =
            (uint32_t)(slot * slotU16 + off[plane] + y * stride[plane] + x);
        uint32_t &w = sc.ring[e >> 1];
        const uint32_t sh = (e & 1u) * 16u;
        w = (w & ~(0xffffu << sh)) | ((uint32_t)(v & 0xffff) << sh);
    };
    for (int slot = 0; slot < 4; ++slot)
        for (int pl = 0; pl < nplanes; ++pl) {
            const bool chroma = (pl == 1 || pl == 2);
            const int maxv = (cs.color_transform == kCtYCoCgR && chroma)
                                 ? kMaxvalChromaCT
                                 : kMaxval8;
            const int pw = planeW[pl] * cs.eyes;
            const int ph = chroma ? ch : cs.h;
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                    put(slot, pl, x, y, rng.range(0, maxv));
        }

    // --- the parameter block.
    sc.params.assign((size_t)NXVW_WARP_HDR_UINTS +
                         (size_t)sc.ntiles * NXVW_WARP_TILE_UINTS,
                     0u);
    uint32_t *W = sc.params.data();
    for (int eye = 0; eye < 2; ++eye) {
        int32_t hh[9];
        make_matrix(cs.warp_kind, cs.w, cs.h, rng, hh);
        for (int sub = 1; sub <= 2; ++sub) {
            uint32_t *m = W + (size_t)(eye * 2 + (sub - 1)) * NXVW_WARP_MAT_UINTS;
            for (int i = 0; i < 9; ++i) m[i] = (uint32_t)hh[i];
            if (sub == 2) {
                // [SYN] 13.3 step 1: halve the translation to nearest, ties
                // away from zero; double the perspective row.
                auto half = [](int32_t v) {
                    return v >= 0 ? (int32_t)((v + 1) >> 1)
                                  : (int32_t)(-(int32_t)((-(int64_t)v + 1) >> 1));
                };
                m[2] = (uint32_t)half(hh[2]);
                m[5] = (uint32_t)half(hh[5]);
                m[6] = (uint32_t)(hh[6] * 2);
                m[7] = (uint32_t)(hh[7] * 2);
            }
            m[9] = (uint32_t)((sub == 2 ? cw : cs.w) / 2);
            m[10] = (uint32_t)((sub == 2 ? ch : cs.h) / 2);
        }
    }
    uint32_t *hdr = W + NXVW_WARP_HDR_RING;
    hdr[0] = (uint32_t)slotU16;
    hdr[1] = (uint32_t)cs.eyes;
    hdr[2] = (uint32_t)sc.colsPerEye;
    hdr[3] = 0;
    for (int i = 0; i < 4; ++i) {
        hdr[4 + i] = (uint32_t)off[i];
        hdr[8 + i] = (uint32_t)stride[i];
        hdr[12 + i] = (uint32_t)planeW[i];
    }

    NxvwWarpTile *t = (NxvwWarpTile *)(W + NXVW_WARP_HDR_UINTS);
    for (int i = 0; i < sc.ntiles; ++i) {
        const int col_all = i % (sc.colsPerEye * cs.eyes);
        const int row = i / (sc.colsPerEye * cs.eyes);
        const int eye = col_all / sc.colsPerEye;
        const int col = col_all % sc.colsPerEye;
        int mode = cs.mode_mix == 0   ? 2 /* WARP_MV */
                   : cs.mode_mix == 2 ? 1 /* STATIC_MV */
                                      : (int)(i % 4);   // 0..3, INTRA included
        // STEREO is legal only on eye 1; keep the sweep to the modes every
        // configuration can carry and let the end-to-end vectors pin STEREO.
        if (mode == 4) mode = 2;
        const bool inter = mode != 3;
        const int res_level = cs.res_pattern ? (i % 3) : 0;
        const int chroma444 = cs.chroma420 ? 0 : 1;
        const bool near_skip = cs.near_skip && mode == 0;
        const bool quad = cs.quad && (mode == 1 || mode == 2);
        t[i].w0 = (uint32_t)mode | ((inter ? 1u : 0u) << 3) |
                  ((uint32_t)eye << 4) | ((uint32_t)res_level << 5) |
                  ((uint32_t)chroma444 << 7) | (0u << 8) |
                  ((quad ? 1u : 0u) << 10) | ((near_skip ? 1u : 0u) << 11);
        t[i].tx = col;
        t[i].ty = row;
        t[i].mvx = rng.range(-128, 127);
        t[i].mvy = rng.range(-128, 127);
        t[i].quad = quad ? (uint32_t)rng.next() : 0u;
        // Every seventh tile of a mixed scene names a reference this decoder
        // does not hold, which is the "leave mid-grey" arm of the kernel --
        // reachable only by a skipped tile in a frame with no usable slot, and
        // therefore by no conformance vector.
        t[i].refBase = (cs.mode_mix == 1 && (i % 7) == 6)
                           ? 0xffffffffu
                           : (uint32_t)(rng.range(0, 3) * slotU16);
        t[i].qp = rng.range(0, 63);
        t[i].ns0 = near_skip ? (uint32_t)(rng.next() & 0xffffffu) : 0u;
        t[i].ns1 = near_skip ? (uint32_t)(rng.next() & 0xffffffu) : 0u;
        t[i].ns2 = near_skip ? (uint32_t)(rng.next() & 0xffffffu) : 0u;
    }
}

// ============================================================== the GPU run
bool runGpu(Ctx &c, const Scene &sc, std::vector<int16_t> &out) {
    const VkDeviceSize ringBytes = (VkDeviceSize)sc.ring.size() * 4;
    const VkDeviceSize parBytes = (VkDeviceSize)sc.params.size() * 4;
    const VkDeviceSize wpBytes =
        (VkDeviceSize)sc.ntiles * sc.push.wpredStrideI16 * 2;

    Buf bRing = createBuffer(c, ringBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buf bPar = createBuffer(c, parBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buf bOut = createBuffer(c, wpBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(bRing.mapped, sc.ring.data(), (size_t)ringBytes);
    std::memcpy(bPar.mapped, sc.params.data(), (size_t)parBytes);
    // 0xAA is not a value the kernel can produce for a plane it writes, so a
    // slot the kernel skipped is visible rather than accidentally right.
    std::memset(bOut.mapped, 0xAA, (size_t)wpBytes);

    VkDescriptorSetLayoutBinding b[3]{};
    for (int i = 0; i < 3; ++i) {
        b[i].binding = (uint32_t)i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 3;
    dl.pBindings = b;
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(c.dev, &dl, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)sizeof(NxvwWarpPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VkPipelineLayout layout;
    VKCHECK(vkCreatePipelineLayout(c.dev, &pl, nullptr, &layout));
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(warp_pred_spv);
    sm.pCode = warp_pred_spv;
    VkShaderModule mod;
    VKCHECK(vkCreateShaderModule(c.dev, &sm, nullptr, &mod));
    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipe;
    VKCHECK(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                     &pipe));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VkDescriptorPool pool;
    VKCHECK(vkCreateDescriptorPool(c.dev, &dp, nullptr, &pool));
    VkDescriptorSetAllocateInfo da{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &dsl;
    VkDescriptorSet set;
    VKCHECK(vkAllocateDescriptorSets(c.dev, &da, &set));
    VkDescriptorBufferInfo bi[3] = {{bRing.buf, 0, VK_WHOLE_SIZE},
                                    {bPar.buf, 0, VK_WHOLE_SIZE},
                                    {bOut.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(c.dev, 3, w, 0, nullptr);

    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(c.dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                            &set, 0, nullptr);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       (uint32_t)sizeof(NxvwWarpPush), &sc.push);
    vkCmdDispatch(cb, (uint32_t)sc.ntiles, 1, 1);
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VKCHECK(vkCreateFence(c.dev, &fi, nullptr, &fence));
    VKCHECK(vkQueueSubmit(c.queue, 1, &si, fence));
    VKCHECK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, 60ull * 1000000000ull));
    vkDestroyFence(c.dev, fence, nullptr);
    vkFreeCommandBuffers(c.dev, c.pool, 1, &cb);

    out.resize((size_t)sc.ntiles * sc.push.wpredStrideI16);
    std::memcpy(out.data(), bOut.mapped, (size_t)wpBytes);

    vkDestroyDescriptorPool(c.dev, pool, nullptr);
    vkDestroyPipeline(c.dev, pipe, nullptr);
    vkDestroyShaderModule(c.dev, mod, nullptr);
    vkDestroyPipelineLayout(c.dev, layout, nullptr);
    vkDestroyDescriptorSetLayout(c.dev, dsl, nullptr);
    destroyBuffer(c, bRing);
    destroyBuffer(c, bPar);
    destroyBuffer(c, bOut);
    return true;
}

// ================================================================== main
const Case kCases[] = {
    // name                     w    h  eyes 420 a  ct mix res quad ns warp
    {"identity_444",          192, 128, 1,  0, 0, 0,  0,  0,  0,  0,  0},
    {"warp_444",              192, 128, 1,  0, 0, 0,  0,  0,  0,  0,  1},
    {"warp_420",              192, 128, 1,  1, 0, 0,  0,  0,  0,  0,  1},
    {"warp_420_edge",         192, 128, 1,  1, 0, 0,  0,  0,  0,  0,  2},
    {"mixed_modes_444",       256, 192, 1,  0, 0, 0,  1,  0,  0,  0,  1},
    {"mixed_modes_420",       256, 192, 1,  1, 0, 0,  1,  0,  0,  0,  1},
    {"static_444",            192, 128, 1,  0, 0, 0,  2,  0,  0,  0,  2},
    {"res_cycle_420",         256, 192, 1,  1, 0, 0,  0,  1,  0,  0,  1},
    {"res_cycle_444",         256, 192, 1,  0, 0, 0,  0,  1,  0,  0,  1},
    {"quad_444",              192, 128, 1,  0, 0, 0,  0,  0,  1,  0,  1},
    {"quad_420_res",          256, 192, 1,  1, 0, 0,  0,  1,  1,  0,  1},
    {"near_skip_444",         256, 192, 1,  0, 0, 0,  1,  0,  0,  1,  1},
    {"near_skip_420",         256, 192, 1,  1, 0, 0,  1,  0,  0,  1,  1},
    {"ycocgr_444",            192, 128, 1,  0, 0, 1,  1,  0,  1,  1,  1},
    {"alpha_444",             192, 128, 1,  0, 1, 0,  0,  0,  0,  0,  1},
    {"stereo_444",            128, 128, 2,  0, 0, 0,  1,  0,  1,  1,  1},
    {"stereo_420_edge",       128, 128, 2,  1, 0, 0,  1,  1,  1,  1,  2},
    // A picture whose width and height are not multiples of 64: the last tile
    // column and row run past the plane and the clamp-to-edge fetch is what
    // has to agree.
    {"odd_size_420",          200, 140, 1,  1, 0, 0,  1,  0,  1,  1,  1},
    {"odd_size_444",          200, 140, 1,  0, 0, 0,  1,  0,  0,  1,  2},
};

}  // namespace

int main(int argc, char **argv) {
    std::string deviceName;
    bool verbose = false;
    int seeds = 3;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--device-name" && i + 1 < argc) deviceName = argv[++i];
        else if (a == "--verbose") verbose = true;
        else if (a == "--seeds" && i + 1 < argc) seeds = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr,
                         "usage: %s [--device-name SUBSTR] [--seeds N] "
                         "[--verbose]\n",
                         argv[0]);
            return 2;
        }
    }
    if (const char *e = std::getenv("NXVC_VKD_DEVICE"))
        if (deviceName.empty()) deviceName = e;

    Ctx c;
    if (!initVulkan(c, deviceName)) return 77;
    std::printf("-- device: %s\n", c.props.deviceName);

    int checked = 0, failed = 0;
    long long bad_samples = 0;
    for (const Case &cs : kCases) {
        for (int s = 0; s < seeds; ++s) {
            Scene sc;
            build_scene(cs, (uint32_t)(0x51ED0000u + s * 7919 + checked), sc);
            std::vector<int16_t> gpu;
            if (!runGpu(c, sc, gpu)) return 1;

            std::vector<int16_t> cpu((size_t)sc.ntiles * sc.push.wpredStrideI16,
                                     (int16_t)0xAAAA);
            InterModelInput in;
            in.push = sc.push;
            in.params = sc.params.data();
            in.ring = sc.ring.data();
            inter_model_predict(in, cpu.data());

            long long bad = 0;
            size_t first = 0;
            for (size_t i = 0; i < cpu.size(); ++i)
                if (cpu[i] != gpu[i]) {
                    if (!bad) first = i;
                    ++bad;
                }
            ++checked;
            if (bad) {
                ++failed;
                bad_samples += bad;
                const int tile = (int)(first / sc.push.wpredStrideI16);
                std::printf("FAIL %s seed %d: %lld differing samples, first at "
                            "tile %d slot %d (cpu %d, gpu %d)\n",
                            cs.name, s, bad, tile,
                            (int)(first % sc.push.wpredStrideI16), cpu[first],
                            gpu[first]);
            } else if (verbose) {
                std::printf("ok   %s seed %d (%d tiles)\n", cs.name, s,
                            sc.ntiles);
            }
        }
    }
    std::printf("-- %d scene(s) checked, %d failure(s), %lld differing "
                "samples\n",
                checked, failed, bad_samples);
    return failed ? 1 : 0;
}
