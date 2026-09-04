// nxvc-warpdiff -- GPU vs CPU bit-exactness harness for the warp predictor.
//
// Runs warp/glsl/warp_tile.comp on every Vulkan device the loader offers and
// diffs the result against warp/ref against the same random references and
// random homographies. The exit criterion for the module is ZERO mismatching
// pixels; anything else is a bug that would drift the encoder's reference
// away from the client's forever.
//
// The Vulkan setup here is deliberately minimal and self-contained. vk/common/
// is being built in parallel; this harness does not depend on it yet and is
// expected to be swapped over once that lands.
//
// Exit codes: 0 = all devices matched, 1 = mismatch or error,
//             77 = no usable Vulkan device (ctest treats this as a skip).
//
// SPDX-License-Identifier: Apache-2.0

#include <vulkan/vulkan.h>

#include <cstdio>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/warp.h"

using namespace nxvc::warp;

// ---------------------------------------------------------------------------
// Corpus (kept in sync with tests/warp/warp_corpus.h; duplicated rather than
// shared so the tool stays runnable outside the test tree)
// ---------------------------------------------------------------------------

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return static_cast<uint32_t>(next() >> 32); }
    int32_t range(int32_t lo, int32_t hi) {
        return lo + static_cast<int32_t>(u32() % static_cast<uint32_t>(hi - lo + 1));
    }
};

// std430 layout, must match TileParam in warp_tile.comp exactly (80 bytes).
struct TileParamGPU {
    int32_t h[9];
    int32_t ox, oy, tx, ty, mvx, mvy, filt, mode;
    int32_t pad0, pad1, pad2;
};
static_assert(sizeof(TileParamGPU) == 80, "std430 mismatch");

struct PushConst {
    int32_t ref_w, ref_h, ref_stride, max_value;
};

// ---------------------------------------------------------------------------

#define VK_CHECK(x)                                                                 \
    do {                                                                            \
        VkResult _r = (x);                                                          \
        if (_r != VK_SUCCESS) {                                                     \
            std::fprintf(stderr, "%s:%d: %s failed (%d)\n", __FILE__, __LINE__, #x, \
                         static_cast<int>(_r));                                     \
            return false;                                                           \
        }                                                                           \
    } while (0)

static std::vector<uint32_t> load_spv(const char* path) {
    std::vector<uint32_t> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0 && (n % 4) == 0) {
        out.resize(static_cast<size_t>(n) / 4);
        if (std::fread(out.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n))
            out.clear();
    }
    std::fclose(f);
    return out;
}

struct Device {
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkPhysicalDeviceMemoryProperties memprops{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    std::string name;
};

struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

static bool alloc_buf(Device& d, VkDeviceSize size, Buf* out) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(d.dev, &bi, nullptr, &out->buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(d.dev, out->buf, &req);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < d.memprops.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (d.memprops.memoryTypes[i].propertyFlags & want) == want) {
            idx = i;
            break;
        }
    }
    if (idx == UINT32_MAX) {
        std::fprintf(stderr, "no host-visible memory type\n");
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = idx;
    VK_CHECK(vkAllocateMemory(d.dev, &ai, nullptr, &out->mem));
    VK_CHECK(vkBindBufferMemory(d.dev, out->buf, out->mem, 0));
    VK_CHECK(vkMapMemory(d.dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->mapped));
    out->size = size;
    return true;
}

static void free_buf(Device& d, Buf& b) {
    if (b.mapped) vkUnmapMemory(d.dev, b.mem);
    if (b.buf) vkDestroyBuffer(d.dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(d.dev, b.mem, nullptr);
    b = Buf{};
}

static bool build_pipeline(Device& d, const std::vector<uint32_t>& spv) {
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = spv.size() * 4;
    si.pCode = spv.data();
    VK_CHECK(vkCreateShaderModule(d.dev, &si, nullptr, &d.shader));

    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 3;
    dli.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(d.dev, &dli, nullptr, &d.dsl));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConst)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(d.dev, &pli, nullptr, &d.playout));

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = d.shader;
    ci.stage.pName = "main";
    ci.layout = d.playout;
    VK_CHECK(vkCreateComputePipelines(d.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &d.pipe));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(d.dev, &dpi, nullptr, &d.dpool));

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = d.qfam;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(d.dev, &cpi, nullptr, &d.pool));
    return true;
}

