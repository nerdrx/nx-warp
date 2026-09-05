// nxvc-vkdec-wrap -- the COMPOSITOR's sequence around the decoder, timed.
//
// nxvc-vkdec measures the decoder.  This measures what a client pays to USE
// it, because on the Pico 4 that turned out to be the larger number: a live
// WiVRn session reported ~47 ms of decode wall for a 48 KB 1088x1088 frame
// that nxvc-vkdec decodes in 12 ms of GPU and 13 ms of wall on the same
// device.  The difference is the shape of the client's frame loop, not the
// kernels, and this tool reproduces that shape so the pieces can be priced
// one at a time.
//
// The sequence is client/decoder/nxwarp/nxwarp_decoder.cpp decode_unit() in
// WiVRn, on a device this tool creates and hands to every decoder instance
// exactly as the client hands it Monado's:
//
//   wait(previous frame)            nxvc owns one command buffer, so the
//                                   next record cannot start until the last
//                                   submission retires
//   decode(ASYNC)                   under the queue lock
//   wait(this frame)                only because the Adreno refuses a
//                                   timeline semaphore, so there is no
//                                   object the queue could wait on instead
//   record + submit the copy        the decoder's images into a pool image
//   wait(the copy's fence)          again for want of a semaphore
//
//   --mode wrapper   all of the above (the shipped client)
//   --mode binsem    the mid-frame host wait replaced by the binary
//                    semaphore this decoder now signals on request
//   --mode nocopy    no copy at all: what the client would pay if the pool
//                    item were the decoder's own image
//   --mode decode    decode and wait, nothing else: nxvc-vkdec's number
//
//   --streams N      N decoder instances on ONE queue, round-robin, which is
//                    what two eyes plus a quad layer actually do
//   --defer          submit every stream's decode before waiting on any of
//                    them (only meaningful with --streams > 1).  NOT valid
//                    with --mode binsem: the deferred submit does not carry
//                    the signal flag, so the copy would wait on a semaphore
//                    nothing signals.  Use it with --mode wrapper.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <nxvc/nxvc_vk.h>

namespace {

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch())
        .count();
}

