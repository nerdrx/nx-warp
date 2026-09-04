// nxvc/vk/commands.hpp - command pools, one-shot submits, persistent buffers,
// timeline semaphores and timestamp query pools.
//
// The per-frame cost target in 3.6 is under 300 us of CPU: one vkQueueSubmit
// of *pre-recorded* command buffers, push constants for the frame data, no
// descriptor updates.  So the persistent path is the important one; the
// one-shot helper is for setup and tests only.
#pragma once

#include <nxvc/vk/context.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nxvc::vk {

// ------------------------------------------------------------------- pools
class CommandPool {
public:
    CommandPool() = default;
    // `resettable` gives VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    // `transient` marks the pool for short-lived one-shot buffers.
    CommandPool(const Context& ctx, bool resettable = true, bool transient = false);
    ~CommandPool();
    CommandPool(CommandPool&&) noexcept;
    CommandPool& operator=(CommandPool&&) noexcept;
    CommandPool(const CommandPool&) = delete;
    CommandPool& operator=(const CommandPool&) = delete;

    [[nodiscard]] VkCommandPool handle() const noexcept { return pool_; }
    [[nodiscard]] VkCommandBuffer allocate(
        VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) const;
    [[nodiscard]] std::vector<VkCommandBuffer> allocate(uint32_t count,
        VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) const;
    void free(std::span<const VkCommandBuffer> bufs) const;
    void reset(bool release_resources = false) const;

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------- one-shot
// Record, submit, fence-wait, free.  Setup only.
class OneShot {
public:
    explicit OneShot(const Context& ctx);
    ~OneShot();
    OneShot(const OneShot&) = delete;
    OneShot& operator=(const OneShot&) = delete;

    [[nodiscard]] VkCommandBuffer cmd() const noexcept { return cmd_; }
    operator VkCommandBuffer() const noexcept { return cmd_; }  // NOLINT
    // Ends recording, submits and waits.  Called by the destructor if the
    // caller did not; call it explicitly when you want the exception.
    void submitAndWait(uint64_t timeout_ns = 5'000'000'000ull);

private:
    const Context* ctx_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool submitted_ = false;
};

// Convenience: run `fn` inside a one-shot submit.
void immediate(const Context& ctx, const std::function<void(VkCommandBuffer)>& fn);

// -------------------------------------------------------------- persistent
// A ring of N pre-recorded primary command buffers, one per in-flight frame
// slot.  Record once at stream start, submit forever.
class PersistentCommands {
public:
    PersistentCommands() = default;
    PersistentCommands(const Context& ctx, uint32_t slots, bool resettable = true);

    [[nodiscard]] uint32_t slots() const noexcept {
        return static_cast<uint32_t>(buffers_.size());
    }
    [[nodiscard]] VkCommandBuffer at(uint32_t slot) const { return buffers_[slot]; }

    // Re-record one slot.  `fn` gets a command buffer already in the recording
    // state; End is called for you.
    void record(uint32_t slot, const std::function<void(VkCommandBuffer)>& fn,
                VkCommandBufferUsageFlags flags = 0) const;

private:
    CommandPool pool_;
    std::vector<VkCommandBuffer> buffers_;
};

// ------------------------------------------------------------- timeline sem
// 3.6: the compositor signals value F when frame F is rendered; each encoder
// row group signals 8F + g and the network thread waits on the next value.
class TimelineSemaphore {
public:
    TimelineSemaphore() = default;
    explicit TimelineSemaphore(const Context& ctx, uint64_t initial = 0,
                               bool exportable = false);
    ~TimelineSemaphore();
    TimelineSemaphore(TimelineSemaphore&&) noexcept;
    TimelineSemaphore& operator=(TimelineSemaphore&&) noexcept;
    TimelineSemaphore(const TimelineSemaphore&) = delete;
    TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;

    [[nodiscard]] VkSemaphore handle() const noexcept { return sem_; }
    [[nodiscard]] uint64_t value() const;
    void signalHost(uint64_t value) const;
    // Returns false on timeout rather than throwing: a decoder that misses a
    // deadline conceals, it does not abort (4.6.1).
    [[nodiscard]] bool wait(uint64_t value, uint64_t timeout_ns = UINT64_MAX) const;

