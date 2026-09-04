// The headset-side UDP receive path.
//
// PAPER 4.11 claims "the receive interrupt sits on one core of the XR2 and
// saturates around 80k pps", and PAPER 4.1 puts the honest ceiling of the client
// receive path between 50k and 100k pps with recvmmsg. This class exists to
// measure that number rather than cite it.
//
// Everything the paper prescribes as a mitigation is here and each one reports
// whether the ROM actually granted it:
//   * recvmmsg with a batch of 64 (AppConfig::recv_batch)
//   * SO_RCVBUF raised as far as it will go, granted size read back (PAPER 4.11
//     "Request 8 MB, verify it was granted (vendor ROMs cap rmem_max)")
//   * SCHED_FIFO on the receive thread, graceful fallback to SCHED_OTHER
//   * affinity to the fastest core the ROM will let us have
//   * a lock-free ring into the decode thread, written by recvmmsg in place
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "nxc_config.h"
#include "nxc_ring.h"

namespace nxc {

// What the ROM actually allowed. Every field is reported on the HUD and in the
// stats report, because a self-test number without these is not interpretable.
struct ReceiveCaps {
    int      rcvbuf_requested = 0;
    int      rcvbuf_granted   = 0;   // getsockopt value; Linux reports 2x the real
                                     // buffer, so the usable half is granted/2
    bool     rcvbuf_forced    = false;  // SO_RCVBUFFORCE succeeded (needs CAP_NET_ADMIN)
    bool     sched_fifo       = false;
    int      sched_policy     = 0;      // the policy actually in force
    int      sched_priority   = 0;
    uint32_t affinity_mask    = 0;      // mask actually set, 0 = not set
    int      chosen_cpu       = -1;
    uint64_t chosen_cpu_khz   = 0;
    std::string notes;                  // human readable failure reasons
};

struct ReceiveStats {
    std::atomic<uint64_t> datagrams{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> batches{0};       // recvmmsg calls that returned > 0
    std::atomic<uint64_t> batch_msgs{0};    // to derive the mean batch fill
    std::atomic<uint64_t> drops_ring_full{0};
    std::atomic<uint64_t> recv_errors{0};
    std::atomic<uint64_t> wakeups{0};
};

class Receiver {
public:
    explicit Receiver(const AppConfig& cfg);
    ~Receiver();

    // Binds the socket and applies every tuning knob. Returns false only if the
    // socket cannot be created or bound; a knob that the ROM refuses is recorded
    // in caps() and is not an error.
    bool open();

    void start();
    void stop();

    SpscRing&           ring()  { return *ring_; }
    const ReceiveCaps&  caps()  const { return caps_; }
    ReceiveStats&       stats() { return stats_; }

    // Source of the most recent datagram, for feedback and stats reports.
    // Returns false until at least one datagram has arrived.
    bool peer(void* out_sockaddr, uint32_t* out_len) const;

    // Sends a packet back to the learned peer on the same socket (feedback,
    // TRANSPORT.md 8, and the self-test stats report).
    bool send_to_peer(const void* data, size_t len);

    // Kernel tid of the receive thread, so its CPU time can be sampled on its
    // own (the "single core receive" risk of PAPER 4.11 is a per-thread claim,
    // not a per-process one). 0 until the thread has started.
    int rx_tid() const { return rx_tid_.load(std::memory_order_relaxed); }

    // /proc/net/snmp Udp: RcvbufErrors and InErrors. These are the kernel's own
    // account of what the receive path dropped, which is the number that decides
    // whether a pps ceiling is the socket buffer or the CPU.
    struct SnmpUdp { uint64_t in_errors = 0; uint64_t rcvbuf_errors = 0; bool ok = false; };
    static SnmpUdp read_snmp_udp();

private:
    void thread_main();
    void apply_thread_tuning();

    AppConfig                 cfg_;
    int                       fd_ = -1;
    std::unique_ptr<SpscRing> ring_;
    ReceiveCaps               caps_;
    ReceiveStats              stats_;
    std::thread               thread_;
    std::atomic<bool>         running_{false};
    std::atomic<int>          rx_tid_{0};

    // Peer address, published by the receive thread and read by the app thread.
    mutable std::atomic<uint32_t> peer_len_{0};
    uint8_t                       peer_addr_[28] = {};
};

// ---------------------------------------------------------------- CPU sampling

// Reads utime+stime for a task from /proc. `tid` of 0 means the whole process.
// Returns ticks, or UINT64_MAX on failure.
uint64_t read_cpu_ticks(int tid);
long     clock_ticks_per_sec();

// Picks the fastest CPU the process is allowed to run on, by
// cpufreq/cpuinfo_max_freq. On a Pixel 7 (Tensor G2, 4x A55 + 2x A78 + 2x X1)
// this lands on an X1; on an XR2 Gen 1 it lands on the prime Kryo. Returns -1 if
// cpufreq is not readable, in which case the caller leaves affinity alone rather
// than guessing a topology.
int pick_fastest_cpu(uint64_t* out_khz);

}  // namespace nxc
