// NX Warp datagram wire format -- client-side parser.
//
// Normative source: docs/TRANSPORT.md sections 2 (24-byte header), 3 (payload
// layout and the 4-byte tile directory entry), 8 (feedback packet). This file is
// a *transcription* of that document, not an independent design.
//
// When transport/ lands, everything here is replaced by the library's own
// declarations; see nxc_transport.h for the seam and what has to be deleted.
#pragma once

#include <cstdint>
#include <cstring>

namespace nxc {

inline constexpr uint8_t  kVersion       = 1;
inline constexpr uint32_t kHeaderBytes   = 24;
inline constexpr uint32_t kTagBytes      = 16;
inline constexpr uint32_t kPoseHdrBytes  = 26;
inline constexpr uint32_t kDirEntryBytes = 4;

// TRANSPORT.md 2.1, flags nibble in byte 0 bits 4..7.
enum HeaderFlags : uint8_t {
    kFlagKeyframeRun     = 1u << 0,   // bit 4 of byte 0
    kFlagPartialFrame    = 1u << 1,   // bit 5
    kFlagLossless        = 1u << 2,   // bit 6
    kFlagLastRunOfFrame  = 1u << 3,   // bit 7
};

// TRANSPORT.md 2.2, capability byte.
enum Caps : uint8_t {
    kCapFec         = 1u << 0,
    kCapMultipath   = 1u << 1,
    kCapJumbo       = 1u << 2,
    kCapFragment    = 1u << 3,
    kCapPoseHdr     = 1u << 4,
    kCapRleFeedback = 1u << 5,
};

// TRANSPORT.md 3.1, mode field of a tile directory entry.
enum TileMode : uint8_t {
    kModeWarpSkip = 0, kModeStaticMv = 1, kModeWarpMv = 2, kModeIntra = 3, kModeStereo = 4,
};

// ---------------------------------------------------------------- header

struct DatagramHeader {
    uint8_t  version;
    uint8_t  flags;        // HeaderFlags, already shifted down to bits 0..3
    uint8_t  stream_id;
    uint16_t frame_id;
    uint16_t tile_first;
    uint8_t  tile_count;   // 0 marks a parity datagram (TRANSPORT.md 2, D11)
    uint8_t  layer_id;     // 0..15 on the wire, 0..3 used
    uint8_t  ref_delta;    // 0..2, 3 = intra
    uint8_t  frag_idx;     // 0..3
    uint8_t  frag_count;   // fragments minus one
    uint8_t  tile_class;   // 0 A, 1 B, 2 C
    uint8_t  band;         // 0..6, 7 = not band addressed
    uint8_t  pose_hdr;     // payload begins with the 26-byte frame/pose header
    uint8_t  caps;
    uint16_t pose_seq;
    uint16_t path_seq;     // 14 bits
    uint8_t  path_id;      // 2 bits
    uint8_t  fec_group;
    uint8_t  fec_idx;
    uint8_t  fec_k;        // 0 = no FEC on this datagram
    uint32_t tx_ts;        // server clock, us
    uint16_t payload_len;  // ciphertext bytes, EXCLUDING the 16-byte tag (D12)
    uint16_t enc_us;

    bool is_parity() const { return tile_count == 0; }
};

inline uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (uint32_t(p[1]) << 8));
}
inline uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void wr_u16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}

// Parses the fixed 24-byte header. Returns false only for a short buffer; every
// *semantic* check (version, caps, ranges) belongs to the depacketizer so that
// each rejection can be counted separately (TRANSPORT.md section 12).
inline bool parse_header(const uint8_t* p, size_t len, DatagramHeader* h) {
    if (len < kHeaderBytes) return false;
    h->version     = uint8_t(p[0] & 0x0f);
    h->flags       = uint8_t(p[0] >> 4);
    h->stream_id   = p[1];
    h->frame_id    = rd_u16(p + 2);
    h->tile_first  = rd_u16(p + 4);
    h->tile_count  = p[6];
    h->layer_id    = uint8_t(p[7] & 0x0f);
    h->ref_delta   = uint8_t((p[7] >> 4) & 0x03);
    h->frag_idx    = uint8_t((p[7] >> 6) & 0x03);
    h->frag_count  = uint8_t(p[8] & 0x03);
    h->tile_class  = uint8_t((p[8] >> 2) & 0x03);
    h->band        = uint8_t((p[8] >> 4) & 0x07);
    h->pose_hdr    = uint8_t((p[8] >> 7) & 0x01);
    h->caps        = p[9];
    h->pose_seq    = rd_u16(p + 10);
    uint16_t ps    = rd_u16(p + 12);
    h->path_seq    = uint16_t(ps & 0x3fff);
    h->path_id     = uint8_t(ps >> 14);
    h->fec_group   = p[14];
    h->fec_idx     = uint8_t(p[15] & 0x0f);
    h->fec_k       = uint8_t(p[15] >> 4);
    h->tx_ts       = rd_u32(p + 16);
    h->payload_len = rd_u16(p + 20);
    h->enc_us      = rd_u16(p + 22);
    return true;
}

