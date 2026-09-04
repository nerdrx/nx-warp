// nxvc_transport - receiver: depacketize, place, recover, conceal, feed back.
// Normative: docs/TRANSPORT.md 7, 8, 11, 12.
#ifndef NXVC_TRANSPORT_RECEIVER_H
#define NXVC_TRANSPORT_RECEIVER_H

#include <deque>
#include <map>
#include <unordered_set>

#include "nxvc/transport/aead.h"
#include "nxvc/transport/fec.h"
#include "nxvc/transport/telemetry.h"
#include "nxvc/transport/wire.h"

namespace nxt {

struct TileMeta {
    uint16_t pose_seq = 0;
    uint8_t age = 0;
    TileState state = TileState::kEmpty;
    bool late = false;
    bool recovered = false;
};

// The 4-slot display ring of PAPER 4.3.  The transport owns only the metadata;
// the pixels live in the decoder's images.
class FrameRing {
  public:
    explicit FrameRing(const StreamConfig& cfg);
    struct Slot {
        bool used = false;
        uint16_t frame_id = 0;
        std::vector<TileMeta> meta;  // layers * tiles
        std::unordered_set<uint64_t> seen_data;
        std::unordered_set<uint64_t> seen_parity;
        std::unordered_set<uint64_t> fec_repaired;  // dk of datagrams rebuilt by FEC
        std::vector<uint8_t> band_deadline_passed;
        // Times each band has been carried in a feedback packet.  After
        // kMaxFeedbackBands reports the band is frozen (TRANSPORT.md D17).
        std::vector<uint8_t> band_reports;
    };
    // Returns the slot for `frame_id`, evicting and resetting it if it holds an
    // older frame.  Returns nullptr for a frame older than the ring.
    Slot* acquire(uint16_t frame_id);
    Slot* find(uint16_t frame_id);
    const Slot* find(uint16_t frame_id) const;
    TileMeta& at(Slot& s, uint8_t layer, uint32_t tile) {
        return s.meta[size_t(layer) * cfg_.tiles_per_frame() + tile];
    }
    const StreamConfig& cfg() const { return cfg_; }
    uint16_t newest() const { return newest_; }
    bool started() const { return started_; }

  private:
    StreamConfig cfg_;
    Slot slots_[kRingSlots];
    uint16_t newest_ = 0;
    bool started_ = false;
};

// PAPER 4.3 deadline policy.
class DeadlineController {
  public:
    uint32_t offset_us() const { return offset_us_; }
    // `miss_fraction` is the share of the frame's tiles not DECODED at their
    // band deadlines.
    void on_frame(double miss_fraction);
    bool moved() const { return moved_; }
    void clear_moved() { moved_ = false; }

  private:
    uint32_t offset_us_ = 0;
    int consecutive_miss_ = 0;
    int clean_frames_ = 0;
    bool moved_ = false;
};

struct ReceiverStats {
    uint64_t datagrams = 0, data_datagrams = 0, parity_datagrams = 0;
    uint64_t wire_bytes = 0, tile_bytes = 0;
    uint64_t duplicates = 0;
    uint64_t auth_fail = 0, bad_version = 0, bad_caps = 0, bad_directory = 0;
    uint64_t bad_range = 0, stale_frame = 0, replay = 0, frozen_band = 0;
    uint64_t tiles_placed = 0, tiles_late = 0, tiles_concealed = 0;
    uint64_t fec_recovered = 0, fec_failed = 0, fec_groups = 0;
    uint64_t fec_recovered_bytes = 0;
    // A group whose k-th block arrives while other members are still in flight
    // is repaired eagerly; the original then shows up and is suppressed.  These
    // count the repairs that turned out not to have been needed.
    uint64_t fec_recovered_redundant = 0;
    uint64_t fec_recovered_redundant_bytes = 0;
    uint64_t path_datagrams[kMaxPaths] = {0, 0};
    uint64_t path_bytes[kMaxPaths] = {0, 0};
    uint64_t path_gaps[kMaxPaths] = {0, 0};
    uint64_t feedback_packets = 0, feedback_bytes = 0;
};

class Receiver {
  public:
    Receiver(const StreamConfig& cfg, const Aead* aead, const Key& session_key,
             const Key& session_salt);

