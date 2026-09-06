// nxvc-atlas-gpu-test -- the ATLAS compose kernel against the CPU model.
//
// Three tables are carried forward over a run of frames and compared:
//
//   REF    atlas_model.cpp, the oracle, advanced eagerly on the CPU;
//   EAGER  atlas_compose.comp dispatched over EVERY entry of BOTH eyes once
//          per frame -- the frame-complete path, and the default;
//          compared against REF after every single frame;
//   LAZY   the same kernel dispatched over the CODED entries only, each
//          advanced from its private `advanced_to` up to N one step at a time,
//          then FLUSHED at the end.
//
// The exit criterion is byte-identity of all 64 bytes of all 578 entries, on
// both counts.  That is the equivalence docs/ATLAS-DECODER.md requires: "the
// same input decoded frame-complete and decoded as tile runs in arrival order,
// both flushed, must produce a byte-identical atlas".  This harness covers the
// table half of it; the pixel half arrives with nxvc_vk_decode_tiles.
//
// The point of running LAZY at 300 frames with a 64-deep ring is that the ring
// WRAPS, so the host's forced-flush policy -- advance any entry about to fall
// out of the ring, because the steps to advance it will no longer exist -- is
// exercised rather than assumed.
//
// Own minimal Vulkan boilerplate, the same shape passA/, passB/ and inter/
// already carry: plain Vulkan 1.1, no extensions, no window system.
//
// Exit 0 = the two agree, 1 = a mismatch, 77 = no usable ICD (a ctest skip).
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

#include "atlas_model.h"
#include "atlas_compose.spv.h"

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
    uint32_t *u32() const { return (uint32_t *)mapped; }
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

