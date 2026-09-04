// nxvc_transport - shared types and constants.
// Normative reference: docs/TRANSPORT.md section 1.
#ifndef NXVC_TRANSPORT_COMMON_H
#define NXVC_TRANSPORT_COMMON_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nxt {

inline constexpr uint8_t kVersion = 1;

inline constexpr size_t kHeaderBytes = 24;
inline constexpr size_t kTagBytes = 16;
inline constexpr size_t kNonceBytes = 12;
inline constexpr size_t kKeyBytes = 32;
inline constexpr size_t kDirEntryBytes = 4;
inline constexpr size_t kPoseHeaderBytes = 26;
inline constexpr size_t kFecReserveBytes = 44;  // TRANSPORT.md 5.1
inline constexpr size_t kDefaultMtu = 1400;

inline constexpr int kMaxPaths = 2;
inline constexpr int kRingSlots = 4;
inline constexpr int kShadowFrames = 8;
inline constexpr int kFecMaxK = 10;
inline constexpr int kFecMaxM = 4;
inline constexpr int kMaxTilesPerRun = 255;
inline constexpr int kMaxFeedbackBands = 3;

// ------------------------------------------------------------------ flags
enum HeaderFlag : uint8_t {
    kFlagKeyframeRun = 1u << 0,
    kFlagPartialFrame = 1u << 1,
    kFlagLossless = 1u << 2,
    kFlagLastRunOfFrame = 1u << 3,
};

enum Cap : uint8_t {
    kCapFec = 1u << 0,
    kCapMultipath = 1u << 1,
    kCapJumbo = 1u << 2,
    kCapFragment = 1u << 3,
    kCapPoseHdr = 1u << 4,
    kCapRleFeedback = 1u << 5,
};

enum FeedbackFlag : uint8_t {
    kFbDeadlineMoved = 1u << 0,
    kFbPath0Stalled = 1u << 1,
    kFbPath1Stalled = 1u << 2,
    kFbRekeyReq = 1u << 3,
};

enum class TileClass : uint8_t { kA = 0, kB = 1, kC = 2 };

enum class TileMode : uint8_t {
    kWarpSkip = 0,
    kStaticMv = 1,
    kWarpMv = 2,
    kIntra = 3,
    kStereo = 4,
};

// Receiver per-tile state (TRANSPORT.md 7.3).
enum class TileState : uint8_t {
    kEmpty = 0,
    kDecoded = 1,
    kConcealed = 2,
    kUndecodable = 3,
};

// Sender shadow per-tile knowledge (TRANSPORT.md 9).
enum class ShadowState : uint8_t {
    kUnknown = 0,   // no feedback for this band yet
    kReceived = 1,  // feedback bit set (or reported late)
    kConcealed = 2, // feedback bit clear: client ran the deterministic warp
};

inline constexpr uint8_t kRefIntra = 3;

// ------------------------------------------------------------------ config
struct StreamConfig {
    uint8_t stream_id = 0;
    uint16_t cols = 68;
    uint16_t rows = 34;
    uint16_t band_rows = 6;
    uint8_t layers = 1;
    size_t mtu = kDefaultMtu;
    uint8_t caps = kCapFec | kCapMultipath | kCapPoseHdr | kCapRleFeedback;
    uint32_t frame_period_us = 11111;  // 90 Hz