static void destroy_device(Device& d) {
    if (!d.dev) return;
    if (d.pipe) vkDestroyPipeline(d.dev, d.pipe, nullptr);
    if (d.playout) vkDestroyPipelineLayout(d.dev, d.playout, nullptr);
    if (d.dsl) vkDestroyDescriptorSetLayout(d.dev, d.dsl, nullptr);
    if (d.dpool) vkDestroyDescriptorPool(d.dev, d.dpool, nullptr);
    if (d.shader) vkDestroyShaderModule(d.dev, d.shader, nullptr);
    if (d.pool) vkDestroyCommandPool(d.dev, d.pool, nullptr);
    vkDestroyDevice(d.dev, nullptr);
    d = Device{};
}

// ---------------------------------------------------------------------------

struct Config {
    int tiles = 10000;
    int width = 512;
    int height = 512;
    int batch = 512;
    uint64_t seed = 0xA11CEull;
    bool all_devices = true;
    bool verbose = false;
};

static void make_case_int(Rng& rng, int frame_w, int frame_h, TileParamGPU* p, Homography* H,
                          int32_t mv[2], Filter* f, Mode* m) {
    H->ox = frame_w / 2;
    H->oy = frame_h / 2;
    H->h[0] = (1 << kQNum) + rng.range(-(1 << 17), 1 << 17);
    H->h[1] = rng.range(-(1 << 17), 1 << 17);
    H->h[2] = rng.range(-200, 200) * (1 << kQNum);
    H->h[3] = rng.range(-(1 << 17), 1 << 17);
    H->h[4] = (1 << kQNum) + rng.range(-(1 << 17), 1 << 17);
    H->h[5] = rng.range(-200, 200) * (1 << kQNum);
    H->h[6] = rng.range(-200000, 200000);
    H->h[7] = rng.range(-200000, 200000);
    H->h[8] = 1 << kQDen;

    const int32_t tx = rng.range(0, frame_w / kTile - 1) * kTile;
    const int32_t ty = rng.range(0, frame_h / kTile - 1) * kTile;
    mv[0] = rng.range(-256, 256);
    mv[1] = rng.range(-256, 256);
    *f = (rng.u32() & 1u) ? kFilterCatmullRom : kFilterBilinear;
    *m = (rng.u32() % 8u == 0u) ? kModeStatic : kModeWarp;

    for (int i = 0; i < 9; ++i) p->h[i] = H->h[i];
    p->ox = H->ox;
    p->oy = H->oy;
    p->tx = tx;
    p->ty = ty;
    p->mvx = mv[0];
    p->mvy = mv[1];
    p->filt = static_cast<int32_t>(*f);
    p->mode = static_cast<int32_t>(*m);
    p->pad0 = p->pad1 = p->pad2 = 0;
}