inline void write_header(uint8_t* p, const DatagramHeader& h) {
    std::memset(p, 0, kHeaderBytes);
    p[0]  = uint8_t((h.version & 0x0f) | (uint8_t(h.flags & 0x0f) << 4));
    p[1]  = h.stream_id;
    wr_u16(p + 2, h.frame_id);
    wr_u16(p + 4, h.tile_first);
    p[6]  = h.tile_count;
    p[7]  = uint8_t((h.layer_id & 0x0f) | ((h.ref_delta & 0x03) << 4) | ((h.frag_idx & 0x03) << 6));
    p[8]  = uint8_t((h.frag_count & 0x03) | ((h.tile_class & 0x03) << 2) |
                    ((h.band & 0x07) << 4) | ((h.pose_hdr & 0x01) << 7));
    p[9]  = h.caps;
    wr_u16(p + 10, h.pose_seq);
    wr_u16(p + 12, uint16_t((h.path_seq & 0x3fff) | (uint16_t(h.path_id & 0x03) << 14)));
    p[14] = h.fec_group;
    p[15] = uint8_t((h.fec_idx & 0x0f) | ((h.fec_k & 0x0f) << 4));
    wr_u32(p + 16, h.tx_ts);
    wr_u16(p + 20, h.payload_len);
    wr_u16(p + 22, h.enc_us);
}

// ---------------------------------------------------------------- directory

// TRANSPORT.md 3.1, one u32 per tile.
struct TileDirEntry {
    uint16_t len;        // 0..4095, 0 = empty (skip) tile
    uint8_t  qp;         // 0..63
    uint8_t  mode;       // TileMode
    uint8_t  res_level;  // 0 full, 1 half, 2 quarter, 3 DC-plane (PAPER 4.6.1)
    uint8_t  lossless;
    uint8_t  chroma444;
    uint8_t  alpha;
};

inline TileDirEntry parse_dir_entry(uint32_t w) {
    TileDirEntry e{};
    e.len       = uint16_t(w & 0xfff);
    e.qp        = uint8_t((w >> 12) & 0x3f);
    e.mode      = uint8_t((w >> 18) & 0x07);
    e.res_level = uint8_t((w >> 21) & 0x03);
    e.lossless  = uint8_t((w >> 23) & 0x01);
    e.chroma444 = uint8_t((w >> 24) & 0x01);
    e.alpha     = uint8_t((w >> 25) & 0x01);
    return e;
}

inline uint32_t pack_dir_entry(const TileDirEntry& e) {
    return uint32_t(e.len & 0xfff) | (uint32_t(e.qp & 0x3f) << 12) |
           (uint32_t(e.mode & 0x07) << 18) | (uint32_t(e.res_level & 0x03) << 21) |
           (uint32_t(e.lossless & 1) << 23) | (uint32_t(e.chroma444 & 1) << 24) |
           (uint32_t(e.alpha & 1) << 25);
}

// ---------------------------------------------------------------- path_seq

// TRANSPORT.md 4.2: extend the 14-bit wire counter to the full 64-bit counter.
// Used here for loss accounting (the AEAD nonce it also feeds is the stub's
// business, not ours).
inline uint64_t extend_path_seq(uint64_t expected, uint16_t seq14) {
    const int64_t span = 0x4000;
    int64_t base = int64_t(expected & ~uint64_t(0x3fff));
    int64_t best = base + seq14;
    int64_t bestd = best - int64_t(expected);
    if (bestd < 0) bestd = -bestd;
    for (int64_t cand : {base + seq14 - span, base + seq14 + span}) {
        if (cand < 0) continue;
        int64_t d = cand - int64_t(expected);
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = cand; }
    }
    if (best < 0) best = seq14;
    return uint64_t(best);
}

// ---------------------------------------------------------------- self test

// The nxvc-blast self-test trailer. NOT part of the NX Warp wire format: it is
// appended after the payload by the blaster and stripped by the client so that
// the pps self test has ground truth (absolute sequence) independent of the
// 14-bit path_seq, which wraps every 16384 datagrams -- 0.18 s at 90 kpps.
inline constexpr uint32_t kSelfTestMagic = 0x4e584254u;  // "NXBT"
struct SelfTestTrailer {
    uint32_t magic;
    uint32_t abs_seq;    // absolute datagram index from the blaster
    uint64_t send_ts_us; // blaster's CLOCK_MONOTONIC; only differences are used
};
inline constexpr uint32_t kSelfTestTrailerBytes = 16;

// The client's stats report, sent back to the blaster once a second so that a
// device run needs no adb. Little endian, its own magic so it cannot be
// confused with a feedback packet.
inline constexpr uint32_t kStatsMagic = 0x4e585253u;  // "NXRS"
struct StatsReport {
    uint32_t magic;
    uint32_t seq;
    uint64_t window_us;
    uint64_t rx_datagrams;
    uint64_t rx_bytes;
    uint64_t lost_datagrams;   // from abs_seq gaps
    uint64_t reordered;
    uint32_t rcvbuf_granted;
    uint32_t sched_policy;     // 1 = SCHED_FIFO granted, 0 = fallback
    uint32_t affinity_mask;
    uint32_t cpu_permille_proc;
    uint32_t cpu_permille_rx;
    uint32_t drops_ring_full;
    uint32_t udp_rcvbuf_errors; // /proc/net/snmp delta, if readable
    uint32_t pad;
};

}  // namespace nxc
