// NX Warp Phase 0 bench, Android frontend.
//
// NativeActivity, Vulkan 1.1, no OpenXR. A swapchain is presented every frame
// so the display stays active and the compositor keeps running, and the dummy
// reprojection pass is submitted every frame as co-tenant load, exactly as
// PAPER 3.4 requires.
//
// Options arrive as an intent string extra named "args", so run.sh can say
//   am start -n <pkg>/android.app.NativeActivity --es args "--kernels k1,k2"
#include "nxb_bench.h"
#include "k6_hybrid.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan_android.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace nxb;

namespace {

struct Swapchain
{
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageLayout> layouts;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
};

bool createSwapchain(VkCtx& ctx, VkSurfaceKHR surface, Swapchain& sc)
{
    VkSurfaceCapabilitiesKHR caps{};
    NXB_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.phys, surface, &caps));

    uint32_t nFmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.phys, surface, &nFmt, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nFmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.phys, surface, &nFmt, fmts.data());
    VkSurfaceFormatKHR pick = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM)
        { pick = f; break; }

    sc.format = pick.format;
    sc.extent = caps.currentExtent;
    if (sc.extent.width == 0xffffffffu) sc.extent = {1080, 2400};

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = count;
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = sc.extent;
    ci.imageArrayLayers = 1;
    // TRANSFER_DST because the bench copies its output image into the
    // swapchain; it never renders with a graphics pipeline.
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR))
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync: one frame per refresh
    ci.clipped = VK_TRUE;
    NXB_VK(vkCreateSwapchainKHR(ctx.dev, &ci, nullptr, &sc.handle));

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx.dev, sc.handle, &n, nullptr);
    sc.images.resize(n);
    vkGetSwapchainImagesKHR(ctx.dev, sc.handle, &n, sc.images.data());
    sc.layouts.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    NXB_VK(vkCreateSemaphore(ctx.dev, &si, nullptr, &sc.acquired));
    NXB_VK(vkCreateSemaphore(ctx.dev, &si, nullptr, &sc.rendered));

    NXB_LOG("swapchain: %ux%u, %u images, format %d, FIFO",
            sc.extent.width, sc.extent.height, n, int(sc.format));
    return true;
}

VkSurfaceKHR makeAndroidSurface(VkInstance inst, void* user)
{
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = static_cast<ANativeWindow*>(user);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    NXB_VK(vkCreateAndroidSurfaceKHR(inst, &ci, nullptr, &s));
    return s;
}

