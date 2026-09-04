// nxvc_transport - row-band scheduler and pacer.  Normative: PAPER 4.2, 4.6.
#ifndef NXVC_TRANSPORT_SCHEDULER_H
#define NXVC_TRANSPORT_SCHEDULER_H

#include "nxvc/transport/common.h"

namespace nxt {

// Spreads a band's datagrams over its share of the frame period ("pacing spreads
// a band's datagrams over its encode time", PAPER 4.2), and stretches the send
// window when the frame overruns its bit budget by more than 15 % (PAPER 4.6).
class BandScheduler {
  public:
    struct Plan {
        uint32_t start_us = 0;
        uint32_t interval_us = 0;
        uint32_t span_us = 0;
        size_t datagrams = 0;
        uint32_t tx_at(size_t i) const {
            return start_us + uint32_t(interval_us * i);
        }
    };

    explicit BandScheduler(const StreamConfig& cfg) : cfg_(cfg) {}

    void begin_frame(uint32_t render_finish_us, uint32_t frame_bit_budget) {
        frame_start_us_ = render_finish_us;
        budget_bits_ = frame_bit_budget;
        frame_bits_ = 0;
        stretch_ = 1.0;
    }

    // Pacing span for one band.  Defaults to the frame period divided by the
    // band count; the integration sets it to the band's encode time (PAPER 4.2).
    void set_band_span_us(uint32_t us) { span_override_us_ = us; }

    void note_band_bits(uint32_t bits) {
        frame_bits_ += bits;
        if (budget_bits_ && frame_bits_ > uint64_t(budget_bits_) * 115 / 100)
            stretch_ = double(frame_bits_) / (double(budget_bits_) * 1.15);
    }

    // `encode_finish_us` is when this band's encode finished on the server clock.
    Plan plan(uint8_t band, uint32_t encode_finish_us, size_t datagrams) const {
        Plan p;
        p.datagrams = datagrams;
        p.start_us = encode_finish_us;
        uint32_t nominal = span_override_us_
                               ? span_override_us_
                               : cfg_.frame_period_us / (cfg_.bands() ? cfg_.bands() : 1);
        p.span_us = uint32_t(double(nominal) * stretch_);
        p.interval_us = datagrams > 1 ? uint32_t(p.span_us / (datagrams - 1)) : 0;
        (void)band;
        return p;
    }

    double stretch() const { return stretch_; }

  private:
    StreamConfig cfg_;
    uint32_t frame_start_us_ = 0;
    uint32_t budget_bits_ = 0;
    uint64_t frame_bits_ = 0;
    double stretch_ = 1.0;
    uint32_t span_override_us_ = 0;
};

}  // namespace nxt

#endif
