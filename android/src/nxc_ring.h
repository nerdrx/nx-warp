// Single-producer / single-consumer datagram ring.
//
// The point of this class is that it is also the *receive buffer*: the producer
// claims a contiguous span of slots, points recvmmsg's iovecs straight at them,
// and publishes what the kernel actually delivered. There is no copy between the
// socket and the depacketizer, which matters at the 50-100 kpps the receive path
// is being measured against (PAPER 4.1, 4.11).
//
// Producer: the receive thread. Consumer: the decode thread. No mutexes, no
// allocation after construction, no false sharing between the two indices.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace nxc {

class SpscRing {
public:
    // `slots` must be a power of two so the wrap is a mask.
    SpscRing(uint32_t slots, uint32_t slot_bytes)
        : slots_(slots), mask_(slots - 1), slot_bytes_(slot_bytes) {
        // 64-byte aligned so a slot never straddles a cache line boundary it
        // does not have to.
        data_ = static_cast<uint8_t*>(std::aligned_alloc(64, size_t(slots) * slot_bytes));
        len_  = static_cast<uint32_t*>(std::aligned_alloc(64, size_t(slots) * sizeof(uint32_t)));
        src_  = static_cast<SrcAddr*>(std::aligned_alloc(64, size_t(slots) * sizeof(SrcAddr)));
        if (data_ && len_) {
            // Touch every page once now rather than taking the faults inside the
            // first burst of recvmmsg.
            std::memset(data_, 0, size_t(slots) * slot_bytes);
            std::memset(len_, 0, size_t(slots) * sizeof(uint32_t));
        }
    }
    ~SpscRing() { std::free(data_); std::free(len_); std::free(src_); }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    bool valid() const { return data_ && len_ && src_; }

    // 16 bytes is enough for a sockaddr_in; IPv6 sources are recorded truncated
    // and only used to pick the feedback destination, which the app also learns
    // from the first datagram via a full recvfrom.
    struct SrcAddr { uint8_t bytes[28]; uint32_t len; };

    uint32_t slot_bytes() const { return slot_bytes_; }
    uint32_t slots() const { return slots_; }

    // ------------------------------------------------------------ producer

    // Number of contiguous free slots starting at the write cursor, capped at
    // `want` and at the distance to the end of the buffer (so the caller's
    // iovecs are all inside one linear span).
    uint32_t claim(uint32_t want) {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        uint32_t free_slots = slots_ - uint32_t(tail - head);
        if (free_slots == 0) return 0;
        // Leave one slot empty so head == tail always means "empty".
        free_slots -= 1;
        const uint32_t to_end = slots_ - uint32_t(tail & mask_);
        uint32_t n = want;
        if (n > free_slots) n = free_slots;
        if (n > to_end) n = to_end;
        return n;
    }

    // Base pointer of the claimed span (only valid up to claim()'s return).
    uint8_t*  claim_data() { return data_ + size_t(tail_.load(std::memory_order_relaxed) & mask_) * slot_bytes_; }
    uint32_t* claim_lens() { return len_  + size_t(tail_.load(std::memory_order_relaxed) & mask_); }
    SrcAddr*  claim_srcs() { return src_  + size_t(tail_.load(std::memory_order_relaxed) & mask_); }

    // Make `n` slots visible to the consumer. Lengths must already be stored.
    void publish(uint32_t n) {
        tail_.store(tail_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

    // ------------------------------------------------------------ consumer

    uint32_t available() const {
        return uint32_t(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_relaxed));
    }

    const uint8_t* peek(uint32_t i, uint32_t* out_len, const SrcAddr** out_src = nullptr) const {
        const uint64_t idx = head_.load(std::memory_order_relaxed) + i;
        const size_t s = size_t(idx & mask_);
        *out_len = len_[s];
        if (out_src) *out_src = &src_[s];
        return data_ + s * slot_bytes_;
    }

    void consume(uint32_t n) {
        head_.store(head_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

    // Depth in slots, for the HUD. Racy by construction and that is fine.
    uint32_t depth() const { return available(); }

private:
    uint8_t*   data_ = nullptr;
    uint32_t*  len_  = nullptr;
    SrcAddr*   src_  = nullptr;
    uint32_t   slots_;
    uint32_t   mask_;
    uint32_t   slot_bytes_;

    // On separate cache lines: the producer spins on tail_ and reads head_ once
    // per batch, the consumer does the mirror image. Sharing a line here costs
    // a coherence miss per datagram at 90 kpps.
    alignas(64) std::atomic<uint64_t> head_{0};  // consumer cursor
    alignas(64) std::atomic<uint64_t> tail_{0};  // producer cursor
};

}  // namespace nxc
