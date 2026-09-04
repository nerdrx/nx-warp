// nxvc_transport - encoder-side client shadow.  Normative: docs/TRANSPORT.md 9.
#ifndef NXVC_TRANSPORT_SHADOW_H
#define NXVC_TRANSPORT_SHADOW_H

#include "nxvc/transport/common.h"
#include "nxvc/transport/wire.h"

namespace nxt {

// The sender's mirror of the client's reference ring, rebuilt from per-band
// feedback bitmaps.  Keeps kShadowFrames frames of history (PAPER 6.6).
class ClientShadow {
  public:
    explicit ClientShadow(const StreamConfig& cfg);

    // Start encoding a new frame: allocates its history entry with every tile
    // UNKNOWN.  Must be called in increasing frame order.
    void begin_frame(uint16_t frame_id);

    // Fold one feedback packet in.  Bits set mark RECEIVED (never downgraded, so
    // a late tile reported by a later cumulative packet upgrades CONCEALED ->
    // RECEIVED), bits clear mark CONCEALED.
    void apply_feedback(const FeedbackPacket& fb);

    // TRANSPORT.md 9: newest of N-1, N-2, N-3 whose 3x3 neighbourhood is exact.
    // Returns 0..2, or kRefIntra (3).
    uint8_t reference_choice(uint16_t frame_id, uint16_t row, uint16_t col) const;

    bool exact(uint16_t frame_id, uint16_t row, uint16_t col) const;
    bool neighbourhood_exact(uint16_t frame_id, uint16_t row, uint16_t col) const;
    ShadowState state(uint16_t frame_id, uint16_t row, uint16_t col) const;
    bool band_known(uint16_t frame_id, uint8_t band) const;

    // Frames since this position was last exact, saturating at kShadowFrames.
    // Feeds the encoder's rolling intra refresh.
    uint8_t staleness(uint16_t newest_frame, uint16_t row, uint16_t col) const;

    // Counters for telemetry / the simulator.
    uint64_t feedback_packets() const { return feedback_packets_; }
    uint64_t feedback_bytes() const { return feedback_bytes_; }
    void add_feedback_bytes(size_t n) { feedback_bytes_ += n; ++feedback_packets_; }

  private:
    struct Frame {
        bool used = false;
        uint16_t frame_id = 0;
        std::vector<ShadowState> state;
        std::vector<uint8_t> band_known;
    };

    const Frame* find(uint16_t frame_id) const;
    Frame* find(uint16_t frame_id);
    bool exact_uncached(const Frame& f, uint32_t idx) const;

    StreamConfig cfg_;
    uint32_t tiles_ = 0;
    std::vector<Frame> ring_;                 // kShadowFrames entries
    mutable std::vector<int8_t> cache_;       // kShadowFrames * tiles, -1 unknown
    mutable std::vector<uint8_t> in_progress_;
    uint64_t feedback_packets_ = 0;
    uint64_t feedback_bytes_ = 0;
};

}  // namespace nxt

#endif
