// nxvc_transport - multipath striper.  Normative: docs/TRANSPORT.md 10.
#ifndef NXVC_TRANSPORT_MULTIPATH_H
#define NXVC_TRANSPORT_MULTIPATH_H

#include "nxvc/transport/packetizer.h"

namespace nxt {

struct PathInfo {
    bool configured = false;
    bool up = true;
    double rate_bps = 100e6;   // measured delivery rate (BBR estimate)
    uint32_t rtt_us = 0;
    uint32_t rtt_base_us = 0;  // minimum filtered
    uint64_t last_rx_us = 0;
    bool stalled = false;
    int probe_bands = 0;       // consecutive good bands while probing
    // Counters
    uint64_t datagrams = 0, bytes = 0;
};

class Striper {
  public:
    explicit Striper(const StreamConfig& cfg) : cfg_(cfg) {}

    void configure_path(uint8_t id, double rate_bps, uint32_t rtt_us);
    void update_rate(uint8_t id, double rate_bps) {
        if (id < kMaxPaths) paths_[id].rate_bps = rate_bps;
    }
    void update_rtt(uint8_t id, uint32_t rtt_us);
    // Called once per band: `now_us` is the sender clock.  A path with no traffic
    // received for 20 ms while another flows is stalled (TRANSPORT.md 10).
    void tick_band(uint64_t now_us);
    void note_rx(uint8_t id, uint64_t now_us);

    int up_paths() const;
    bool duplicate_class_a() const { return dup_a_; }
    const PathInfo& path(uint8_t id) const { return paths_[id]; }

    // Decide duplication for the coming band from its byte totals.
    void begin_band(size_t band_bytes, size_t class_a_bytes, uint32_t band_period_us);

    // Paths this unit is sent on.  Class A may be duplicated; B and C get one.
    std::vector<uint8_t> assign(const SendUnit& u);

  private:
    StreamConfig cfg_;
    PathInfo paths_[kMaxPaths];
    double sent_[kMaxPaths] = {0, 0};  // weighted-least-loaded accumulator
    bool dup_a_ = false;
};

}  // namespace nxt

#endif