static bool run_device(Device& d, const Config& cfg, const std::vector<uint32_t>& refpacked,
                       const RefImage& refimg, long* mismatches, long* pixels) {
    Buf bref{}, bout{}, bpar{};
    const VkDeviceSize out_bytes =
        static_cast<VkDeviceSize>(cfg.batch) * kTile * kTile * sizeof(uint32_t);
    if (!alloc_buf(d, refpacked.size() * 4, &bref)) return false;
    if (!alloc_buf(d, out_bytes, &bout)) return false;
    if (!alloc_buf(d, static_cast<VkDeviceSize>(cfg.batch) * sizeof(TileParamGPU), &bpar))
        return false;
    std::memcpy(bref.mapped, refpacked.data(), refpacked.size() * 4);

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = d.dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &d.dsl;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(d.dev, &dai, &ds));

    VkDescriptorBufferInfo bi[3] = {{bref.buf, 0, VK_WHOLE_SIZE},
                                    {bout.buf, 0, VK_WHOLE_SIZE},
                                    {bpar.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = ds;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(d.dev, 3, w, 0, nullptr);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = d.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(d.dev, &cai, &cmd));

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(d.dev, &fi, nullptr, &fence));

    PushConst pc{refimg.width, refimg.height, refimg.width, refimg.max_value};

    Rng rng(cfg.seed);
    std::vector<uint16_t> cpu(static_cast<size_t>(kTile) * kTile * 4);
    std::vector<TileParamGPU> params(cfg.batch);
    std::vector<Homography> hs(cfg.batch);
    std::vector<std::array<int32_t, 2>> mvs(cfg.batch);
    std::vector<Filter> fs(cfg.batch);
    std::vector<Mode> ms(cfg.batch);

    long done = 0;
    int first_reported = 0;
    while (done < cfg.tiles) {
        const int n = static_cast<int>(
            (cfg.tiles - done) < cfg.batch ? (cfg.tiles - done) : cfg.batch);
        for (int i = 0; i < n; ++i) {
            make_case_int(rng, refimg.width, refimg.height, &params[i], &hs[i], mvs[i].data(),
                          &fs[i], &ms[i]);
        }
        std::memcpy(bpar.mapped, params.data(), static_cast<size_t>(n) * sizeof(TileParamGPU));
        std::memset(bout.mapped, 0xCD, static_cast<size_t>(out_bytes));

        VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bbi));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.playout, 0, 1, &ds, 0,
                                nullptr);
        vkCmdPushConstants(cmd, d.playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, static_cast<uint32_t>(n), 1, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(vkResetFences(d.dev, 1, &fence));
        VK_CHECK(vkQueueSubmit(d.queue, 1, &si, fence));
        VK_CHECK(vkWaitForFences(d.dev, 1, &fence, VK_TRUE, 60ull * 1000 * 1000 * 1000));

        const uint32_t* gpu = static_cast<const uint32_t*>(bout.mapped);
        for (int i = 0; i < n; ++i) {
            warp_tile(refimg, params[i].tx, params[i].ty, hs[i], mvs[i].data(), fs[i], ms[i],
                      cpu.data(), kTile * 4);
            const uint32_t* g = gpu + static_cast<size_t>(i) * kTile * kTile;
            for (int k = 0; k < kTile * kTile; ++k) {
                const uint32_t got = g[k];
                const uint32_t want = static_cast<uint32_t>(cpu[k * 4 + 0]) |
                                      (static_cast<uint32_t>(cpu[k * 4 + 1]) << 8) |
                                      (static_cast<uint32_t>(cpu[k * 4 + 2]) << 16) |
                                      (static_cast<uint32_t>(cpu[k * 4 + 3]) << 24);
                ++(*pixels);
                if (got != want) {
                    ++(*mismatches);
                    if (first_reported < 8) {
                        ++first_reported;
                        std::fprintf(stderr,
                                     "  MISMATCH tile %ld px(%d,%d) mode=%d filt=%d "
                                     "gpu=%08x cpu=%08x  tile=(%d,%d) mv=(%d,%d)\n",
                                     done + i, k % kTile, k / kTile, params[i].mode,
                                     params[i].filt, got, want, params[i].tx, params[i].ty,
                                     params[i].mvx, params[i].mvy);
                    }
                }
            }
        }
        done += n;
        if (cfg.verbose) std::fprintf(stderr, "  %ld/%d tiles\n", done, cfg.tiles);
    }

    vkDestroyFence(d.dev, fence, nullptr);
    free_buf(d, bref);
    free_buf(d, bout);
    free_buf(d, bpar);
    return true;
}