Buf createBuffer(const Ctx &c, VkDeviceSize size) {
    Buf b;
    b.size = size ? size : 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = b.size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
    std::memset(b.mapped, 0, (size_t)b.size);
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
    app.pApplicationName = "nxvc-atlas-gpu-test";
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
    // A named device that is not present is a SKIP, never a silent fallback:
    // "the two desktop ICDs agree" is not evidence about a third part.
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

// ============================================================ the scene
struct Rng {
    std::mt19937_64 g;
    explicit Rng(uint64_t s) : g(s) {}
    uint32_t u32() { return (uint32_t)(g() >> 32); }
    int range(int lo, int hi) { return lo + (int)(u32() % (uint32_t)(hi - lo + 1)); }
    double uni() { return std::uniform_real_distribution<double>(-1.0, 1.0)(g); }
};

// A homography in the wire's scales satisfying 3.1.1 for a w x h eye.  `scale`
// pushes it toward the edge of the envelope: at 1.0 a composition chain runs
// for tens of frames, at 6.0 it trips within a handful, so a single run sees
// both the surviving and the invalidating case on the same table.
void make_legal(Rng &rng, int32_t w, int32_t h, double scale, int32_t H[9]) {
    const double one = (double)(1 << kWarpQNum);
    for (;;) {
        const double a = 1.0 + rng.uni() * 0.05 * scale;
        const double b = rng.uni() * 0.05 * scale;
        const double c = rng.uni() * 0.05 * scale;
        const double d = 1.0 + rng.uni() * 0.05 * scale;
        const double tx = rng.uni() * 64.0 * scale;
        const double ty = rng.uni() * 64.0 * scale;
        const double lim = 0.25 * (double)kWarpH22 / (double)(w / 2 + h / 2);
        const double p = rng.uni() * lim * scale;
        const double q = rng.uni() * lim * scale;
        H[0] = (int32_t)llround(a * one);
        H[1] = (int32_t)llround(b * one);
        H[2] = (int32_t)llround(tx * one);
        H[3] = (int32_t)llround(c * one);
        H[4] = (int32_t)llround(d * one);
        H[5] = (int32_t)llround(ty * one);
        H[6] = (int32_t)llround(p);
        H[7] = (int32_t)llround(q);
        H[8] = kWarpH22;
        if (atlas_in_envelope(H, w, h)) return;
    }
}

// The 64-byte wire record of 13.12.1, packed and unpacked in one place.
void entry_pack(const AtlasEntry &e, uint32_t *w) {
    for (int k = 0; k < 9; ++k) w[k] = (uint32_t)e.C[k];
    w[NXVW_ATLAS_OFF_SRC] = e.src_frame;
    w[NXVW_ATLAS_OFF_PACK] = nxvw_atlas_pack(e.gen, e.flags, e.res_level);
    for (int k = NXVW_ATLAS_OFF_RSVD; k < NXVW_ATLAS_ENTRY_UINTS; ++k) w[k] = 0u;
}

int g_fail = 0;

// Byte-identity of the whole table, reported by the FIRST disagreeing word so
// a failure names a tile and a field rather than "the tables differ".
bool compare_tables(const uint32_t *got, const std::vector<AtlasEntry> &want,
                    const char *what, uint32_t frame) {
    static const char *fieldName[NXVW_ATLAS_ENTRY_UINTS] = {
        "C[0]", "C[1]", "C[2]", "C[3]", "C[4]", "C[5]", "C[6]", "C[7]", "C[8]",
        "src_frame", "gen|flags|res_level",
        "reserved[0]", "reserved[1]", "reserved[2]", "reserved[3]",
        "reserved[4]"};
    std::vector<uint32_t> ref(want.size() * NXVW_ATLAS_ENTRY_UINTS);
    for (size_t n = 0; n < want.size(); ++n)
        entry_pack(want[n], &ref[n * NXVW_ATLAS_ENTRY_UINTS]);
    for (size_t k = 0; k < ref.size(); ++k) {
        if (got[k] == ref[k]) continue;
        const size_t n = k / NXVW_ATLAS_ENTRY_UINTS;
        const size_t f = k % NXVW_ATLAS_ENTRY_UINTS;
        std::printf("FAIL %s frame %u: entry %zu field %s: gpu 0x%08x, "
                    "model 0x%08x\n",
                    what, frame, n, fieldName[f], got[k], ref[k]);
        ++g_fail;
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    bool verbose = false;
    std::string deviceName;
    if (const char *d = std::getenv("NXVC_VKD_DEVICE")) deviceName = d;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--verbose") verbose = true;
        else if (a == "--device" && i + 1 < argc) deviceName = argv[++i];
    }

    Ctx c;
    if (!initVulkan(c, deviceName)) return 77;
    std::printf("device: %s\n", c.props.deviceName);

    // ---- the scene: the v1 stereo configuration, 1088x1088 an eye.
    const int lumaW = 1088, lumaH = 1088;
    const int colsPerEye = (lumaW + 63) / 64;      // 17
    const int rows = (lumaH + 63) / 64;            // 17
    const int eyes = 2;
    const uint32_t entries = (uint32_t)(rows * colsPerEye * eyes);   // 578
    const uint32_t frames = 300;
    const uint32_t genMax = 0;   // the reference DECODER passes 0: no cap
    std::printf("%u entries (%d rows x %d cols x %d eyes), %u frames, "
                "H ring %u deep\n",
                entries, rows, colsPerEye, eyes, frames, NXVW_ATLAS_HRING);

    // ---- pipeline
    VkShaderModule mod;
    {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = sizeof(atlas_compose_spv);
        si.pCode = atlas_compose_spv;
        VKCHECK(vkCreateShaderModule(c.dev, &si, nullptr, &mod));
    }
    VkDescriptorSetLayoutBinding binds[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayout dsl;
    {
        VkDescriptorSetLayoutCreateInfo li{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 4;
        li.pBindings = binds;
        VKCHECK(vkCreateDescriptorSetLayout(c.dev, &li, nullptr, &dsl));
    }
    VkPipelineLayout pl;
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(NxvwAtlasPush)};
        VkPipelineLayoutCreateInfo pi{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &dsl;
        pi.pushConstantRangeCount = 1;
        pi.pPushConstantRanges = &pr;
        VKCHECK(vkCreatePipelineLayout(c.dev, &pi, nullptr, &pl));
    }
    VkPipeline pipe;
    {
        VkComputePipelineCreateInfo ci{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = pl;
        VKCHECK(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                         &pipe));
    }
    VkDescriptorPool dpool;
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
        VkDescriptorPoolCreateInfo pi{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        VKCHECK(vkCreateDescriptorPool(c.dev, &pi, nullptr, &dpool));
    }

    // ---- buffers.  Two independent table/advanced_to pairs (EAGER, LAZY)
    //      sharing one H ring and one selection list.
    const VkDeviceSize tblBytes = (VkDeviceSize)entries * NXVW_ATLAS_ENTRY_BYTES;
    Buf tblE = createBuffer(c, tblBytes);
    Buf tblL = createBuffer(c, tblBytes);
    Buf advE = createBuffer(c, (VkDeviceSize)entries * 4);
    Buf advL = createBuffer(c, (VkDeviceSize)entries * 4);
    Buf hring = createBuffer(c,
        (VkDeviceSize)NXVW_ATLAS_HRING * eyes * NXVW_ATLAS_HSLOT_UINTS * 4);
    Buf sel = createBuffer(c, (VkDeviceSize)entries * 4);

    VkDescriptorSet dsE, dsL;
    {
        VkDescriptorSetLayout ls[2] = {dsl, dsl};
        VkDescriptorSetAllocateInfo ai{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dpool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = ls;
        VkDescriptorSet sets[2];
        VKCHECK(vkAllocateDescriptorSets(c.dev, &ai, sets));
        dsE = sets[0];
        dsL = sets[1];
    }
    auto bindSet = [&](VkDescriptorSet ds, Buf &tbl, Buf &adv) {
        VkDescriptorBufferInfo bi[4] = {
            {tbl.buf, 0, VK_WHOLE_SIZE},
            {adv.buf, 0, VK_WHOLE_SIZE},
            {hring.buf, 0, VK_WHOLE_SIZE},
            {sel.buf, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = ds;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(c.dev, 4, w, 0, nullptr);
    };
    bindSet(dsE, tblE, advE);
    bindSet(dsL, tblL, advL);

    VkCommandBuffer cb;
    {
        VkCommandBufferAllocateInfo ai{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = c.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(c.dev, &ai, &cb));
    }
    VkFence fence;
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKCHECK(vkCreateFence(c.dev, &fi, nullptr, &fence));
    }

    auto dispatch = [&](VkDescriptorSet ds, const NxvwAtlasPush &push) {
        VKCHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VKCHECK(vkBeginCommandBuffer(cb, &bi));
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                                &ds, 0, nullptr);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);
        vkCmdDispatch(cb, (push.entryCount + 63u) / 64u, 1, 1);
        VKCHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VKCHECK(vkResetFences(c.dev, 1, &fence));
        VKCHECK(vkQueueSubmit(c.queue, 1, &si, fence));
        VKCHECK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    };

    // ---- the initial table.  Every entry starts CODED at frame 0, which is
    //      what a `tile_map_reset` followed by a first frame leaves behind:
    //      C is the identity, gen 0, valid.  A tenth are `static`, which is
    //      the WayVR-panel case that must advance `gen` and NOT `C`, and a
    //      tenth start invalid, which must not be touched at all.
    Rng rng(0x5eed'a71a'5c00'0001ull);
    std::vector<AtlasEntry> ref(entries);
    for (uint32_t n = 0; n < entries; ++n) {
        AtlasEntry &e = ref[n];
        atlas_identity(e.C);
        e.src_frame = 0;
        e.gen = 0;
        e.res_level = (uint32_t)rng.range(0, 2);
        const int roll = rng.range(0, 9);
        e.flags = NXVW_ATLAS_FLAG_VALID;
        if (roll == 0) e.flags = 0u;                              // invalid
        else if (roll == 1) e.flags |= NXVW_ATLAS_FLAG_STATIC;    // static
    }
    for (uint32_t n = 0; n < entries; ++n) {
        entry_pack(ref[n], tblE.u32() + n * NXVW_ATLAS_ENTRY_UINTS);
        entry_pack(ref[n], tblL.u32() + n * NXVW_ATLAS_ENTRY_UINTS);
        advE.u32()[n] = 0u;
        advL.u32()[n] = 0u;
    }
    // The host's shadow of `advanced_to` for the LAZY path.  The decoder keeps
    // this anyway, to know which entries are about to fall out of the ring.
    std::vector<uint32_t> lazyAdv(entries, 0u);

    uint32_t nCoded = 0, nInvalidated = 0, nForced = 0, nStatic = 0;
    std::vector<uint32_t> selHost;
    selHost.reserve(entries);

    for (uint32_t f = 1; f <= frames && !g_fail; ++f) {
        // ---- this frame's per-eye H.  One frame in eight carries
        //      `warp_present == 0`, which contributes NO step at all: not a
        //      composition and not a `gen` increment.
        const bool warpPresent = (rng.range(0, 7) != 0);
        // The scale walks up and down so a run sees both the long surviving
        // chain and the composition that trips the envelope.
        const double scale = 1.0 + 5.0 * (double)((f * 37u) % 23u) / 22.0;
        int32_t Hm[2][9];
        for (int e = 0; e < eyes; ++e) make_legal(rng, lumaW, lumaH, scale, Hm[e]);
        {
            const uint32_t slot0 = (f % NXVW_ATLAS_HRING) * (uint32_t)eyes;
            for (int e = 0; e < eyes; ++e) {
                uint32_t *s = hring.u32() +
                              (slot0 + (uint32_t)e) * NXVW_ATLAS_HSLOT_UINTS;
                for (int k = 0; k < 9; ++k) s[k] = (uint32_t)Hm[e][k];
                s[9] = warpPresent ? NXVW_ATLAS_HFLAG_WARP_PRESENT : 0u;
            }
        }

        // ---- REF: the eager advance on the CPU, the oracle.
        if (warpPresent)
            for (uint32_t n = 0; n < entries; ++n) {
                const int eye =
                    nxvw_atlas_eye_of((int)n, colsPerEye, eyes);
                const bool wasValid =
                    (ref[n].flags & NXVW_ATLAS_FLAG_VALID) != 0u;
                atlas_advance_entry(ref[n], Hm[eye], lumaW, lumaH, genMax);
                if (wasValid && (ref[n].flags & NXVW_ATLAS_FLAG_VALID) == 0u)
                    ++nInvalidated;
            }

        NxvwAtlasPush push{};
        push.entryCount = entries;
        push.colsPerEye = (uint32_t)colsPerEye;
        push.eyes = (uint32_t)eyes;
        push.lumaW = lumaW;
        push.lumaH = lumaH;
        push.genMax = genMax;
        push.targetFrame = f;
        push.sel = NXVW_ATLAS_SEL_ALL;

        // ---- EAGER: one dispatch over every entry of both eyes.
        dispatch(dsE, push);

        // ---- LAZY: the coded entries, plus any entry about to fall out of
        //      the ring.  An entry whose `advanced_to` is HRING behind cannot
        //      be advanced at all -- the steps no longer exist -- so the host
        //      advances it while they still do.  That is the decoder's real
        //      policy and it is why the ring depth is not the thing that makes
        //      the lazy advance safe; the per-step envelope check is.
        selHost.clear();
        std::vector<uint8_t> coded(entries, 0u);
        for (uint32_t n = 0; n < entries; ++n) {
            // The coded rate SWINGS, 2 % to 21 %, so one run sees both the
            // short gap (a step or two of lazy advance) and the long one that
            // makes an entry fall out of a 64-deep ring and forces the host to
            // advance it while the steps still exist.  At a flat 15 % that
            // second path never fired and the ring-wrap policy was untested.
            const int rate = 2 + (int)(f % 20u);
            const bool isCoded = (rng.range(0, 99) < rate);
            const bool falling = (f - lazyAdv[n]) >= NXVW_ATLAS_HRING - 1u;
            if (isCoded) coded[n] = 1u;
            if (falling && !isCoded) ++nForced;
            if (isCoded || falling) selHost.push_back(n);
        }
        if (!selHost.empty()) {
            std::memcpy(sel.mapped, selHost.data(), selHost.size() * 4);
            NxvwAtlasPush lp = push;
            lp.entryCount = (uint32_t)selHost.size();
            lp.sel = NXVW_ATLAS_SEL_LIST;
            dispatch(dsL, lp);
            for (uint32_t n : selHost) lazyAdv[n] = f;
        }

        // ---- EAGER against the oracle, EVERY frame.  A divergence is caught
        //      at the frame it happens rather than 300 frames later.
        if (!compare_tables(tblE.u32(), ref, "eager", f)) break;

        // ---- the write-back of 13.12.3 step 3, on all three tables.  A tile
        //      whose mode produced reconstructed samples resets its entry
        //      completely.  Pass B does this on the GPU under
        //      nxvc_vk_decode_tiles; here it is the host, because what is
        //      under test is the composition and not the store.
        for (uint32_t n = 0; n < entries; ++n) {
            if (!coded[n]) continue;
            ++nCoded;
            AtlasEntry &e = ref[n];
            atlas_identity(e.C);
            e.src_frame = f;
            e.gen = 0;
            e.res_level = (uint32_t)rng.range(0, 2);
            const bool st = (rng.range(0, 9) == 0);   // mode == STATIC_MV
            if (st) ++nStatic;
            e.flags = NXVW_ATLAS_FLAG_VALID |
                      (st ? NXVW_ATLAS_FLAG_STATIC : 0u);
            entry_pack(e, tblE.u32() + n * NXVW_ATLAS_ENTRY_UINTS);
            entry_pack(e, tblL.u32() + n * NXVW_ATLAS_ENTRY_UINTS);
            advE.u32()[n] = f;
            advL.u32()[n] = f;
            lazyAdv[n] = f;
        }
    }

    // ---- the flush.  With a lazy advance some entries are behind at a frame
    //      boundary, so the table is not the eager form's table until every
    //      entry is advanced to N.  Materialising it is what conformance and
    //      any client read of the table observe.
    if (!g_fail) {
        NxvwAtlasPush push{};
        push.entryCount = entries;
        push.colsPerEye = (uint32_t)colsPerEye;
        push.eyes = (uint32_t)eyes;
        push.lumaW = lumaW;
        push.lumaH = lumaH;
        push.genMax = genMax;
        push.targetFrame = frames;
        push.sel = NXVW_ATLAS_SEL_ALL;
        dispatch(dsL, push);
        compare_tables(tblL.u32(), ref, "lazy-flushed", frames);
    }

    std::printf("-- %u tiles coded (%u of them STATIC_MV), %u entries "
                "invalidated by the envelope, %u forced-advanced before the "
                "ring wrapped\n",
                nCoded, nStatic, nInvalidated, nForced);
    if (verbose)
        std::printf("-- table %u entries x %u B = %.1f kB\n", entries,
                    (unsigned)NXVW_ATLAS_ENTRY_BYTES,
                    (double)entries * NXVW_ATLAS_ENTRY_BYTES / 1024.0);

    vkDestroyFence(c.dev, fence, nullptr);
    vkDestroyDescriptorPool(c.dev, dpool, nullptr);
    vkDestroyPipeline(c.dev, pipe, nullptr);
    vkDestroyPipelineLayout(c.dev, pl, nullptr);
    vkDestroyDescriptorSetLayout(c.dev, dsl, nullptr);
    vkDestroyShaderModule(c.dev, mod, nullptr);
    destroyBuffer(c, tblE);
    destroyBuffer(c, tblL);
    destroyBuffer(c, advE);
    destroyBuffer(c, advL);
    destroyBuffer(c, hring);
    destroyBuffer(c, sel);
    vkDestroyCommandPool(c.dev, c.pool, nullptr);
    vkDestroyDevice(c.dev, nullptr);
    vkDestroyInstance(c.inst, nullptr);

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
