// commands.cpp - pools, one-shot submits, persistent buffers, semaphores,
// timestamp query pools.
#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <cstdio>
#include <utility>

namespace nxvc::vk {

// -------------------------------------------------------------- CommandPool
CommandPool::CommandPool(const Context& ctx, bool resettable, bool transient)
    : ctx_(&ctx) {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = ctx.queueFamily();
    if (resettable) ci.flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (transient) ci.flags |= VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    NXVC_VK_CHECK(vkCreateCommandPool(ctx.device(), &ci, nullptr, &pool_));
}

void CommandPool::destroy() noexcept {
    if (ctx_ && pool_) vkDestroyCommandPool(ctx_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

CommandPool::~CommandPool() { destroy(); }

CommandPool::CommandPool(CommandPool&& o) noexcept : ctx_(o.ctx_), pool_(o.pool_) {
    o.ctx_ = nullptr;
    o.pool_ = VK_NULL_HANDLE;
}

CommandPool& CommandPool::operator=(CommandPool&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        pool_ = o.pool_;
        o.ctx_ = nullptr;
        o.pool_ = VK_NULL_HANDLE;
    }
    return *this;
}

VkCommandBuffer CommandPool::allocate(VkCommandBufferLevel level) const {
    return allocate(1, level)[0];
}

std::vector<VkCommandBuffer> CommandPool::allocate(uint32_t count,
                                                   VkCommandBufferLevel level) const {
    std::vector<VkCommandBuffer> out(count);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = level;
    ai.commandBufferCount = count;
    NXVC_VK_CHECK(vkAllocateCommandBuffers(ctx_->device(), &ai, out.data()));
    return out;
}

void CommandPool::free(std::span<const VkCommandBuffer> bufs) const {
    if (bufs.empty()) return;
    vkFreeCommandBuffers(ctx_->device(), pool_, static_cast<uint32_t>(bufs.size()),
                         bufs.data());
}

void CommandPool::reset(bool release_resources) const {
    NXVC_VK_CHECK(vkResetCommandPool(
        ctx_->device(), pool_,
        release_resources ? VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT : 0));
}

// ------------------------------------------------------------------ OneShot
OneShot::OneShot(const Context& ctx) : ctx_(&ctx) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = ctx.queueFamily();
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    NXVC_VK_CHECK(vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool_));

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    NXVC_VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &ai, &cmd_));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    NXVC_VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
}

void OneShot::submitAndWait(uint64_t timeout_ns) {
    if (submitted_) return;
    submitted_ = true;
    NXVC_VK_CHECK(vkEndCommandBuffer(cmd_));

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    NXVC_VK_CHECK(vkCreateFence(ctx_->device(), &fci, nullptr, &fence));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    const VkResult sr = vkQueueSubmit(ctx_->queue(), 1, &si, fence);
    if (sr == VK_SUCCESS) {
        const VkResult wr = vkWaitForFences(ctx_->device(), 1, &fence, VK_TRUE, timeout_ns);
        vkDestroyFence(ctx_->device(), fence, nullptr);
        if (wr != VK_SUCCESS) throwVk(wr, "vkWaitForFences", __FILE__, __LINE__);
    } else {
        vkDestroyFence(ctx_->device(), fence, nullptr);
        throwVk(sr, "vkQueueSubmit", __FILE__, __LINE__);
    }
}

OneShot::~OneShot() {
    if (!submitted_) {
        // Do not throw out of a destructor; a failed setup submit shows up as
        // a device-lost or a validation error the caller will see anyway.
        try {
            submitAndWait();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[nxvc-vk] one-shot submit failed: %s\n", e.what());
        }
    }
    if (pool_) vkDestroyCommandPool(ctx_->device(), pool_, nullptr);
}

void immediate(const Context& ctx, const std::function<void(VkCommandBuffer)>& fn) {
    OneShot one(ctx);
    fn(one.cmd());
    one.submitAndWait();
}

// -------------------------------------------------------- PersistentCommands
PersistentCommands::PersistentCommands(const Context& ctx, uint32_t slots,
                                       bool resettable)
    : pool_(ctx, resettable, false) {
    if (slots == 0) throw Error(NXVC_VK_ERR_ARG, "zero command slots");
    buffers_ = pool_.allocate(slots);
}

void PersistentCommands::record(uint32_t slot,
                                const std::function<void(VkCommandBuffer)>& fn,
                                VkCommandBufferUsageFlags flags) const {
    VkCommandBuffer cmd = buffers_.at(slot);
    NXVC_VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = flags;
    NXVC_VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    fn(cmd);
    NXVC_VK_CHECK(vkEndCommandBuffer(cmd));
}

// --------------------------------------------------------- TimelineSemaphore
TimelineSemaphore::TimelineSemaphore(const Context& ctx, uint64_t initial,
                                     bool exportable)
    : ctx_(&ctx) {
    if (!(ctx.probe().caps & NXVC_VK_CAP_TIMELINE_SEMAPHORE))
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "device has no timelineSemaphore feature");

    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue = initial;

    VkExportSemaphoreCreateInfo eci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
#if defined(_WIN32)
    eci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
