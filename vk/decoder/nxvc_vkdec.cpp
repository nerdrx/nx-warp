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
#include <thread>
#include <string>
#include <vector>

#include "nxvc/nxvc_vk.h"
#include "nxvc_vkdec_parse.h"
#include "passA/syntax_constants.h"
#include "passB/syntax_constants.h"
// [ATLAS] The one description of the table, the H ring and the push blocks,
// and the host bookkeeping that goes with them.  atlas_layout.h is shared
// verbatim with atlas_compose.comp and atlas_tiles.comp, so the host cannot
// drift from the kernels it drives.
#include "atlas/atlas_layout.h"
#include "atlas/atlas_state.h"

#include "rans_decode.spv.h"
#include "rans_decode_lite.spv.h"
#include "reconstruct.spv.h"
#include "reconstruct_v1.spv.h"
#include "reconstruct_skip.spv.h"
#include "reconstruct_skip_store.spv.h"
#include "reconstruct_copy.spv.h"
#include "reconstruct_v1_x8.spv.h"
#include "reconstruct_x8.spv.h"
#include "warp_pred.spv.h"
// [ATLAS] 13.12.3 step 1, and the coded-tile kernel that brackets Pass W.
#include "atlas_compose.spv.h"
#include "atlas_tiles.spv.h"
#include "atlas_view8.spv.h"
#include "atlas_view16.spv.h"

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
    // [timing] Ticks the QUEUE FAMILY actually counts.  Vulkan guarantees 64
    // valid bits only where `timestampComputeAndGraphics` is set AND the
    // family has both bits; on a compute-only family the count is whatever
    // `timestampValidBits` says, and the bits ABOVE it are UNDEFINED.  Reading
    // a 64-bit result and subtracting without masking is therefore reading
    // driver-defined garbage in the high bits -- which this decoder did.
    uint32_t ts_valid_bits = 0;
    uint64_t ts_mask = ~0ull;

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
    // Which of the four Pass B segment pairs were WRITTEN this frame.  A pair
    // is written only when its dispatch ran, so the readback must be told
    // which exist rather than assuming a contiguous range.
    uint32_t ts_seg_mask = 0;
    uint32_t ts_limit = 14;
    // Frames whose timestamps could not be read within the poll budget.  A
    // counter rather than a log line: on a driver that never publishes them
    // this would otherwise print every frame.
    uint64_t ts_dropped = 0;
    bool ts_pending = false;
    bool astats_pending = false;

    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslA = VK_NULL_HANDLE, dslB = VK_NULL_HANDLE;
    VkPipelineLayout plA = VK_NULL_HANDLE, plB = VK_NULL_HANDLE;
    VkShaderModule smA = VK_NULL_HANDLE;
    // [entropy-lite] The same kernel compiled with a workgroup sized for the
    // Lite path, which puts ONE tile in a workgroup: 128 threads a tile
    // against the 256 rANS wants.  44 % on an Adreno 650 at 289 tiles; see
    // passA/CMakeLists.txt and passA/README.md.
    VkShaderModule smALite = VK_NULL_HANDLE;
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
    // [inter] The same tile kind, predicting for itself instead of reading
    // back what Pass W wrote.  See passB/CMakeLists.txt.
    VkShaderModule smBSkipStore = VK_NULL_HANDLE;
    // [passb] skip_kind == 3: the copy path for identity tiles.
    VkShaderModule smBCopy = VK_NULL_HANDLE;
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
    // [ATLAS] Two more sets, two more layouts, two more modules.  Neither
    // kernel has a specialisation constant, so both pipelines are created once
    // in make_layouts() rather than through a cache keyed on a frame's shape.
    VkDescriptorSetLayout dslAC = VK_NULL_HANDLE, dslAT = VK_NULL_HANDLE;
    VkPipelineLayout plAC = VK_NULL_HANDLE, plAT = VK_NULL_HANDLE;
    VkShaderModule smAC = VK_NULL_HANDLE, smAT = VK_NULL_HANDLE;
    VkDescriptorSet dsetAC = VK_NULL_HANDLE, dsetAT = VK_NULL_HANDLE;
    VkPipeline pipeAC = VK_NULL_HANDLE, pipeAT = VK_NULL_HANDLE;
    // [ATLAS] The display view (13.12.5): its own set, and two pipelines --
    // the one-tap 8-bit form and the three-plane R16 one.  Two BUILDS and not
    // a specialisation constant, because the image format qualifier is part of
    // the type and there is nothing a constant could select between.
    VkDescriptorSetLayout dslAV = VK_NULL_HANDLE;
    VkPipelineLayout plAV = VK_NULL_HANDLE;
    VkShaderModule smAV8 = VK_NULL_HANDLE, smAV16 = VK_NULL_HANDLE;
    VkDescriptorSet dsetAV = VK_NULL_HANDLE;
    VkPipeline pipeAV8 = VK_NULL_HANDLE, pipeAV16 = VK_NULL_HANDLE;
    Img imgViewY, imgViewC, imgViewCr;
    uint32_t atlas_view = 0;   // nxvc_vkd_atlas_view
    VkDescriptorSet dsetA = VK_NULL_HANDLE, dsetB = VK_NULL_HANDLE;
    std::map<uint32_t, VkPipeline> pipesA;  // lanes | ctx_stride<<8 | xfl<<16
    // key: (format << 40) | (dirSched << 32) | storeWords
    std::map<uint64_t, VkPipeline> pipesB;

    // ---- buffers
    Buf staging, bBits, bDesc, bTables, bCoef, bCbf, bStatus, bRecs, bWgt,
        bModes, bOrder, bRead, bPlanar;
    // [inter] The four-slot reference ring, the predictor Pass W hands to
    // Pass B, and the parameter block that drives both.
    Buf bRing, bWPred, bWarp;
    // [ATLAS] The three objects that replace the ring ([SYN] 13.12.1), plus
    // the two lists the kernels are driven by.
    //
    //   bTable    64 B per tile position of every eye, the normative table
    //   bAdv      one u32 per entry, `advanced_to`.  DECODER-PRIVATE and
    //             deliberately NOT in the table: 13.12.1's 64 bytes are fully
    //             specified, its 20 reserved bytes are zero and compared, and
    //             when the composition ran is not part of the atlas
    //   bHRing     NXVW_ATLAS_HRING slots x eyes x 10 uints -- nine matrix
    //             words and a flags word whose bit 0 is `warp_present`.  The
    //             flags word is not padding: a frame with warp_present == 0
    //             contributes NO step at all, and the bit cannot be inferred
    //             from a matrix whose h22 is 2^29 for every legal value
    //   bASel     the index list the compose dispatch walks under SEL_LIST
    //   bACoded   this frame's coded tiles, by TABLE index, for MATGEN and
    //             WRITEBACK
    //   bAStatus  MATGEN's deferred 13.12.4 refusal: bit 0 and the FIRST
    //             offending tile index, read back once the frame completes
    Buf bTable, bAdv, bHRing, bASel, bACoded, bAStatus;
    // The atlas PIXELS are the ring buffer with ONE slot instead of four --
    // byte-for-byte the layout nxvw_ring_layout() already computes, at a fixed
    // address instead of curSlot's.  So `bRing` IS the atlas under ATLAS and
    // there is no second pixel buffer: warp_pred.glsl reads the reference
    // through that layout, it is pinned byte-for-byte against the encoder, and
    // ADR-0029 turns on it being unmodified.  Memory falls 4:1, which is the
    // ADR's argument for ref_sel == 0.
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
    // [passb] Tiles of each eye whose prediction is a straight copy.
    uint32_t order_ncopy[2] = {0, 0};
    // [inter] Tiles dispatched on each Pass B module in eye pass 0, for the
    // per-module timestamps.
    uint32_t seg_tiles[4] = {0, 0, 0, 0};
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

    // ---- [ATLAS] state
    // Set from the stream's tool bit 31 at parse_stream_header().  It is the
    // one switch: under it the reference is the atlas, the skip module and the
    // display store do not run, and the normative output is the atlas rather
    // than a picture.
    bool atlas_mode = false;
    // [SYN] 13.12.11, tool bit 34.  The stream may carry PICTURE frames, so
    // the ring needs a SECOND slot: slot 0 is the atlas and slot 1 holds the
    // picture assembled from it, which is the reference the ordinary path
    // then predicts from.  The reconstruction is written straight back into
    // slot 0 -- the atlas it is about to become -- which is exactly the
    // two-picture bound 13.12.11's "Memory" note describes.
    bool atlas_modes = false;
    // Set per frame from frame flags bit 5, before build_warp_params().
    bool picture_frame = false;
    // The last frame number decoded, which is where a base patch's entry sits
    // on the COMPOSITION clock -- not its `src_frame`, which is provenance.
    uint32_t last_frame = 0;
    bool have_frame = false;
    // The host half of 13.12.3 -- monotonicity, the lazy selection and the
    // ring window.  Host and not device because every answer is host-known,
    // and reading either back per frame would put a stall in every frame,
    // which is the one thing tile streaming exists to remove.
    nxvw::AtlasHostState astate;
    // Scratch for the two index lists, rebuilt per call rather than per frame
    // so a tile RUN and a whole frame take the same path.
    std::vector<uint32_t> asel, acoded;

    // ---- [inter] state
    InterCtx inter{};
    // The ring's layout, from nxvw_ring_layout().  Fixed for the stream.
    int ringOff[4] = {}, ringStride[4] = {}, ringPlaneW[4] = {};
    int ringSlotU16 = 0;
    int wpredStrideI16 = 0;
    // The staging buffer's inter blocks.
    VkDeviceSize offWarp = 0;
    VkDeviceSize offPlanar = 0;   // [planar] the validated planar bodies
    // Host-side scratch for the parameter block, rebuilt per frame.
    std::vector<uint32_t> warp_words;

    nxvc_vkd_stats stats{};
    std::string err = "";
    std::string device_name = "";
    uint64_t tools_mask = 0;   // 0 until probe_device(); see tools_supported_for()
};

namespace {

using D = nxvc_vk_decoder;

// [timing] The timestamp query pool's depth, in ONE place.
//
// It is a constant and not a literal because the three sites that must agree
// about it -- the pool's `queryCount`, the per-frame `vkCmdResetQueryPool`,
// and the `ts_count` that `vkGetQueryPoolResults` reads -- drifted apart the
// moment a fourth Pass B segment was added: the pool grew to 14 and the reset
// stayed at 12.  Queries 12 and 13 were then WRITTEN every inter frame and
// never RESET, and a `vkGetQueryPoolResults` with `WAIT_BIT` over 14 queries
// waits forever for two results that can never become available.
//
// No GPU fault, no validation error on a permissive driver, and no symptom at
// all on a frame that uses only the first four queries -- so intra decoded and
// every inter stream wedged, on the one part that enforces it.
//
//   0-3    frame / Pass A / Pass B / end
//   4-5    Pass W
//   6-13   the FOUR Pass B module segments of eye pass 0 (copy, skip, coded,
//          intra_dir), two timestamps each
constexpr uint32_t kQueryCount = 14;


// The VkResult spelled the way the spec spells it.  A caller reading a log
// should not have to look up -1000069000, which is the number that cost this
// project a device round: the pool was one descriptor short and the only
// report anyone had was "vulkan error".
const char *vkresult_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
        return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    default: return "VkResult";
    }
}

// ------------------------------------------------- the create diagnostic
// nxvc_vk_decoder_last_error() needs a decoder, so a failure that happens
// BEFORE there is one -- or one whose caller does not know the library hands
// the half-built decoder back for exactly this purpose -- has nowhere to be
// read.  The WiVRn client's report of the out-of-pool bug was the whole of
// "nxvc_vk_decoder_create: vulkan error", which named neither the call nor
// the VkResult, and that is the entire diagnostic budget of a headset.
//
// So every failure path also writes here, and this is readable with no handle
// at all.  Thread-local because two threads may create decoders at once and
// a diagnostic that races is worse than none.  Never NULL, never empty.
char *create_err_buf() {
    static thread_local char b[512] = "no error";
    return b;
}
void set_create_err(const char *s) {
    char *b = create_err_buf();
    std::snprintf(b, 512, "%s", s && s[0] ? s : "unspecified failure");
}

nxvc_vkd_status seterr(D *d, nxvc_vkd_status st, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    d->err = b;
    set_create_err(b);
    return st;
}

// Used where the failure is before or without a decoder object.
nxvc_vkd_status createerr(nxvc_vkd_status st, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    set_create_err(b);
    return st;
}

