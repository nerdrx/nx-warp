// nxvc_transport - latency telemetry.  Normative: docs/TRANSPORT.md 11, PAPER 4.9.
#ifndef NXVC_TRANSPORT_TELEMETRY_H
#define NXVC_TRANSPORT_TELEMETRY_H

#include <algorithm>
#include <deque>

#include "nxvc/transport/common.h"

namespace nxt {

struct BandStamps {
    uint16_t frame_id = 0;
    uint8_t band = 0;
    uint8_t path_id = 0;
    // server clock
    uint32_t render_finish_ts = 0;
    uint32_t encode_finish_ts = 0;
    uint32_t first_tx_ts = 0;
    uint32_t last_tx_ts = 0;
    // client clock
    uint32_t first_rx_ts = 0;
    uint32_t last_rx_ts = 0;
    uint32_t decode_finish_ts = 0;
    uint32_t deadline_ts = 0;
    uint16_t decode_us = 0;
    int32_t clock_offset_us = 0;  // client - server, per path, from the integration

    int32_t encode_us() const { return int32_t(encode_finish_ts - render_finish_ts); }
    int32_t queue_us() const { return int32_t(first_tx_ts - encode_finish_ts); }
    int32_t air_us() const {
        return int32_t(first_rx_ts - first_tx_ts) - clock_offset_us;
    }
    int32_t spread_us() const { return int32_t(last_rx_ts - first_rx_ts); }
    int32_t margin_us() const { return int32_t(deadline_ts - decode_finish_ts); }
    // Render finish to band decoded, in client-clock terms.
    int32_t band_latency_us() const {
        return int32_t(decode_finish_ts - render_finish_ts) - clock_offset_us;
    }
};

class Percentiles {
  public:
    explicit Percentiles(size_t window = 512) : window_(window) {}
    void add(int32_t v) {
        v_.push_back(v);
        if (v_.size() > window_) v_.pop_front();
    }
    size_t count() const { return v_.size(); }
    int32_t pct(double p) const {
        if (v_.empty()) return 0;
        std::vector<int32_t> s(v_.begin(), v_.end());
        std::sort(s.begin(), s.end());
        size_t i = size_t(p * double(s.size() - 1) + 0.5);
        return s[std::min(i, s.size() - 1)];
    }
    int32_t p50() const { return pct(0.50); }
    int32_t p99() const { return pct(0.99); }
    double mean() const {
        if (v_.empty()) return 0;
        double t = 0;
        for (int32_t x : v_) t += x;
        return t / double(v_.size());
    }

  private:
    size_t window_;
    std::deque<int32_t> v_;
};

class Telemetry {
  public:
    void add(const BandStamps& s) {
        stamps_.push_back(s);
        if (stamps_.size() > 4096) stamps_.pop_front();
        encode.add(s.encode_us());
        queue.add(s.queue_us());
        air.add(s.air_us());
        spread.add(s.spread_us());
        decode.add(int32_t(s.decode_us));
        margin.add(s.margin_us());
        band_latency.add(s.band_latency_us());
    }
    Percentiles encode, queue, air, spread, decode, margin, band_latency;
    const std::deque<BandStamps>& stamps() const { return stamps_; }

  private:
    std::deque<BandStamps> stamps_;
};

}  // namespace nxt

#endif
