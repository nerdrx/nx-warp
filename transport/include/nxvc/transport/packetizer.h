// nxvc_transport - sender packetizer.  Normative: docs/TRANSPORT.md 3, 5, 6.
#ifndef NXVC_TRANSPORT_PACKETIZER_H
#define NXVC_TRANSPORT_PACKETIZER_H

#include "nxvc/transport/common.h"
#include "nxvc/transport/wire.h"

namespace nxt {

// A datagram before path assignment and encryption.
struct PendingDatagram {
    DatagramHeader hdr;
    ByteVec plaintext;  // pose header (optional) || directory || tile bitstreams
    bool parity = false;
};

// A unit of path scheduling: either one unprotected datagram, or a whole FEC
// group (its data datagrams; parity is generated after encryption).
// TRANSPORT.md 10: a FEC group is assigned to a single path.
struct SendUnit {
    TileClass cls = TileClass::kA;
    uint8_t band = 0;
    uint8_t layer = 0;
    uint8_t group = 0;
    int m = 0;  // parity datagrams to generate for this unit
    std::vector<PendingDatagram> data;
    size_t bytes() const {
        size_t n = 0;
        for (const auto& d : data) n += kHeaderBytes + d.plaintext.size() + kTagBytes;
        return n;
    }
};

struct FrameContext {
    uint16_t frame_id = 0;
    uint16_t pose_seq = 0;
    uint32_t tx_ts = 0;
    uint16_t enc_us = 0;
    bool partial_frame = false;
    bool last_band = false;
    const PoseHeader* pose = nullptr;  // replicated in the first datagram of a band
};

// Parity counts per class (PAPER 4.4), adjustable by measured loss.
struct FecPolicy {
    int parity[3] = {3, 1, 0};
    void set_from_loss(double loss_fraction);
};

class Packetizer {
  public:
    enum class OversizePolicy { kReject, kDropTile, kFragment };
    enum class Status { kOk, kOversizeTile, kBadInput };

    Packetizer(const StreamConfig& cfg, OversizePolicy policy = OversizePolicy::kReject)
        : cfg_(cfg), policy_(policy) {}

    void set_policy(OversizePolicy p) { policy_ = p; }
    void set_fec(const FecPolicy& f) { fec_ = f; }
    const FecPolicy& fec() const { return fec_; }

    // Tiles of one band, ordered by (layer, row, col).  Runs are homogeneous in
    // layer, ref_delta, class and row (TRANSPORT.md 3.2).
    Status packetize_band(uint8_t band, std::span<const TileInput> tiles,
                          const FrameContext& ctx, std::vector<SendUnit>* out);

    size_t oversize_tiles() const { return oversize_tiles_; }

  private:
    StreamConfig cfg_;
    OversizePolicy policy_;
    FecPolicy fec_;
    size_t oversize_tiles_ = 0;
    uint8_t next_group_ = 0;
};

}  // namespace nxt

#endif