#define VKTRY(d, expr)                                                    \
    do {                                                                  \
        VkResult _r = (expr);                                             \
        if (_r != VK_SUCCESS)                                             \
            return seterr((d), NXVC_VKD_ERR_VULKAN, "%s failed: %s (%d)", \
                          #expr, vkresult_name(_r), (int)_r);             \
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
                      "vkCreateInstance failed: %s (%d)", vkresult_name(r),
                      (int)r);
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
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkCreateDevice failed: %s (%d)",
                      vkresult_name(r), (int)r);
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
    // [timing] `timestampPeriod` is NANOSECONDS PER TICK and is what converts
    // a tick delta to time; `timestampValidBits` is how many bits of the
    // counter the queue family actually drives.  BOTH are needed and only the
    // first was being read.
    d->ts_period = d->props.limits.timestampPeriod;
    d->ts_valid_bits = 0;
    {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        if (qn) vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &qn, qs.data());
        if (d->qfam < qn) d->ts_valid_bits = qs[d->qfam].timestampValidBits;
    }
    // [timing] An OVERRIDE for the tick rate, which exists for two reasons and
    // both are about the device this decoder could not otherwise diagnose.
    //
    // It PROVES THE GATE: `NXVC_VKD_TS_PERIOD=200` makes the bench's
    // self-check fail on a device where it otherwise passes, so the check is
    // known to be capable of failing rather than merely never having failed.
    //
    // And it tests a HYPOTHESIS without a rebuild: if a driver's reported
    // period is wrong, the corrected value can be tried directly on the
    // device -- which is the only way to tell a wrong rate from a slow GPU,
    // because the two look identical in a duration.
    if (const char *ov = std::getenv("NXVC_VKD_TS_PERIOD")) {
        const double v = std::atof(ov);
        if (v > 0.0) d->ts_period = (float)v;
    }
    d->ts_mask = (d->ts_valid_bits == 0 || d->ts_valid_bits >= 64)
                     ? ~0ull
                     : ((1ull << d->ts_valid_bits) - 1ull);
    // A family that drives ZERO bits does not support timestamps at all, and
    // a query pool on it returns nothing meaningful.  Refuse rather than
    // report a number about nothing.
    d->have_timestamps = d->props.limits.timestampComputeAndGraphics != 0 &&
                         d->props.limits.timestampPeriod > 0.f &&
                         d->ts_valid_bits > 0;
    // [timing] Turn the query pool off entirely.  A diagnostic switch, and a
    // pointed one: the segment timers arm 14 queries and write pairs around
    // dispatches that a given frame may not issue, so "does it still wedge
    // with no timestamps at all" separates a TIMING-instrumentation hang from
    // a decode hang in one run and without a bisect.
    if (std::getenv("NXVC_VKD_NO_TIMESTAMPS")) d->have_timestamps = false;
    // [timing] Cap how many queries a frame arms, so which GROUP of timers
    // wedges a device can be found without a rebuild per hypothesis:
    //   4  frame/PassA/PassB/end only -- what an intra frame already arms
    //   6  ... plus Pass W's pair
    //  14  ... plus the four Pass B segment pairs (the default)
    d->ts_limit = kQueryCount;
    if (const char *e = std::getenv("NXVC_VKD_TS_LIMIT")) {
        const int v = std::atoi(e);
        if (v >= 4) d->ts_limit = (uint32_t)v;
    }

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
// ---------------------------------------------- descriptor set shapes
//
// One table, three passes.  The set layouts are built from it AND the
// descriptor pool is its sum, so a pass that gains a binding cannot leave the
// pool behind.
//
// It has left the pool behind three times: bindings 8 and 9 when the tile map
// and the sparse unit lengths arrived, bindings 13-15 with the inter path, and
// Pass W's binding 3 with the tile order buffer.  Every time the comment
// saying "keep these in step" was already there, every time RADV and lavapipe
// handed out descriptors past the declared pool size and said nothing, and
// every time the Adreno 650 -- which returns the conformant
// VK_ERROR_OUT_OF_POOL_MEMORY -- was the only thing that noticed.  The third
// time it cost the headset the whole decoder: every stream failed at
// nxvc_vk_decoder_create.  A comment that has failed three times is not a
// mechanism.
struct SetShape {
    int bufs;
    int imgs;
    constexpr int total() const { return bufs + imgs; }
};

// Pass A: bitstream, descriptors, tables, coefficients, CBF bits, status,
// [v3] intra modes, [sparse] unit lengths.
constexpr SetShape kSetA{8, 0};
// Pass B: buffers 0-2, 7-9 and [inter] 13-15; images 3-6 and [unorm] 10-12.
// [planar] Buffer 16 is the validated planar body, appended after the inter
// bindings so nothing that already referenced a binding has to move.
constexpr SetShape kSetB{10, 7};
// Pass W: ring in, params in, predictor out, tile order in.
constexpr SetShape kSetW{4, 0};
// [ATLAS] atlas_compose.comp: table, advanced_to, H ring, selection list.
// [stats] Binding 4 is the status/counter buffer: the valid-entry count is
// decided by the envelope check ON THE DEVICE, so the host cannot know it
// without either a readback per frame -- the one thing tile streaming exists
// to remove -- or a counter the kernel already touching the table maintains.
constexpr SetShape kSetAC{5, 0};
// [ATLAS] atlas_tiles.comp: table, advanced_to, warp params, coded list,
// status.  It is a SEPARATE layout from the compose one rather than a union of
// the two, because binding 2 is the H ring in one kernel and the warp
// parameter buffer in the other -- and a set layout that lied about which
// would be a validation error on a good driver and a silent wrong read on a
// bad one.
constexpr SetShape kSetAT{5, 0};
// [ATLAS] The display view: the atlas in, three storage images out.  Three in
// BOTH forms -- the 8-bit one binds a 1x1 placeholder as its third, because an
// unbound descriptor is not legal and a placeholder costs nothing.
constexpr SetShape kSetAV{1, 3};

// Every set the pool must serve, in one list, so the two sums below cannot
// fall behind the layouts.  The atlas sets are the fourth and fifth time this
// table has grown; the comment above says what happened the previous three.
constexpr SetShape kSets[] = {kSetA, kSetB, kSetW, kSetAC, kSetAT, kSetAV};
constexpr int kNumSets = (int)(sizeof(kSets) / sizeof(kSets[0]));
constexpr int sum_bufs() {
    int n = 0;
    for (int i = 0; i < kNumSets; ++i) n += kSets[i].bufs;
    return n;
}
constexpr int sum_imgs() {
    int n = 0;
    for (int i = 0; i < kNumSets; ++i) n += kSets[i].imgs;
    return n;
}
constexpr int kPoolBufs = sum_bufs();
constexpr int kPoolImgs = sum_imgs();
static_assert(kPoolBufs == kSetA.bufs + kSetB.bufs + kSetW.bufs + kSetAC.bufs +
                               kSetAT.bufs + kSetAV.bufs,
              "the descriptor pool is sized from kSets and every set must be "
              "in it: a set the pool does not count is VK_ERROR_OUT_OF_POOL_"
              "MEMORY on the Adreno 650 and nothing at all on RADV or "
              "lavapipe, which is how this went unnoticed three times");