// ---- intent extras, so run.sh can drive the run without a Java layer
std::string intentArgs(android_app* app)
{
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};

    std::string out;
    jobject activity = app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getIntent = env->GetMethodID(actCls, "getIntent", "()Landroid/content/Intent;");
    if (getIntent)
    {
        jobject intent = env->CallObjectMethod(activity, getIntent);
        if (intent)
        {
            jclass intentCls = env->GetObjectClass(intent);
            jmethodID getExtra = env->GetMethodID(
                intentCls, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
            jstring key = env->NewStringUTF("args");
            jstring val = (jstring)env->CallObjectMethod(intent, getExtra, key);
            if (val)
            {
                const char* c = env->GetStringUTFChars(val, nullptr);
                if (c) { out = c; env->ReleaseStringUTFChars(val, c); }
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    app->activity->vm->DetachCurrentThread();
    return out;
}

int kidFromName(const std::string& s)
{
    if (s == "k1")  return K1_COPY;
    if (s == "k2")  return K2_GATHER4;
    if (s == "k2b") return K2B_SAMPLER;
    if (s == "k3")  return K3_IDCT;
    if (s == "k4")  return K4_RANS;
    if (s == "k5")  return K5_FULL;
    if (s == "k6")  return K6_HYBRID;
    return -1;
}

void applyArgs(Config& cfg, const std::string& args, bool& selftest)
{
    std::vector<std::string> tok;
    size_t p = 0;
    while (p < args.size())
    {
        while (p < args.size() && args[p] == ' ') ++p;
        size_t q = args.find(' ', p);
        if (q == std::string::npos) q = args.size();
        if (q > p) tok.push_back(args.substr(p, q - p));
        p = q + 1;
    }
    for (size_t i = 0; i < tok.size(); ++i)
    {
        const std::string& a = tok[i];
        auto next = [&]() -> std::string { return (i + 1 < tok.size()) ? tok[++i] : std::string(); };
        if (a == "--kernels")
        {
            std::string list = next();
            if (list == "all") { cfg.kernelMask = (1u << KID_COUNT) - 1u; continue; }
            cfg.kernelMask = 0;
            size_t s = 0;
            while (s <= list.size())
            {
                size_t e = list.find(',', s);
                if (e == std::string::npos) e = list.size();
                std::string t = list.substr(s, e - s);
                int k = t.empty() ? -1 : kidFromName(t);
                if (k >= 0) cfg.kernelMask |= 1u << k;
                s = e + 1;
            }
        }
        else if (a == "--frames")      cfg.frames = atoi(next().c_str());
        else if (a == "--warmup")      cfg.warmup = atoi(next().c_str());
        else if (a == "--width")       cfg.width = atoi(next().c_str());
        else if (a == "--height")      cfg.height = atoi(next().c_str());
        else if (a == "--no-cotenant") cfg.cotenant = false;
        else if (a == "--thermal")     cfg.thermalSeconds = atof(next().c_str());
        else if (a == "--label")       cfg.label = next();
        else if (a == "--validation")  cfg.validation = true;
        else if (a == "--selftest")    selftest = true;
    }
}

struct AppState
{
    android_app* app = nullptr;
    bool windowReady = false;
    bool destroyRequested = false;
};

void onAppCmd(android_app* app, int32_t cmd)
{
    AppState* st = static_cast<AppState*>(app->userData);
    switch (cmd)
    {
    case APP_CMD_INIT_WINDOW: st->windowReady = (app->window != nullptr); break;
    case APP_CMD_TERM_WINDOW: st->windowReady = false; break;
    case APP_CMD_DESTROY:     st->destroyRequested = true; break;
    default: break;
    }
}

void pumpEvents(android_app* app)
{
    int events;
    android_poll_source* source;
    while (ALooper_pollOnce(0, nullptr, &events, (void**)&source) >= 0)
    {
        if (source) source->process(app, source);
        if (app->destroyRequested) return;
    }
}

} // namespace

void android_main(android_app* app)
{
    AppState st;
    st.app = app;
    app->userData = &st;
    app->onAppCmd = onAppCmd;

    NXB_LOG("=== NX Warp Phase 0 bench starting ===");

    // Wait for a window: the display must be active for the gate to mean
    // anything (PAPER 3.4).
    while (!st.windowReady && !app->destroyRequested)
    {
        int events;
        android_poll_source* source;
        if (ALooper_pollOnce(-1, nullptr, &events, (void**)&source) >= 0 && source)
            source->process(app, source);
    }
    if (app->destroyRequested) return;

    Config cfg;
    cfg.kernelMask = (1u << K1_COPY) | (1u << K2_GATHER4) | (1u << K2B_SAMPLER) |
                     (1u << K3_IDCT) | (1u << K4_RANS) | (1u << K5_FULL);
    bool selftest = false;
    std::string args = intentArgs(app);
    if (!args.empty())
    {
        NXB_LOG("intent args: %s", args.c_str());
        applyArgs(cfg, args, selftest);
    }

    std::string outDir = app->activity->internalDataPath ? app->activity->internalDataPath : ".";
    cfg.outPath = outDir + "/nxwarp-phase0.json";

    VkCtx ctx;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::vector<const char*> instExt = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    std::vector<const char*> devExt = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };
    if (!ctx.create(instExt, devExt, cfg.validation,
                    makeAndroidSurface, app->window, &surface))
    {
        NXB_LOG("Vulkan device creation failed");
        ANativeActivity_finish(app->activity);
        return;
    }

    NXB_LOG("device: %s", ctx.info.name.c_str());
    NXB_LOG("subgroup size %u (min %u max %u), ballot %s",
            ctx.info.subgroupSize, ctx.info.subgroupMin, ctx.info.subgroupMax,
            ctx.info.subgroupBallot ? "yes" : "NO");
    NXB_LOG("timestampPeriod %.3f ns, validBits %u, shared mem %u B",
            double(ctx.info.timestampPeriod), ctx.info.timestampValidBits,
            ctx.info.maxSharedMemory);

    Swapchain sc;
    createSwapchain(ctx, surface, sc);

    Bench bench;
    if (!bench.init(ctx, cfg))
    {
        NXB_LOG("bench init failed");
        ctx.destroy();
        ANativeActivity_finish(app->activity);
        return;
    }

    // ---- K6: try the real hybrid base; fall back to the synthetic stand-in.
    HybridBase hybrid;
    bool wantK6 = (cfg.kernelMask & (1u << K6_HYBRID)) != 0;
    if (wantK6)
    {
        std::string assetPath = outDir + "/base.hevc";
        if (hybrid.start(ctx, cfg.width, cfg.height, 90, assetPath) && hybrid.ready())
        {
            bench.setHybridBase(hybrid.view(), hybrid.sampler());
            NXB_LOG("K6: hybrid base ready (%s)", hybrid.status().c_str());
        }
        else
            NXB_LOG("K6: %s -- Pass C runs against a synthetic base",
                    hybrid.status().c_str());
    }

    std::string selftestMsg;
    if (selftest)
    {
        std::string a, b;
        bool okB = bench.verifyPassB(&b);
        bool okA = bench.verifyPassA(&a);
        NXB_LOG("%s", b.c_str());
        NXB_LOG("%s", a.c_str());
        selftestMsg = b + " | " + a;
        (void)okA; (void)okB;
    }

    Runner runner;
    runner.init(ctx, bench);

    uint32_t imageIndex = 0;
    bool haveImage = false;

    RunHooks hooks;
    hooks.preFrame = [&](int) -> bool {
        pumpEvents(app);
        if (app->destroyRequested) return false;
        VkResult r = vkAcquireNextImageKHR(ctx.dev, sc.handle, UINT64_MAX,
                                           sc.acquired, VK_NULL_HANDLE, &imageIndex);
        haveImage = (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR);
        if (wantK6) hybrid.poll();
        return true;
    };
    hooks.extraRecord = [&](VkCommandBuffer cmd) {
        if (!haveImage) return;
        VkImage dst = sc.images[imageIndex];

        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.image = dst;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        Image& src = bench.displayImage();
        VkImageMemoryBarrier sb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        sb.image = src.img;
        sb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &sb);

        // R8G8B8A8_UINT and the swapchain's UNORM are size-compatible, so a
        // copy is legal where a blit would not be. A 1:1 crop is enough: the
        // display is proof of life, not a deliverable.
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {std::min(sc.extent.width, src.w), std::min(sc.extent.height, src.h), 1};
        vkCmdCopyImage(cmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);

        sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        sb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &sb);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    hooks.postFrame = [&]() {
        if (!haveImage) return;
        // The bench submit is already fence-waited by the runner, so the
        // image is ready; present without extra semaphores.
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc.handle;
        pi.pImageIndices = &imageIndex;
        vkQueuePresentKHR(ctx.queue, &pi);
        // Consume the acquire semaphore so it can be reused: a wait-only
        // submit is the cheapest legal way to do that.
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sc.acquired;
        si.pWaitDstStageMask = &stage;
        vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE);
        haveImage = false;
    };

    std::vector<KernelResult> results;
    if (cfg.thermalSeconds > 0.0)
        results.push_back(runner.runPass(K5_FULL, cfg, hooks));
    else
        for (int k = 0; k < KID_COUNT; ++k)
        {
            if (!(cfg.kernelMask & (1u << k))) continue;
            results.push_back(runner.runPass(k, cfg, hooks));
            if (app->destroyRequested) break;
        }

    RunInfo info;
    info.device = ctx.info;
    info.cfg = cfg;
    info.platform = "android";
    info.mode = cfg.thermalSeconds > 0.0 ? "thermal" : "bench";
    info.verdict = verdictFor(results);
    if (wantK6 && !hybrid.latencies().empty())
        info.hybridDecodeLatencyP50 = percentile(hybrid.latencies(), 0.50);

    std::string table = buildTable(info, results);
    // logcat truncates long lines, so the table goes out one line at a time.
    for (size_t p = 0, q; p < table.size(); p = q + 1)
    {
        q = table.find('\n', p);
        if (q == std::string::npos) q = table.size();
        __android_log_print(ANDROID_LOG_INFO, "nxwarp-bench", "%s",
                            table.substr(p, q - p).c_str());
    }
    if (!selftestMsg.empty())
        NXB_LOG("selftest: %s", selftestMsg.c_str());

    std::string json = buildJson(info, results);
    if (FILE* f = fopen(cfg.outPath.c_str(), "wb"))
    {
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        NXB_LOG("RESULT_JSON %s", cfg.outPath.c_str());
    }
    else
        NXB_LOG("could not write %s", cfg.outPath.c_str());

    NXB_LOG("=== NX Warp Phase 0 bench done ===");

    if (wantK6) hybrid.stop();
    runner.destroy();
    bench.destroy();
    vkDeviceWaitIdle(ctx.dev);
    if (sc.acquired) vkDestroySemaphore(ctx.dev, sc.acquired, nullptr);
    if (sc.rendered) vkDestroySemaphore(ctx.dev, sc.rendered, nullptr);
    if (sc.handle) vkDestroySwapchainKHR(ctx.dev, sc.handle, nullptr);
    if (surface) vkDestroySurfaceKHR(ctx.instance, surface, nullptr);
    ctx.destroy();

    ANativeActivity_finish(app->activity);
    // Drain the looper so the finish is actually delivered.
    while (!app->destroyRequested)
    {
        int events;
        android_poll_source* source;
        if (ALooper_pollOnce(100, nullptr, &events, (void**)&source) >= 0 && source)
            source->process(app, source);
    }
}