    uint32_t tiles_per_frame() const { return uint32_t(cols) * rows; }
    uint8_t bands() const {
        return uint8_t((rows + band_rows - 1) / band_rows);
    }
    uint8_t band_of_row(uint16_t row) const {
        uint8_t b = uint8_t(row / band_rows);
        uint8_t n = bands();
        return b >= n ? uint8_t(n - 1) : b;
    }
    uint16_t first_row_of_band(uint8_t band) const {
        return uint16_t(band * band_rows);
    }
    uint16_t rows_in_band(uint8_t band) const {
        uint16_t first = first_row_of_band(band);
        uint16_t last = uint16_t(band + 1 == bands() ? rows : first + band_rows);
        return uint16_t(last - first);
    }
    uint32_t tiles_in_band(uint8_t band) const {
        return uint32_t(cols) * rows_in_band(band);
    }
    uint32_t tile_index(uint16_t row, uint16_t col) const {
        return uint32_t(row) * cols + col;
    }
    uint16_t row_of(uint32_t tile_index) const { return uint16_t(tile_index / cols); }
    uint16_t col_of(uint32_t tile_index) const { return uint16_t(tile_index % cols); }
    uint32_t tile_in_band(uint16_t row, uint16_t col) const {
        uint8_t b = band_of_row(row);
        return uint32_t(row - first_row_of_band(b)) * cols + col;
    }
    bool fec_enabled() const { return (caps & kCapFec) != 0; }
    size_t run_payload_budget() const {
        size_t b = mtu - kHeaderBytes - kTagBytes - (fec_enabled() ? kFecReserveBytes : 0);
        return b & ~size_t(3);  // keep the directory 4-byte aligned
    }
    size_t max_tile_bytes() const { return run_payload_budget() - kDirEntryBytes; }
};

// ------------------------------------------------------------------ tiles
// A tile as the encoder hands it to the transport: an opaque blob plus placement.
struct TileInput {
    uint16_t frame_id = 0;
    uint8_t layer_id = 0;
    uint16_t row = 0;
    uint16_t col = 0;
    TileClass cls = TileClass::kA;
    uint8_t ref_delta = kRefIntra;
    uint8_t qp = 0;
    TileMode mode = TileMode::kIntra;
    uint8_t res_level = 0;
    bool lossless = false;
    bool chroma444 = false;
    bool alpha = false;
    std::span<const uint8_t> bytes;
};

// A tile as the receiver hands it to the decoder.
struct TileOutput {
    uint16_t frame_id = 0;
    uint8_t layer_id = 0;
    uint16_t row = 0;
    uint16_t col = 0;
    TileClass cls = TileClass::kA;
    uint8_t ref_delta = kRefIntra;
    uint16_t pose_seq = 0;
    uint8_t qp = 0;
    TileMode mode = TileMode::kIntra;
    uint8_t res_level = 0;
    bool lossless = false;
    bool late = false;
    bool recovered = false;
    std::span<const uint8_t> bytes;
};

// ------------------------------------------------------------- little endian
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
inline uint64_t rd64(const uint8_t* p) {
    return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32);
}
inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline void wr64(uint8_t* p, uint64_t v) {
    wr32(p, uint32_t(v));
    wr32(p + 4, uint32_t(v >> 32));
}

// 16-bit sequence comparison (RFC 1982 style).
inline bool seq_newer(uint16_t a, uint16_t b) {
    return int16_t(uint16_t(a - b)) > 0;
}

// TRANSPORT.md 4.2: extend a narrow wire sequence against a 64-bit expectation.
inline uint64_t extend_seq_bits(uint64_t expected, uint64_t val, int bits) {
    const uint64_t span = 1ull << bits;
    uint64_t base = expected & ~(span - 1);
    uint64_t best = base + val;
    auto dist = [&](uint64_t c) { return c > expected ? c - expected : expected - c; };
    uint64_t bd = dist(best);
    if (base >= span) {
        uint64_t c = base - span + val;
        if (dist(c) < bd) { best = c; bd = dist(c); }
    }
    uint64_t c2 = base + span + val;
    if (dist(c2) < bd) best = c2;
    return best;
}
inline uint64_t extend_seq14(uint64_t expected, uint16_t seq14) {
    return extend_seq_bits(expected, seq14 & 0x3FFF, 14);
}
inline uint64_t extend_seq16(uint64_t expected, uint16_t v) {
    return extend_seq_bits(expected, v, 16);
}

using ByteVec = std::vector<uint8_t>;

}  // namespace nxt

#endif