// Pass B's bindings are not contiguous by type -- the images keep the numbers
// they have always had -- so its layout is built from a predicate rather than
// from a count.  This is what ties that predicate back to the table: change
// one without the other and it is a compile error on every host, rather than
// an out-of-pool failure on one device.
constexpr bool passB_is_image(int i) {
    return (i >= 3 && i <= 6) || (i >= 10 && i <= 12);
}
constexpr int passB_image_count() {
    int n = 0;
    for (int i = 0; i < kSetB.total(); ++i)
        if (passB_is_image(i)) ++n;
    return n;
}
static_assert(passB_image_count() == kSetB.imgs,
              "Pass B's image bindings and kSetB disagree: the descriptor pool "
              "is sized from kSetB, so this would be an out-of-pool failure on "
              "a driver that enforces the pool (the Adreno 650 does; RADV and "
              "lavapipe do not)");

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
    VKTRY(d, set_layout(kSetA.bufs, kSetA.imgs, &d->dslA));
    // Pass B: buffers 0-2, images 3-6, then [v3] buffers 7 (modes) and 8 (the
    // workgroup -> tile map) and [sparse] 9 (unit lengths).  The images keep
    // their bindings so nothing that already referenced them has to move.
    {
        // [unorm] and 10-12, the normalised twins of 3, 5 and 6.
        // [inter] and 13-15: the predictor Pass W wrote, the reference-ring
        // slot this frame writes, and the parameter block's ring geometry.
        VkDescriptorSetLayoutBinding b[kSetB.total()]{};
        for (int i = 0; i < kSetB.total(); ++i) {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = passB_is_image(i)
                                      ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = (uint32_t)kSetB.total();
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
    VKTRY(d, set_layout(kSetW.bufs, kSetW.imgs, &d->dslW));
    VkPushConstantRange pcW{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)sizeof(nxvw::NxvwWarpPush)};
    pl.pSetLayouts = &d->dslW;
    pl.pPushConstantRanges = &pcW;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plW));

    // [ATLAS] The compose set and the coded-tile set.
    VKTRY(d, set_layout(kSetAC.bufs, kSetAC.imgs, &d->dslAC));
    VkPushConstantRange pcAC{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             (uint32_t)sizeof(nxvw::NxvwAtlasPush)};
    pl.pSetLayouts = &d->dslAC;
    pl.pPushConstantRanges = &pcAC;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plAC));

    VKTRY(d, set_layout(kSetAV.bufs, kSetAV.imgs, &d->dslAV));
    struct AtlasViewPush { int32_t v[10]; };
    VkPushConstantRange pcAV{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             (uint32_t)sizeof(AtlasViewPush)};
    pl.pSetLayouts = &d->dslAV;
    pl.pPushConstantRanges = &pcAV;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plAV));

    VKTRY(d, set_layout(kSetAT.bufs, kSetAT.imgs, &d->dslAT));
    VkPushConstantRange pcAT{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             (uint32_t)sizeof(nxvw::NxvwAtlasTilePush)};
    pl.pSetLayouts = &d->dslAT;
    pl.pPushConstantRanges = &pcAT;
    VKTRY(d, vkCreatePipelineLayout(d->dev, &pl, nullptr, &d->plAT));

    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(rans_decode_spv);
    sm.pCode = rans_decode_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smA));
    sm.codeSize = sizeof(rans_decode_lite_spv);
    sm.pCode = rans_decode_lite_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smALite));
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
    sm.codeSize = sizeof(reconstruct_skip_store_spv);
    sm.pCode = reconstruct_skip_store_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smBSkipStore));
    sm.codeSize = sizeof(reconstruct_copy_spv);
    sm.pCode = reconstruct_copy_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smBCopy));
    sm.codeSize = sizeof(warp_pred_spv);
    sm.pCode = warp_pred_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smW));
    sm.codeSize = sizeof(atlas_compose_spv);
    sm.pCode = atlas_compose_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smAC));
    sm.codeSize = sizeof(atlas_tiles_spv);
    sm.pCode = atlas_tiles_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smAT));
    sm.codeSize = sizeof(atlas_view8_spv);
    sm.pCode = atlas_view8_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smAV8));
    sm.codeSize = sizeof(atlas_view16_spv);
    sm.pCode = atlas_view16_spv;
    VKTRY(d, vkCreateShaderModule(d->dev, &sm, nullptr, &d->smAV16));

    // [ATLAS] Neither kernel takes a specialisation constant, so both
    // pipelines are made once here.  Every other pipeline in this decoder is
    // built through a cache keyed on a frame's shape because it IS specialised
    // -- lane count, context stride, transform size, output format -- and
    // these two are not: one thread per table entry, no shared memory, no
    // variant.
    auto make_pipe = [&](VkShaderModule mod, VkPipelineLayout lay,
                         VkPipeline *out) {
        VkPipelineShaderStageCreateInfo st_{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st_.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st_.module = mod;
        st_.pName = "main";
        VkComputePipelineCreateInfo ci{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = st_;
        ci.layout = lay;
        return vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci,
                                        nullptr, out);
    };
    VKTRY(d, make_pipe(d->smAC, d->plAC, &d->pipeAC));
    VKTRY(d, make_pipe(d->smAT, d->plAT, &d->pipeAT));
    VKTRY(d, make_pipe(d->smAV8, d->plAV, &d->pipeAV8));
    VKTRY(d, make_pipe(d->smAV16, d->plAV, &d->pipeAV16));

    // Summed from the same table the three set layouts are built from, so it
    // cannot fall behind them.  See kSetA / kSetB / kSetW above for why that
    // matters more than it looks.
    VkDescriptorPoolSize sz[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)kPoolBufs},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)kPoolImgs}};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kNumSets;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sz;
    VKTRY(d, vkCreateDescriptorPool(d->dev, &dp, nullptr, &d->dpool));
    VkDescriptorSetLayout ls[kNumSets] = {d->dslA, d->dslB, d->dslW, d->dslAC,
                                          d->dslAT, d->dslAV};
    VkDescriptorSet sets[kNumSets];
    VkDescriptorSetAllocateInfo da{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = d->dpool;
    da.descriptorSetCount = kNumSets;
    da.pSetLayouts = ls;
    VKTRY(d, vkAllocateDescriptorSets(d->dev, &da, sets));
    d->dsetA = sets[0];
    d->dsetB = sets[1];
    d->dsetW = sets[2];
    d->dsetAC = sets[3];
    d->dsetAT = sets[4];
    d->dsetAV = sets[5];
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
    // [entropy-lite] The Lite module is the same source with a workgroup
    // sized for one tile rather than for rANS's lane clusters.  Everything
    // that crosses the host boundary is the same in both: the dispatch is one
    // workgroup per tile either way, the buffer layouts are per unit, and the
    // prefix sum's result does not depend on how many threads computed it.
    ci.stage.module = lite ? d->smALite : d->smA;
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
                      "passA[%s] lanes=%u mode=%u tpg=%u ctx=%u xfl=%u",
                      lite ? "lite" : "rans", lanes, mode, tpg, ctx_stride,
                      xform_large);
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
                           VkPipeline *out, int skip_kind = 0) {
    const uint32_t sched = d->dir_sched;
    // `skip_kind` is 0, 1 or 2 and so needs TWO bits.  It had one, at 59, from
    // when it was a bool -- and 2 << 59 is bit 60, which is `split_tool`.  A
    // frame with XFORM_4X4_SPLIT therefore handed the WARP_SKIP dispatch the
    // GENERAL module out of this cache, which reads a WPred buffer that by
    // then is deliberately not written for those tiles: the skip tiles came
    // out black, and only on a stream that sets one particular tool.
    // 57-58 is the free pair; every other field's shift is unchanged.
    uint64_t key = ((uint64_t)(uint32_t)skip_kind << 57) |
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
        skip_kind == 3 ? d->smBCopy
        : skip_kind == 2 ? d->smBSkipStore
        : skip_kind == 1
            ? d->smBSkip
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
                      "passB[%s] fmt=%u fmt2=%d sched=%u storeWords=%u "
                      "lds=%zuB intraDir=%d xformLarge=%d",
                      skip_kind == 2   ? "skip_store"
                      : skip_kind == 1 ? "skip"
                                       : "coded",
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
    // [planar] kPlanarUintsPerTile uints per tile, 104 B, allocated whether or
    // not the stream uses the mode: it is a descriptor that must be bound, and
    // 104 B a tile against the coefficient slot's 12.5 KB is not worth making
    // conditional.  Only WRITTEN when a frame carries a planar tile.
    if ((st = make_buf(d, d->bPlanar,
                       (VkDeviceSize)(ntiles + 1) *
                           nxvw::kPlanarUintsPerTile * 4,
                       kSsbo, false)))
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
        // [ATLAS] The atlas pixels ARE one ring slot, so an ATLAS stream
        // allocates a quarter of what the four-slot ring costs -- which is
        // ADR-0029's argument for `ref_sel == 0` turned into an allocation.
        const VkDeviceSize ringBytes =
            d->atlas_mode ? (VkDeviceSize)slot * (d->atlas_modes ? 2 : 1) * 2
            : want_inter  ? (VkDeviceSize)slot * 4 * 2
                          : 4;
        const VkDeviceSize wpredBytes =
            want_inter ? (VkDeviceSize)ntiles * d->wpredStrideI16 * 2 : 4;
        // [ATLAS] MATGEN writes one matrix PAIR per coded tile into this same
        // buffer at the tile record's `mat_idx`, so the buffer grows by a
        // pair per tile POSITION.  It is sized for every position rather than
        // for a frame's coded ones because `mat_idx` is the position's index:
        // a run of tiles arriving out of order must land at a fixed address,
        // and packing the region per frame would move every other tile's
        // matrix under it.
        const VkDeviceSize warpBytes =
            (VkDeviceSize)(NXVW_WARP_HDR_UINTS +
                           (size_t)ntiles * NXVW_WARP_TILE_UINTS +
                           (d->atlas_mode
                                ? (size_t)ntiles * 2 * NXVW_WARP_MAT_UINTS
                                : 0u)) * 4;
        if ((st = make_buf(d, d->bRing, ringBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bWPred, wpredBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bWarp, warpBytes, kSsbo, false))) return st;
        d->inter.resize(ntiles);
    }
    // [ATLAS] The table and everything that indexes it.  All six exist
    // whatever the stream's tools say -- an unbound descriptor is not legal --
    // but only an ATLAS stream pays for more than a placeholder.
    {
        const uint32_t entries = ntiles;   // one per tile POSITION of every eye
        const VkDeviceSize tabBytes =
            d->atlas_mode
                ? (VkDeviceSize)entries * NXVW_ATLAS_ENTRY_UINTS * 4
                : 4;
        const VkDeviceSize advBytes =
            d->atlas_mode ? (VkDeviceSize)entries * 4 : 4;
        // Ten uints per (slot, eye): nine matrix words and a flags word.  At
        // the v1 stereo configuration that is 64 * 2 * 10 * 4 = 5120 B, and
        // the flags word is the eleventh percent of it rather than padding.
        const VkDeviceSize hringBytes =
            d->atlas_mode ? (VkDeviceSize)NXVW_ATLAS_HRING * si.eyes *
                                NXVW_ATLAS_HSLOT_UINTS * 4
                          : 4;
        const VkDeviceSize listBytes =
            d->atlas_mode ? (VkDeviceSize)entries * 4 : 4;
        if ((st = make_buf(d, d->bTable, tabBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bAdv, advBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bHRing, hringBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bASel, listBytes, kSsbo, false))) return st;
        if ((st = make_buf(d, d->bACoded, listBytes, kSsbo, false)))
            return st;
        // FOUR uints, host-visible: MATGEN's deferred 13.12.4 refusal, the
        // FIRST tile it refused (so the report names a tile and not a frame),
        // and the two validity counters -- valid-after-advance from the
        // compose dispatch and newly-validated from the write-back, which sum
        // to the exact post-frame count.
        if ((st = make_buf(d, d->bAStatus, 16,
                           kSsbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)))
            return st;
        // [SYN] 13.12.1: on tile_map_reset the whole table is zeroed, which
        // makes every entry invalid and leaves the atlas pixels undefined.
        // The host mirror of that starts here; the device buffer is cleared
        // on the first frame that sets the flag.
        d->astate.reset(d->atlas_mode ? entries : 0u);
        // [ATLAS] The display view's images are 1x1 placeholders until a view
        // is SELECTED: the two forms want different FORMATS, not merely
        // different sizes, so they are allocated by
        // nxvc_vk_decoder_set_atlas_view() rather than here.  A placeholder
        // still has to exist, because an unbound descriptor is not legal.
        d->atlas_view = 0;
        if ((st = make_img(d, d->imgViewY, VK_FORMAT_R8_UNORM, 1, 1)))
            return st;
        if ((st = make_img(d, d->imgViewC, VK_FORMAT_R8G8_UNORM, 1, 1)))
            return st;
        if ((st = make_img(d, d->imgViewCr, VK_FORMAT_R16_UINT, 1, 1)))
            return st;
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
    VkDescriptorBufferInfo bPlanarInfo{d->bPlanar.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo b3[3] = {{d->bWPred.buf, 0, VK_WHOLE_SIZE},
                                    {d->bRing.buf, 0, VK_WHOLE_SIZE},
                                    {d->bWarp.buf, 0, VK_WHOLE_SIZE}};
    // [inter] Binding 3 is the tile order, the same buffer Pass B reads.  Pass
    // W used to index tiles by gl_WorkGroupID.x directly, which is fine while
    // it dispatches over every tile in raster order and impossible the moment
    // it does not -- and the WARP_SKIP bypass needs it to cover exactly the
    // range build_tile_order() partitioned.  Going through the order buffer
    // costs one uint load per workgroup and changes no output address.
    VkDescriptorBufferInfo avb{d->bRing.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo avi[3] = {
        {VK_NULL_HANDLE, d->imgViewY.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgViewC.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgViewCr.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo wI[4] = {{d->bRing.buf, 0, VK_WHOLE_SIZE},
                                    {d->bWarp.buf, 0, VK_WHOLE_SIZE},
                                    {d->bWPred.buf, 0, VK_WHOLE_SIZE},
                                    {d->bOrder.buf, 0, VK_WHOLE_SIZE}};
    // [ATLAS] The compose set (table, advanced_to, H ring, selection list) and
    // the coded-tile set (table, advanced_to, warp params, coded list,
    // status).  Binding 2 differs between them, which is why they are two
    // layouts rather than one union.
    VkDescriptorBufferInfo ac[5] = {{d->bTable.buf, 0, VK_WHOLE_SIZE},
                                    {d->bAdv.buf, 0, VK_WHOLE_SIZE},
                                    {d->bHRing.buf, 0, VK_WHOLE_SIZE},
                                    {d->bASel.buf, 0, VK_WHOLE_SIZE},
                                    {d->bAStatus.buf, 0, VK_WHOLE_SIZE}};
    VkDescriptorBufferInfo at[5] = {{d->bTable.buf, 0, VK_WHOLE_SIZE},
                                    {d->bAdv.buf, 0, VK_WHOLE_SIZE},
                                    {d->bWarp.buf, 0, VK_WHOLE_SIZE},
                                    {d->bACoded.buf, 0, VK_WHOLE_SIZE},
                                    {d->bAStatus.buf, 0, VK_WHOLE_SIZE}};
    // [planar] +1 for Pass B binding 16.
    VkWriteDescriptorSet w[29 + kSetAC.total() + kSetAT.total() +
                          kSetAV.total()]{};
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
    // [planar] binding 16.
    {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetB;
        w[nw].dstBinding = 16;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &bPlanarInfo;
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
    for (int i = 0; i < kSetAC.bufs; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetAC;
        w[nw].dstBinding = (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &ac[i];
        ++nw;
    }
    // [ATLAS] The display view's set.  `avb` and `avi` are declared at FUNCTION
    // scope beside every other descriptor info in this function, and that is
    // load-bearing rather than stylistic: `vkUpdateDescriptorSets` is called
    // once at the end, so anything it points at must outlive the block that
    // filled it in.  A brace-scoped local here was a dangling pointer the
    // driver dereferenced -- a segfault inside the ICD, from a function that
    // looked like it had already returned successfully.
    for (int i = 0; i < 1; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetAV;
        w[nw].dstBinding = 0;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &avb;
        ++nw;
    }
    for (int i = 0; i < 3; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetAV;
        w[nw].dstBinding = (uint32_t)(1 + i);
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[nw].pImageInfo = &avi[i];
        ++nw;
    }
    for (int i = 0; i < kSetAT.bufs; ++i) {
        w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nw].dstSet = d->dsetAT;
        w[nw].dstBinding = (uint32_t)i;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[nw].pBufferInfo = &at[i];
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
// [ATLAS] Where tile `t`'s matrix pair lives in the warp parameter buffer, as
// a uint offset.  The region follows the tile records, two 12-uint matrices per
// tile POSITION -- not per coded tile -- because `mat_idx` has to be a stable
// address: tile runs arrive out of order and a per-frame packing would move
// every other tile's matrix under it.
static inline uint32_t atlas_mat_idx(uint32_t ntiles, uint32_t t) {
    return (uint32_t)NXVW_WARP_HDR_UINTS + ntiles * NXVW_WARP_TILE_UINTS +
           t * 2u * NXVW_WARP_MAT_UINTS;
}

void build_warp_params(D *d, const FrameParse &fp, uint32_t ntiles) {
    const StreamInfo &si = d->si;
    d->warp_words.assign((size_t)NXVW_WARP_HDR_UINTS +
                             (size_t)ntiles * NXVW_WARP_TILE_UINTS +
                             (d->atlas_mode
                                  ? (size_t)ntiles * 2 * NXVW_WARP_MAT_UINTS
                                  : 0u),
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
    // [ATLAS] There is ONE slot, so this frame writes slot 0 whatever its
    // number is.  `cur_slot` is `frame_number mod 4` for the ring, and the
    // atlas buffer is a quarter of that size -- so leaving it would be an
    // out-of-bounds store three frames in four, not merely a wrong address.
    // [ATLAS] Slot 0 always, for BOTH modes: an ATLAS frame stores its coded
    // tiles into the atlas, and a PICTURE frame writes its reconstruction
    // straight into the atlas it is about to become.  The ASSEMBLE pass is the
    // one thing that writes slot 1, and it overrides this in its own header.
    h[3] = d->atlas_mode ? 0u : fp.cur_slot;
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
        if (d->atlas_mode) {
            // [ATLAS] An INTRA tile never reaches `emit_warp` -- the parse
            // only builds a Pass W record for a tile that needs prediction --
            // so its warp record is value-initialised, and a zero `w0` reads
            // back as mode 0, which is WARP_SKIP and not INTRA.
            //
            // That is invisible to Pass W, which never runs on an INTRA tile,
            // and fatal to MATGEN, which asks exactly the [SYN] 13.12.4
            // question "is this a non-INTRA mode against an invalid entry?"
            // -- and on the reset frame EVERY entry is invalid.  Left alone it
            // refuses tile 0 of frame 0 of every ATLAS stream there is.
            //
            // So the fields 13.12 reads are filled from the Pass B record,
            // which has the real mode.  Bit 3 stays CLEAR: this is not an
            // inter tile and Pass W must not predict it.
            if (!(t[i].w0 & 8u) && i < fp.recs.size()) {
                const uint32_t r0 = fp.recs[i].w0, r1 = fp.recs[i].w1;
                t[i].w0 = (r1 & 7u) | (((r0 >> 2) & 1u) << 4) |
                          (((r1 >> 3) & 3u) << 5) | (((r1 >> 5) & 1u) << 7);
            }
            // Every tile Pass W predicts gets its own matrix slot, and
            // `mat_idx` stops being the sentinel.  An INTRA tile keeps
            // NXVW_WARP_MAT_NONE: there is no prediction to build a matrix
            // for, and MATGEN returns on the sentinel before writing one.
            //
            // ONLY on an ATLAS frame.  [SYN] 13.12.11: a PICTURE frame is the
            // ordinary process and "`warp_ext()` applies frame-wide", so its
            // tiles predict through the FRAME's four matrices and `mat_idx`
            // stays the sentinel.  Leaving it set here pointed every tile at
            // the matrix region the ASSEMBLE had just filled with the
            // entries' own `C` -- so a PICTURE frame that followed any frame
            // with a non-identity `C` predicted through the wrong matrix, and
            // 6 of v91's 9 tiles came out wrong in ~4050 of 4096 samples.
            if ((t[i].w0 & 8u) && !d->picture_frame)
                t[i].mat_idx = atlas_mat_idx(ntiles, i);
            // The atlas is ONE slot, so `refBase` loses its slot arithmetic:
            // there is nowhere else to read from and `ref_sel` is 0 for every
            // tile of an ATLAS stream by ADR-0029.  A tile the parse could not
            // resolve keeps its 0xffffffff, and Pass W still treats that as
            // "no usable reference".
            // An ATLAS frame predicts from the atlas itself (slot 0); a
            // PICTURE frame predicts from the picture ASSEMBLED out of it
            // (slot 1), which is the whole of 13.12.11's plumbing -- from
            // there down it is the ordinary non-atlas path, unmodified.
            if (t[i].refBase != 0xffffffffu)
                t[i].refBase = d->picture_frame
                                   ? (uint32_t)d->ringSlotU16
                                   : 0u;
        }
    }
}

// [ATLAS] One frame's per-eye homography into the H ring slot the frame
// number addresses, plus the flags word whose bit 0 is `warp_present`.
//
// The flags word is not padding and it cannot be dropped: a frame with
// `warp_present == 0` contributes NO step at all -- not a composition and not
// a `gen` increment, because [SYN] 13.12.3 step 1 is conditioned on it in its
// entirety -- and the bit cannot be inferred from the matrix, whose h22 is
// 2^29 for every legal value.  So it travels with the slot.
void build_h_slot(const D *d, const FrameParse &fp, uint32_t frame,
                  uint32_t *out) {
    for (uint32_t eye = 0; eye < d->si.eyes; ++eye) {
        uint32_t *h = out + (size_t)eye * NXVW_ATLAS_HSLOT_UINTS;
        for (int i = 0; i < 9; ++i) h[i] = (uint32_t)fp.warp[eye].h[i];
        h[9] = fp.warp_present ? NXVW_ATLAS_HFLAG_WARP_PRESENT : 0u;
    }
    (void)frame;
}

// `sup` marks tiles [SYN] 13.12.6 has superseded: they are partitioned with
// the skipped ones, which under ATLAS is the range that is never dispatched.
// That is the whole of "a superseded tile is dropped": not its metadata and
// not its PIXELS either, and putting it in the skip range is what stops Pass W
// and Pass B from reconstructing it over a position that holds something
// newer.
void build_tile_order(D *d, const FrameParse &fp, uint32_t ntiles,
                      const std::vector<uint8_t> *sup = nullptr) {
    d->order.resize(ntiles);
    d->order_nodir[0] = d->order_nodir[1] = 0;
    d->order_nskip[0] = d->order_nskip[1] = 0;
    d->order_ncopy[0] = d->order_ncopy[1] = 0;
    // [passb] Whether each eye's LUMA plane matrix is exactly the identity.
    // Testing luma is enough: plane_homography() derives the chroma matrix by
    // halving h[2]/h[5] and doubling h[6]/h[7], and half_round(0) is 0 and
    // 0 * 2 is 0, so an identity luma matrix gives an identity chroma matrix.
    bool identity_corners[2] = {false, false};
    for (uint32_t eye = 0; eye < d->si.eyes && eye < 2; ++eye)
        identity_corners[eye] = nxvcvk::plane_matrix_is_identity(
            nxvcvk::plane_homography(fp.warp[eye], (int)d->si.width,
                                     (int)d->si.height, 1));
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
        // [passb] FOUR ranges now, and the copy one leads.  A skip tile whose
        // prediction is its reference unchanged runs a module with no
        // coordinate pipeline at all, and the decision is made HERE rather
        // than in the kernel: the host has already parsed every warp record
        // and computed the plane homography, so it can answer the question
        // once per tile instead of once per workgroup.
        auto copyMid = beg;
        if (fp.any_inter) {
            copyMid = std::stable_partition(beg, end, [&](uint32_t t) {
                // [SYN] 13.12.6: a superseded tile is DROPPED, not
                // reconstructed, and the skip range is what never gets
                // dispatched.  It must not reach the copy module either --
                // copying it would write pixels over a position that already
                // holds a newer generation, which is the one thing dropping it
                // exists to prevent.
                if (sup && t < sup->size() && (*sup)[t]) return false;
                const uint32_t w1 = fp.recs[t].w1;
                const int mode = int(w1 & 7u);
                if (mode != 0) return false;   // WARP_SKIP only; see the header
                if (t >= fp.warp_tiles.size()) return false;
                const auto &wt = fp.warp_tiles[t];
                // A tile with no reference is mid-grey, not a copy.
                if (wt.refBase == 0xffffffffu) return false;
#ifdef NXVC_VKD_FORCE_COPY
                // VALIDATION ONLY, and it produces a WRONG picture on any tile
                // whose warp is not already the identity: every WARP_SKIP tile
                // is claimed for the copy module regardless of its pose.  It
                // answers the question the byte-identity test cannot answer by
                // passing -- does the copy path ever FIRE?  If forcing it
                // leaves the conformance set green, no fixture tile reached it
                // and the pass was vacuous.  A failure is the result wanted.
                if (!nxvw::nxvw_wt_near_skip(wt.w0) &&
                    nxvw::nxvw_wt_res_level(wt.w0) == 0)
                    return true;
#endif
                return nxvcvk::warp_tile_is_copy(
                    mode, wt.mvx, wt.mvy, wt.quad,
                    nxvw::nxvw_wt_quad(wt.w0) != 0, nxvw::nxvw_wt_near_skip(wt.w0) != 0,
                    nxvw::nxvw_wt_res_level(wt.w0), identity_corners[nxvw::nxvw_wt_eye(wt.w0)]);
            });
            d->order_ncopy[pass] = (uint32_t)(copyMid - beg);
        }
        auto skipMid = copyMid;
        if (fp.any_inter) {
            skipMid = std::stable_partition(copyMid, end, [&](uint32_t t) {
                if (sup && t < sup->size() && (*sup)[t]) return true;
                // [planar] A PLANAR tile is mode 5, so it lands on the CODED
                // side of this partition, and that is load-bearing beyond the
                // dispatch shape.
                //
                // [SYN] 13.12.3 step 3: under the atlas a CODED tile seeds its
                // atlas entry, and a planar tile is a coded tile -- in fact the
                // cheapest possible new patch for that store, since it needs no
                // reference and can seed an entry that has none
                // (docs/LOWPOLY-MODE.md 6).  The atlas decoder builds `acoded`
                // as the range of this order AFTER `order_nskip` and dispatches
                // MATGEN/WRITEBACK over it, so a planar tile seeds its entry by
                // construction here, and would silently stop doing so if it
                // were ever moved to the skip side.
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
    // Cleared here so a caller that reads the create diagnostic after a
    // SUCCESSFUL create does not see the last failure of a previous one.
    set_create_err("no error");
    if (!ci || !out)
        return createerr(NXVC_VKD_ERR_ARG,
                         "nxvc_vk_decoder_create: %s must not be NULL",
                         !ci ? "create_info" : "out");
    *out = nullptr;
    D *d = new (std::nothrow) D();
    if (!d)
        return createerr(NXVC_VKD_ERR_NOMEM,
                         "nxvc_vk_decoder_create: out of memory allocating the "
                         "decoder");
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
            return createerr(NXVC_VKD_ERR_ARG,
                             "nxvc_vk_decoder_create: adopting a device needs "
                             "all five handles; %s is NULL",
                             !d->phys ? "physical_device" : "queue");
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
    if (VkResult r = vkCreateCommandPool(d->dev, &cp, nullptr, &d->pool)) {
        *out = d;
        return seterr(d, NXVC_VKD_ERR_VULKAN,
                      "vkCreateCommandPool failed: %s (%d)", vkresult_name(r),
                      (int)r);
    }
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = d->pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (VkResult r = vkAllocateCommandBuffers(d->dev, &cb, &d->cmd)) {
        *out = d;
        return seterr(d, NXVC_VKD_ERR_VULKAN,
                      "vkAllocateCommandBuffers failed: %s (%d)",
                      vkresult_name(r), (int)r);
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
        // 0-3 frame/PassA/PassB/end, 4-5 Pass W, 6-11 the three Pass B
        // module segments of eye pass 0.  The segment pair is what says which
        // MODULE a frame's Pass B time is in, which a single Pass B number
        // cannot: at 81 % WARP_SKIP the skip module and the coded modules are
        // both in it.  Written only for pass 0; a stereo frame's second eye is
        // the same three modules over the other half of the tiles.
        // [passb] 14: four Pass B segments (queries 6..13) rather than three.
        qp.queryCount = kQueryCount;
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
        if (d->smBSkipStore)
            vkDestroyShaderModule(d->dev, d->smBSkipStore, nullptr);
        if (d->smBCopy) vkDestroyShaderModule(d->dev, d->smBCopy, nullptr);
        for (auto &kv : d->pipesA) vkDestroyPipeline(d->dev, kv.second, nullptr);
        for (auto &kv : d->pipesB) vkDestroyPipeline(d->dev, kv.second, nullptr);
        if (d->smA) vkDestroyShaderModule(d->dev, d->smA, nullptr);
        if (d->smALite) vkDestroyShaderModule(d->dev, d->smALite, nullptr);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                if (d->smB[i][j])
                    vkDestroyShaderModule(d->dev, d->smB[i][j], nullptr);
        if (d->pipeW) vkDestroyPipeline(d->dev, d->pipeW, nullptr);
        if (d->smW) vkDestroyShaderModule(d->dev, d->smW, nullptr);
        // [ATLAS]
        if (d->pipeAV8) vkDestroyPipeline(d->dev, d->pipeAV8, nullptr);
        if (d->pipeAV16) vkDestroyPipeline(d->dev, d->pipeAV16, nullptr);
        if (d->smAV8) vkDestroyShaderModule(d->dev, d->smAV8, nullptr);
        if (d->smAV16) vkDestroyShaderModule(d->dev, d->smAV16, nullptr);
        if (d->plAV) vkDestroyPipelineLayout(d->dev, d->plAV, nullptr);
        if (d->pipeAC) vkDestroyPipeline(d->dev, d->pipeAC, nullptr);
        if (d->pipeAT) vkDestroyPipeline(d->dev, d->pipeAT, nullptr);
        if (d->smAC) vkDestroyShaderModule(d->dev, d->smAC, nullptr);
        if (d->smAT) vkDestroyShaderModule(d->dev, d->smAT, nullptr);
        if (d->plA) vkDestroyPipelineLayout(d->dev, d->plA, nullptr);
        if (d->plB) vkDestroyPipelineLayout(d->dev, d->plB, nullptr);
        if (d->plW) vkDestroyPipelineLayout(d->dev, d->plW, nullptr);
        if (d->plAC) vkDestroyPipelineLayout(d->dev, d->plAC, nullptr);
        if (d->plAT) vkDestroyPipelineLayout(d->dev, d->plAT, nullptr);
        if (d->dpool) vkDestroyDescriptorPool(d->dev, d->dpool, nullptr);
        if (d->dslA) vkDestroyDescriptorSetLayout(d->dev, d->dslA, nullptr);
        if (d->dslB) vkDestroyDescriptorSetLayout(d->dev, d->dslB, nullptr);
        if (d->dslW) vkDestroyDescriptorSetLayout(d->dev, d->dslW, nullptr);
        if (d->dslAC) vkDestroyDescriptorSetLayout(d->dev, d->dslAC, nullptr);
        if (d->dslAT) vkDestroyDescriptorSetLayout(d->dev, d->dslAT, nullptr);
        if (d->dslAV) vkDestroyDescriptorSetLayout(d->dev, d->dslAV, nullptr);
        if (d->binsem) vkDestroySemaphore(d->dev, d->binsem, nullptr);
        if (d->queries) vkDestroyQueryPool(d->dev, d->queries, nullptr);
        if (d->timeline) vkDestroySemaphore(d->dev, d->timeline, nullptr);
        if (d->fence) vkDestroyFence(d->dev, d->fence, nullptr);
        if (d->pool) vkDestroyCommandPool(d->dev, d->pool, nullptr);
        for (Buf *b : {&d->staging, &d->bBits, &d->bDesc, &d->bTables,
                       &d->bCoef, &d->bCbf, &d->bStatus, &d->bRecs, &d->bWgt,
                       &d->bModes, &d->bOrder, &d->bRead, &d->bULen,
                       &d->bULenHost, &d->bRing, &d->bWPred, &d->bWarp,
                       &d->bTable, &d->bAdv, &d->bHRing, &d->bASel,
                       &d->bACoded, &d->bAStatus, &d->bPlanar})
            destroy_buf(d, *b);
        for (Img *i : {&d->imgRgba, &d->imgRgb10, &d->imgLuma, &d->imgCbCr,
                       &d->imgRgbaN, &d->imgLumaN, &d->imgCbCrN,
                       &d->imgViewY, &d->imgViewC, &d->imgViewCr})
            destroy_img(d, *i);
        if (d->own_device) vkDestroyDevice(d->dev, nullptr);
    }
    if (d->own_instance && d->inst) vkDestroyInstance(d->inst, nullptr);
    delete d;
}

extern "C" uint64_t nxvc_vk_decoder_tools_supported(void) {
    return nxvcvk::tools_supported();
}

extern "C" uint64_t nxvc_vk_decoder_tools_for(uint32_t vendor_id,
                                             const char *device_name) {
    // The same table probe_device() uses, so a handshake answered before the
    // decoder exists cannot disagree with the decoder that is created later.
    return nxvcvk::tools_supported_for(vendor_id, device_name);
}

extern "C" uint64_t nxvc_vk_decoder_tools(const nxvc_vk_decoder *d) {
    // The build-wide mask when there is no decoder to ask; a device may accept
    // less, and this is the number a handshake must use.
    return d && d->tools_mask ? d->tools_mask : nxvcvk::tools_supported();
}

extern "C" const char *nxvc_vk_decoder_last_error(const nxvc_vk_decoder *d) {
    return d ? d->err.c_str() : "null decoder";
}

extern "C" const char *nxvc_vk_decoder_last_create_error(void) {
    return create_err_buf();
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
    // [ATLAS] The one switch, and it is set BEFORE make_resources() because
    // the allocation differs: the atlas is ONE ring slot, not four, and the
    // table and the H ring only exist for a stream that asked for them.
    //
    // Tool bit 31 is not in `kToolsSupported` yet, so `parse_stream_header`
    // has already refused any stream that sets it and this is false for every
    // stream that reaches here.  The bit joins the mask in the commit that
    // makes the decode path honour it; wiring the resources first keeps that
    // commit to the path itself.
    d->atlas_mode = (d->si.tools & (1ull << 31)) != 0;
    // [SYN] 13.12.11: tool bit 34 says the stream may switch modes per frame,
    // which is what makes the second ring slot necessary.
    d->atlas_modes = d->atlas_mode && (d->si.tools & (1ull << 34)) != 0;
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
// [stats] The device-side atlas counters, read on the same schedule as the
// timestamps: when the frame has COMPLETED, never by stalling for it.  An
// async client -- which is every real compositor -- would otherwise get either
// a stall or the previous frame's number with no way to tell which.
static void collect_atlas_stats(D *d) {
    if (!d->astats_pending) return;
    d->astats_pending = false;
    if (!d->atlas_mode || !d->bAStatus.mapped) return;
    const uint32_t *as = (const uint32_t *)d->bAStatus.mapped;
    // Word 2 is what survived the advance, word 3 what the write-back newly
    // validated.  On a PICTURE frame neither dispatch runs over every entry
    // and 13.12.11 step 3 validates all of them, so the host knows the answer
    // exactly without reading anything.
    d->stats.atlas_entries_valid =
        d->stats.frame_mode == 2u ? d->si.tile_count : as[2] + as[3];
    if (d->stats.atlas_entries_valid > d->si.tile_count)
        d->stats.atlas_entries_valid = d->si.tile_count;
}

static void collect_timestamps(D *d) {
    if (!d->ts_pending) return;
    d->ts_pending = false;
    if (!d->have_timestamps || !d->queries) return;
    // [passb] 14, not 12: four Pass B segments occupy queries 6..13, and this
    // buffer is what vkGetQueryPoolResults is handed `sizeof ts` for.  At 12 it
    // was a 96-byte array asked to receive 112 bytes.
    uint64_t ts[kQueryCount] = {};
    static_assert(sizeof(ts) / sizeof(ts[0]) == kQueryCount,
                  "the readback buffer, the pool's queryCount and the "
                  "per-frame reset are one number; they have drifted twice");
    const uint32_t nq = d->ts_count;
    if (nq < 4) return;
    // The BASE queries -- 0-3 always, and 4-5 whenever the segment timers were
    // armed at all -- are written on every frame that reaches here, so they
    // are read as one contiguous range.
    const uint32_t nbase = nq > 6 ? 6u : nq;
    // BOUNDED, never WAIT_BIT.  This runs only after the frame's fence or
    // timeline has already signalled, so every result is expected to be
    // available immediately; `VK_QUERY_RESULT_WAIT_BIT` therefore buys nothing
    // and costs everything, because a driver that never marks a query
    // available turns a statistics read into an unkillable hang.
    //
    // That is not hypothetical: on the Adreno 650 every INTER frame wedged
    // here -- no GPU fault, no validation error, the process unkillable by
    // anything but SIGKILL -- while intra frames and the same streams with
    // timestamps disabled ran fine.  A measurement path must never be able to
    // hang a decode, so it polls briefly and gives up.
    auto poll = [&](uint32_t first, uint32_t count, uint64_t *dst) -> bool {
        for (int attempt = 0; attempt < 50; ++attempt) {
            const VkResult r = vkGetQueryPoolResults(
                d->dev, d->queries, first, count,
                sizeof(uint64_t) * count, dst, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);
            if (r == VK_SUCCESS) return true;
            if (r != VK_NOT_READY) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return false;   // ~10 ms; the frame is already complete, so this is a
                        // broken driver rather than a slow one
    };
    if (!poll(0, nbase, ts)) {
        d->ts_dropped++;
        return;
    }
    // The segment pairs, one at a time and ONLY the ones whose dispatch ran.
    // Waiting on a query that was never written is a hang, not an error, and
    // that is exactly how this wedged the Adreno.
    uint32_t segmask = d->ts_seg_mask;
    for (uint32_t g = 0; g < 4 && nq > 6; ++g) {
        if (!(segmask & (1u << g))) continue;
        const uint32_t q = 6u + 2u * g;
        if (!poll(q, 2, &ts[q])) segmask &= ~(1u << g);
    }
    // [timing] Mask to the bits the queue actually drives BEFORE subtracting,
    // and take the difference modulo that width so a counter wrap is a small
    // positive delta rather than a number near 2^64.  `ts_mask` is all-ones on
    // a 64-bit family, so this is the identity there.
    const uint64_t m = d->ts_mask;
    for (uint32_t i = 0; i < nq; ++i) ts[i] &= m;
    auto delta = [m](uint64_t b, uint64_t a) -> double {
        return (double)((b - a) & m);
    };
    const double k = (double)d->ts_period / 1e6;
    d->stats.pass_a_ms = delta(ts[1], ts[0]) * k;
    // [inter] Pass W sits inside the Pass A -> Pass B window, so the
    // reported Pass B is the predictor plus the reconstruction; the
    // predictor's own share is broken out rather than hidden.
    d->stats.pass_b_ms = delta(ts[2], ts[1]) * k;
    d->stats.gpu_ms = delta(ts[3], ts[0]) * k;
    // [passb] Zeroed rather than left holding the previous frame's value when
    // this frame did not measure them.  `ts_count` is 12 on an inter frame and
    // 4 otherwise, so on an intra frame neither Pass W nor the segment pairs
    // were written -- and the header has always PROMISED pass_w_ms is "0 on a
    // frame with no inter tile", which it was not: a HUD sampling an intra
    // frame read the last inter frame's warp time as this frame's.  An intra
    // frame's Pass B is one segment anyway, so pass_b_ms is already the
    // reconstruction there and the breakdown has nothing to add.
    if (nq >= 6) {
        d->stats.pass_w_ms = delta(ts[5], ts[4]) * k;
    } else {
        d->stats.pass_w_ms = 0;
    }
    // [passb] The three Pass B segments, broken out into the stats struct so a
    // caller can show A / W / B honestly instead of reading pass_b_ms as "the
    // reconstruction".  On a live inter stream the skip segment -- the warp of
    // the skipped tiles -- is most of that window, and nothing in the ABI said
    // so.  The env-gated print below stays: it is the same numbers, in a form
    // that needs no caller.
    if (nq >= 14) {
        // A segment whose pair was never written reports 0.000 -- which is
        // what it cost, and what the old always-write form reported too.  Its
        // tile count below says whether that 0 means "no tiles" or "the
        // device could not time it".
        auto segms = [&](uint32_t g) {
            return (segmask & (1u << g))
                       ? delta(ts[7 + 2 * g], ts[6 + 2 * g]) * k
                       : 0.0;
        };
        d->stats.pass_b_identity_ms = segms(0);
        d->stats.pass_b_skip_ms = segms(1);
        d->stats.pass_b_coded_ms = segms(2);
        d->stats.pass_b_dir_ms = segms(3);
    } else {
        d->stats.pass_b_identity_ms = 0;
        d->stats.pass_b_skip_ms = 0;
        d->stats.pass_b_coded_ms = 0;
        d->stats.pass_b_dir_ms = 0;
    }
    // [planar] The tile COUNTS are not timestamps: the partition that produces
    // them is built on the host, in build_tile_order(), and is the same
    // partition whether or not the device can time the segments.  They are
    // therefore reported unconditionally, where they used to be zeroed
    // alongside the times.
    //
    // That distinction is what makes the mode's cost legible. The segment
    // timers arm only on a frame with an inter tile (ts_count above), so a
    // planar-only frame measures no milliseconds -- but it does dispatch
    // planar tiles, and a caller reading "0 ms over 0 tiles" cannot tell that
    // from a frame that had none.  "0 ms over 96 tiles" says which it was, and
    // matches how the client already reads the counts: a segment with tiles
    // and no time is one the device could not measure.
    //
    // A planar tile is none of identity-copy, WARP_SKIP or INTRA -- it is mode
    // 5, it carries its own picture and it has no warp matrix at all -- so
    // build_tile_order puts it in the CODED group and it is counted in
    // `tiles_coded_seg`, which is exactly what <nxvc/nxvc_vk.h> defines that
    // field as: "every other non-INTRA tile".
    d->stats.tiles_identity_seg = d->seg_tiles[0];
    d->stats.tiles_skip_seg = d->seg_tiles[1];
    d->stats.tiles_coded_seg = d->seg_tiles[2];
    d->stats.tiles_dir_seg = d->seg_tiles[3];
    // [ATLAS stats] The same number as `tiles_skip_seg` under a name that says
    // what it MEANS, and it belongs HERE with the other counts rather than at
    // submit time: the segment populations are only known once the frame has
    // been recorded, so assigning it earlier published the PREVIOUS frame's
    // count under this frame's other figures.
    //
    // Segment 1, not 0 -- the identity/copy partition took index 0 when it
    // landed, and an alias that kept indexing 0 would silently report the COPY
    // population as the warped one.
    //
    // Under an ATLAS frame it is 0: a skipped tile is not reconstructed at
    // all, which is the deletion the whole ADR exists for.
    d->stats.tiles_warped_skip =
        d->stats.frame_mode == 1u ? 0u : d->seg_tiles[1];
    // [inter] Per-module Pass B, eye pass 0.  Env-gated because it is a
    // measurement aid rather than part of the ABI, and because a segment that
    // did not run leaves its pair equal and would otherwise print 0.000 three
    // times on an intra frame.
    if (nq >= 14 && std::getenv("NXVC_VKD_SEG_MS")) {
        std::fprintf(stderr,
                     "[segms] copy %.4f  skip %.4f  coded %.4f  intra_dir %.4f"
                     "  (tiles %u/%u/%u/%u)\n",
                     d->stats.pass_b_identity_ms, d->stats.pass_b_skip_ms,
                     d->stats.pass_b_coded_ms, d->stats.pass_b_dir_ms,
                     d->seg_tiles[0], d->seg_tiles[1], d->seg_tiles[2],
                     d->seg_tiles[3]);
    }
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_wait(nxvc_vk_decoder *d,
                                                uint64_t timeout_ns) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (d->timeline_value == 0) return NXVC_VKD_OK;
    if (d->timeline == VK_NULL_HANDLE) {
        if (!d->fence_pending) { collect_timestamps(d); collect_atlas_stats(d);
                                 return NXVC_VKD_OK; }
        VkResult fr =
            vkWaitForFences(d->dev, 1, &d->fence, VK_TRUE, timeout_ns);
        if (fr == VK_TIMEOUT) return NXVC_VKD_ERR_INTERNAL;
        if (fr != VK_SUCCESS)
            return seterr(d, NXVC_VKD_ERR_VULKAN, "vkWaitForFences: %s (%d)",
                          vkresult_name(fr), (int)fr);
        d->fence_pending = false;
        collect_timestamps(d);
    collect_atlas_stats(d);
        collect_atlas_stats(d);
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
        return seterr(d, NXVC_VKD_ERR_VULKAN, "vkWaitSemaphores: %s (%d)", vkresult_name(r),
                      (int)r);
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
    if (m->astats_pending && frame_complete(m)) collect_atlas_stats(m);
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
    // A refusal names the constraint and the tile, not just "malformed": the
    // parser records both (nxvcvk::last_parse_reject).
    if (st == NXVC_VKD_ERR_BITSTREAM)
        return seterr(d, st, "frame: %s -- %s",
                      nxvc_vk_decoder_status_string(st),
                      nxvcvk::last_parse_reject_text());
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
    // [SYN] 13.12.11.  A PICTURE frame is decoded by the ORDINARY non-ATLAS
    // process against a reference assembled from the atlas; an ATLAS frame is
    // 13.12 as written.  Everything below branches on these two rather than on
    // `atlas_mode`, because half of the ATLAS path must NOT run on a PICTURE
    // frame -- no advance, no MATGEN, no write-back, and the skip module comes
    // back on, because a PICTURE frame really does reconstruct every tile.
    d->picture_frame = d->atlas_mode && fp.picture_frame != 0;
    const bool picture = d->picture_frame;
    const bool atlas_frame = d->atlas_mode && !picture;
    // [inter] The Pass W parameter block: the ring geometry and the four
    // conjugated matrices, then one record per tile.
    build_warp_params(d, fp, ntiles);
    const VkDeviceSize warpBytes = (VkDeviceSize)d->warp_words.size() * 4;
    d->offWarp = o;
    o = align_up(o + warpBytes, 256);
    // [planar] Only staged when the frame carries a planar tile; every other
    // frame moves nothing and leaves the buffer as it was, which no tile
    // reads.
    const VkDeviceSize planarBytes =
        fp.any_planar ? (VkDeviceSize)fp.planar.size() * 4 : 0;
    d->offPlanar = o;
    o = align_up(o + planarBytes, 256);
    // ---- [ATLAS] this frame's H slot, and the two index lists.
    //
    // The lists are built from `build_tile_order()`'s partition rather than
    // from a second walk of the tile records: the leading `order_nskip` range
    // of each eye's segment IS the frame's skipped tiles, so what follows it
    // is exactly the coded ones, already in dispatch order.  Under [SYN]
    // 13.12 a skipped tile writes NOTHING to the atlas, so that range is
    // simply never dispatched -- which is the whole of "Pass A and Pass B over
    // coded tiles only" and needs no parser change at all.
    VkDeviceSize hringBytes = 0, aselBytes = 0, acodedBytes = 0;
    if (atlas_frame) {
        build_tile_order(d, fp, ntiles);
        const uint32_t passes = fp.any_stereo_tile ? d->si.eyes : 1u;
        const uint32_t per = ntiles / passes;
        d->acoded.clear();
        for (uint32_t pass = 0; pass < passes; ++pass) {
            const uint32_t nskip = fp.any_inter ? d->order_nskip[pass] : 0u;
            for (uint32_t i = nskip; i < per; ++i)
                d->acoded.push_back(d->order[(size_t)pass * per + i]);
        }
        // Which entries must be advanced before this frame's tiles read them.
        // The frame-complete path advances EVERY entry -- that is the eager
        // form of 13.12.3 and what the flushed atlas is defined as -- so the
        // selection list is only built for the tile-run path.  Here the
        // dispatch is SEL_ALL and covers every entry of both eyes in one go.
        d->asel.clear();
        // [SYN] 13.12.6, and it applies to a frame's OWN CODED TILES and not
        // only to base patches: a tile of frame N at a position whose
        // `src_frame` is already >= N has been overtaken and is DROPPED.  That
        // is the ordinary case once a base patch is in play -- 13.12.9 lets a
        // patch carry a `src_frame` ahead of the stream, and v88 pins exactly
        // it -- and without this the write-back reset a position the patch had
        // already claimed, throwing away the newer generation.
        //
        // `apply()` runs HERE, before anything is dispatched, which is the
        // ordering `vk.atlas.state` pins: a coded tile is tested against the
        // PREVIOUS state, never against a write this same frame makes.
        {
            nxvw::AtlasApply aa = d->astate.apply(
                d->acoded.data(), (uint32_t)d->acoded.size(), fp.frame_number);
            d->stats.tiles_superseded = (uint32_t)aa.superseded.size();
            d->acoded.swap(aa.accepted);
            if (!aa.superseded.empty()) {
                // Re-partition with the superseded tiles alongside the
                // skipped ones, so neither Pass W nor Pass B touches them, and
                // rebuild the coded list from the order that results.
                std::vector<uint8_t> sup(ntiles, 0u);
                for (uint32_t t : aa.superseded)
                    if (t < ntiles) sup[t] = 1u;
                build_tile_order(d, fp, ntiles, &sup);
                d->acoded.clear();
                for (uint32_t pass = 0; pass < passes; ++pass) {
                    const uint32_t ns = d->order_nskip[pass];
                    for (uint32_t i = ns; i < per; ++i)
                        d->acoded.push_back(d->order[(size_t)pass * per + i]);
                }
            }
        }
        hringBytes = (VkDeviceSize)d->si.eyes * NXVW_ATLAS_HSLOT_UINTS * 4;
        acodedBytes = (VkDeviceSize)d->acoded.size() * 4;
    }
    // [SYN] 13.12.11 step 1.  The ASSEMBLE pass writes slot 1 and reads slot
    // 0, so it needs its own copy of the warp HEADER -- the records come from
    // the kernel itself.  Only the header differs, and only in `curSlot`.
    const VkDeviceSize offWarpA = o;
    const VkDeviceSize warpABytes =
        picture ? (VkDeviceSize)NXVW_WARP_HDR_UINTS * 4 : 0;
    o = align_up(o + warpABytes, 256);
    const VkDeviceSize offHRing = o;
    o = align_up(o + hringBytes, 256);
    const VkDeviceSize offACoded = o;
    o = align_up(o + acodedBytes, 256);
    (void)aselBytes;
    if ((st = make_buf(d, d->staging, o, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       true)))
        return st;
    uint8_t *sp = (uint8_t *)d->staging.mapped;
    std::memcpy(sp + d->offBits, bytes, fp.frame_bytes);
    if (descBytes) std::memcpy(sp + d->offDesc, fp.desc.data(), descBytes);
    std::memcpy(sp + d->offTables, fp.cum.data(), tabBytes);
    std::memcpy(sp + d->offRecs, fp.recs.data(), recBytes);
    std::memcpy(sp + d->offWgt, fp.weights, sizeof fp.weights);
    // Already built above under ATLAS, where the coded list is derived from
    // it; building it twice would be harmless and is still wrong to write.
    if (!atlas_frame) build_tile_order(d, fp, ntiles);
    std::memcpy(sp + d->offOrder, d->order.data(), (size_t)ntiles * 4);
    if (warpBytes)
        std::memcpy(sp + d->offWarp, d->warp_words.data(), (size_t)warpBytes);
    if (planarBytes)
        std::memcpy(sp + d->offPlanar, fp.planar.data(), (size_t)planarBytes);
    if (warpABytes) {
        std::memcpy(sp + offWarpA, d->warp_words.data(), (size_t)warpABytes);
        // The one field that differs: the ASSEMBLE stores into slot 1.
        ((uint32_t *)(sp + offWarpA))[NXVW_WARP_HDR_RING + 3] = 1u;
    }
    if (atlas_frame) {
        uint32_t hslot[2 * NXVW_ATLAS_HSLOT_UINTS] = {};
        build_h_slot(d, fp, fp.frame_number, hslot);
        std::memcpy(sp + offHRing, hslot, (size_t)hringBytes);
        if (acodedBytes)
            std::memcpy(sp + offACoded, d->acoded.data(),
                        (size_t)acodedBytes);
    }

    // ---- 3. record ----------------------------------------------------
    VKTRY(d, vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKTRY(d, vkBeginCommandBuffer(d->cmd, &bi));
    if (d->have_timestamps) {
        // The WHOLE pool, every frame.  Resetting fewer than were created
        // leaves the tail permanently unavailable, and a WAIT_BIT read that
        // covers it never returns.
        vkCmdResetQueryPool(d->cmd, d->queries, 0, kQueryCount);
        d->ts_seg_mask = 0u;
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
    copy(d->bPlanar, d->offPlanar, planarBytes);
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

    // ---- [ATLAS] 13.12.3 step 1: compose and renorm --------------------
    // ONE dispatch per frame over EVERY entry of BOTH eyes.  289 entries an
    // eye is already in the starved region of the workgroup-count curve
    // (passA/README.md) and splitting it per eye would halve the occupancy of
    // each half for no gain: the thread reads its own eye out of its index.
    if (atlas_frame) {
        // [SYN] 13.12.1: on `tile_map_reset` the whole table is zeroed, which
        // makes every entry invalid, and the atlas pixels are undefined until
        // written.  `advanced_to` is filled with THIS frame rather than zero:
        // it is not part of the atlas, and leaving it at zero would make the
        // kernel try to walk an invalid entry forward by `frame_number`
        // steps.  An invalid entry costs one early return either way, but the
        // fill is what keeps that true after the first coded tile lands.
        if (fp.flags & 1u) {
            vkCmdFillBuffer(d->cmd, d->bTable.buf, 0, d->bTable.size, 0u);
            vkCmdFillBuffer(d->cmd, d->bAdv.buf, 0, d->bAdv.size,
                            fp.frame_number);
            d->astate.reset(ntiles);
            d->astate.mark_all_advanced(fp.frame_number);
        }
        // MATGEN's deferred 13.12.4 refusal starts clear every frame.
        vkCmdFillBuffer(d->cmd, d->bAStatus.buf, 0, d->bAStatus.size, 0u);
        if (hringBytes) {
            VkBufferCopy c{offHRing,
                           (VkDeviceSize)(fp.frame_number % NXVW_ATLAS_HRING) *
                               d->si.eyes * NXVW_ATLAS_HSLOT_UINTS * 4,
                           hringBytes};
            vkCmdCopyBuffer(d->cmd, d->staging.buf, d->bHRing.buf, 1, &c);
        }
        if (acodedBytes) {
            VkBufferCopy c{offACoded, 0, acodedBytes};
            vkCmdCopyBuffer(d->cmd, d->staging.buf, d->bACoded.buf, 1, &c);
        }
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        d->astate.record_frame(fp.frame_number);

        nxvw::NxvwAtlasPush ap{};
        ap.entryCount = ntiles;
        ap.colsPerEye = d->si.tiles_x;
        ap.eyes = d->si.eyes;
        ap.lumaW = (int)d->si.width;
        ap.lumaH = (int)d->si.height;
        ap.genMax = 0u;   // the reference DECODER passes 0: no cap
        ap.targetFrame = fp.frame_number;
        // The frame-complete path advances EVERY entry, which is exactly the
        // eager form of 13.12.3 -- and the flushed atlas the normative output
        // is defined as.  The tile-run path is what uses SEL_LIST.
        ap.sel = NXVW_ATLAS_SEL_ALL;
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d->plAC, 0, 1, &d->dsetAC, 0, nullptr);
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeAC);
        vkCmdPushConstants(d->cmd, d->plAC, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           (uint32_t)sizeof ap, &ap);
        vkCmdDispatch(d->cmd, (ntiles + 63u) / 64u, 1, 1);
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        d->astate.mark_all_advanced(fp.frame_number);
    }

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
    VkPipeline pipeBCopy = VK_NULL_HANDLE;
    // [ATLAS] `reconstruct_skip_store` does not run at all: under [SYN] 13.12
    // a skipped tile is NOT reconstructed -- it stays in the atlas at the
    // generation that last coded it and the display warp reaches it there.
    // This is the deletion the whole ADR is for: 8.889 of Pass B's 10.760 ms
    // at 289 tiles was that module over ~250 skipped tiles.  The skip RANGE
    // still exists in `order`; it is simply never dispatched, by Pass W or by
    // Pass B.
    // A PICTURE frame reconstructs every tile, skipped ones included, so the
    // module comes back: it is only an ATLAS frame that leaves them untouched.
    const bool anySkip = !atlas_frame &&
                         (d->order_nskip[0] != 0 || d->order_nskip[1] != 0);
    if (anySkip && !d->need_alpha_pass &&
        (st = pipeline_b(d, d->out_format,
                         fuse ? (int32_t)nxvw::kOutRgba8
                              : (int32_t)nxvw::kOutNone,
                         fp.push.sparse, storeWords, 0, (int32_t)fp.split4, 0,
                         inter_pred_on(d, interStream),
                         ring_store_on(d, interStream), &pipeBSkip, 2)))
        return st;

    // [passb] The copy module, built only when a tile actually qualifies.  On
    // an encoder that never emits an exactly-identity pose this is never
    // created, so the path costs nothing it is not used for.
    // [ATLAS] Not on an atlas frame: a copy is a reconstruction, and an atlas
    // frame leaves its skipped tiles where they are.
    const bool anyCopy = !atlas_frame &&
                         (d->order_ncopy[0] != 0 || d->order_ncopy[1] != 0);
    if (anyCopy && !d->need_alpha_pass &&
        (st = pipeline_b(d, d->out_format,
                         fuse ? (int32_t)nxvw::kOutRgba8
                              : (int32_t)nxvw::kOutNone,
                         fp.push.sparse, storeWords, 0, (int32_t)fp.split4, 0,
                         inter_pred_on(d, interStream),
                         ring_store_on(d, interStream), &pipeBCopy, 3)))
        return st;

    VkPipeline pipeWp = VK_NULL_HANDLE;
    if (fp.any_inter && (st = pipeline_w(d, &pipeWp))) return st;

    // [ATLAS] The two ops of the coded-tile kernel.  MATGEN runs BEFORE Pass W
    // and reads each entry's `C` as it stands after the advance ([SYN] 13.12.4,
    // "read after step 1"), conjugates it for sub 1 and sub 2 and writes the
    // pair at the tile record's `mat_idx`.  WRITEBACK runs after Pass B has
    // stored the pixels and applies 13.12.3 step 3.  They are two ops of one
    // kernel because they share every buffer and every index derivation.
    auto atlas_tile_op = [&](uint32_t op) {
        if (!d->atlas_mode) return;
        // MATERIALISE covers every ENTRY; the coded-tile ops walk the list.
        const bool all = op == NXVW_ATLAS_OP_MATERIALISE;
        if (!all && (!atlas_frame || d->acoded.empty())) return;
        if (all && !picture) return;
        nxvw::NxvwAtlasTilePush tp{};
        tp.tileCount = all ? ntiles : (uint32_t)d->acoded.size();
        tp.op = op;
        tp.frame = fp.frame_number;
        tp.colsPerEye = d->si.tiles_x;
        tp.lumaW = (int)d->si.width;
        tp.lumaH = (int)d->si.height;
        tp.chromaW = (int)d->si.cw;
        tp.chromaH = (int)d->si.ch;
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d->plAT, 0, 1, &d->dsetAT, 0, nullptr);
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeAT);
        vkCmdPushConstants(d->cmd, d->plAT, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           (uint32_t)sizeof tp, &tp);
        vkCmdDispatch(d->cmd, (tp.tileCount + 63u) / 64u, 1, 1);
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        // Pass B's set has to come back: this kernel bound its own.
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d->plB, 0, 1, &d->dsetB, 0, nullptr);
        vkCmdPushConstants(d->cmd, d->plB, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           (uint32_t)sizeof(nxvw::NxvwPassBPush), &fp.push);
    };
    // ---- [SYN] 13.12.11 step 1: ASSEMBLE ------------------------------
    // The picture the atlas holds, at the pose it holds it at -- frame N-1's
    // pose, because the advance has NOT run on a PICTURE frame.  Built into
    // ring slot 1, which the ordinary path below then predicts from; from
    // there down nothing is atlas-aware.
    //
    // It runs on the UNMODIFIED predictor and the unmodified WARP_SKIP store.
    // The ASSEMBLE op writes each entry's Pass W record -- mode from the
    // entry's own `static`, zero vector, and `refBase = 0xffffffff` when the
    // entry is INVALID, which warp_pred.glsl already answers with the correct
    // per-plane mid-grey.  That is 13.12.5's rule for an invalid entry, and it
    // is what lets validity stay DEVICE-side: no readback, no stall.
    if (picture) {
        VkPipeline pipeAsm = VK_NULL_HANDLE;
        if ((st = pipeline_b(d, (uint32_t)nxvw::kOutNone,
                             (int32_t)nxvw::kOutNone, fp.push.sparse,
                             storeWords, 0, (int32_t)fp.split4, 0,
                             inter_pred_on(d, interStream),
                             ring_store_on(d, interStream), &pipeAsm, 2)))
            return st;
        // The assemble header: identical but for `curSlot`, which is 1.
        {
            VkBufferCopy c{offWarpA, 0, warpABytes};
            vkCmdCopyBuffer(d->cmd, d->staging.buf, d->bWarp.buf, 1, &c);
        }
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        nxvw::NxvwAtlasTilePush ap{};
        ap.tileCount = ntiles;
        ap.op = NXVW_ATLAS_OP_ASSEMBLE;
        ap.frame = fp.frame_number;
        ap.colsPerEye = d->si.tiles_x;
        ap.lumaW = (int)d->si.width;
        ap.lumaH = (int)d->si.height;
        ap.chromaW = (int)d->si.cw;
        ap.chromaH = (int)d->si.ch;
        ap.refBase = 0u;   // the atlas is slot 0
        ap.eyes = d->si.eyes;
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d->plAT, 0, 1, &d->dsetAT, 0, nullptr);
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeAT);
        vkCmdPushConstants(d->cmd, d->plAT, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           (uint32_t)sizeof ap, &ap);
        vkCmdDispatch(d->cmd, (ntiles + 63u) / 64u, 1, 1);
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        // One dispatch of the skip-store module over EVERY tile: it runs the
        // predictor itself rather than reading back what Pass W wrote, so the
        // assemble needs no Pass W of its own.
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->plB,
                                0, 1, &d->dsetB, 0, nullptr);
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeAsm);
        vkCmdPushConstants(d->cmd, d->plB, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           (uint32_t)sizeof(nxvw::NxvwPassBPush), &fp.push);
        vkCmdDispatchBase(d->cmd, 0, 0, 0, ntiles, 1, 1);
        ++dispatches;
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        // ASSEMBLE overwrote every tile RECORD, so the frame's own records --
        // and the header's destination slot -- have to be put back before the
        // ordinary path reads them.
        {
            VkBufferCopy c{d->offWarp, 0,
                           (VkDeviceSize)d->warp_words.size() * 4};
            vkCmdCopyBuffer(d->cmd, d->staging.buf, d->bWarp.buf, 1, &c);
        }
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
    }

    atlas_tile_op(NXVW_ATLAS_OP_MATGEN);

    for (uint32_t pass = 0; pass < eyePasses; ++pass) {
        const uint32_t base = pass * tilesPerEye;
        // [inter] How many of this eye's tiles the WARP_SKIP module takes.
        // It is computed HERE, before Pass W, because both passes have to
        // agree: whatever Pass B's skip module does not take, Pass W must
        // still predict.  A frame with skip tiles but no skip PIPELINE -- the
        // alpha second-store configuration, which that module deliberately
        // does not carry -- folds this to zero, and then Pass W covers
        // everything exactly as it did before the split.
        // [ATLAS] There is no skip PIPELINE and the skipped tiles must still
        // be excluded, which is the opposite of the alpha case below: there,
        // folding `nskip` to zero puts the skips back on a module that CAN
        // reconstruct them, and here they must not be reconstructed at all.
        // So the range is kept and segment 0 is dispatched on nothing.
        const uint32_t nskip =
            (pipeBSkip != VK_NULL_HANDLE || atlas_frame)
                ? d->order_nskip[pass]
                : 0u;
        if (fp.any_inter) {
            if (d->have_timestamps && pass == 0 && d->ts_limit > 4)
                vkCmdWriteTimestamp(d->cmd,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    d->queries, 4);
            wpush.eyeFilter = fp.any_stereo_tile ? (int)pass : -1;
            vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    d->plW, 0, 1, &d->dsetW, 0, nullptr);
            vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeWp);
            vkCmdPushConstants(d->cmd, d->plW, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               (uint32_t)sizeof(nxvw::NxvwWarpPush), &wpush);
            // [inter] Over this eye's CODED tiles only.  The skip range leads
            // each eye's segment of the order buffer, and the module that
            // takes it now runs the predictor itself -- so predicting those
            // tiles here as well would write a WPred slot nothing ever reads.
            // That is the saving: 12.3 KB stored and 12.3 KB loaded per
            // skipped tile, both gone.
            //
            // The range is one eye's, so `eyeFilter` no longer has anything
            // to reject; it stays set because it is also what keeps a STEREO
            // tile in the pass that has its reference.
            if (nskip < tilesPerEye) {
                vkCmdDispatchBase(d->cmd, base + nskip, 0, 0,
                                  tilesPerEye - nskip, 1, 1);
                ++dispatches;
            }
            if (d->have_timestamps && pass == 0 && d->ts_limit > 4)
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
        // already inside [0, nodir), and folding `nskip` to zero above puts
        // them on the module that would have had them before this split.
        const uint32_t nodir =
            fp.push.intraDir != 0 ? d->order_nodir[pass] : tilesPerEye;
        // [passb] The copy range leads, so four segments.  It collapses to the
        // previous three the moment no tile qualifies, which is every frame on
        // an encoder that does not snap a near-identity pose to the identity.
        //
        // [ATLAS] An atlas frame reconstructs no skipped tile at all, and a
        // copy IS a reconstruction -- the tile is meant to stay in the atlas
        // at the generation that last coded it.  So the copy range is
        // dispatched over ZERO tiles there for exactly the reason the skip
        // range is, while `segBase` keeps using `ncopy` and `nskip` so the
        // later segments still start where their tiles do.
        // `ncopy` is what the copy module ACTUALLY takes, which is not the
        // same as how many tiles qualified.  The skip segment starts where
        // the copy module stopped, so the two degrade correctly and
        // differently:
        //   atlas frame          -- neither runs; the qualifying tiles are
        //                           left in the atlas, which is the point;
        //   no copy pipeline     -- ncopy folds to 0 and the skip segment
        //     (the alpha            starts at `base` and covers them, so they
        //      configuration)       are reconstructed the way they were before
        //                           this split existed.
        const uint32_t ncopy =
            (!atlas_frame && pipeBCopy != VK_NULL_HANDLE) ? d->order_ncopy[pass]
                                                          : 0u;
        const uint32_t seg[4] = {ncopy, atlas_frame ? 0u : nskip - ncopy,
                                 nodir - nskip, tilesPerEye - nodir};
        const uint32_t segBase[4] = {base, base + ncopy, base + nskip,
                                     base + nodir};
        VkPipeline segPipe[4] = {pipeBCopy, pipeBSkip, pipeB[0], pipeB[1]};
        VkPipeline segPipeA[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, pipeBa[0],
                                  pipeBa[1]};
        for (int g = 0; g < 4; ++g) {
            if (pass == 0) d->seg_tiles[g] = seg[g];
            // A segment's timestamp pair is written ONLY when its dispatch
            // actually runs.
            //
            // It used to write both ends for an EMPTY segment too, so that a
            // WAIT_BIT readback over a contiguous range never waited on an
            // unwritten query.  That reasoning is sound about the readback and
            // wrong about the device: on the Adreno 650 a frame that arms the
            // segment timers and then writes a pair with NO DISPATCH BETWEEN
            // THEM wedges -- the fence never signals, with no GPU fault and no
            // validation error.  Every inter stream hung; intra, which arms
            // only four queries and no segment pairs, did not.  Both desktop
            // ICDs accept it, so nothing caught it until the device ran.
            //
            // The readback's constraint is met instead by telling it WHICH
            // pairs exist (`ts_seg_mask`) and having it read only those, so
            // no query is ever waited on that was not written.  An empty
            // segment still reports 0.000 ms with its tile count beside it,
            // which is where that number always came from.
            const bool tsSeg =
                d->have_timestamps && pass == 0 && d->ts_limit > 6;
            if (seg[g] == 0) continue;
            if (tsSeg) {
                d->ts_seg_mask |= 1u << g;
                vkCmdWriteTimestamp(d->cmd,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    d->queries, 6 + 2 * (uint32_t)g);
            }
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
            if (tsSeg)
                vkCmdWriteTimestamp(d->cmd,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    d->queries, 7 + 2 * (uint32_t)g);
        }
        if (pass + 1 < eyePasses)
            buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT);
    }
    // [ATLAS] 13.12.3 step 3, after every eye pass has stored its pixels:
    // C := identity, src_frame := N, gen := 0, static := (mode == STATIC_MV),
    // valid := 1, res_level, and `advanced_to := N` beside it.  It runs after
    // the loop and not inside it because a STEREO tile of eye 1 predicts from
    // eye 0 of THIS frame, so eye 0's entries must not be reset until both
    // passes have read them.
    atlas_tile_op(NXVW_ATLAS_OP_WRITEBACK);
    // [SYN] 13.12.11 step 3: the reconstruction becomes the atlas, at EVERY
    // position.  The host mirror is `rebase_picture()` for the positions this
    // frame did not code plus `commit()` for the ones it did -- in that order,
    // and after `apply()`, which is what keeps a PICTURE frame's own coded
    // tiles from superseding themselves.
    // ---- [ATLAS] the display view (13.12.5), after the atlas is final -----
    // Non-normative, and produced BESIDE the atlas rather than instead of it:
    // the u16 layout stays what Pass W reads and what conformance compares.
    // It covers the whole picture rather than the frame's coded tiles only --
    // correctness first; the coded-only form is the optimisation the device
    // pricing is for, and it is a dispatch bound, not a semantic change.
    if (d->atlas_view) {
        image_to_general(d->cmd, d->imgViewY.img);
        image_to_general(d->cmd, d->imgViewC.img);
        image_to_general(d->cmd, d->imgViewCr.img);
        buffer_barrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        int32_t vp[10] = {d->ringOff[0],    d->ringOff[1],
                          d->ringOff[2],    d->ringStride[0],
                          d->ringStride[1], d->ringStride[2],
                          (int32_t)(d->si.width * d->si.eyes),
                          (int32_t)d->si.height,
                          (int32_t)(d->si.cw * d->si.eyes),
                          (int32_t)d->si.ch};
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                d->plAV, 0, 1, &d->dsetAV, 0, nullptr);
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          d->atlas_view == (uint32_t)NXVC_VKD_ATLAS_VIEW_R8
                              ? d->pipeAV8
                              : d->pipeAV16);
        vkCmdPushConstants(d->cmd, d->plAV, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           (uint32_t)sizeof vp, vp);
        vkCmdDispatch(d->cmd, ((uint32_t)vp[6] + 7u) / 8u,
                      ((uint32_t)vp[7] + 7u) / 8u, 1);
        ++dispatches;
    }

    atlas_tile_op(NXVW_ATLAS_OP_MATERIALISE);
    if (atlas_frame)
        d->astate.commit(d->acoded.data(), (uint32_t)d->acoded.size(),
                         fp.frame_number);
    if (picture) {
        d->astate.rebase_picture(d->acoded.data(), (uint32_t)d->acoded.size(),
                                 fp.frame_number);
        d->astate.commit(d->acoded.data(), (uint32_t)d->acoded.size(),
                         fp.frame_number);
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
    d->stats.rows_elided = fp.rows_elided;
    // [ATLAS stats] What the WIRE said, not what the decoder chose: the tools
    // word decides whether this is an atlas stream at all and frame flags
    // bit 5 decides which of 13.12.11's two modes the frame took.
    d->stats.frame_mode = !d->atlas_mode ? 0u : (picture ? 2u : 1u);
    d->stats.tiles_assembled = picture ? ntiles : 0u;
    if (picture) ++d->stats.picture_frames;
    // Carried forward until the counters come back; the frame in flight has
    // not finished, so last frame's figure is the honest answer meanwhile.
    d->astats_pending = d->atlas_mode;
    d->last_frame = fp.frame_number;
    d->have_frame = true;
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

    d->ts_count = d->have_timestamps
                      ? (fp.any_inter ? (d->ts_limit < kQueryCount
                                             ? (d->ts_limit >= 6 ? 6u : 4u)
                                             : kQueryCount)
                                      : 4u)
                      : 0u;
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

    // [ATLAS] [SYN] 13.12.4: "a tile with mode != INTRA whose own atlas entry
    // has valid == 0 is BITSTREAM".  Only the DEVICE knows that -- validity is
    // decided by the envelope check inside the composition -- so MATGEN
    // records it in a status word and the refusal is made here, once the frame
    // has completed.  Deferred, not absent: the alternative is a readback per
    // frame, which is the one thing tile streaming exists to remove.
    //
    // Bit 0 is the refusal and bits 8-31 of the second word carry the FIRST
    // offending tile, so the report names a tile rather than a frame.
    if (atlas_frame && d->bAStatus.mapped) {
        const uint32_t *as = (const uint32_t *)d->bAStatus.mapped;
        if (as[0] & NXVW_ATLAS_STATUS_INVALID_REF)
            return seterr(d, NXVC_VKD_ERR_BITSTREAM,
                          "atlas: tile %u codes a non-INTRA mode against an "
                          "INVALID entry ([SYN] 13.12.4)",
                          as[1]);
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

// --------------------------------------------------- [ATLAS] atlas readback
// The atlas lives in device-local memory like every other decoder buffer, so
// reading it is a copy through a host-visible staging buffer and a wait.  It
// is a CONFORMANCE and inspection path, not a per-frame one: a client under
// ATLAS consumes the atlas on the GPU through the published handles and never
// calls these.
namespace {
nxvc_vkd_status atlas_readback(D *d, const Buf &src, VkDeviceSize bytes,
                               void *out) {
    nxvc_vkd_status st = nxvc_vk_decoder_wait(d, UINT64_MAX);
    if (st) return st;
    Buf host{};
    if ((st = make_buf(d, host, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)))
        return st;
    VKTRY(d, vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKTRY(d, vkBeginCommandBuffer(d->cmd, &bi));
    VkBufferCopy c{0, 0, bytes};
    vkCmdCopyBuffer(d->cmd, src.buf, host.buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    VKTRY(d, vkEndCommandBuffer(d->cmd));
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.commandBufferCount = 1;
    su.pCommandBuffers = &d->cmd;
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKTRY(d, vkCreateFence(d->dev, &fi, nullptr, &f));
    VkResult r = vkQueueSubmit(d->queue, 1, &su, f);
    if (r == VK_SUCCESS) r = vkWaitForFences(d->dev, 1, &f, VK_TRUE, ~0ull);
    vkDestroyFence(d->dev, f, nullptr);
    if (r != VK_SUCCESS) {
        destroy_buf(d, host);
        return seterr(d, NXVC_VKD_ERR_VULKAN, "atlas readback: %s (%d)",
                      vkresult_name(r), (int)r);
    }
    std::memcpy(out, host.mapped, (size_t)bytes);
    destroy_buf(d, host);
    return NXVC_VKD_OK;
}
}  // namespace

// [ATLAS] Rebind the view set after the images have been (re)made.
static void atlas_view_rebind(D *d) {
    VkDescriptorBufferInfo avb{d->bRing.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo avi[3] = {
        {VK_NULL_HANDLE, d->imgViewY.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgViewC.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, d->imgViewCr.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[4]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = d->dsetAV;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[0].pBufferInfo = &avb;
    for (int i = 0; i < 3; ++i) {
        w[1 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[1 + i].dstSet = d->dsetAV;
        w[1 + i].dstBinding = (uint32_t)(1 + i);
        w[1 + i].descriptorCount = 1;
        w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1 + i].pImageInfo = &avi[i];
    }
    vkUpdateDescriptorSets(d->dev, 4, w, 0, nullptr);
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_set_atlas_view(
    nxvc_vk_decoder *d, nxvc_vkd_atlas_view view) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (!d->atlas_mode)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "the display view exists only for an ATLAS stream");
    // [SYN] 13.12.1: under a colour transform the chroma planes carry the
    // extra bit that transform produces -- 9 bits for an 8-bit stream -- and
    // an 8-bit UNORM cannot hold them.  Refused rather than truncated: a
    // silently wrong picture is worse than an unavailable optimisation.
    if (view == NXVC_VKD_ATLAS_VIEW_R8 && d->si.color_transform != 0)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "the one-tap 8-bit view needs CT_NONE; this stream's "
                      "colour transform is %u, whose chroma is 9-bit",
                      d->si.color_transform);
    nxvc_vkd_status st = nxvc_vk_decoder_wait(d, UINT64_MAX);
    if (st) return st;
    const StreamInfo &si = d->si;
    const uint32_t VW = si.width * si.eyes, VH = si.height;
    const uint32_t VCW = si.cw * si.eyes, VCH = si.ch;
    if (view == NXVC_VKD_ATLAS_VIEW_R8) {
        if ((st = make_img(d, d->imgViewY, VK_FORMAT_R8_UNORM, VW, VH)))
            return st;
        if ((st = make_img(d, d->imgViewC, VK_FORMAT_R8G8_UNORM, VCW, VCH)))
            return st;
        if ((st = make_img(d, d->imgViewCr, VK_FORMAT_R16_UINT, 1, 1)))
            return st;
    } else if (view == NXVC_VKD_ATLAS_VIEW_R16) {
        if ((st = make_img(d, d->imgViewY, VK_FORMAT_R16_UINT, VW, VH)))
            return st;
        if ((st = make_img(d, d->imgViewC, VK_FORMAT_R16_UINT, VCW, VCH)))
            return st;
        if ((st = make_img(d, d->imgViewCr, VK_FORMAT_R16_UINT, VCW, VCH)))
            return st;
    } else {
        if ((st = make_img(d, d->imgViewY, VK_FORMAT_R8_UNORM, 1, 1)))
            return st;
        if ((st = make_img(d, d->imgViewC, VK_FORMAT_R8G8_UNORM, 1, 1)))
            return st;
        if ((st = make_img(d, d->imgViewCr, VK_FORMAT_R16_UINT, 1, 1)))
            return st;
    }
    atlas_view_rebind(d);
    d->atlas_view = (uint32_t)view;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_atlas_view nxvc_vk_decoder_atlas_view(
    const nxvc_vk_decoder *d) {
    return d ? (nxvc_vkd_atlas_view)d->atlas_view : NXVC_VKD_ATLAS_VIEW_NONE;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_atlas_images(
    const nxvc_vk_decoder *d, nxvc_vkd_atlas_images *o) {
    if (!d || !o) return NXVC_VKD_ERR_ARG;
    if (!d->atlas_view)
        return NXVC_VKD_ERR_UNSUPPORTED;
    const Img *im[3] = {&d->imgViewY, &d->imgViewC, &d->imgViewCr};
    for (int i = 0; i < 3; ++i) {
        const bool real =
            !(d->atlas_view == (uint32_t)NXVC_VKD_ATLAS_VIEW_R8 && i == 2);
        o->image[i] = real ? im[i]->img : VK_NULL_HANDLE;
        o->view[i] = real ? im[i]->view : VK_NULL_HANDLE;
        o->format[i] = real ? im[i]->fmt : VK_FORMAT_UNDEFINED;
        o->width[i] = real ? im[i]->w : 0;
        o->height[i] = real ? im[i]->h : 0;
    }
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_atlas_view_read(
    nxvc_vk_decoder *d, int plane, uint8_t *out, size_t cap, uint32_t *w,
    uint32_t *h, uint32_t *bps) {
    if (!d || plane < 0 || plane > 2) return NXVC_VKD_ERR_ARG;
    if (!d->atlas_view)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED, "no display view selected");
    const Img *im = plane == 0   ? &d->imgViewY
                    : plane == 1 ? &d->imgViewC
                                 : &d->imgViewCr;
    if (d->atlas_view == (uint32_t)NXVC_VKD_ATLAS_VIEW_R8 && plane == 2)
        return NXVC_VKD_ERR_ARG;
    uint32_t bytes = 1;
    if (im->fmt == VK_FORMAT_R8G8_UNORM) bytes = 2;
    else if (im->fmt == VK_FORMAT_R16_UINT) bytes = 2;
    if (w) *w = im->w;
    if (h) *h = im->h;
    if (bps) *bps = bytes;
    const size_t need = (size_t)im->w * im->h * bytes;
    if (!out) return NXVC_VKD_OK;
    if (cap < need)
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "atlas_view_read: need %zu bytes, given %zu", need, cap);
    nxvc_vkd_status st = nxvc_vk_decoder_wait(d, UINT64_MAX);
    if (st) return st;
    Buf host{};
    if ((st = make_buf(d, host, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)))
        return st;
    VKTRY(d, vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKTRY(d, vkBeginCommandBuffer(d->cmd, &bi));
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {im->w, im->h, 1};
    vkCmdCopyImageToBuffer(d->cmd, im->img, VK_IMAGE_LAYOUT_GENERAL, host.buf,
                           1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    VKTRY(d, vkEndCommandBuffer(d->cmd));
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.commandBufferCount = 1;
    su.pCommandBuffers = &d->cmd;
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKTRY(d, vkCreateFence(d->dev, &fi, nullptr, &f));
    VkResult r = vkQueueSubmit(d->queue, 1, &su, f);
    if (r == VK_SUCCESS) r = vkWaitForFences(d->dev, 1, &f, VK_TRUE, ~0ull);
    vkDestroyFence(d->dev, f, nullptr);
    if (r != VK_SUCCESS) {
        destroy_buf(d, host);
        return seterr(d, NXVC_VKD_ERR_VULKAN, "view readback: %s (%d)",
                      vkresult_name(r), (int)r);
    }
    std::memcpy(out, host.mapped, need);
    destroy_buf(d, host);
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_timestamp_info(
    const nxvc_vk_decoder *d, float *period_ns, uint32_t *valid_bits) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (period_ns) *period_ns = d->have_timestamps ? d->ts_period : 0.f;
    if (valid_bits) *valid_bits = d->ts_valid_bits;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_vk_handles(
    const nxvc_vk_decoder *d, VkInstance *instance,
    VkPhysicalDevice *physical_device, VkDevice *device, VkQueue *queue,
    uint32_t *queue_family) {
    if (!d) return NXVC_VKD_ERR_ARG;
    if (instance) *instance = d->inst;
    if (physical_device) *physical_device = d->phys;
    if (device) *device = d->dev;
    if (queue) *queue = d->queue;
    if (queue_family) *queue_family = d->qfam;
    return NXVC_VKD_OK;
}

extern "C" nxvc_vkd_status nxvc_vk_atlas_write_tiles(
    nxvc_vk_decoder *d, uint32_t eye, uint32_t first_tile, uint32_t count,
    const nxvc_vkd_atlas_src *src, uint32_t src_frame, uint32_t submit_flags,
    uint32_t *applied, uint32_t *superseded) {
    if (applied) *applied = 0;
    if (superseded) *superseded = 0;
    if (!d) return NXVC_VKD_ERR_ARG;
    if (!d->atlas_mode)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "nxvc_vk_atlas_write_tiles: not an ATLAS stream");
    if (!count) return NXVC_VKD_OK;
    if (!src || src->buffer == VK_NULL_HANDLE)
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "nxvc_vk_atlas_write_tiles: a buffer source is required");
    if (src->image != VK_NULL_HANDLE)
        return seterr(d, NXVC_VKD_ERR_UNSUPPORTED,
                      "nxvc_vk_atlas_write_tiles: an image source is reserved "
                      "-- [SYN] 13.12.9's channel order is only safe through a "
                      "buffer the caller has already mapped");
    const StreamInfo &si = d->si;
    const uint32_t perEye = si.tiles_x * si.tiles_y;
    if (eye >= si.eyes || first_tile > perEye || count > perEye - first_tile)
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "nxvc_vk_atlas_write_tiles: run [%u,%u) leaves eye %u "
                      "(%u tiles)",
                      first_tile, first_tile + count, eye, perEye);
    nxvc_vkd_status st = nxvc_vk_decoder_wait(d, UINT64_MAX);
    if (st) return st;

    // The run is within the eye, in that eye's row-major order; the TABLE is
    // eye-minor ([SYN] 3.3).  The API takes `eye` explicitly so the caller
    // stays on the pixel side of the two eye conventions and the decoder
    // derives the table index.
    std::vector<uint32_t> want;
    want.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t k = first_tile + i;
        const uint32_t row = k / si.tiles_x, col = k % si.tiles_x;
        want.push_back(nxvw::nxvw_atlas_index((int)row, (int)eye, (int)col,
                                              (int)si.tiles_x, (int)si.eyes));
    }
    // 13.12.9's ordering rule, and it is the SAME `>=` a coded tile takes:
    // a base tile can never move a position backwards over a coded one, and
    // the two sources compose under one rule rather than two.
    nxvw::AtlasApply ap = d->astate.apply(want.data(), (uint32_t)want.size(),
                                          src_frame);
    if (applied) *applied = (uint32_t)ap.accepted.size();
    if (superseded) *superseded = (uint32_t)ap.superseded.size();
    if (ap.accepted.empty()) return NXVC_VKD_OK;

    // The accepted positions, as maximal runs of consecutive COLUMNS within
    // one tile row: that is what turns a scattered per-tile copy into row
    // strips, which the device measurement makes worth doing rather than
    // tidy.  `accepted` is in the caller's order, which is row-major within
    // the eye, so a run is a maximal ascending stretch of `k`.
    std::vector<uint32_t> ks;
    ks.reserve(ap.accepted.size());
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t k = first_tile + i;
        const uint32_t row = k / si.tiles_x, col = k % si.tiles_x;
        const uint32_t n = (uint32_t)nxvw::nxvw_atlas_index(
            (int)row, (int)eye, (int)col, (int)si.tiles_x, (int)si.eyes);
        if (std::find(ap.accepted.begin(), ap.accepted.end(), n) !=
            ap.accepted.end())
            ks.push_back(k);
    }
    if (ks.empty()) return NXVC_VKD_OK;

    // `bACoded` is already sized for EVERY entry by make_resources(), and the
    // accepted run can never be longer, so it is reused rather than
    // reallocated.  Shrinking it here would have been silent corruption a
    // frame later: the next decode copies its own coded list into the same
    // buffer, and that list is usually longer than a patch's run.
    Buf up{};
    const VkDeviceSize listBytes = (VkDeviceSize)ap.accepted.size() * 4;
    if ((st = make_buf(d, up, listBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       true)))
        return st;
    std::memcpy(up.mapped, ap.accepted.data(), (size_t)listBytes);

    VKTRY(d, vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKTRY(d, vkBeginCommandBuffer(d->cmd, &bi));
    { VkBufferCopy c{0, 0, listBytes};
      vkCmdCopyBuffer(d->cmd, up.buf, d->bACoded.buf, 1, &c); }

    // The pixels, plane by plane, one region per SAMPLE ROW of each strip.
    std::vector<VkBufferCopy> regions;
    const uint32_t tile = si.tile_size ? si.tile_size : 64u;
    size_t i0 = 0;
    while (i0 < ks.size()) {
        size_t i1 = i0 + 1;
        while (i1 < ks.size() && ks[i1] == ks[i1 - 1] + 1 &&
               ks[i1] / si.tiles_x == ks[i0] / si.tiles_x)
            ++i1;
        const uint32_t row = ks[i0] / si.tiles_x;
        const uint32_t c0 = ks[i0] % si.tiles_x;
        const uint32_t ncol = (uint32_t)(i1 - i0);
        for (int p = 0; p < si.nplanes(); ++p) {
            const uint32_t sub = (p == 1 || p == 2) && si.chroma != 1 ? 2u : 1u;
            const uint32_t tw = tile / sub, th = tile / sub;
            const uint32_t pw = (uint32_t)d->ringPlaneW[p];
            const uint32_t ph = (p == 1 || p == 2) ? si.ch : si.height;
            const uint32_t x0 = c0 * tw;
            const uint32_t x1 = std::min((c0 + ncol) * tw, pw);
            const uint32_t y0 = row * th;
            const uint32_t y1 = std::min(y0 + th, ph);
            if (x0 >= x1 || y0 >= y1) continue;
            for (uint32_t y = y0; y < y1; ++y) {
                const VkDeviceSize e = (VkDeviceSize)d->ringOff[p] +
                                       (VkDeviceSize)y * d->ringStride[p] +
                                       eye * pw + x0;
                VkBufferCopy c{};
                c.srcOffset = src->offset + e * 2;
                c.dstOffset = e * 2;
                c.size = (VkDeviceSize)(x1 - x0) * 2;
                regions.push_back(c);
            }
        }
        i0 = i1;
    }
    if (!regions.empty())
        vkCmdCopyBuffer(d->cmd, src->buffer, d->bRing.buf,
                        (uint32_t)regions.size(), regions.data());
    buffer_barrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    nxvw::NxvwAtlasTilePush tp{};
    tp.tileCount = (uint32_t)ap.accepted.size();
    tp.op = NXVW_ATLAS_OP_BASE_PATCH;
    tp.frame = src_frame;
    tp.colsPerEye = si.tiles_x;
    tp.eyes = si.eyes;
    tp.advanceTo = d->have_frame ? d->last_frame : src_frame;
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->plAT, 0,
                            1, &d->dsetAT, 0, nullptr);
    vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeAT);
    vkCmdPushConstants(d->cmd, d->plAT, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       (uint32_t)sizeof tp, &tp);
    vkCmdDispatch(d->cmd, (tp.tileCount + 63u) / 64u, 1, 1);
    VKTRY(d, vkEndCommandBuffer(d->cmd));

    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.commandBufferCount = 1;
    su.pCommandBuffers = &d->cmd;
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKTRY(d, vkCreateFence(d->dev, &fi, nullptr, &f));
    VkResult r = vkQueueSubmit(d->queue, 1, &su, f);
    if (r == VK_SUCCESS) r = vkWaitForFences(d->dev, 1, &f, VK_TRUE, ~0ull);
    vkDestroyFence(d->dev, f, nullptr);
    destroy_buf(d, up);
    if (r != VK_SUCCESS)
        return seterr(d, NXVC_VKD_ERR_VULKAN, "atlas patch: %s (%d)",
                      vkresult_name(r), (int)r);
    d->astate.commit(ap.accepted.data(), (uint32_t)ap.accepted.size(),
                     src_frame);
    // `commit()` moves `advanced_to` with `src_frame`; a base patch's two
    // clocks are different, so put the composition one back.
    d->astate.mark_advanced(ap.accepted,
                            d->have_frame ? d->last_frame : src_frame);
    (void)submit_flags;
    return NXVC_VKD_OK;
}

extern "C" size_t nxvc_vk_decoder_atlas_table_size(const nxvc_vk_decoder *d) {
    if (!d || !d->atlas_mode) return 0;
    return (size_t)d->si.tile_count * NXVW_ATLAS_ENTRY_BYTES;
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_atlas_table(nxvc_vk_decoder *d,
                                                       uint8_t *out,
                                                       size_t cap) {
    if (!d || !out) return NXVC_VKD_ERR_ARG;
    if (!d->atlas_mode)
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "nxvc_vk_decoder_atlas_table: not an ATLAS stream "
                      "(tool bit 31 is not set)");
    const size_t need = nxvc_vk_decoder_atlas_table_size(d);
    if (cap < need)
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "nxvc_vk_decoder_atlas_table: need %zu bytes, given %zu",
                      need, cap);
    return atlas_readback(d, d->bTable, (VkDeviceSize)need, out);
}

extern "C" nxvc_vkd_status nxvc_vk_decoder_atlas_plane(
    nxvc_vk_decoder *d, int plane, uint16_t *out, size_t cap, uint32_t *w,
    uint32_t *h, uint32_t *stride) {
    // `out == nullptr` with `cap == 0` is a SIZE QUERY: it fills `w`, `h` and
    // `stride` and copies nothing.  A caller has to be able to ask how big a
    // plane is before allocating for it, and returning ERR_ARG before writing
    // those three left every querying caller with zeroes -- which reads as
    // "this plane does not exist" and silently folded NOTHING into the
    // conformance digest.
    if (!d || plane < 0 || plane > 3) return NXVC_VKD_ERR_ARG;
    if (!out && cap) return NXVC_VKD_ERR_ARG;
    if (!d->atlas_mode)
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "nxvc_vk_decoder_atlas_plane: not an ATLAS stream "
                      "(tool bit 31 is not set)");
    if (plane >= d->si.nplanes()) return NXVC_VKD_ERR_ARG;
    const uint32_t pw = (uint32_t)d->ringPlaneW[plane];
    const uint32_t ph =
        (plane == 1 || plane == 2) ? d->si.ch : d->si.height;
    const uint32_t str = (uint32_t)d->ringStride[plane];
    if (w) *w = pw;
    if (h) *h = ph;
    if (stride) *stride = str;
    const size_t need = (size_t)str * ph;
    if (!out) return NXVC_VKD_OK;   // the size query is answered
    if (cap < need)
        return seterr(d, NXVC_VKD_ERR_ARG,
                      "nxvc_vk_decoder_atlas_plane: need %zu u16, given %zu",
                      need, cap);
    // The plane's slice of the single atlas slot.  `ringOff` is in u16
    // elements, which is what the layout computes and what Pass B stores at.
    Buf tmp = d->bRing;
    (void)tmp;
    nxvc_vkd_status st = nxvc_vk_decoder_wait(d, UINT64_MAX);
    if (st) return st;
    std::vector<uint16_t> all((size_t)d->ringSlotU16);
    if ((st = atlas_readback(d, d->bRing,
                             (VkDeviceSize)d->ringSlotU16 * 2, all.data())))
        return st;
    std::memcpy(out, all.data() + d->ringOff[plane], need * 2);
    return NXVC_VKD_OK;
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