int main(int argc, char** argv) {
    Config cfg;
    const char* spv_path = nullptr;
#ifdef NXVC_WARP_SPV_PATH
    spv_path = NXVC_WARP_SPV_PATH;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto nextInt = [&]() { return (i + 1 < argc) ? std::atoi(argv[++i]) : 0; };
        if (a == "--tiles") cfg.tiles = nextInt();
        else if (a == "--width") cfg.width = nextInt();
        else if (a == "--height") cfg.height = nextInt();
        else if (a == "--batch") cfg.batch = nextInt();
        else if (a == "--seed") cfg.seed = static_cast<uint64_t>(nextInt());
        else if (a == "--all-devices") cfg.all_devices = true;
        else if (a == "--first-device") cfg.all_devices = false;
        else if (a == "--verbose") cfg.verbose = true;
        else if (a == "--spv" && i + 1 < argc) spv_path = argv[++i];
        else if (a == "--help") {
            std::printf(
                "nxvc-warpdiff [--tiles N] [--width W] [--height H] [--batch N]\n"
                "              [--seed S] [--all-devices|--first-device] [--spv FILE]\n");
            return 0;
        }
    }

    if (!spv_path) {
        std::fprintf(stderr, "no SPIR-V path (build with glslc/glslangValidator, or --spv)\n");
        return 77;
    }
    std::vector<uint32_t> spv = load_spv(spv_path);
    if (spv.empty()) {
        std::fprintf(stderr, "cannot read SPIR-V at %s\n", spv_path);
        return 77;
    }

    // Reference picture: the same integer mix the CPU tests use.
    std::vector<uint16_t> refdata(static_cast<size_t>(cfg.width) * cfg.height * 4);
    {
        Rng rng(0x5EEDull);
        for (int y = 0; y < cfg.height; ++y) {
            for (int x = 0; x < cfg.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const uint32_t r = rng.u32();
                    int32_t v;
                    switch ((x / 17 + y / 13 + c) % 3) {
                        case 0: v = static_cast<int32_t>(r % 256u); break;
                        case 1: v = ((x ^ y) & 8) ? 255 : 0; break;
                        default: v = ((x * 3 + y * 5 + c * 7) * 255 / (cfg.width + cfg.height)) % 256;
                    }
                    refdata[(static_cast<size_t>(y) * cfg.width + x) * 4 + c] =
                        static_cast<uint16_t>(v);
                }
            }
        }
    }
    RefImage refimg;
    refimg.data = refdata.data();
    refimg.width = cfg.width;
    refimg.height = cfg.height;
    refimg.stride = cfg.width * 4;
    refimg.channels = 4;
    refimg.max_value = 255;

    std::vector<uint32_t> refpacked(static_cast<size_t>(cfg.width) * cfg.height);
    for (size_t i = 0; i < refpacked.size(); ++i) {
        refpacked[i] = static_cast<uint32_t>(refdata[i * 4 + 0]) |
                       (static_cast<uint32_t>(refdata[i * 4 + 1]) << 8) |
                       (static_cast<uint32_t>(refdata[i * 4 + 2]) << 16) |
                       (static_cast<uint32_t>(refdata[i * 4 + 3]) << 24);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc-warpdiff";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        std::fprintf(stderr, "no Vulkan loader/ICD: SKIP\n");
        return 77;
    }

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    if (ndev == 0) {
        std::fprintf(stderr, "no Vulkan physical devices: SKIP\n");
        vkDestroyInstance(inst, nullptr);
        return 77;
    }
    std::vector<VkPhysicalDevice> phys(ndev);
    vkEnumeratePhysicalDevices(inst, &ndev, phys.data());
    if (!cfg.all_devices) phys.resize(1);

    int failures = 0;
    int ran = 0;
    for (VkPhysicalDevice pd : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
        uint32_t qfam = UINT32_MAX;
        for (uint32_t i = 0; i < nq; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
        }
        if (qfam == UINT32_MAX) {
            std::printf("device '%s': no compute queue, skipped\n", props.deviceName);
            continue;
        }

        Device d;
        d.phys = pd;
        d.qfam = qfam;
        d.name = props.deviceName;
        vkGetPhysicalDeviceMemoryProperties(pd, &d.memprops);

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(pd, &dci, nullptr, &d.dev) != VK_SUCCESS) {
            std::printf("device '%s': vkCreateDevice failed, skipped\n", props.deviceName);
            continue;
        }
        vkGetDeviceQueue(d.dev, qfam, 0, &d.queue);

        if (!build_pipeline(d, spv)) {
            std::printf("device '%s': pipeline creation failed\n", props.deviceName);
            destroy_device(d);
            ++failures;
            continue;
        }

        long mism = 0, px = 0;
        const bool ok = run_device(d, cfg, refpacked, refimg, &mism, &px);
        ++ran;
        std::printf("device '%s' (driver %u.%u.%u): %d tiles, %ld pixels, %ld mismatches -- %s\n",
                    d.name.c_str(), VK_VERSION_MAJOR(props.driverVersion),
                    VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion),
                    cfg.tiles, px, mism, (ok && mism == 0) ? "PASS" : "FAIL");
        if (!ok || mism != 0) ++failures;
        destroy_device(d);
    }

    vkDestroyInstance(inst, nullptr);
    if (ran == 0) {
        std::fprintf(stderr, "no usable device: SKIP\n");
        return 77;
    }
    if (failures) {
        std::printf("\n%d device(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all %d device(s) bit-exact against the CPU reference\n", ran);
    return 0;
}