#else
    // SYNC_FD cannot carry a timeline; opaque fd can.
    eci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    if (exportable) tci.pNext = &eci;

    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    ci.pNext = &tci;
    NXVC_VK_CHECK(vkCreateSemaphore(ctx.device(), &ci, nullptr, &sem_));
}

void TimelineSemaphore::destroy() noexcept {
    if (ctx_ && sem_) vkDestroySemaphore(ctx_->device(), sem_, nullptr);
    sem_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

TimelineSemaphore::~TimelineSemaphore() { destroy(); }

TimelineSemaphore::TimelineSemaphore(TimelineSemaphore&& o) noexcept
    : ctx_(o.ctx_), sem_(o.sem_) {
    o.ctx_ = nullptr;
    o.sem_ = VK_NULL_HANDLE;
}

TimelineSemaphore& TimelineSemaphore::operator=(TimelineSemaphore&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        sem_ = o.sem_;
        o.ctx_ = nullptr;
        o.sem_ = VK_NULL_HANDLE;
    }
    return *this;
}

uint64_t TimelineSemaphore::value() const {
    uint64_t v = 0;
    const auto fn = ctx_->fns().getSemaphoreCounterValue;
    if (!fn) throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkGetSemaphoreCounterValue absent");
    NXVC_VK_CHECK(fn(ctx_->device(), sem_, &v));
    return v;
}

void TimelineSemaphore::signalHost(uint64_t value) const {
    const auto fn = ctx_->fns().signalSemaphore;
    if (!fn) throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkSignalSemaphore absent");
    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    si.semaphore = sem_;
    si.value = value;
    NXVC_VK_CHECK(fn(ctx_->device(), &si));
}

bool TimelineSemaphore::wait(uint64_t value, uint64_t timeout_ns) const {
    const auto fn = ctx_->fns().waitSemaphores;
    if (!fn) throw Error(NXVC_VK_ERR_UNSUPPORTED, "vkWaitSemaphores absent");
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem_;
    wi.pValues = &value;
    const VkResult r = fn(ctx_->device(), &wi, timeout_ns);
    if (r == VK_TIMEOUT) return false;
    if (r != VK_SUCCESS) throwVk(r, "vkWaitSemaphores", __FILE__, __LINE__);
    return true;
}

void TimelineSemaphore::submit(VkCommandBuffer cmd, uint64_t wait_value,
                               uint64_t signal_value,
                               VkPipelineStageFlags wait_stage) const {
    VkTimelineSemaphoreSubmitInfo tsi{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.waitSemaphoreValueCount = wait_value ? 1u : 0u;
    tsi.pWaitSemaphoreValues = wait_value ? &wait_value : nullptr;
    tsi.signalSemaphoreValueCount = signal_value ? 1u : 0u;
    tsi.pSignalSemaphoreValues = signal_value ? &signal_value : nullptr;

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &tsi;
    si.waitSemaphoreCount = tsi.waitSemaphoreValueCount;
    si.pWaitSemaphores = wait_value ? &sem_ : nullptr;
    si.pWaitDstStageMask = wait_value ? &wait_stage : nullptr;
    si.commandBufferCount = cmd ? 1u : 0u;
    si.pCommandBuffers = cmd ? &cmd : nullptr;
    si.signalSemaphoreCount = tsi.signalSemaphoreValueCount;
    si.pSignalSemaphores = signal_value ? &sem_ : nullptr;
    NXVC_VK_CHECK(vkQueueSubmit(ctx_->queue(), 1, &si, VK_NULL_HANDLE));
}

// ----------------------------------------------------------- BinarySemaphore
BinarySemaphore::BinarySemaphore(const Context& ctx, bool exportable) : ctx_(&ctx) {
    VkExportSemaphoreCreateInfo eci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
#if defined(_WIN32)
    eci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    eci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
#endif
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (exportable) ci.pNext = &eci;
    NXVC_VK_CHECK(vkCreateSemaphore(ctx.device(), &ci, nullptr, &sem_));
}

void BinarySemaphore::destroy() noexcept {
    if (ctx_ && sem_) vkDestroySemaphore(ctx_->device(), sem_, nullptr);
    sem_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

BinarySemaphore::~BinarySemaphore() { destroy(); }

BinarySemaphore::BinarySemaphore(BinarySemaphore&& o) noexcept
    : ctx_(o.ctx_), sem_(o.sem_) {
    o.ctx_ = nullptr;
    o.sem_ = VK_NULL_HANDLE;
}

BinarySemaphore& BinarySemaphore::operator=(BinarySemaphore&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        sem_ = o.sem_;
        o.ctx_ = nullptr;
        o.sem_ = VK_NULL_HANDLE;
    }
    return *this;
}

// ------------------------------------------------------------ TimestampPool
TimestampPool::TimestampPool(const Context& ctx, std::vector<std::string> pass_names,
                             uint32_t slots)
    : ctx_(&ctx), names_(std::move(pass_names)), slots_(slots) {
    if (names_.empty() || slots_ == 0)
        throw Error(NXVC_VK_ERR_ARG, "TimestampPool needs passes and slots");
    if (!(ctx.probe().caps & NXVC_VK_CAP_TIMESTAMP_QUERY)) {
        // Not an error: a device without timestamps still decodes, it just
        // reports no per-pass timing.  valid() stays false and every record
        // call is a no-op.
        return;
    }
    period_ns_ = ctx.probe().timestamp_period_ns;
    const uint32_t bits = ctx.probe().timestamp_valid_bits;
    ts_mask_ = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);

    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = slots_ * static_cast<uint32_t>(names_.size()) * 2u;
    NXVC_VK_CHECK(vkCreateQueryPool(ctx.device(), &ci, nullptr, &pool_));
}

