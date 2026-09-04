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

// Parity per class (PAPER 4.4).  v2 states the class parity as a *ratio* of the
// realised group size rather than a fixed count, so a short group at the end of
// a band does not pay 3 parity blocks for 3 data blocks.  Nominal ratios are the
// paper's 30 / 10 / 0 percent, with a floor of one parity block for class A.
struct FecPolicy {
    int ratio_pct[3] = {30, 10, 0};
    int min_parity[3] = {1, 0, 0};
    void set_from_loss(double loss_fraction);

    // Decision D25: the ladder keys off measured *headroom* first and loss
    // second.  Class A parity is unconditional (floor one block); class B and C
    // parity are spent only when the link has room for them, because on a link
    // near saturation the parity bytes push more tiles past the band deadline
    // than they recover.  `headroom` is 1 - wire_rate / delivery_rate.
    static constexpr double kBcHeadroom = 0.50;      // class B/C gate
    static constexpr double kBcHeadroomDrop = 0.42;  // hysteresis on the way down
    void set_from_headroom(double headroom, double loss_fraction, bool bc_was_on);

    // What the sender actually runs by default (decision D25).  Class A parity
    // at the nominal ratio and nothing else: measured over eight scenarios from
    // 0 to 65 percent headroom, the class B row cost tiles in every one, and
    // the loss escalation cost tiles wherever it fired.  `set_from_headroom`
    // stays available for a future sweep against a quality metric, but is not
    // the default, because no headroom tested made class B pay.
    void set_class_a_only(double loss_fraction) {
        ratio_pct[0] = loss_fraction < 0.001 ? 20 : 30;
        ratio_pct[1] = 0;
        ratio_pct[2] = 0;
        min_parity[0] = 1;
        min_parity[1] = 0;
        min_parity[2] = 0;
    }
    // Parity blocks for a group of k data datagrams of class `cls`.
    int parity_for(uint8_t cls, int k) const {
        if (cls > 2 || k <= 0) return 0;
        int m = (ratio_pct[cls] * k + 50) / 100;
        if (m < min_parity[cls]) m = min_parity[cls];
        if (m > kFecMaxM) m = kFecMaxM;
        return m;
    }
};

class Packetizer {
  public:
    enum class OversizePolicy { kReject, kDropTile, kFragment };
    enum class Status { kOk, kOversizeTile, kBadInput };

    Packetizer(const StreamConfig& cfg, OversizePolicy policy = OversizePolicy::kReject)
        : cfg_(cfg), policy_(policy) {}

    void set_policy(OversizePolicy p) { policy_ = p; }
    void set_class_break_min(size_t n) { class_break_min_ = n; }
    // Reproduce the v1 packetizer for A/B measurement: runs homogeneous in
    // tile_class and ref_delta, FEC groups in transmission order.
    void set_v1_compat(bool on) { v1_ = on; }
    bool v1_compat() const { return v1_; }
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
    size_t class_break_min_ = kClassBreakMin;
    bool v1_ = false;
    uint8_t next_group_ = 0;
};

}  // namespace nxt

#endif
