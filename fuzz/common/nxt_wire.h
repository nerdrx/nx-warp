// nxt_wire.h -- parse/mutate/serialize model of the NX Warp transport wire
// formats: the 24-byte datagram header and its plaintext payload
// (docs/TRANSPORT.md 2 and 3, PAPER 4.1) and the feedback packet
// (docs/TRANSPORT.md 8).
//
// Datagram plaintext:
//     [26]   frame/pose header      -- iff pose_hdr
//     [4*N]  tile directory         -- N == tile_count
//     [...]  tile bitstreams, concatenated in tile order
//
// The receiver MUST discard a datagram whose directory lengths do not sum to
// the plaintext length, so the interesting mutations are exactly the ones that
// keep the sum consistent (to reach the placement code) and the ones that
// break it by one (to reach the length check).  A byte-level mutator produces
// almost none of the first kind, which is why this model exists.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXFUZZ_NXT_WIRE_H
#define NXFUZZ_NXT_WIRE_H

#include <cstdint>
#include <cstring>
#include <vector>

#include "nxfuzz.h"

namespace nxf::nxt {

inline constexpr size_t kHeaderBytes = 24;
inline constexpr size_t kPoseBytes = 26;
inline constexpr size_t kDirEntryBytes = 4;
inline constexpr size_t kMaxTiles = 24;      // keep corpus entries small
inline constexpr size_t kMaxTileBytes = 512;

// -------------------------------------------------------------- the datagram
struct Datagram {
    uint8_t hdr[kHeaderBytes] = {};
    uint8_t pose[kPoseBytes] = {};
    std::vector<uint32_t> dir;                 // one per tile, low 12 bits = len
    std::vector<std::vector<uint8_t>> tiles;   // parallel to dir
    std::vector<uint8_t> parity;               // parity payload when tile_count == 0
    int64_t payload_len_override = -1;         // desync hdr payload_len
    int64_t tile_count_override = -1;          // desync hdr tile_count
    bool keep_dir_len = false;                 // do not repair dir[i].len