    void set_epoch(uint16_t e) { epoch_ = e; }
    void set_negotiated_caps(uint8_t c) { caps_ = c; }
    // v1 repaired a group as soon as k blocks were present; kept for A/B runs.
    void set_eager_fec(bool on) { eager_fec_ = on; }

    // Feed one datagram.  Delivered tiles are appended to `tiles`, whose byte
    // spans point into `scratch` (valid until the next call).
    bool on_datagram(std::span<const uint8_t> wire, uint8_t path_id, uint64_t now_us,
                     std::vector<TileOutput>* tiles);

    // The decoder found a tile's bitstream corrupt: clear its feedback bit.
    void mark_tile_undecodable(uint16_t frame_id, uint8_t layer, uint16_t row,
                               uint16_t col);

    // Run the band deadline: conceal, close FEC groups, build the (encrypted)
    // feedback packet to send back on `path_id`.  Returns the wire bytes.
    ByteVec band_deadline(uint16_t frame_id, uint8_t band, uint64_t now_us,
                          uint16_t decode_us, uint8_t path_id);

    // Classification at present time (TRANSPORT.md 7.5).
    struct Presentation {
        uint32_t fresh = 0, stale = 0, concealed = 0, undecodable = 0, empty = 0;
        bool partial() const { return concealed || undecodable || empty; }
    };
    Presentation classify(uint16_t frame_id, uint8_t layer = 0) const;

    FrameRing& ring() { return ring_; }
    const FrameRing& ring() const { return ring_; }
    DeadlineController& deadline() { return deadline_; }
    ReceiverStats stats;
    Telemetry telemetry;

    // Per-path loss over the last second, as a fraction.
    double path_loss(uint8_t path_id) const;
    uint32_t path_rtt_us(uint8_t path_id) const { return path_[path_id].rtt_us; }
    void set_path_rtt(uint8_t path_id, uint32_t rtt_us) {
        if (path_id < kMaxPaths) path_[path_id].rtt_us = rtt_us;
    }

  private:
    struct PathRx {
        uint64_t expect = 0;
        uint64_t highest = 0;
        bool started = false;
        uint64_t win_first = 0;
        uint64_t win_count = 0;
        uint64_t win_start_us = 0;
        double loss = 0.0;
        uint32_t rtt_us = 0;
    };
    struct GroupState {
        FecGroupDecoder dec;
        uint16_t frame_id = 0;
        uint8_t band = 0;
        uint8_t path_id = 0;
        bool closed = false;
    };

    bool process(std::span<const uint8_t> wire, uint8_t path_id, uint64_t now_us,
                 bool from_fec, std::vector<TileOutput>* tiles, int depth);
    void account_seq(uint8_t path_id, uint64_t ext, uint64_t now_us);
    uint64_t group_key(const DatagramHeader& h, uint8_t path_id) const;

    StreamConfig cfg_;
    const Aead* aead_;
    Key subkey_dn_[kMaxPaths];
    Key subkey_up_[kMaxPaths];
    uint16_t epoch_ = 0;
    uint8_t caps_ = 0xFF;
    bool eager_fec_ = false;
    uint64_t up_seq_ = 0;  // feedback nonce counter, (frame * bands + band)
    uint64_t frame_ext_ = 0;
    bool frame_started_ = false;

    FrameRing ring_;
    DeadlineController deadline_;
    PathRx path_[kMaxPaths];
    std::map<uint64_t, GroupState> groups_;
    std::deque<BandReport> recent_bands_;
    std::vector<ByteVec> arena_;

    // Per-frame miss accounting for the deadline controller.
    uint16_t acct_frame_ = 0;
    bool acct_valid_ = false;
    uint32_t acct_total_ = 0, acct_missed_ = 0;
    uint8_t band_rx_seen_[8] = {0};
    uint32_t band_rx_first_[8] = {0};
    uint32_t band_rx_last_[8] = {0};
    uint32_t band_late_[8] = {0};
    uint32_t band_fec_rec_[8] = {0};
    uint32_t band_fec_fail_[8] = {0};
};

}  // namespace nxt

#endif
