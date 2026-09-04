// nxvc_transport - on-the-wire structures.  Normative: docs/TRANSPORT.md 2, 3, 8.
#ifndef NXVC_TRANSPORT_WIRE_H
#define NXVC_TRANSPORT_WIRE_H

#include "nxvc/transport/common.h"

namespace nxt {

// ------------------------------------------------------- 24-byte datagram header
struct DatagramHeader {
    uint8_t version = kVersion;
    uint8_t flags = 0;        // HeaderFlag bits, 4 bits on the wire
    uint8_t stream_id = 0;
    uint16_t frame_id = 0;
    uint16_t tile_first = 0;
    uint8_t tile_count = 0;   // 0 == parity datagram
    uint8_t layer_id = 0;     // 4 bits
    uint8_t ref_delta = kRefIntra;  // 2 bits
    uint8_t frag_idx = 0;     // 2 bits
    uint8_t frag_count = 0;   // 2 bits, fragments-minus-one
    uint8_t tile_class = 0;   // 2 bits
    uint8_t band = 7;         // 3 bits, 7 == not band addressed
    bool pose_hdr = false;
    uint8_t caps = 0;
    uint16_t pose_seq = 0;
    uint16_t path_seq = 0;    // 14 bits
    uint8_t path_id = 0;      // 2 bits
    uint8_t fec_group = 0;
    uint8_t fec_idx = 0;      // 4 bits
    uint8_t fec_k = 0;        // 4 bits, 0 == no FEC
    uint32_t tx_ts = 0;
    uint16_t payload_len = 0; // ciphertext bytes, tag excluded
    uint16_t enc_us = 0;

    bool is_parity() const { return tile_count == 0; }
};

void encode_header(const DatagramHeader& h, uint8_t* out24);
bool decode_header(const uint8_t* in24, DatagramHeader* out);

// ------------------------------------------------------------- tile directory
struct TileDirEntry {
    uint16_t len = 0;       // 12 bits
    uint8_t qp = 0;         // 6 bits
    uint8_t mode = 0;       // 3 bits
    uint8_t res_level = 0;  // 2 bits
    bool lossless = false;
    bool chroma444 = false;
    bool alpha = false;
};

uint32_t pack_dir_entry(const TileDirEntry& e);
TileDirEntry unpack_dir_entry(uint32_t v);

// ------------------------------------------------------------------- feedback
enum class BitmapMode : uint8_t { kRaw = 0, kAll = 1, kRle = 2 };

struct BandReport {
    uint16_t frame_id = 0;
    uint8_t band = 0;
    bool complete = false;
    bool deadline_missed = false;
    uint32_t rx_ts_first = 0;
    uint32_t rx_ts_last = 0;
    uint16_t decode_us = 0;
    uint16_t conceal_tiles = 0;
    uint16_t late_tiles = 0;
    uint8_t fec_recovered = 0;
    uint8_t fec_failed = 0;
    // One entry per tile of the band, index == tile_in_band.
    std::vector<uint8_t> received;  // 0/1, not packed
};

struct FeedbackPacket {
    uint8_t version = kVersion;
    uint8_t flags = 0;
    uint8_t stream_id = 0;
    uint16_t tiles_in_band = 0;
    std::vector<BandReport> bands;  // newest first, 1..3
    uint8_t path_loss[kMaxPaths] = {0, 0};
    uint8_t path_rtt_ms[kMaxPaths] = {0, 0};
};

// `allow_rle` follows CAP_RLE_FEEDBACK.  Returns the serialised packet.
ByteVec encode_feedback(const FeedbackPacket& fb, bool allow_rle);
bool decode_feedback(std::span<const uint8_t> in, FeedbackPacket* out);

// ---------------------------------------------------------------- pose header
struct PoseHeader {
    uint16_t pose_seq = 0;
    int16_t quat[4] = {0, 0, 0, 0};  // Q15 x,y,z,w
    int32_t pos_mm_q8[3] = {0, 0, 0};
    uint32_t render_finish_ts = 0;
};
void encode_pose_header(const PoseHeader& p, uint8_t* out26);
bool decode_pose_header(const uint8_t* in26, PoseHeader* out);

}  // namespace nxt

#endif