    // TRANSPORT.md 2 (v2, decision D19): byte 8 is
    //   band [2:0] | pose_hdr [3] | fec_m [6:4] | reserved [7]
    bool has_pose() const { return (hdr[8] & 0x08) != 0; }
    uint8_t tile_count() const { return hdr[6]; }
    bool is_parity() const { return hdr[6] == 0; }
};

inline uint32_t dir_len(uint32_t v) { return get_bits(v, 0, 12); }
inline uint32_t dir_set_len(uint32_t v, uint32_t len) { return set_bits(v, 0, 12, len); }

inline std::vector<uint8_t> serialize_plaintext(const Datagram &d) {
    std::vector<uint8_t> pt;
    if (d.is_parity()) {
        pt = d.parity;
        return pt;
    }
    if (d.has_pose()) putn(pt, d.pose, kPoseBytes);
    size_t n = d.dir.size();
    for (size_t i = 0; i < n; ++i) {
        uint32_t e = d.dir[i];
        if (!d.keep_dir_len)
            e = dir_set_len(e, uint32_t(i < d.tiles.size() ? d.tiles[i].size() : 0) & 0xfff);
        put32(pt, e);
    }
    for (const auto &t : d.tiles) putn(pt, t.data(), t.size());
    return pt;
}

// Header (24 bytes) followed by the plaintext.  The harness is what turns this
// into a wire datagram by sealing the plaintext under the AEAD, because random
// bytes never pass a tag check and a fuzzer that only ever exercises
// "auth_fail" is measuring nothing.
inline std::vector<uint8_t> serialize(const Datagram &d) {
    std::vector<uint8_t> pt = serialize_plaintext(d);
    std::vector<uint8_t> out;
    uint8_t h[kHeaderBytes];
    std::memcpy(h, d.hdr, kHeaderBytes);
    if (d.tile_count_override >= 0) h[6] = uint8_t(d.tile_count_override);
    else if (!d.is_parity()) h[6] = uint8_t(d.dir.size() & 0xff);
    set16(h + 20, d.payload_len_override >= 0 ? uint16_t(d.payload_len_override)
                                              : uint16_t(pt.size() & 0xffff));
    putn(out, h, kHeaderBytes);
    putn(out, pt.data(), pt.size());
    return out;
}

inline bool parse(const uint8_t *data, size_t size, Datagram &d) {
    if (size < kHeaderBytes) return false;
    std::memcpy(d.hdr, data, kHeaderBytes);
    size_t off = kHeaderBytes;
    if (d.is_parity()) {
        d.parity.assign(data + off, data + size);
        if (d.parity.size() > 4096) d.parity.resize(4096);
        return true;
    }
    if (d.has_pose()) {
        size_t k = size - off < kPoseBytes ? size - off : kPoseBytes;
        std::memcpy(d.pose, data + off, k);
        off += k;
    }
    size_t want = d.hdr[6];
    if (want > kMaxTiles) want = kMaxTiles;
    for (size_t i = 0; i < want && off + kDirEntryBytes <= size; ++i) {
        d.dir.push_back(get32(data + off));
        off += kDirEntryBytes;
    }
    for (size_t i = 0; i < d.dir.size(); ++i) {
        size_t len = dir_len(d.dir[i]);
        if (len > kMaxTileBytes) len = kMaxTileBytes;
        if (off + len > size) len = size - off;
        d.tiles.emplace_back(data + off, data + off + len);
        off += len;
    }
    if (d.dir.size() != d.hdr[6]) d.tile_count_override = int64_t(d.hdr[6]);
    return true;
}

// ------------------------------------------------------------------ generate
inline Datagram gen_datagram(Rng &rng) {
    Datagram d;
    // Byte 0: version [3:0], flags [7:4].
    d.hdr[0] = uint8_t(1 | (rng.below(16) << 4));
    d.hdr[1] = uint8_t(rng.below(2));                       // stream_id
    set16(d.hdr + 2, uint16_t(rng.below(8)));               // frame_id
    // tile_first: mostly inside a 68x34 grid, on a row boundary often enough
    // that a homogeneous run is actually produced.
    uint16_t row = uint16_t(rng.below(34)), col = uint16_t(rng.below(68));
    set16(d.hdr + 4, uint16_t(row * 68 + col));
    uint8_t n = uint8_t(1 + rng.below(6));
    if (col + n > 68) n = uint8_t(68 - col);
    d.hdr[6] = n;
    // byte 7: layer_id [1:0] | frag_idx [3:2] | frag_count [5:4] | fec_class [7:6]
    d.hdr[7] = uint8_t(0u | (0u << 2) | (0u << 4) | (rng.below(3) << 6));
    uint8_t band = uint8_t(row / 6);
    if (band > 5) band = 5;
    bool pose = rng.chance(3);
    // byte 8: band [2:0] | pose_hdr [3] | fec_m [6:4] | reserved [7]
    d.hdr[8] = uint8_t(uint32_t(band) | (pose ? 0x08u : 0u) | (rng.below(5) << 4));
    d.hdr[9] = uint8_t(0x3f & rng.u8());                    // caps
    set16(d.hdr + 10, uint16_t(rng.below(180)));            // pose_seq
    set16(d.hdr + 12, uint16_t(rng.below(16384) | (rng.below(2) << 14)));  // path_seq/id
    d.hdr[14] = uint8_t(rng.below(4));                      // fec_group
    uint8_t k = uint8_t(1 + rng.below(10)), idx = uint8_t(rng.below(14));
    d.hdr[15] = uint8_t((idx & 0xf) | (k << 4));
    set32(d.hdr + 16, rng.u32());                           // tx_ts
    set16(d.hdr + 22, uint16_t(rng.below(4000)));           // enc_us
    for (size_t i = 0; i < kPoseBytes; ++i) d.pose[i] = rng.u8();

    for (uint8_t i = 0; i < n; ++i) {
        size_t len = rng.chance(6) ? 0 : rng.below(200);
        std::vector<uint8_t> t(len);
        for (auto &b : t) b = rng.u8();
        uint32_t e = 0;
        e = set_bits(e, 12, 6, rng.below(64));   // qp
        e = set_bits(e, 18, 3, rng.below(5));    // mode
        e = set_bits(e, 21, 2, rng.below(4));    // res_level
        e = set_bits(e, 23, 1, rng.below(2));    // lossless
        e = set_bits(e, 24, 1, rng.below(2));    // chroma444
        e = set_bits(e, 25, 1, rng.below(2));    // alpha
        e = set_bits(e, 26, 2, rng.below(3));    // tile_class (v2), 3 reserved
        e = set_bits(e, 28, 2, rng.below(4));    // ref_delta (v2)
        d.dir.push_back(e);
        d.tiles.push_back(std::move(t));
    }
    if (rng.chance(10)) {  // a parity datagram
        d.hdr[6] = 0;
        d.dir.clear();
        d.tiles.clear();
        size_t L = rng.below(600);
        d.parity.resize(L + 4);
        for (auto &b : d.parity) b = rng.u8();
        set16(d.parity.data(), uint16_t(L + 2));
    }
    return d;
}

// ------------------------------------------------------------------- mutate
inline void mutate_header(Rng &rng, Datagram &d) {
    switch (rng.below(14)) {
        case 0:  // version nibble: anything but 1 must be dropped
            d.hdr[0] = uint8_t((d.hdr[0] & 0xf0) | (rng.chance(2) ? 1u : rng.below(16)));
            break;
        case 1: d.hdr[0] = uint8_t((d.hdr[0] & 0x0f) | (rng.below(16) << 4)); break;  // flags
        case 2: d.hdr[1] = rng.edge_u8(); break;                       // stream_id
        case 3: set16(d.hdr + 2, rng.edge_u16()); break;               // frame_id
        case 4: {  // tile_first: out of range, and runs that cross a row
            static const uint16_t v[] = {0, 1, 67, 68, 135, 2311, 2312, 2313, 0xffff};
            set16(d.hdr + 4, rng.chance(2) ? v[rng.below(sizeof(v) / sizeof(v[0]))]
                                           : rng.edge_u16());
            break;
        }
        case 5:  // tile_count, 0 == parity
            d.tile_count_override = rng.chance(3) ? -1 : int64_t(rng.edge_u8());
            break;
        case 6:  // layer_id / frag_idx / frag_count / fec_class
            d.hdr[7] = uint8_t(rng.below(4) | (rng.below(4) << 2) | (rng.below(4) << 4) |
                               (rng.below(4) << 6));
            break;
        case 7:  // band / pose_hdr / fec_m, and the byte's reserved top bit
            d.hdr[8] = uint8_t(rng.below(8) | (rng.below(2) << 3) | (rng.below(8) << 4) |
                               (rng.chance(6) ? 0x80u : 0u));
            break;
        case 8: d.hdr[9] = rng.edge_u8(); break;                       // caps
        case 9: set16(d.hdr + 10, rng.edge_u16()); break;              // pose_seq
        case 10: set16(d.hdr + 12, rng.edge_u16()); break;             // path_seq / path_id
        case 11:  // fec_group / fec_idx / fec_k
            d.hdr[14] = rng.edge_u8();
            d.hdr[15] = uint8_t(rng.below(16) | (rng.below(16) << 4));
            break;
        case 12: set32(d.hdr + 16, rng.edge_u32()); break;             // tx_ts
        default:  // payload_len: desync against the real plaintext length
            d.payload_len_override = rng.chance(3) ? -1 : int64_t(rng.edge_u16());
            set16(d.hdr + 22, rng.edge_u16());                         // enc_us
            break;
    }
}

inline void mutate_datagram(Rng &rng, Datagram &d) {
    switch (rng.below(12)) {
        case 0:
        case 1:
        case 2: mutate_header(rng, d); break;
        case 3:  // a directory entry's descriptor fields
            if (!d.dir.empty()) {
                uint32_t &e = d.dir[rng.below(uint32_t(d.dir.size()))];
                switch (rng.below(5)) {
                    case 0: e = set_bits(e, 12, 6, rng.below(64)); break;   // qp
                    case 1: e = set_bits(e, 18, 3, rng.below(8)); break;    // mode 5..7 reserved
                    case 2: e = set_bits(e, 21, 2, rng.below(4)); break;    // res_level
                    case 3: e = set_bits(e, 23, 3, rng.below(8)); break;    // lossless/444/alpha
                    default:
                        e = set_bits(e, 26, 2, rng.below(4));   // tile_class, 3 reserved
                        e = set_bits(e, 28, 2, rng.below(4));   // ref_delta
                        if (rng.chance(4)) e = set_bits(e, 30, 2, rng.below(4));  // reserved
                        break;
                }
            }
            break;
        case 4:  // a directory entry's length, deliberately not repaired
            if (!d.dir.empty()) {
                uint32_t &e = d.dir[rng.below(uint32_t(d.dir.size()))];
                static const uint32_t v[] = {0, 1, 4094, 4095, 0xfff};
                e = dir_set_len(e, rng.chance(2) ? v[rng.below(5)] : rng.below(4096));
                d.keep_dir_len = true;
            }
            break;
        case 5:  // tile bytes
            if (!d.tiles.empty())
                mutate_bytes(d.tiles[rng.below(uint32_t(d.tiles.size()))], kMaxTileBytes, rng);
            break;
        case 6:  // add / drop a tile (directory and bytes stay in step)
            if (rng.chance(2) && !d.dir.empty()) {
                size_t i = rng.below(uint32_t(d.dir.size()));
                d.dir.erase(d.dir.begin() + long(i));
                if (i < d.tiles.size()) d.tiles.erase(d.tiles.begin() + long(i));
            } else if (d.dir.size() < kMaxTiles) {
                d.dir.push_back(d.dir.empty() ? 0u : d.dir.back());
                std::vector<uint8_t> t(rng.below(120));
                for (auto &b : t) b = rng.u8();
                d.tiles.push_back(std::move(t));
            }
            break;
        case 7:  // desync directory against bytes: drop bytes, keep entries
            if (!d.tiles.empty()) {
                d.tiles.pop_back();
                d.keep_dir_len = true;
            }
            break;
        case 8:  // pose header presence and content (v2: byte 8 bit 3)
            d.hdr[8] = uint8_t((d.hdr[8] & 0xf7) | (rng.below(2) << 3));
            d.pose[rng.below(kPoseBytes)] = rng.edge_u8();
            break;
        case 9:  // parity payload: u16 L || block(L+2)
            if (d.is_parity() || rng.chance(3)) {
                d.hdr[6] = 0;
                d.tile_count_override = 0;
                if (d.parity.size() < 2) d.parity.resize(2 + rng.below(64));
                switch (rng.below(3)) {
                    case 0: set16(d.parity.data(), rng.edge_u16()); break;
                    case 1: mutate_bytes(d.parity, 2048, rng); break;
                    default:
                        d.parity.resize(rng.below(600));
                        break;
                }
            }
            break;
        case 10: d.keep_dir_len = false; break;  // repair, to get back to valid
        default: {
            Rng sub(rng.next());
            d = gen_datagram(sub);
            break;
        }
    }
}

inline size_t mutate_datagram_bytes(uint8_t *data, size_t size, size_t max_size, unsigned seed) {
    Rng rng(mix_seed(seed, size));
    Datagram d;
    if (!parse(data, size, d)) d = gen_datagram(rng);
    int steps = 1 + int(rng.below(3));
    for (int i = 0; i < steps; ++i) mutate_datagram(rng, d);
    std::vector<uint8_t> out = serialize(d);
    if (rng.chance(20) && !out.empty()) out.resize(1 + rng.below(uint32_t(out.size())));
    return emit(out, data, max_size);
}

// ================================================================= feedback
// TRANSPORT.md 8: 8-byte header, 1..3 band records of 20 bytes + a bitmap,
// then a 4-byte trailer.
inline constexpr size_t kFbHeaderBytes = 8;
inline constexpr size_t kFbRecordBytes = 20;
inline constexpr size_t kFbTrailerBytes = 4;

struct BandRec {
    uint8_t rec[kFbRecordBytes] = {};
    std::vector<uint8_t> bitmap;
    uint8_t bitmap_mode() const { return uint8_t(rec[3] & 3); }
};

struct Feedback {
    uint8_t hdr[kFbHeaderBytes] = {};
    std::vector<BandRec> bands;
    uint8_t trailer[kFbTrailerBytes] = {};
    int64_t band_count_override = -1;
    int64_t truncate_to = -1;
};

inline std::vector<uint8_t> serialize(const Feedback &f) {
    std::vector<uint8_t> out;
    uint8_t h[kFbHeaderBytes];
    std::memcpy(h, f.hdr, kFbHeaderBytes);
    h[5] = uint8_t(f.band_count_override >= 0 ? f.band_count_override : f.bands.size());
    putn(out, h, kFbHeaderBytes);
    for (const auto &b : f.bands) {
        putn(out, b.rec, kFbRecordBytes);
        putn(out, b.bitmap.data(), b.bitmap.size());
    }
    putn(out, f.trailer, kFbTrailerBytes);
    if (f.truncate_to >= 0 && static_cast<size_t>(f.truncate_to) < out.size())
        out.resize(static_cast<size_t>(f.truncate_to));
    return out;
}

inline size_t bitmap_bytes(uint8_t mode, uint16_t tiles_in_band, const uint8_t *p, size_t avail) {
    switch (mode & 3) {
        case 0: {  // RAW
            size_t n = (static_cast<size_t>(tiles_in_band) + 7) / 8;
            return n > avail ? avail : n;
        }
        case 1: return 0;  // ALL
        case 2: {          // RLE: u8 nruns, then nruns * (u16 start, u8 len)
            if (avail == 0) return 0;
            size_t n = 1 + static_cast<size_t>(p[0]) * 3;
            return n > avail ? avail : n;
        }
        default: return 0;  // reserved
    }
}

inline bool parse(const uint8_t *data, size_t size, Feedback &f) {
    if (size < kFbHeaderBytes) return false;
    std::memcpy(f.hdr, data, kFbHeaderBytes);
    uint16_t tib = get16(f.hdr + 6);
    size_t off = kFbHeaderBytes;
    size_t want = f.hdr[5];
    if (want > 8) want = 8;
    for (size_t i = 0; i < want; ++i) {
        if (off + kFbRecordBytes > size) break;
        BandRec b;
        std::memcpy(b.rec, data + off, kFbRecordBytes);
        off += kFbRecordBytes;
        size_t n = bitmap_bytes(b.bitmap_mode(), tib, data + off, size - off);
        b.bitmap.assign(data + off, data + off + n);
        off += n;
        f.bands.push_back(std::move(b));
    }
    if (f.bands.size() != f.hdr[5]) f.band_count_override = int64_t(f.hdr[5]);
    if (off + kFbTrailerBytes <= size) std::memcpy(f.trailer, data + off, kFbTrailerBytes);
    return true;
}

inline Feedback gen_feedback(Rng &rng) {
    Feedback f;
    f.hdr[0] = uint8_t(1 | (rng.below(16) << 4));
    f.hdr[1] = uint8_t(rng.below(2));               // stream_id
    set16(f.hdr + 2, uint16_t(rng.below(8)));       // frame_id
    f.hdr[4] = uint8_t(rng.below(6));               // band
    uint16_t tib = rng.chance(3) ? uint16_t(rng.below(600)) : 408;
    set16(f.hdr + 6, tib);
    size_t nb = 1 + rng.below(3);
    for (size_t i = 0; i < nb; ++i) {
        BandRec b;
        set16(b.rec, uint16_t(rng.below(8)));
        b.rec[2] = uint8_t(rng.below(6));
        uint8_t mode = uint8_t(rng.below(3));
        b.rec[3] = uint8_t(mode | (rng.below(2) << 2) | (rng.below(2) << 3));
        set32(b.rec + 4, rng.u32());
        set32(b.rec + 8, rng.u32());
        set16(b.rec + 12, uint16_t(rng.below(4000)));
        set16(b.rec + 14, uint16_t(rng.below(tib ? tib : 1)));
        set16(b.rec + 16, uint16_t(rng.below(64)));
        b.rec[18] = uint8_t(rng.below(8));
        b.rec[19] = uint8_t(rng.below(8));
        if (mode == 0) b.bitmap.resize((static_cast<size_t>(tib) + 7) / 8);
        else if (mode == 2) {
            uint8_t nruns = uint8_t(rng.below(8));
            b.bitmap.push_back(nruns);
            for (uint8_t r = 0; r < nruns; ++r) {
                put16(b.bitmap, uint16_t(rng.below(tib ? tib : 1)));
                b.bitmap.push_back(uint8_t(rng.below(64)));
            }
        }
        for (size_t j = (b.bitmap.empty() ? 0 : 1); j < b.bitmap.size(); ++j)
            if (mode == 0) b.bitmap[j] = rng.u8();
        f.bands.push_back(std::move(b));
    }
    for (auto &t : f.trailer) t = rng.u8();
    return f;
}

inline void mutate_feedback(Rng &rng, Feedback &f) {
    BandRec *b = f.bands.empty() ? nullptr : &f.bands[rng.below(uint32_t(f.bands.size()))];
    switch (rng.below(11)) {
        case 0: f.hdr[0] = uint8_t((f.hdr[0] & 0xf0) | rng.below(16)); break;  // version
        case 1: f.hdr[0] = uint8_t((f.hdr[0] & 0x0f) | (rng.below(16) << 4)); break;
        case 2: set16(f.hdr + 2, rng.edge_u16()); f.hdr[4] = rng.edge_u8(); break;
        case 3:  // band_count: the spec allows 1..3
            f.band_count_override = rng.chance(3) ? -1 : int64_t(rng.edge_u8());
            break;
        case 4:  // tiles_in_band drives the RAW bitmap length
            set16(f.hdr + 6, rng.chance(2) ? rng.edge_u16() : uint16_t(rng.below(4096)));
            break;
        case 5:  // bitmap_mode, including the reserved 3
            if (b) b->rec[3] = uint8_t(rng.below(4) | (rng.below(16) << 2));
            break;
        case 6:  // an RLE run table that promises more than the bitmap holds
            if (b) {
                b->rec[3] = uint8_t((b->rec[3] & 0xfc) | 2);
                if (b->bitmap.empty()) b->bitmap.push_back(0);
                b->bitmap[0] = rng.edge_u8();
                if (rng.chance(2)) b->bitmap.resize(1 + rng.below(24));
            }
            break;
        case 7:  // truncate or extend a bitmap without touching its mode
            if (b) {
                size_t n = rng.chance(2) ? rng.below(80) : b->bitmap.size() / 2;
                size_t old = b->bitmap.size();
                b->bitmap.resize(n);
                for (size_t i = old; i < n; ++i) b->bitmap[i] = rng.u8();
            }
            break;
        case 8:  // counters: conceal_tiles / late_tiles / fec counts
            if (b) {
                set16(b->rec + 14, rng.edge_u16());
                set16(b->rec + 16, rng.edge_u16());
                b->rec[18] = rng.edge_u8();
                b->rec[19] = rng.edge_u8();
            }
            break;
        case 9:  // add / drop a band record
            if (rng.chance(2) && f.bands.size() > 1) f.bands.pop_back();
            else if (b && f.bands.size() < 6) f.bands.push_back(*b);
            break;
        default:
            for (auto &t : f.trailer) t = rng.edge_u8();
            f.truncate_to = rng.chance(3) ? int64_t(rng.below(64)) : -1;
            break;
    }
}

inline size_t mutate_feedback_bytes(uint8_t *data, size_t size, size_t max_size, unsigned seed) {
    Rng rng(mix_seed(seed, size));
    Feedback f;
    if (!parse(data, size, f)) f = gen_feedback(rng);
    int steps = 1 + int(rng.below(3));
    for (int i = 0; i < steps; ++i) mutate_feedback(rng, f);
    std::vector<uint8_t> out = serialize(f);
    return emit(out, data, max_size);
}

}  // namespace nxf::nxt

#endif  // NXFUZZ_NXT_WIRE_H
