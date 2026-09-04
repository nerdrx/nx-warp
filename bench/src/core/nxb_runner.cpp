// One measured pass over one kernel: 120 warm-up frames then 600 measured,
// or a wall-clock thermal run (PAPER 3.4).
//
// Each kernel gets its own pass. Recording all of K1..K6 into one frame would
// push the frame past a vsync period and destroy the co-tenancy the gate is
// supposed to model; one kernel per frame keeps the device at the frame rate
// it would really run at.
#include "nxb_bench.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace nxb {

bool Runner::init(VkCtx& ctx, Bench& bench)
{
    ctx_ = &ctx;
    bench_ = &bench;

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = Bench::kSlots;
    NXB_VK(vkAllocateCommandBuffers(ctx.dev, &ai, cmd_));

    for (uint32_t i = 0; i < Bench::kSlots; ++i)
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        NXB_VK(vkCreateFence(ctx.dev, &fi, nullptr, &fence_[i]));
    }
    return true;
}

void Runner::destroy()
{
    if (!ctx_) return;
    vkDeviceWaitIdle(ctx_->dev);
    for (uint32_t i = 0; i < Bench::kSlots; ++i)
        if (fence_[i]) vkDestroyFence(ctx_->dev, fence_[i], nullptr);
    vkFreeCommandBuffers(ctx_->dev, ctx_->pool, Bench::kSlots, cmd_);
    ctx_ = nullptr;
}

KernelResult Runner::runPass(int kid, const Config& cfg, const RunHooks& hooks)
{
    KernelResult res;
    res.name = kidName(kid);

    // PAPER 3.4 thresholds.
    switch (kid)
    {
    case K1_COPY:     res.thresholdGbps = 20.0; break;
    case K2_GATHER4:  res.thresholdP50 = 3.0; break;
    case K2B_SAMPLER: break;                       // informational
    case K3_IDCT:     res.thresholdP50 = 2.5; break;
    case K4_RANS:     res.thresholdP50 = 1.5; break;
    case K5_FULL:     res.thresholdP50 = 5.0; res.thresholdP99 = 7.0; break;
    case K6_HYBRID:   res.thresholdP50 = 2.0; break;
    default: break;
    }

    std::string why;
    if (!bench_->available(kid, &why)) { res.skipReason = why; return res; }

    const bool thermal = cfg.thermalSeconds > 0.0;
    const int totalFrames = thermal ? 0 : (cfg.warmup + cfg.frames);

    std::vector<double> samples;
    std::vector<std::vector<double>> byMinute;
    samples.reserve(size_t(std::max(cfg.frames, 1)));

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    NXB_LOG("pass %s: %s", res.name.c_str(),
            thermal ? "thermal mode" : "120+600 mode");

    for (int i = 0; ; ++i)
    {
        if (thermal) { if (elapsed() >= cfg.thermalSeconds) break; }
        else if (i >= totalFrames) break;

        if (hooks.preFrame && !hooks.preFrame(i)) break;

        uint32_t slot = uint32_t(i) % Bench::kSlots;
        VkCommandBuffer cmd = cmd_[slot];
        NXB_VK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        NXB_VK(vkBeginCommandBuffer(cmd, &bi));

        bench_->resetQueries(cmd, slot);
        if (cfg.cotenant)
        {
            // Jitter so nothing can be hoisted, cached or skipped.
            float jx = float((i % 17) - 8) * 0.25f;
            float jy = float((i % 23) - 11) * 0.25f;
            bench_->recordCotenant(cmd, jx, jy);
        }
        bench_->recordKernel(cmd, kid, slot);
        if (hooks.extraRecord) hooks.extraRecord(cmd);
        NXB_VK(vkEndCommandBuffer(cmd));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        NXB_VK(vkQueueSubmit(ctx_->queue, 1, &si, fence_[slot]));
        NXB_VK(vkWaitForFences(ctx_->dev, 1, &fence_[slot], VK_TRUE, UINT64_MAX));
        NXB_VK(vkResetFences(ctx_->dev, 1, &fence_[slot]));

        double ms = bench_->readMs(slot);

        if (hooks.postFrame) hooks.postFrame();

        bool measured = thermal ? true : (i >= cfg.warmup);
        if (measured && ms >= 0.0)
        {
            samples.push_back(ms);
            if (thermal)
            {
                size_t minute = size_t(elapsed() / 60.0);
                if (byMinute.size() <= minute) byMinute.resize(minute + 1);
                byMinute[minute].push_back(ms);
            }
        }

        if (!thermal && i == cfg.warmup - 1)
            NXB_LOG("  %s: warm-up done", res.name.c_str());
        if (thermal && (i % 600) == 0)
            NXB_LOG("  %s: %.0f s elapsed, %zu samples", res.name.c_str(),
                    elapsed(), samples.size());
    }

    res.ran = !samples.empty();
    if (!res.ran) { res.skipReason = "no timestamp results"; return res; }

    res.frames = int(samples.size());
    res.p50 = percentile(samples, 0.50);
    res.p95 = percentile(samples, 0.95);
    res.p99 = percentile(samples, 0.99);
    res.minMs = *std::min_element(samples.begin(), samples.end());
    res.maxMs = *std::max_element(samples.begin(), samples.end());
    double sum = 0.0;
    for (double v : samples) sum += v;
    res.mean = sum / double(samples.size());

    if (res.thresholdGbps > 0.0)
    {
        double bytes = bench_->bytesMoved(kid);
        if (bytes > 0.0 && res.p50 > 0.0)
            res.gbPerSec = bytes / (res.p50 * 1e-3) / 1e9;
    }

    for (auto& m : byMinute)
        res.minuteP50.push_back(m.empty() ? 0.0 : percentile(m, 0.50));
    if (!res.minuteP50.empty())
    {
        res.firstMinuteP50 = res.minuteP50.front();
        res.lastMinuteP50 = res.minuteP50.back();
    }

    NXB_LOG("  %s: p50 %.3f  p95 %.3f  p99 %.3f ms over %d frames",
            res.name.c_str(), res.p50, res.p95, res.p99, res.frames);
    return res;
}

} // namespace nxb
