// nxvc-passA-test - headless Vulkan harness for the Pass A entropy decoder.
//
// Builds a corpus of encoded tiles, decodes it on the GPU with
// rans_decode.comp, decodes the same corpus with the CPU model, and requires
// ZERO mismatches.  Reports decode time from timestamp queries.
//
// The Vulkan boilerplate here is deliberately minimal and self-contained;
// vk/common may replace it later.
//
//   nxvc-passA-test [--device SUBSTR] [--tiles N] [--seed S]
//                   [--mode ballot|lds|both] [--subgroup N] [--iters N]
//                   [--entropy rans|lite] [--intra]
//                   [--list] [--validate] [--spv PATH] [--quick]

#include <vulkan/vulkan.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "passA_test_corpus.h"

#if __has_include("rans_decode.spv.h")
#include "rans_decode.spv.h"
#define HAVE_EMBEDDED_SPV 1
#endif

using namespace nxwarp_passA;
using namespace nxwarp_passA::test;

namespace {

// ---------------------------------------------------------------------------

#define VKCHECK(x)                                                          \
    do {                                                                    \
        VkResult r_ = (x);                                                  \
        if (r_ != VK_SUCCESS) {                                             \
            std::fprintf(stderr, "%s:%d: %s failed (%d)\n", __FILE__,       \
                         __LINE__, #x, int(r_));                            \
            std::exit(2);                                                   \
        }                                                                   \
    } while (0)

struct Options {
    std::string device;
    uint32_t tiles = 2048;
    uint64_t seed = 12345;
    std::string mode = "both";
    int subgroup = 0;  // 0 = driver default
    int iters = 8;
    bool list = false;
    bool validate = false;
    bool quick = false;
    std::string spv;
    // [entropy-lite] Which entropy tool to build the corpus with and to
    // specialise the pipeline for: "rans" (default) or "lite".
    std::string entropy = "rans";
    // [entropy-lite] INTRA_DIR: mode units in the unit list.  Only the Lite
    // encoder codes them, so this is rejected with --entropy rans.  Off by
    // default so the rANS and Lite corpora are the same tiles.
    bool intra = false;
};

// ---------------------------------------------------------------------------
struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memprops{};
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceSubgroupProperties subgroup{};
    VkPhysicalDeviceSubgroupSizeControlProperties sgsize{};
    bool has_size_control = false;
    float timestamp_period = 1.0f;
    uint32_t timestamp_valid_bits = 0;
};

struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

uint32_t find_mem(const Gpu &g, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (g.memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    std::fprintf(stderr, "no memory type for flags 0x%x\n", want);
    std::exit(2);
}

Buf make_buf(const Gpu &g, VkDeviceSize size, VkBufferUsageFlags usage,
             VkMemoryPropertyFlags props) {
    Buf b;
    b.size = size ? size : 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = b.size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(g.device, &bi, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.device, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem(g, req.memoryTypeBits, props);
    VKCHECK(vkAllocateMemory(g.device, &ai, nullptr, &b.mem));
    VKCHECK(vkBindBufferMemory(g.device, b.buf, b.mem, 0));
    return b;
}

void destroy_buf(const Gpu &g, Buf &b) {
    if (b.buf) vkDestroyBuffer(g.device, b.buf, nullptr);
    if (b.mem) vkFreeMemory(g.device, b.mem, nullptr);
    b = Buf{};
}

// One-shot command buffer.
VkCommandBuffer begin_cmd(const Gpu &g) {
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(g.device, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

void end_cmd(const Gpu &g, VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VKCHECK(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(g.queue));
    vkFreeCommandBuffers(g.device, g.pool, 1, &cb);
}

void upload(const Gpu &g, Buf &dst, const void *src, VkDeviceSize bytes) {
    if (!bytes) return;
    Buf stage = make_buf(g, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void *p = nullptr;
    VKCHECK(vkMapMemory(g.device, stage.mem, 0, bytes, 0, &p));
    std::memcpy(p, src, size_t(bytes));
    vkUnmapMemory(g.device, stage.mem);
    VkCommandBuffer cb = begin_cmd(g);
    VkBufferCopy c{0, 0, bytes};
    vkCmdCopyBuffer(cb, stage.buf, dst.buf, 1, &c);
    end_cmd(g, cb);
    destroy_buf(g, stage);
}

void download(const Gpu &g, Buf &src, void *dst, VkDeviceSize bytes) {
    if (!bytes) return;
    Buf stage = make_buf(g, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCommandBuffer cb = begin_cmd(g);
    VkBufferCopy c{0, 0, bytes};
    vkCmdCopyBuffer(cb, src.buf, stage.buf, 1, &c);
    end_cmd(g, cb);
    void *p = nullptr;
    VKCHECK(vkMapMemory(g.device, stage.mem, 0, bytes, 0, &p));
    std::memcpy(dst, p, size_t(bytes));
    vkUnmapMemory(g.device, stage.mem);
    destroy_buf(g, stage);
}

// ---------------------------------------------------------------------------
std::vector<uint32_t> load_spv(const Options &opt) {
    if (!opt.spv.empty()) {
        std::FILE *f = std::fopen(opt.spv.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", opt.spv.c_str());
            std::exit(2);
        }
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint32_t> v(size_t(n) / 4);
        if (std::fread(v.data(), 1, size_t(n), f) != size_t(n)) std::exit(2);
        std::fclose(f);
        return v;
    }
#ifdef HAVE_EMBEDDED_SPV
    return std::vector<uint32_t>(
        rans_decode_spv, rans_decode_spv + (sizeof(rans_decode_spv) / 4));
#else
    std::fprintf(stderr, "no embedded SPIR-V; pass --spv PATH\n");
    std::exit(2);
#endif
}

// ---------------------------------------------------------------------------
struct RunResult {
    size_t coef_mismatch = 0;
    size_t cbf_mismatch = 0;
    size_t ulen_mismatch = 0;
    size_t mode_mismatch = 0;
    size_t status_bad = 0;
    double best_ms = 0.0;
    bool timed = false;
};

// `coef_fill` is what both the GPU buffer and the CPU model's array start
// from.  Under the sparse layout Pass A does not zero the coefficient region,
// so starting both sides from the same non-zero pattern is what proves the
// kernel wrote exactly the slots the model wrote and no others.
constexpr int16_t kCoefFill = int16_t(0x5555);

RunResult run_gpu(const Gpu &g, const Options &opt, const Corpus &c,
                  const std::vector<uint32_t> &spv, uint32_t mode,
                  uint32_t sparse, const std::vector<int16_t> &ref_coef,
                  const std::vector<uint32_t> &ref_cbf,
                  const std::vector<uint32_t> &ref_ulen,
                  const std::vector<uint32_t> &ref_modes) {
    RunResult res;
    const uint32_t num_tiles = uint32_t(c.tiles.size());

    // --- buffers -----------------------------------------------------------
    const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    std::vector<uint32_t> tiles_flat(size_t(num_tiles) * kTileDescUints);
    for (uint32_t t = 0; t < num_tiles; ++t) {
        tiles_flat[t * kTileDescUints + kTdBitsOffset] = c.tiles[t].bits_offset;
        tiles_flat[t * kTileDescUints + kTdBitsLength] = c.tiles[t].bits_length;
        tiles_flat[t * kTileDescUints + kTdCoefOffset] = c.tiles[t].coef_offset;
        tiles_flat[t * kTileDescUints + kTdCbfOffset] = c.tiles[t].cbf_offset;
        tiles_flat[t * kTileDescUints + kTdModeOffset] = t * kModeRegionUints;
        tiles_flat[t * kTileDescUints + kTdUnitLenOffset] =
            t * kUnitLenWordsPerTile;
    }

    const VkDeviceSize coef_bytes = VkDeviceSize(num_tiles) * c.coef_stride * 2;
    const VkDeviceSize cbf_bytes = VkDeviceSize(num_tiles) * c.cbf_words * 4;
    const VkDeviceSize status_bytes = VkDeviceSize(num_tiles) * 4;
    // [v3] binding 6: the per-block intra modes Pass A writes for Pass B.
    const VkDeviceSize mode_bytes =
        VkDeviceSize(num_tiles) * kModeRegionUints * 4;
    // [sparse] binding 7: one byte per coding unit, LAST + 1.
    const VkDeviceSize ulen_bytes =
        VkDeviceSize(num_tiles) * kUnitLenWordsPerTile * 4;

    Buf b_bits = make_buf(g, c.bits.size(), su, dl);
    Buf b_tiles = make_buf(g, tiles_flat.size() * 4, su, dl);
    Buf b_tables = make_buf(g, c.table_flat.size() * 4, su, dl);
    Buf b_coef = make_buf(g, coef_bytes, su, dl);
    Buf b_cbf = make_buf(g, cbf_bytes, su, dl);
    Buf b_status = make_buf(g, status_bytes, su, dl);
    Buf b_modes = make_buf(g, mode_bytes, su, dl);
    Buf b_ulen = make_buf(g, ulen_bytes, su, dl);

    std::vector<int16_t> coef_seed(size_t(coef_bytes / 2), kCoefFill);
    upload(g, b_coef, coef_seed.data(), coef_bytes);
    upload(g, b_bits, c.bits.data(), c.bits.size());
    upload(g, b_tiles, tiles_flat.data(), tiles_flat.size() * 4);
    upload(g, b_tables, c.table_flat.data(), c.table_flat.size() * 4);

    // --- descriptors -------------------------------------------------------
    VkDescriptorSetLayoutBinding binds[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 8;
    dli.pBindings = binds;
    VkDescriptorSetLayout dsl;
    VKCHECK(vkCreateDescriptorSetLayout(g.device, &dli, nullptr, &dsl));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkPipelineLayoutCreateInfo pli{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout plo;
    VKCHECK(vkCreatePipelineLayout(g.device, &pli, nullptr, &plo));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
    VkDescriptorPoolCreateInfo dpi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VkDescriptorPool dpool;
    VKCHECK(vkCreateDescriptorPool(g.device, &dpi, nullptr, &dpool));

    VkDescriptorSetAllocateInfo dai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    VKCHECK(vkAllocateDescriptorSets(g.device, &dai, &dset));

    VkBuffer bufs[8] = {b_bits.buf,   b_tiles.buf, b_tables.buf, b_coef.buf,
                        b_cbf.buf,    b_status.buf, b_modes.buf, b_ulen.buf};
    VkDescriptorBufferInfo dbi[8]{};
    VkWriteDescriptorSet wr[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
        dbi[i] = {bufs[i], 0, VK_WHOLE_SIZE};
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[i].dstSet = dset;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[i].pBufferInfo = &dbi[i];
    }
    vkUpdateDescriptorSets(g.device, 8, wr, 0, nullptr);

    // --- pipeline ----------------------------------------------------------
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.size() * 4;
    smi.pCode = spv.data();
    VkShaderModule sm;
    VKCHECK(vkCreateShaderModule(g.device, &smi, nullptr, &sm));

    struct SpecData {
        uint32_t read_ptr_mode;
        uint32_t tiles_per_group;
        // [entropy-lite] kEntropyRans or kEntropyLiteFixed; main() branches
        // on it at the top, so the two paths never share a barrier.
        uint32_t entropy_mode;
        // [minor 6] The context-table stride, which sizes the shared
        // cumulative-frequency table.  This harness builds its corpora with
        // the widest model, so it always compiles the wide kernel.
        uint32_t ctx_stride;
    } spec{mode, kTilesPerGroup, c.entropy, uint32_t(kNumCtx)};
    VkSpecializationMapEntry sme[4] = {
        {kSpecIdReadPtrMode, offsetof(SpecData, read_ptr_mode), 4},
        {kSpecIdWorkgroupTiles, offsetof(SpecData, tiles_per_group), 4},
        {kSpecIdEntropyMode, offsetof(SpecData, entropy_mode), 4},
        {kSpecIdCtxStride, offsetof(SpecData, ctx_stride), 4}};
    VkSpecializationInfo spi{4, sme, sizeof(spec), &spec};

    VkComputePipelineCreateInfo cpi{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.stage.pSpecializationInfo = &spi;
    cpi.layout = plo;

    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    if (g.has_size_control) {
        cpi.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        if (opt.subgroup > 0) {
            rss.requiredSubgroupSize = uint32_t(opt.subgroup);
            cpi.stage.pNext = &rss;
        }
    }

    VkPipeline pipe;
    VKCHECK(vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                     &pipe));

    // --- timestamps --------------------------------------------------------
    VkQueryPool qpool = VK_NULL_HANDLE;
    if (g.timestamp_valid_bits > 0) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        VKCHECK(vkCreateQueryPool(g.device, &qpi, nullptr, &qpool));
    }

    // [entropy-lite] The Lite path is one workgroup per tile.
    const uint32_t groups = group_count(num_tiles, kLanes, c.entropy);
    struct Push {
        uint32_t num_tiles, frame_nplanes, coef_stride, cbf_words, tools,
            sparse;
    } push{num_tiles, c.frame_nplanes, c.coef_stride,
           c.cbf_words, c.tools,       sparse};

    double best = 1e30;
    for (int it = 0; it < opt.iters; ++it) {
        VkCommandBuffer cb = begin_cmd(g);
        if (qpool) {
            vkCmdResetQueryPool(cb, qpool, 0, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plo, 0, 1,
                                &dset, 0, nullptr);
        vkCmdPushConstants(cb, plo, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof push, &push);
        vkCmdDispatch(cb, groups, 1, 1);
        if (qpool)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool,
                                1);
        end_cmd(g, cb);

        if (qpool) {
            uint64_t ts[2] = {0, 0};
            if (vkGetQueryPoolResults(g.device, qpool, 0, 2, sizeof(ts), ts,
                                      sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT |
                                          VK_QUERY_RESULT_WAIT_BIT) ==
                VK_SUCCESS) {
                uint64_t mask = g.timestamp_valid_bits >= 64
                                    ? ~0ull
                                    : ((1ull << g.timestamp_valid_bits) - 1);
                double ms = double((ts[1] & mask) - (ts[0] & mask)) *
                            double(g.timestamp_period) / 1e6;
                if (ms > 0 && ms < best) { best = ms; res.timed = true; }
            }
        }
    }
    if (res.timed) res.best_ms = best;

    // --- compare -----------------------------------------------------------
    std::vector<int16_t> coef(size_t(coef_bytes / 2));
    std::vector<uint32_t> cbf(size_t(cbf_bytes / 4));
    std::vector<uint32_t> status(num_tiles);
    std::vector<uint32_t> ulen(size_t(ulen_bytes / 4));
    std::vector<uint32_t> gmodes(size_t(mode_bytes / 4));
    download(g, b_coef, coef.data(), coef_bytes);
    download(g, b_cbf, cbf.data(), cbf_bytes);
    download(g, b_status, status.data(), status_bytes);
    download(g, b_ulen, ulen.data(), ulen_bytes);
    download(g, b_modes, gmodes.data(), mode_bytes);

    for (size_t i = 0; i < status.size(); ++i)
        if (status[i] != kStatusOk) ++res.status_bad;
    for (size_t i = 0; i < coef.size(); ++i)
        if (coef[i] != ref_coef[i]) {
            if (res.coef_mismatch == 0)
                std::fprintf(stderr,
                             "  first coef mismatch: tile %zu index %zu "
                             "gpu=%d cpu=%d\n",
                             i / c.coef_stride, i % c.coef_stride, int(coef[i]),
                             int(ref_coef[i]));
            ++res.coef_mismatch;
        }
    for (size_t i = 0; i < cbf.size(); ++i)
        if (cbf[i] != ref_cbf[i]) ++res.cbf_mismatch;
    // [v3/entropy-lite] binding 6.  With no INTRA_DIR both sides are all
    // zero, which still proves the kernel zeroed the region.
    for (size_t i = 0; i < gmodes.size(); ++i)
        if (gmodes[i] != ref_modes[i]) {
            if (res.mode_mismatch == 0)
                std::fprintf(stderr,
                             "  first mode mismatch: word %zu (tile %zu) "
                             "gpu=%08x cpu=%08x\n",
                             i, i / kModeRegionUints, gmodes[i], ref_modes[i]);
            ++res.mode_mismatch;
        }
    if (sparse)
        for (size_t i = 0; i < ulen.size(); ++i)
            if (ulen[i] != ref_ulen[i]) {
                if (res.ulen_mismatch == 0)
                    std::fprintf(stderr,
                                 "  first len mismatch: word %zu (tile %zu) "
                                 "gpu=%08x cpu=%08x\n",
                                 i, i / kUnitLenWordsPerTile, ulen[i],
                                 ref_ulen[i]);
                ++res.ulen_mismatch;
            }

    if (qpool) vkDestroyQueryPool(g.device, qpool, nullptr);
    vkDestroyPipeline(g.device, pipe, nullptr);
    vkDestroyShaderModule(g.device, sm, nullptr);
    vkDestroyDescriptorPool(g.device, dpool, nullptr);
    vkDestroyPipelineLayout(g.device, plo, nullptr);
    vkDestroyDescriptorSetLayout(g.device, dsl, nullptr);
    destroy_buf(g, b_bits);
    destroy_buf(g, b_tiles);
    destroy_buf(g, b_tables);
    destroy_buf(g, b_coef);
    destroy_buf(g, b_ulen);
    destroy_buf(g, b_cbf);
    destroy_buf(g, b_status);
    destroy_buf(g, b_modes);
    return res;
}

// ---------------------------------------------------------------------------
// Exit 77 (ctest SKIP_RETURN_CODE) when there is no usable ICD.
int no_icd(const char *why) {
    std::printf("SKIP: %s\n", why);
    return 77;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--device") opt.device = val();
        else if (a == "--tiles") opt.tiles = uint32_t(std::stoul(val()));
        else if (a == "--seed") opt.seed = std::stoull(val());
        else if (a == "--mode") opt.mode = val();
        else if (a == "--subgroup") opt.subgroup = std::stoi(val());
        else if (a == "--iters") opt.iters = std::stoi(val());
        else if (a == "--spv") opt.spv = val();
        else if (a == "--entropy") opt.entropy = val();
        else if (a == "--intra") opt.intra = true;
        else if (a == "--list") opt.list = true;
        else if (a == "--validate") opt.validate = true;
        else if (a == "--quick") { opt.quick = true; opt.tiles = 128; opt.iters = 1; }
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    // --- instance ----------------------------------------------------------
    Gpu g;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc-passA-test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    if (opt.validate) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
    }
    VkResult ir = vkCreateInstance(&ici, nullptr, &g.instance);
    if (ir != VK_SUCCESS) return no_icd("vkCreateInstance failed (no ICD?)");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    if (n == 0) return no_icd("no Vulkan physical devices");
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g.instance, &n, devs.data());

    int chosen = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (opt.list) std::printf("device %u: %s\n", i, p.deviceName);
        if (chosen < 0 &&
            (opt.device.empty() || std::string(p.deviceName).find(opt.device) !=
                                       std::string::npos))
            chosen = int(i);
    }
    if (opt.list) return 0;
    if (chosen < 0) return no_icd("no device matched --device");
    g.phys = devs[chosen];

    // --- properties --------------------------------------------------------
    g.sgsize = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    g.subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    g.subgroup.pNext = &g.sgsize;
    VkPhysicalDeviceProperties2 p2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &g.subgroup;
    vkGetPhysicalDeviceProperties2(g.phys, &p2);
    g.props = p2.properties;
    g.timestamp_period = g.props.limits.timestampPeriod;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.memprops);

    // --- required features -------------------------------------------------
    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(g.phys, &f2);

    if (!f2.features.shaderInt16)
        return no_icd("device lacks shaderInt16");
    if (!f11.storageBuffer16BitAccess)
        return no_icd("device lacks storageBuffer16BitAccess");
    if (!(g.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT))
        return no_icd("device lacks subgroup ballot");
    g.has_size_control = f13.subgroupSizeControl != VK_FALSE;

    std::printf("device: %s (driver %u, api %u.%u.%u)\n", g.props.deviceName,
                g.props.driverVersion, VK_API_VERSION_MAJOR(g.props.apiVersion),
                VK_API_VERSION_MINOR(g.props.apiVersion),
                VK_API_VERSION_PATCH(g.props.apiVersion));
    std::printf("subgroup: size %u (min %u max %u), size_control %d\n",
                g.subgroup.subgroupSize, g.sgsize.minSubgroupSize,
                g.sgsize.maxSubgroupSize, int(g.has_size_control));

    // --- queue / device ----------------------------------------------------
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qn, qf.data());
    int qi = -1;
    for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi = int(i); break; }
    if (qi < 0) return no_icd("no compute queue");
    g.qfamily = uint32_t(qi);
    g.timestamp_valid_bits = qf[qi].timestampValidBits;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.qfamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features e13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.subgroupSizeControl = f13.subgroupSizeControl;
    e13.computeFullSubgroups = f13.computeFullSubgroups;
    VkPhysicalDeviceVulkan11Features e11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.storageBuffer16BitAccess = VK_TRUE;
    e11.pNext = &e13;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.features.shaderInt16 = VK_TRUE;
    e2.pNext = &e11;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VKCHECK(vkCreateDevice(g.phys, &dci, nullptr, &g.device));
    vkGetDeviceQueue(g.device, g.qfamily, 0, &g.queue);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = g.qfamily;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKCHECK(vkCreateCommandPool(g.device, &cpi, nullptr, &g.pool));

    // --- corpus + CPU model ------------------------------------------------
    CorpusConfig cfg;
    cfg.num_tiles = opt.tiles;
    cfg.seed = opt.seed;
    // [entropy-lite] Same tiles, same coefficients, same RNG stream either
    // way: only the tool that codes them changes.
    if (opt.entropy == "lite") cfg.entropy = kEntropyLiteFixed;
    else if (opt.entropy != "rans") {
        std::fprintf(stderr, "--entropy must be rans or lite\n");
        return 2;
    }
    if (opt.intra) {
        if (cfg.entropy != kEntropyLiteFixed) {
            std::fprintf(stderr,
                         "--intra needs --entropy lite: the rANS test encoder "
                         "does not code mode units\n");
            return 2;
        }
        cfg.intra_dir = true;
    }
    Corpus c;
    if (!build_corpus(cfg, c)) {
        std::fprintf(stderr, "corpus build failed\n");
        return 2;
    }
    std::printf(
        "corpus: %s%s, %u tiles, %.3f %s/pixel, %zu payload bytes "
        "(%.0f B/tile), coef stride %u\n",
        c.entropy == kEntropyLiteFixed ? "entropy_lite/fixed" : "rans",
        cfg.intra_dir ? " +intra_dir" : "", uint32_t(c.tiles.size()),
        double(c.total_symbols) / double(c.total_pixels),
        c.entropy == kEntropyLiteFixed ? "fields" : "symbols", c.bits.size(),
        double(c.bits.size()) / double(c.tiles.size()), c.coef_stride);

    // The CPU model in each layout.  The dense one is checked against the
    // generator's own raster-order expectation; the sparse one is the oracle
    // the GPU is compared to, and starts from the same fill pattern the GPU
    // buffer does because Pass A no longer zeroes the region.
    struct ModelRun {
        std::vector<int16_t> coef;
        std::vector<uint32_t> cbf, status, modes, ulen;
    };
    ModelRun cpu[2];
    for (uint32_t sparse = 0; sparse < 2; ++sparse) {
        ModelRun &m = cpu[sparse];
        m.coef.assign(c.expect_coef.size(), sparse ? kCoefFill : int16_t(0));
        m.cbf.assign(c.expect_cbf.size(), 0);
        m.status.assign(c.tiles.size(), 0);
        m.modes.assign(size_t(c.tiles.size()) * kModeRegionUints, 0);
        m.ulen.assign(size_t(c.tiles.size()) * kUnitLenWordsPerTile, 0);
        Inputs in = corpus_inputs(c, kReadPtrBallot, sparse);
        Outputs o;
        o.coef = m.coef.data();
        o.cbf = m.cbf.data();
        o.status = m.status.data();
        o.modes = m.modes.data();
        o.unit_lens = m.ulen.data();
        decode(in, o);
    }
    // The model must reproduce the generator exactly, or the corpus is bad.
    size_t model_bad = 0;
    for (size_t i = 0; i < cpu[0].coef.size(); ++i)
        if (cpu[0].coef[i] != c.expect_coef[i]) ++model_bad;
    for (size_t i = 0; i < cpu[0].cbf.size(); ++i)
        if (cpu[0].cbf[i] != c.expect_cbf[i]) ++model_bad;
    for (size_t i = 0; i < cpu[0].modes.size(); ++i)
        if (cpu[0].modes[i] != c.expect_modes[i]) ++model_bad;
    for (size_t i = 0; i < cpu[0].status.size(); ++i)
        if (cpu[0].status[i] != kStatusOk) ++model_bad;
    if (model_bad) {
        std::fprintf(stderr, "CPU model disagrees with the encoder (%zu)\n",
                     model_bad);
        return 1;
    }
    std::printf("cpu model: matches the encoder exactly\n");

    std::vector<uint32_t> spv = load_spv(opt);

    // --- GPU runs ----------------------------------------------------------
    std::vector<uint32_t> modes;
    if (opt.mode == "ballot" || opt.mode == "both") modes.push_back(kReadPtrBallot);
    if (opt.mode == "lds" || opt.mode == "both") modes.push_back(kReadPtrLdsFallback);

    if (g.subgroup.subgroupSize < kLanes) {
        std::printf(
            "note: subgroupSize %u < 8, the ballot path is not valid here; "
            "testing the LDS fallback only\n",
            g.subgroup.subgroupSize);
        modes.assign(1, kReadPtrLdsFallback);
    }

    int failures = 0;
    for (uint32_t mode : modes)
        for (uint32_t sparse = 0; sparse < 2; ++sparse) {
            const char *mname = mode == kReadPtrBallot ? "ballot" : "lds";
            const ModelRun &m = cpu[sparse];
            RunResult r = run_gpu(g, opt, c, spv, mode, sparse, m.coef, m.cbf,
                                  m.ulen, m.modes);
            std::printf(
                "[%s/%s] coef_mismatch=%zu cbf_mismatch=%zu len_mismatch=%zu "
                "mode_mismatch=%zu status_bad=%zu",
                mname, sparse ? "sparse" : "dense", r.coef_mismatch,
                r.cbf_mismatch, r.ulen_mismatch, r.mode_mismatch,
                r.status_bad);
            if (r.timed)
                std::printf("  decode %.3f ms (%.2f Mtile/s, %.0f ns/tile)",
                            r.best_ms,
                            double(c.tiles.size()) / (r.best_ms * 1000.0),
                            r.best_ms * 1e6 / double(c.tiles.size()));
            std::printf("\n");
            if (r.coef_mismatch || r.cbf_mismatch || r.ulen_mismatch ||
                r.mode_mismatch || r.status_bad)
                ++failures;
        }

    vkDestroyCommandPool(g.device, g.pool, nullptr);
    vkDestroyDevice(g.device, nullptr);
    vkDestroyInstance(g.instance, nullptr);

    std::printf(failures ? "FAILED\n" : "PASSED\n");
    return failures ? 1 : 0;
}
