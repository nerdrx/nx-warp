#include "link.h"

#include <algorithm>
#include <cmath>

namespace nxsim {

bool Link::offer(uint64_t now_us, size_t bytes, uint64_t* arrive_us) {
    ++offered_;

    // Gilbert-Elliott state transition, one step per offered datagram.
    if (bad_) {
        if (u01() < cfg_.p_bad_good) bad_ = false;
    } else {
        if (u01() < cfg_.p_good_bad) bad_ = true;
    }

    // Rare link stalls: the serialisation clock jumps forward.
    if (cfg_.stall_prob_per_datagram > 0.0 && u01() < cfg_.stall_prob_per_datagram) {
        uint32_t span = cfg_.stall_max_us - cfg_.stall_min_us;
        uint32_t stall = cfg_.stall_min_us + uint32_t(u01() * double(span));
        next_free_us_ = std::max(next_free_us_, now_us) + stall;
        ++stalls_;
    }

    // Queue depth in bytes at the head of the queue.
    uint64_t start = std::max(now_us, next_free_us_);
    uint64_t backlog_us = start - now_us;
    uint64_t backlog_bytes =
        uint64_t(double(backlog_us) * cfg_.capacity_bps / 8.0 / 1e6);
    max_queue_bytes_ = std::max(max_queue_bytes_, backlog_bytes);
    if (backlog_bytes + bytes > cfg_.queue_bytes_max) {
        ++dropped_queue_;
        return false;
    }

    double p = bad_ ? cfg_.loss_in_bad : cfg_.loss_random;
    if (p > 0.0 && u01() < p) {
        ++dropped_loss_;
        // A dropped datagram still occupies the air / the wire.
        next_free_us_ = start + uint64_t(double(bytes) * 8.0 * 1e6 / cfg_.capacity_bps);
        return false;
    }

    uint64_t serialize = uint64_t(double(bytes) * 8.0 * 1e6 / cfg_.capacity_bps);
    next_free_us_ = start + serialize;

    double jitter = 0.0;
    if (cfg_.jitter_sigma_us > 0.0) {
        // Half-normal jitter: delays only, never early.
        double u = std::max(1e-12, u01());
        double v = u01();
        double g = std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
        jitter = std::fabs(g) * cfg_.jitter_sigma_us;
    }
    if (cfg_.jitter_tail_p > 0.0 && u01() < cfg_.jitter_tail_p)
        jitter += u01() * double(cfg_.jitter_tail_us);

    delivered_bytes_ += bytes;
    *arrive_us = next_free_us_ + cfg_.base_delay_us + uint64_t(jitter);
    return true;
}

}  // namespace nxsim
