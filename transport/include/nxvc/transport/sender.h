// nxvc_transport - sender facade: packetize, FEC, stripe, encrypt, stamp.
// Normative: docs/TRANSPORT.md 2-6, 10, 11.  No sockets: the caller sends the
// returned datagrams itself.
#ifndef NXVC_TRANSPORT_SENDER_H
#define NXVC_TRANSPORT_SENDER_H

#include "nxvc/transport/aead.h"
#include "nxvc/transport/fec.h"
#include "nxvc/transport/multipath.h"
#include "nxvc/transport/packetizer.h"
#include "nxvc/transport/scheduler.h"
#include "nxvc/transport/shadow.h"
#include "nxvc/transport/telemetry.h"

namespace nxt {

struct Datagram {
    uint8_t path_id = 0;
    uint32_t tx_ts = 0;
    ByteVec bytes;  // header || ciphertext || tag
};

struct SenderStats {
    uint64_t datagrams = 0;
    uint64_t data_datagrams = 0;
    uint64_t parity_datagrams = 0;
    uint64_t duplicated_datagrams = 0;
    uint64_t wire_bytes = 0;
    uint64_t tile_bytes = 0;     // codec payload only
    uint64_t header_bytes = 0;   // 24 per datagram
    uint64_t dir_bytes = 0;      // 4 per tile
    uint64_t tag_bytes = 0;      // 16 per datagram
    uint64_t parity_bytes = 0;   // whole parity datagrams
    uint64_t pose_bytes = 0;     // 26 per band
    uint64_t tiles = 0;
    uint64_t runs = 0;
    uint64_t oversize_tiles = 0;
    uint64_t feedback_packets = 0;
    uint64_t feedback_bytes = 0;
    uint64_t ref_delta_hist[4] = {0, 0, 0, 0};
};

class Sender {
  public:
    Sender(const StreamConfig& cfg, const Aead* aead, const Key& session_key,
           const Key& session_salt);

    ClientShadow& shadow() { return shadow_; }
    const ClientShadow& shadow() const { return shadow_; }
    Striper& striper() { return striper_; }
    BandScheduler& scheduler() { return sched_; }
    Packetizer& packetizer() { return pkt_; }

    void set_epoch(uint16_t e) { epoch_ = e; }

    void begin_frame(uint16_t frame_id, const PoseHeader& pose,
                     uint32_t render_finish_us, uint32_t frame_bit_budget);

    // Tiles of one band, ordered by (layer, row, col).  Returns the datagrams to
    // put on the wire, each already stamped with its path and tx time.
    std::vector<Datagram> send_band(uint8_t band, std::span<const TileInput> tiles,
                                    uint32_t encode_finish_us, uint16_t enc_us,
                                    bool last_band);

    // Feedback arriving on `path_id`.  Decrypts, applies to the shadow, updates
    // path statistics.  Returns false on an auth failure.
    bool on_feedback(std::span<const uint8_t> wire, uint8_t path_id, uint64_t now_us);

    // Convenience for the encoder (TRANSPORT.md 9).
    uint8_t reference_choice(uint16_t frame_id, uint16_t row, uint16_t col) {
        uint8_t d = shadow_.reference_choice(frame_id, row, col);
        ++stats.ref_delta_hist[d];
        return d;
    }

    SenderStats stats;
    Telemetry telemetry;

  private:
    Datagram seal_one(PendingDatagram& pd, uint8_t path_id, uint32_t tx_ts);

    StreamConfig cfg_;
    const Aead* aead_;
    Key subkey_dn_[kMaxPaths];
    Key subkey_up_[kMaxPaths];
    uint16_t epoch_ = 0;
    uint64_t seq_[kMaxPaths] = {0, 0};
    uint64_t frame_ext_ = 0;
    bool frame_started_ = false;

    Packetizer pkt_;
    Striper striper_;
    BandScheduler sched_;
    ClientShadow shadow_;

    uint16_t frame_id_ = 0;
    PoseHeader pose_{};
    uint32_t render_finish_us_ = 0;
};

}  // namespace nxt

#endif