#define VKOK(x)                                                              \
    do {                                                                     \
        VkResult r_ = (x);                                                   \
        if (r_ != VK_SUCCESS) {                                              \
            std::fprintf(stderr, "%s failed: %d\n", #x, (int)r_);            \
            return 1;                                                        \
        }                                                                    \
    } while (0)

struct Dev {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkPhysicalDeviceMemoryProperties mem{};
};

uint32_t pick_mem(const Dev &d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (d.mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

// The pool image the client blits into: one G8_B8R8_2PLANE_420_UNORM the
// renderer samples through a Ycbcr conversion, exactly as WiVRn's pool item.
struct PoolImage {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

int make_pool_image(const Dev &d, uint32_t w, uint32_t h, PoolImage *out) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(d.dev, &ii, nullptr, &out->img));
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(d.dev, out->img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex =
        pick_mem(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        std::fprintf(stderr, "no device-local memory for the pool image\n");
        return 1;
    }
    VKOK(vkAllocateMemory(d.dev, &ai, nullptr, &out->mem));
    VKOK(vkBindImageMemory(d.dev, out->img, out->mem, 0));
    return 0;
}

struct Stream {
    nxvc_vk_decoder *dec = nullptr;
    PoolImage pool;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool fence_armed = false;
};

// The client's copy: two vkCmdCopyImage into the pool image's two planes,
// with the barriers it uses, recorded fresh every frame as it does.
void record_copy(VkCommandBuffer cmd, const nxvc_vkd_images &img,
                 VkImage pool, uint32_t w, uint32_t h) {
    VkImageMemoryBarrier pre[3]{};
    for (int i = 0; i < 2; ++i) {
        pre[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        pre[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        pre[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        pre[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        pre[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        pre[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre[i].image = img.image[i];
        pre[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    pre[2] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pre[2].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[2].image = pool;
    pre[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 3, pre);

    VkImageCopy luma{};
    luma.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    luma.dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
    luma.extent = {w, h, 1};
    VkImageCopy chroma{};
    chroma.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    chroma.dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
    chroma.extent = {w / 2, h / 2, 1};
    vkCmdCopyImage(cmd, img.image[0], VK_IMAGE_LAYOUT_GENERAL, pool,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &luma);
    vkCmdCopyImage(cmd, img.image[1], VK_IMAGE_LAYOUT_GENERAL, pool,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &chroma);

    VkImageMemoryBarrier post{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = pool;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &post);
}

void usage() {
    std::fprintf(stderr,
                 "usage: nxvc-vkdec-wrap --in FILE [options]\n"
                 "  --mode wrapper|binsem|nocopy|decode  (default wrapper)\n"
                 "  --streams N     decoder instances on one queue (default 1)\n"
                 "  --priority low|medium|high|realtime  VK_EXT_global_priority\n"
                 "  --defer         submit every stream before waiting\n"
                 "  --repeat N      frames per stream (default 30)\n");
}

}  // namespace

int main(int argc, char **argv) {
    std::string in, mode = "wrapper";
    int streams = 1, repeat = 30, defer = 0, queues = 1;
    std::string priority;   // "", low, medium, high, realtime
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--in") in = val();
        else if (a == "--mode") mode = val();
        else if (a == "--streams") streams = std::atoi(val());
        else if (a == "--repeat") repeat = std::atoi(val());
        else if (a == "--defer") defer = 1;
        else if (a == "--queues") queues = std::atoi(val());
        else if (a == "--priority") priority = val();
        else { usage(); return 2; }
    }
    if (in.empty() || streams < 1) { usage(); return 2; }
    const bool do_copy = mode == "wrapper" || mode == "binsem";
    const bool use_binsem = mode == "binsem";
    if (mode != "wrapper" && mode != "binsem" && mode != "nocopy" &&
        mode != "decode") { usage(); return 2; }

    std::FILE *f = std::fopen(in.c_str(), "rb");
    if (!f) { std::perror("open"); return 1; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t)sz);
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) return 1;
    std::fclose(f);

    // ---- the device the client owns and every decoder adopts -------------
    Dev d;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VKOK(vkCreateInstance(&ici, nullptr, &d.inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(d.inst, &n, phys.data());
    if (!n) { std::fprintf(stderr, "no Vulkan device\n"); return 77; }
    d.phys = phys[0];
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &nq, qf.data());
    // The queue inventory, printed because the whole question of whether the
    // decode can get off the renderer's queue is answered by it.
    std::printf("queue families: %u\n", nq);
    for (uint32_t i = 0; i < nq; ++i)
        std::printf("  family %u: count %u  flags%s%s%s%s  timestampValidBits %u\n",
                    i, qf[i].queueCount,
                    (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? " GRAPHICS" : "",
                    (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ? " COMPUTE" : "",
                    (qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) ? " TRANSFER" : "",
                    (qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) ? " SPARSE" : "",
                    qf[i].timestampValidBits);
    {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ext(ne);
        vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, ext.data());
        bool have_query = false;
        const char *want[] = {"VK_EXT_global_priority",
                              "VK_KHR_global_priority",
                              "VK_EXT_global_priority_query",
                              "VK_EXT_calibrated_timestamps"};
        for (const char *w : want) {
            bool have = false;
            for (auto &e : ext) if (!std::strcmp(e.extensionName, w)) have = true;
            if (!std::strcmp(w, "VK_EXT_global_priority_query")) have_query = have;
            std::printf("  %-32s %s\n", w, have ? "present" : "ABSENT");
        }
        // What the family will actually GRANT, which is not the same question
        // as whether the extension is advertised: a driver is free to offer
        // the extension and refuse everything above MEDIUM to an unprivileged
        // process.
        if (have_query) {
            std::vector<VkQueueFamilyGlobalPriorityPropertiesEXT> gp(
                nq, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_EXT});
            std::vector<VkQueueFamilyProperties2> qp2(nq);
            for (uint32_t i = 0; i < nq; ++i) {
                qp2[i] = {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2};
                qp2[i].pNext = &gp[i];
            }
            uint32_t n2 = nq;
            vkGetPhysicalDeviceQueueFamilyProperties2(d.phys, &n2, qp2.data());
            for (uint32_t i = 0; i < nq; ++i) {
                std::printf("  family %u grants:", i);
                for (uint32_t k = 0; k < gp[i].priorityCount; ++k) {
                    const char *nm = "?";
                    switch (gp[i].priorities[k]) {
                    case VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT: nm = "LOW"; break;
                    case VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT: nm = "MEDIUM"; break;
                    case VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT: nm = "HIGH"; break;
                    case VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT: nm = "REALTIME"; break;
                    default: break;
                    }
                    std::printf(" %s", nm);
                }
                std::printf("\n");
            }
        }
    }
    d.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { d.qfam = i; break; }
    if (d.qfam == UINT32_MAX) { std::fprintf(stderr, "no compute queue\n"); return 77; }
    // --queues 2 asks the family for a second queue and puts the decoders on
    // it, leaving queue 0 to stand in for the renderer's.  If the family only
    // has one, this clamps and the run is the single-queue case again.
    const uint32_t want_queues = (uint32_t)queues;
    const uint32_t got_queues =
        want_queues < qf[d.qfam].queueCount ? want_queues
                                            : qf[d.qfam].queueCount;
    const float prio[2] = {1.f, 1.f};
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d.qfam;
    qci.queueCount = got_queues;
    qci.pQueuePriorities = prio;
    // The client's device carries the Ycbcr conversion its pool image needs.
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycc{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    ycc.samplerYcbcrConversion = VK_TRUE;
    VkDeviceQueueGlobalPriorityCreateInfoEXT gpi{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT};
    const char *gp_ext = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
    if (!priority.empty()) {
        gpi.globalPriority =
            priority == "low"      ? VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT
            : priority == "medium" ? VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT
            : priority == "high"   ? VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT
                                   : VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT;
        qci.pNext = &gpi;
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &ycc;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (!priority.empty()) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &gp_ext;
    }
    {
        // NOT_PERMITTED is the interesting answer and must not look like a
        // crash: it is the driver saying this process may not ask for that.
        VkResult r = vkCreateDevice(d.phys, &dci, nullptr, &d.dev);
        if (r != VK_SUCCESS) {
            std::printf("  global priority %s REFUSED (vkCreateDevice = %d%s)\n",
                        priority.c_str(), (int)r,
                        r == VK_ERROR_NOT_PERMITTED_EXT ? ", NOT_PERMITTED" : "");
            return 1;
        }
        if (!priority.empty())
            std::printf("  global priority %s granted\n", priority.c_str());
    }
    vkGetDeviceQueue(d.dev, d.qfam, got_queues - 1, &d.queue);
    if (want_queues > 1)
        std::printf("  asked for %u queues in family %u, got %u; decoding on "
                    "queue %u\n",
                    want_queues, d.qfam, got_queues, got_queues - 1);
    vkGetPhysicalDeviceMemoryProperties(d.phys, &d.mem);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(d.phys, &props);

    // ---- one decoder per stream, all adopting that one queue -------------
    std::vector<Stream> S((size_t)streams);
    uint32_t W = 0, H = 0;
    for (auto &s : S) {
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.instance = d.inst;
        ci.physical_device = d.phys;
        ci.device = d.dev;
        ci.queue = d.queue;
        ci.queue_family = d.qfam;
        ci.output_format = NXVC_VKD_OUT_YCBCR420;
        if (nxvc_vk_decoder_create(&ci, &s.dec) != NXVC_VKD_OK) {
            std::fprintf(stderr, "decoder: %s\n",
                         s.dec ? nxvc_vk_decoder_last_error(s.dec) : "?");
            return 1;
        }
        size_t consumed = 0;
        if (nxvc_vk_decoder_parse_stream_header(s.dec, data.data(),
                                                data.size(), &consumed) !=
            NXVC_VKD_OK) {
            std::fprintf(stderr, "stream header: %s\n",
                         nxvc_vk_decoder_last_error(s.dec));
            return 1;
        }
        nxvc_vkd_stream_info si;
        nxvc_vk_decoder_stream_info(s.dec, &si);
        W = si.width;
        H = si.height;
        if (do_copy && make_pool_image(d, W, H, &s.pool)) return 1;
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = d.qfam;
        VKOK(vkCreateCommandPool(d.dev, &cpi, nullptr, &s.cpool));
        VkCommandBufferAllocateInfo cbi{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbi.commandPool = s.cpool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        VKOK(vkAllocateCommandBuffers(d.dev, &cbi, &s.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKOK(vkCreateFence(d.dev, &fi, nullptr, &s.fence));
    }
    const size_t hdr = [&] {
        size_t c = 0;
        nxvc_vk_decoder_parse_stream_header(S[0].dec, data.data(), data.size(),
                                            &c);
        return c;
    }();

    const uint8_t *frame = data.data() + hdr;
    const size_t frame_len = data.size() - hdr;

    // ---- the loop --------------------------------------------------------
    double best_wall = 1e9, sum_wall = 0, sum_wait = 0, sum_dec = 0,
           sum_mid = 0, sum_copy = 0;
    int nmeas = 0;
    // Frames whose per-pass times read zero where the client would have read
    // them.  Anything but 0 here is the async stats path broken again.
    int zero_stat_frames = 0;
    for (int it = 0; it < repeat; ++it) {
        double t_iter0 = now_ms();
        // --defer: every stream's decode goes on the queue before any wait,
        // which is the one thing a single-command-buffer decoder still lets a
        // multi-stream client do.
        if (defer) {
            for (auto &s : S) {
                nxvc_vk_decoder_wait(s.dec, UINT64_MAX);
                size_t c = 0;
                nxvc_vk_decode_frame_ex(s.dec, frame, frame_len,
                                        NXVC_VKD_SUBMIT_ASYNC, &c);
            }
        }
        for (auto &s : S) {
            double t0 = now_ms();
            if (!defer) nxvc_vk_decoder_wait(s.dec, UINT64_MAX);
            double t1 = now_ms();
            uint32_t fl = NXVC_VKD_SUBMIT_ASYNC;
            if (use_binsem) fl |= NXVC_VKD_SUBMIT_SIGNAL_BINARY;
            if (!defer) {
                size_t c = 0;
                if (nxvc_vk_decode_frame_ex(s.dec, frame, frame_len, fl, &c) !=
                    NXVC_VKD_OK) {
                    std::fprintf(stderr, "decode: %s\n",
                                 nxvc_vk_decoder_last_error(s.dec));
                    return 1;
                }
            }
            double t2 = now_ms();

            VkSemaphore bs = use_binsem
                                 ? nxvc_vk_decoder_binary_semaphore(s.dec)
                                 : VK_NULL_HANDLE;
            if (!do_copy || bs == VK_NULL_HANDLE)
                nxvc_vk_decoder_wait(s.dec, UINT64_MAX);
            double t3 = now_ms();

            if (do_copy) {
                nxvc_vkd_images img{};
                nxvc_vk_decoder_images(s.dec, &img);
                if (s.fence_armed) {
                    vkWaitForFences(d.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
                    s.fence_armed = false;
                }
                vkResetCommandBuffer(s.cmd, 0);
                VkCommandBufferBeginInfo bi{
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(s.cmd, &bi);
                record_copy(s.cmd, img, s.pool.img, W, H);
                vkEndCommandBuffer(s.cmd);
                vkResetFences(d.dev, 1, &s.fence);
                const VkPipelineStageFlags ws =
                    VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                su.commandBufferCount = 1;
                su.pCommandBuffers = &s.cmd;
                if (bs != VK_NULL_HANDLE) {
                    su.waitSemaphoreCount = 1;
                    su.pWaitSemaphores = &bs;
                    su.pWaitDstStageMask = &ws;
                }
                VKOK(vkQueueSubmit(d.queue, 1, &su, s.fence));
                // The shipped client has no semaphore to hand the render
                // thread, so it blocks here.  Same here, so the number means
                // the same thing.
                vkWaitForFences(d.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
                // NO nxvc_vk_decoder_wait() here, deliberately: the copy
                // waited on the decoder's semaphore and this fence waited on
                // the copy, so the decode is provably done and the client has
                // no reason to ask the host again.  That is exactly the shape
                // in which the stats used to come back zero, so the tool must
                // keep it to be able to catch that again.
                ;
            }
            double t4 = now_ms();
            // Read the stats exactly where the client reads them: at the end
            // of its own frame, having synchronised only on the GPU.
            {
                nxvc_vkd_stats fs{};
                nxvc_vk_decoder_stats(s.dec, &fs);
                if (it >= 2 && fs.gpu_ms == 0.0) ++zero_stat_frames;
            }
            if (it >= 2) {  // two warm-up frames: pipeline compile, first touch
                sum_wait += t1 - t0;
                sum_dec += t2 - t1;
                sum_mid += t3 - t2;
                sum_copy += t4 - t3;
                sum_wall += t4 - t0;
                if (t4 - t0 < best_wall) best_wall = t4 - t0;
                ++nmeas;
            }
        }
        (void)t_iter0;
    }
    nxvc_vkd_stats st{};
    nxvc_vk_decoder_stats(S[0].dec, &st);
    const double k = nmeas ? 1.0 / nmeas : 0.0;
    std::printf(
        "%s streams=%d%s on %s: %llu B, %u tiles\n"
        "  per frame: wall %.2f (best %.2f)  = wait-prev %.2f + submit %.2f"
        " + wait-decode %.2f + copy+fence %.2f ms\n"
        "  nxvc: passA %.2f passB %.2f gpu %.2f ms"
        "   -> queue wait %.2f ms (submit-to-fence %.2f less GPU %.2f)\n"
        "  stats readable at the client's read point: %d of %d frames\n",
        mode.c_str(), streams, defer ? " (deferred)" : "", props.deviceName,
        (unsigned long long)st.frame_bytes, st.tiles, sum_wall * k,
        best_wall, sum_wait * k, sum_dec * k, sum_mid * k, sum_copy * k,
        st.pass_a_ms, st.pass_b_ms, st.gpu_ms,
        (sum_mid + sum_copy) * k - st.gpu_ms, (sum_mid + sum_copy) * k,
        st.gpu_ms, nmeas - zero_stat_frames, nmeas);

    for (auto &s : S) {
        nxvc_vk_decoder_destroy(s.dec);
        if (s.fence) vkDestroyFence(d.dev, s.fence, nullptr);
        if (s.cpool) vkDestroyCommandPool(d.dev, s.cpool, nullptr);
        if (s.pool.img) vkDestroyImage(d.dev, s.pool.img, nullptr);
        if (s.pool.mem) vkFreeMemory(d.dev, s.pool.mem, nullptr);
    }
    vkDestroyDevice(d.dev, nullptr);
    vkDestroyInstance(d.inst, nullptr);
    return 0;
}