    // Submit `cmd` waiting for `wait_value` at `wait_stage` and signalling
    // `signal_value`.  Either value may be 0 to skip that half.
    void submit(VkCommandBuffer cmd, uint64_t wait_value, uint64_t signal_value,
                VkPipelineStageFlags wait_stage =
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) const;

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    VkSemaphore sem_ = VK_NULL_HANDLE;
};

// A plain binary semaphore, for the imported sync fds of 3.5.
class BinarySemaphore {
public:
    BinarySemaphore() = default;
    explicit BinarySemaphore(const Context& ctx, bool exportable = false);
    ~BinarySemaphore();
    BinarySemaphore(BinarySemaphore&&) noexcept;
    BinarySemaphore& operator=(BinarySemaphore&&) noexcept;
    BinarySemaphore(const BinarySemaphore&) = delete;
    BinarySemaphore& operator=(const BinarySemaphore&) = delete;
    [[nodiscard]] VkSemaphore handle() const noexcept { return sem_; }

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    VkSemaphore sem_ = VK_NULL_HANDLE;
};

// ----------------------------------------------------------------- timing
// VK_QUERY_TYPE_TIMESTAMP pairs around each dispatch, timestampPeriod
// applied, as specified for the Phase 0 gate in 3.4.  The pool holds
// `slots` frames of `passes` pairs so the host never stalls the GPU to read
// back: results are collected `slots-1` frames late.
struct PassTiming {
    std::string name;
    double ms = 0.0;
};

struct TimingReport {
    std::vector<PassTiming> passes;
    double total_ms = 0.0;
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string toJson() const;
};

class TimestampPool {
public:
    TimestampPool() = default;
    // `pass_names` fixes the pass count and the report labels.
    TimestampPool(const Context& ctx, std::vector<std::string> pass_names,
                  uint32_t slots = 3);
    ~TimestampPool();
    TimestampPool(TimestampPool&&) noexcept;
    TimestampPool& operator=(TimestampPool&&) noexcept;
    TimestampPool(const TimestampPool&) = delete;
    TimestampPool& operator=(const TimestampPool&) = delete;

    [[nodiscard]] bool valid() const noexcept { return pool_ != VK_NULL_HANDLE; }
    [[nodiscard]] uint32_t passCount() const noexcept {
        return static_cast<uint32_t>(names_.size());
    }

    // Record at the start of the slot's command buffer.
    void reset(VkCommandBuffer cmd, uint32_t slot) const;
    void begin(VkCommandBuffer cmd, uint32_t slot, uint32_t pass,
               VkPipelineStageFlagBits stage =
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) const;
    void end(VkCommandBuffer cmd, uint32_t slot, uint32_t pass,
             VkPipelineStageFlagBits stage =
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) const;

    // Reads back one slot.  `wait` blocks until the results are available;
    // without it an unavailable slot yields std::nullopt and the caller tries
    // again next frame.
    [[nodiscard]] std::optional<TimingReport> read(uint32_t slot, bool wait = false) const;

    // RAII scope: begin on construction, end on destruction.
    class Scope {
    public:
        Scope(const TimestampPool& p, VkCommandBuffer cmd, uint32_t slot, uint32_t pass)
            : p_(&p), cmd_(cmd), slot_(slot), pass_(pass) {
            if (p_->valid()) p_->begin(cmd_, slot_, pass_);
        }
        ~Scope() { if (p_->valid()) p_->end(cmd_, slot_, pass_); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        const TimestampPool* p_;
        VkCommandBuffer cmd_;
        uint32_t slot_, pass_;
    };

private:
    void destroy() noexcept;
    const Context* ctx_ = nullptr;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    std::vector<std::string> names_;
    uint32_t slots_ = 0;
    double period_ns_ = 1.0;
    uint64_t ts_mask_ = ~0ull;
};

}  // namespace nxvc::vk
