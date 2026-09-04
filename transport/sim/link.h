// nxvc-netsim - modelled links.  See transport/RESULTS.md for the scenarios.
#ifndef NXVC_SIM_LINK_H
#define NXVC_SIM_LINK_H

#include <cstdint>
#include <cstddef>
#include <random>
#include <string>

namespace nxsim {

struct LinkConfig {
    std::string name = "wifi";
    double capacity_bps = 300e6;
    uint32_t base_delay_us = 3000;
    // Jitter is a channel-access delay charged once per aggregate, not per
    // datagram: 802.11 delivers an A-MPDU in order, so datagrams on one path
    // never overtake each other.  The delay shifts the whole aggregate.
    double jitter_sigma_us = 400.0;
    uint32_t aggregate_bytes = 32 * 1024;
    double jitter_tail_p = 0.02;      // probability of a long access excursion
    uint32_t jitter_tail_us = 4000;

    double loss_random = 0.0;         // independent loss

    // Gilbert-Elliott burst loss: good -> bad and bad -> good per datagram.
    double p_good_bad = 0.0;
    double p_bad_good = 0.05;         // mean burst 20 datagrams
    double loss_in_bad = 1.0;

    // Bufferbloat: how deep the queue can get before tail drop.
    uint32_t queue_bytes_max = 256 * 1024;

    // Rare stalls (USB RNDIS/NCM).
    double stall_prob_per_datagram = 0.0;
    uint32_t stall_min_us = 20000;
    uint32_t stall_max_us = 50000;
};

class Link {
  public:
    Link(const LinkConfig& cfg, uint64_t seed) : cfg_(cfg), rng_(seed) {}

    // Offer a datagram of `bytes` at `now_us`.  Returns true if it will be
    // delivered, writing the arrival time; false if it was dropped.
    bool offer(uint64_t now_us, size_t bytes, uint64_t* arrive_us);

    const LinkConfig& cfg() const { return cfg_; }
    uint64_t offered() const { return offered_; }
    uint64_t dropped_loss() const { return dropped_loss_; }
    uint64_t dropped_queue() const { return dropped_queue_; }
    uint64_t stalls() const { return stalls_; }
    uint64_t max_queue_bytes() const { return max_queue_bytes_; }
    // Delivery rate estimate over the whole run, for the striper's weights.
    double delivered_bps(uint64_t span_us) const {
        return span_us ? double(delivered_bytes_) * 8.0 * 1e6 / double(span_us) : 0.0;
    }

  private:
    LinkConfig cfg_;
    std::mt19937_64 rng_;
    bool bad_ = false;
    uint64_t next_free_us_ = 0;
    uint64_t bytes_since_access_ = 0;
    uint64_t offered_ = 0, dropped_loss_ = 0, dropped_queue_ = 0, stalls_ = 0;
    uint64_t delivered_bytes_ = 0, max_queue_bytes_ = 0;
    double u01() {
        return double(rng_() >> 11) * (1.0 / 9007199254740992.0);
    }
};

}  // namespace nxsim

#endif