void TimestampPool::destroy() noexcept {
    if (ctx_ && pool_) vkDestroyQueryPool(ctx_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

TimestampPool::~TimestampPool() { destroy(); }

TimestampPool::TimestampPool(TimestampPool&& o) noexcept
    : ctx_(o.ctx_), pool_(o.pool_), names_(std::move(o.names_)), slots_(o.slots_),
      period_ns_(o.period_ns_), ts_mask_(o.ts_mask_) {
    o.ctx_ = nullptr;
    o.pool_ = VK_NULL_HANDLE;
}

TimestampPool& TimestampPool::operator=(TimestampPool&& o) noexcept {
    if (this != &o) {
        destroy();
        ctx_ = o.ctx_;
        pool_ = o.pool_;
        names_ = std::move(o.names_);
        slots_ = o.slots_;
        period_ns_ = o.period_ns_;
        ts_mask_ = o.ts_mask_;
        o.ctx_ = nullptr;
        o.pool_ = VK_NULL_HANDLE;
    }
    return *this;
}

void TimestampPool::reset(VkCommandBuffer cmd, uint32_t slot) const {
    if (!valid()) return;
    const uint32_t per_slot = static_cast<uint32_t>(names_.size()) * 2u;
    vkCmdResetQueryPool(cmd, pool_, slot * per_slot, per_slot);
}

void TimestampPool::begin(VkCommandBuffer cmd, uint32_t slot, uint32_t pass,
                          VkPipelineStageFlagBits stage) const {
    if (!valid()) return;
    const uint32_t per_slot = static_cast<uint32_t>(names_.size()) * 2u;
    vkCmdWriteTimestamp(cmd, stage, pool_, slot * per_slot + pass * 2u);
}

void TimestampPool::end(VkCommandBuffer cmd, uint32_t slot, uint32_t pass,
                        VkPipelineStageFlagBits stage) const {
    if (!valid()) return;
    const uint32_t per_slot = static_cast<uint32_t>(names_.size()) * 2u;
    vkCmdWriteTimestamp(cmd, stage, pool_, slot * per_slot + pass * 2u + 1u);
}

std::optional<TimingReport> TimestampPool::read(uint32_t slot, bool wait) const {
    if (!valid()) return std::nullopt;
    const uint32_t n = static_cast<uint32_t>(names_.size()) * 2u;
    std::vector<uint64_t> raw(n);
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
    if (wait) flags |= VK_QUERY_RESULT_WAIT_BIT;
    const VkResult r = vkGetQueryPoolResults(
        ctx_->device(), pool_, slot * n, n, raw.size() * sizeof(uint64_t), raw.data(),
        sizeof(uint64_t), flags);
    if (r == VK_NOT_READY) return std::nullopt;
    if (r != VK_SUCCESS) throwVk(r, "vkGetQueryPoolResults", __FILE__, __LINE__);

    TimingReport rep;
    rep.passes.reserve(names_.size());
    for (size_t i = 0; i < names_.size(); ++i) {
        const uint64_t a = raw[i * 2] & ts_mask_;
        const uint64_t b = raw[i * 2 + 1] & ts_mask_;
        // Timestamps wrap at timestampValidBits; the delta is still correct
        // modulo the mask as long as the pass is shorter than the wrap period.
        const uint64_t d = (b - a) & ts_mask_;
        const double ms = static_cast<double>(d) * period_ns_ * 1e-6;
        rep.passes.push_back(PassTiming{names_[i], ms});
        rep.total_ms += ms;
    }
    return rep;
}

std::string TimingReport::toString() const {
    std::string o;
    char b[160];
    for (const auto& p : passes) {
        std::snprintf(b, sizeof b, "  %-20s %8.3f ms\n", p.name.c_str(), p.ms);
        o += b;
    }
    std::snprintf(b, sizeof b, "  %-20s %8.3f ms\n", "total", total_ms);
    o += b;
    return o;
}

std::string TimingReport::toJson() const {
    std::string o = "{\"passes\": [";
    char b[192];
    for (size_t i = 0; i < passes.size(); ++i) {
        std::snprintf(b, sizeof b, "%s{\"name\": \"%s\", \"ms\": %.6f}", i ? ", " : "",
                      jsonEscape(passes[i].name).c_str(), passes[i].ms);
        o += b;
    }
    std::snprintf(b, sizeof b, "], \"total_ms\": %.6f}", total_ms);
    o += b;
    return o;
}

}  // namespace nxvc::vk
