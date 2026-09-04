// nxvc_stream.h -- a parse/mutate/serialize model of the NX Warp v1 bitstream.
//
// Normative layout: docs/SYNTAX.md sections 2, 3, 4 and 11.
//
//   file  := stream_header(64) ext(ext_len) frame*
//   frame := frame_header(40) [matrices(128)] [table_set(120)]* tile_row*
//   row   := row_header(12) tile*
//   tile  := word0(4) word1(4) [mv(2)] [alpha(1)] payload(payload_len)
//
// The point of the model is that a mutation lands on a *field*, and the
// serializer then repairs every length that the field's new value invalidates
// (ext_len, frame_bytes, tile_count, payload_len).  A purely byte-level
// mutator spends essentially all of its budget being rejected by the stream
// header; with this model a mutated stream still reaches the tile decoder.
//
// Structural repair is deliberately not total.  A dedicated family of
// "desync" mutations breaks exactly one length field on purpose, because the
// decoder's length checks are precisely the code that must never read out of
// bounds.  Those are the mutations that found the reproducers in
// fuzz/regressions/.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXFUZZ_NXVC_STREAM_H
#define NXFUZZ_NXVC_STREAM_H

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nxfuzz.h"

namespace nxf::nxv {

// ---------------------------------------------------------------- constants
inline constexpr uint32_t kMagic = 0x3156584Eu;  // 'N','X','V','1'
inline constexpr size_t kStreamHdr = 64;
inline constexpr size_t kFrameHdr = 40;
inline constexpr size_t kRowHdr = 12;
inline constexpr size_t kTileHdr = 8;
inline constexpr size_t kMatrixBytes = 128;
inline constexpr size_t kTableBytes = 120;

// Keep generated and mutated streams small; a fuzz corpus that grows without
// bound stops finding anything.
inline constexpr size_t kMaxFrames = 6;
inline constexpr size_t kMaxRows = 12;
inline constexpr size_t kMaxTilesPerRow = 20;
inline constexpr size_t kMaxPayload = 4096;

// Tools this reference decoder implements (nxvc.h NXVC_TOOLS_SUPPORTED).
inline constexpr uint64_t kToolsSupported = 0x3FFull;  // bits 0..9

// ------------------------------------------------------------------- model
struct Tile {
    uint32_t w0 = 0;
    uint32_t w1 = 0;
    int8_t mv_x = 0, mv_y = 0;
    uint8_t alpha_value = 0;
    std::vector<uint8_t> payload;

    // Fields of word0 / word1 (SYNTAX.md 4.1).
    uint32_t layer() const { return get_bits(w0, 0, 2); }
    uint32_t tile_index() const { return get_bits(w0, 4, 12); }
    uint32_t mode() const { return get_bits(w1, 0, 3); }
    uint32_t alpha_mode() const { return get_bits(w1, 6, 2); }
    bool mv_present() const { return get_bits(w1, 20, 1) != 0; }
    void set_tile_index(uint32_t v) { w0 = set_bits(w0, 4, 12, v); }
    void set_payload_len(uint32_t v) { w0 = set_bits(w0, 16, 16, v); }
};

struct Row {
    uint16_t frame_number = 0;
    uint8_t row_index = 0;
    uint64_t skip_bitmap = 0;
    std::vector<Tile> tiles;
    // Non-zero means "write this tile_count instead of tiles.size()".
    int count_override = -1;
};

struct Frame {
    uint8_t hdr[kFrameHdr] = {};
    std::vector<uint8_t> matrices;                   // 0 or 128 bytes
    std::vector<std::array<uint8_t, kTableBytes>> tables;  // one per set bit
    std::vector<Row> rows;
    // Non-zero means "write this frame_bytes instead of the real length".
    int64_t bytes_override = -1;

    uint8_t quant_matrix() const { return hdr[31]; }
    uint8_t tables_present() const { return hdr[32]; }
};

struct Stream {
    uint8_t hdr[kStreamHdr] = {};
    std::vector<uint8_t> ext;
    std::vector<Frame> frames;
    int64_t ext_len_override = -1;
    // Truncate the serialized result to this many bytes (-1 = whole).
    int64_t truncate_to = -1;

    uint16_t width() const { return get16(hdr + 8); }
    uint16_t height() const { return get16(hdr + 10); }
    uint8_t chroma_format() const { return hdr[15]; }
    uint8_t alpha_present() const { return hdr[40]; }
};

// -------------------------------------------------------------- serializer
inline void serialize_tile(const Tile &t, std::vector<uint8_t> &out) {
    uint32_t w0 = set_bits(t.w0, 16, 16, uint32_t(t.payload.size() & 0xffff));
    put32(out, w0);
    put32(out, t.w1);
    if (t.mv_present()) {
        put8(out, uint8_t(t.mv_x));
        put8(out, uint8_t(t.mv_y));
    }
    if (t.alpha_mode() == 1) put8(out, t.alpha_value);
    putn(out, t.payload.data(), t.payload.size());
}

inline void serialize_row(const Row &r, std::vector<uint8_t> &out) {
    put16(out, r.frame_number);
    put8(out, r.row_index);
    put8(out, uint8_t(r.count_override >= 0 ? r.count_override : int(r.tiles.size())));
    put64(out, r.skip_bitmap);
    for (const auto &t : r.tiles) serialize_tile(t, out);
}

inline void serialize_frame(const Frame &f, std::vector<uint8_t> &out) {
    std::vector<uint8_t> body;
    if (f.quant_matrix() == 255) {
        std::vector<uint8_t> m = f.matrices;
        m.resize(kMatrixBytes, 16);
        putn(body, m.data(), kMatrixBytes);
    }
    size_t ti = 0;
    for (int k = 0; k < 8; ++k) {
        if (!(f.tables_present() & (1u << k))) continue;
        if (ti < f.tables.size()) {
            putn(body, f.tables[ti].data(), kTableBytes);
            ++ti;
        } else {
            body.resize(body.size() + kTableBytes, 16);
        }
    }
    for (const auto &r : f.rows) serialize_row(r, body);

    uint8_t h[kFrameHdr];
    std::memcpy(h, f.hdr, kFrameHdr);
    uint32_t total = uint32_t(kFrameHdr + body.size());
    set32(h + 36, f.bytes_override >= 0 ? uint32_t(f.bytes_override) : total);
    putn(out, h, kFrameHdr);
    putn(out, body.data(), body.size());
}

inline std::vector<uint8_t> serialize(const Stream &s) {
    std::vector<uint8_t> out;
    uint8_t h[kStreamHdr];
    std::memcpy(h, s.hdr, kStreamHdr);
    set16(h + 62, s.ext_len_override >= 0 ? uint16_t(s.ext_len_override)
                                          : uint16_t(s.ext.size() & 0xffff));
    putn(out, h, kStreamHdr);
    putn(out, s.ext.data(), s.ext.size());
    for (const auto &f : s.frames) serialize_frame(f, out);
    if (s.truncate_to >= 0 && static_cast<size_t>(s.truncate_to) < out.size())
        out.resize(static_cast<size_t>(s.truncate_to));
    return out;
}

// ------------------------------------------------------------------ parser
// Tolerant: it never fails on a short buffer, it just stops early.  The point
// is to recover as much structure as the bytes support so a mutation can be
// aimed at a field; anything it cannot parse stays a raw payload tail.
class Reader {
  public:
    Reader(const uint8_t *p, size_t n) : p_(p), n_(n) {}
    size_t left() const { return n_ - o_; }
    bool need(size_t k) const { return left() >= k; }
    const uint8_t *take(size_t k) {
        if (!need(k)) return nullptr;
        const uint8_t *r = p_ + o_;
        o_ += k;
        return r;
    }
    uint8_t u8() { const uint8_t *q = take(1); return q ? *q : 0; }
    uint16_t u16() { const uint8_t *q = take(2); return q ? get16(q) : 0; }
    uint32_t u32() { const uint8_t *q = take(4); return q ? get32(q) : 0; }
    uint64_t u64() { const uint8_t *q = take(8); return q ? get64(q) : 0; }
    size_t pos() const { return o_; }
    void seek(size_t o) { o_ = o < n_ ? o : n_; }

  private:
    const uint8_t *p_;
    size_t n_;
    size_t o_ = 0;
};

inline bool parse_tile(Reader &r, Tile &t) {
    if (!r.need(kTileHdr)) return false;
    t.w0 = r.u32();
    t.w1 = r.u32();
    if (t.mv_present()) {
        t.mv_x = int8_t(r.u8());
        t.mv_y = int8_t(r.u8());
    }
    if (t.alpha_mode() == 1) t.alpha_value = r.u8();
    size_t len = get_bits(t.w0, 16, 16);
    if (len > kMaxPayload) len = kMaxPayload;
    if (len > r.left()) len = r.left();
    const uint8_t *q = r.take(len);
    t.payload.assign(q, q + len);
    return true;
}

inline bool parse_row(Reader &r, Row &row) {
    if (!r.need(kRowHdr)) return false;
    row.frame_number = r.u16();
    row.row_index = r.u8();
    uint8_t count = r.u8();
    row.skip_bitmap = r.u64();
    size_t want = count > kMaxTilesPerRow ? kMaxTilesPerRow : count;
    for (size_t i = 0; i < want; ++i) {
        Tile t;
        if (!parse_tile(r, t)) break;
        row.tiles.push_back(std::move(t));
    }
    if (row.tiles.size() != count) row.count_override = count;
    return true;
}

inline bool parse(const uint8_t *data, size_t size, Stream &s) {
    if (size < kStreamHdr) return false;
    Reader r(data, size);
    std::memcpy(s.hdr, r.take(kStreamHdr), kStreamHdr);
    size_t ext_len = get16(s.hdr + 62);
    size_t take_ext = ext_len > r.left() ? r.left() : ext_len;
    if (take_ext > 4096) take_ext = 4096;
    const uint8_t *e = r.take(take_ext);
    s.ext.assign(e, e + take_ext);
    if (take_ext != ext_len) s.ext_len_override = int64_t(ext_len);

    while (r.need(kFrameHdr) && s.frames.size() < kMaxFrames) {
        size_t frame_start = r.pos();
        Frame f;
        std::memcpy(f.hdr, r.take(kFrameHdr), kFrameHdr);
        uint32_t frame_bytes = get32(f.hdr + 36);

        if (f.quant_matrix() == 255) {
            size_t k = kMatrixBytes > r.left() ? r.left() : kMatrixBytes;
            const uint8_t *m = r.take(k);
            f.matrices.assign(m, m + k);
        }
        for (int k = 0; k < 8; ++k) {
            if (!(f.tables_present() & (1u << k))) continue;
            std::array<uint8_t, kTableBytes> tb{};
            size_t take = kTableBytes > r.left() ? r.left() : kTableBytes;
            const uint8_t *q = r.take(take);
            std::memcpy(tb.data(), q, take);
            f.tables.push_back(tb);
        }

        // Frame extent: trust frame_bytes when it is plausible, otherwise read
        // rows until they stop parsing.
        size_t limit = size;
        if (frame_bytes >= kFrameHdr && frame_start + frame_bytes <= size)
            limit = frame_start + frame_bytes;
        else
            f.bytes_override = int64_t(frame_bytes);

        while (r.pos() < limit && f.rows.size() < kMaxRows) {
            Row row;
            if (!parse_row(r, row)) break;
            f.rows.push_back(std::move(row));
        }
        if (r.pos() < limit) r.seek(limit);
        s.frames.push_back(std::move(f));
        if (r.pos() <= frame_start) break;  // no progress: bail out
    }
    return true;
}

// --------------------------------------------------------------- generation
// A syntactically well-formed stream with random-but-legal header fields.
// Tile payloads are random bytes: without the encoder they cannot be valid
// rANS, and that is fine -- they exercise exactly the entropy-decoder
// rejection paths the seed corpus (real vectors) does not.
inline void gen_stream_header(Rng &rng, uint8_t hdr[kStreamHdr], uint16_t &w, uint16_t &h,
                              bool &chroma444, bool &alpha) {
    std::memset(hdr, 0, kStreamHdr);
    set32(hdr, kMagic);
    hdr[4] = 1;                         // version
    hdr[5] = uint8_t(rng.below(3));     // profile
    hdr[6] = uint8_t(rng.below(4));     // level
    hdr[7] = 0;                         // tile_size: 64x64
    static const uint16_t dims[] = {16, 32, 64, 66, 96, 128, 130, 192, 256};
    w = dims[rng.below(sizeof(dims) / sizeof(dims[0]))];
    h = dims[rng.below(sizeof(dims) / sizeof(dims[0]))];
    set16(hdr + 8, w);
    set16(hdr + 10, h);
    hdr[12] = 1;  // eyes: Phase 1 requires 1
    hdr[13] = 8;  // bit_depth
    hdr[14] = 1;  // num_layers
    chroma444 = rng.chance(2);
    hdr[15] = chroma444 ? 1 : 0;
    uint64_t tools = 1;  // INTRA_DC_PLANE is mandatory
    if (rng.chance(2)) tools |= 1ull << 1;  // TRANSFORM_SKIP
    if (rng.chance(2)) tools |= 1ull << 2;  // RES_LEVEL
    if (chroma444) tools |= 1ull << 3;      // CHROMA444
    alpha = rng.chance(4);
    if (alpha) tools |= 1ull << 4;          // ALPHA
    if (rng.chance(4)) tools |= 1ull << 5;  // LOSSLESS
    if (rng.chance(3)) tools |= 1ull << 6;  // CUSTOM_TABLES
    if (rng.chance(3)) tools |= 1ull << 7;  // NSUB_VAR
    if (chroma444 && rng.chance(3)) tools |= 1ull << 8;  // PER_TILE_CHROMA
    for (int i = 0; i < 8; ++i) hdr[32 + i] = uint8_t(tools >> (8 * i));
    hdr[40] = alpha ? 1 : 0;
    hdr[41] = 0;  // color_transform: YCoCg-R needs 4:4:4 and RGB input
    if (chroma444 && rng.chance(4)) {
        hdr[41] = 1;
        for (int i = 0; i < 8; ++i)
            hdr[32 + i] = uint8_t((tools | (1ull << 9)) >> (8 * i));
    }
    set16(hdr + 62, 0);
}

inline Tile gen_tile(Rng &rng, uint32_t index, bool chroma444, bool alpha) {
    Tile t;
    uint32_t w0 = 0;
    w0 = set_bits(w0, 0, 2, 0);          // layer 0
    w0 = set_bits(w0, 2, 1, 0);          // eye 0
    w0 = set_bits(w0, 4, 12, index);
    t.w0 = w0;
    uint32_t w1 = 0;
    w1 = set_bits(w1, 0, 3, 3);          // INTRA (Phase 1)
    w1 = set_bits(w1, 3, 2, rng.below(3));           // res_level 0..2
    w1 = set_bits(w1, 5, 1, chroma444 && rng.chance(2) ? 1 : 0);
    w1 = set_bits(w1, 6, 2, alpha ? rng.below(3) : 0);
    w1 = set_bits(w1, 8, 6, rng.u8() & 0x3f);        // qp_delta, signed 6-bit
    w1 = set_bits(w1, 14, 3, rng.below(8));          // table_set
    w1 = set_bits(w1, 17, 3, rng.below(6));          // nsub_log2 0..5
    w1 = set_bits(w1, 20, 1, rng.chance(6) ? 1 : 0); // mv_present
    w1 = set_bits(w1, 21, 2, rng.below(4));          // ref_sel
    w1 = set_bits(w1, 23, 1, rng.chance(3) ? 1 : 0); // tskip
    w1 = set_bits(w1, 24, 2, rng.below(4));          // wgt
    t.w1 = w1;
    t.mv_x = int8_t(rng.u8());
    t.mv_y = int8_t(rng.u8());
    t.alpha_value = rng.u8();
    // A payload that at least has room for the per-lane initial rANS states,
    // so the decoder gets past its first length check reasonably often.
    uint32_t lanes = 1u << get_bits(w1, 17, 3);
    size_t len = static_cast<size_t>(4 * lanes) + rng.below(160);
    if (len > kMaxPayload) len = kMaxPayload;
    t.payload.resize(len);
    for (auto &b : t.payload) b = rng.u8();
    return t;
}

inline Stream gen_stream(Rng &rng) {
    Stream s;
    uint16_t w = 0, h = 0;
    bool chroma444 = false, alpha = false;
    gen_stream_header(rng, s.hdr, w, h, chroma444, alpha);
    if (rng.chance(3)) {  // a TLV extension area
        uint16_t type = rng.chance(2) ? uint16_t(0x8000 | rng.below(64))
                                      : uint16_t(rng.below(8));
        uint16_t len = uint16_t(rng.below(24));
        put16(s.ext, type);
        put16(s.ext, len);
        for (uint16_t i = 0; i < len; ++i) s.ext.push_back(rng.u8());
        while (s.ext.size() & 3) s.ext.push_back(0);
    }

    uint32_t tiles_x = (uint32_t(w) + 63) / 64;
    uint32_t tiles_y = (uint32_t(h) + 63) / 64;
    if (tiles_x > kMaxTilesPerRow) tiles_x = uint32_t(kMaxTilesPerRow);
    if (tiles_y > kMaxRows) tiles_y = uint32_t(kMaxRows);

    size_t nframes = 1 + rng.below(2);
    for (size_t fi = 0; fi < nframes; ++fi) {
        Frame f;
        std::memset(f.hdr, 0, kFrameHdr);
        set16(f.hdr, uint16_t(fi));
        for (int i = 0; i < 26; ++i) f.hdr[2 + i] = rng.u8();  // pose, opaque
        f.hdr[28] = uint8_t(rng.below(64));                    // base_qp
        f.hdr[29] = rng.u8();  // chroma_qp_off: an i8, carried as a raw byte
        f.hdr[30] = rng.u8();  // alpha_qp_off: likewise
        f.hdr[31] = rng.chance(6) ? 255 : uint8_t(rng.below(4));
        f.hdr[32] = rng.chance(4) ? uint8_t(1u << rng.below(8)) : 0;  // tables
        f.hdr[33] = 0;                                         // ref_slots
        f.hdr[34] = fi == 0 ? 1 : uint8_t(rng.below(2));       // flags: reset
        if (f.hdr[31] == 255) {
            f.matrices.resize(kMatrixBytes);
            for (auto &b : f.matrices) b = uint8_t(1 + rng.below(32));
        }
        for (int k = 0; k < 8; ++k) {
            if (!(f.hdr[32] & (1u << k))) continue;
            std::array<uint8_t, kTableBytes> tb{};
            for (auto &b : tb) b = rng.u8();
            f.tables.push_back(tb);
        }
        for (uint32_t ry = 0; ry < tiles_y; ++ry) {
            Row row;
            row.frame_number = uint16_t(fi);
            row.row_index = uint8_t(ry);
            row.skip_bitmap = 0;  // Phase 1 rejects a nonzero skip bitmap
            for (uint32_t tx = 0; tx < tiles_x; ++tx)
                row.tiles.push_back(gen_tile(rng, tx, chroma444, alpha));
            f.rows.push_back(std::move(row));
        }
        s.frames.push_back(std::move(f));
    }
    return s;
}

// ----------------------------------------------------------------- mutation
// One stream-header field, replaced with a value drawn from the legal range
// most of the time and from the interesting edges the rest of the time.
inline void mutate_stream_header(Rng &rng, Stream &s) {
    switch (rng.below(14)) {
        case 0:  // magic: rare, it kills all downstream coverage
            if (rng.chance(8)) set32(s.hdr, rng.chance(2) ? kMagic : rng.edge_u32());
            break;
        case 1: s.hdr[4] = rng.chance(2) ? 1 : rng.edge_u8(); break;      // version
        case 2: s.hdr[5] = uint8_t(rng.below(5)); break;                  // profile
        case 3: s.hdr[7] = rng.chance(3) ? uint8_t(rng.below(2)) : rng.edge_u8(); break;
        case 4: {  // width / height: legal range is [16, 4096] and even
            static const uint16_t d[] = {0,  1,   15,  16,  17,   64,   65,
                                         66, 128, 256, 512, 4094, 4096, 4098, 0xffff};
            uint8_t *p = s.hdr + (rng.chance(2) ? 8 : 10);
            set16(p, rng.chance(3) ? d[rng.below(sizeof(d) / sizeof(d[0]))] : rng.edge_u16());
            break;
        }
        case 5: s.hdr[12] = uint8_t(rng.below(4)); break;                 // eyes
        case 6: s.hdr[13] = rng.chance(2) ? 8 : uint8_t(rng.below(3) ? 10 : rng.u8()); break;
        case 7: s.hdr[14] = uint8_t(rng.below(6)); break;                 // num_layers
        case 8: s.hdr[15] = uint8_t(rng.below(3)); break;                 // chroma_format
        case 9: {  // tools: mostly inside the supported mask, sometimes not
            uint64_t t;
            if (rng.chance(3)) t = rng.next();
            else t = (rng.next() & kToolsSupported) | 1ull;
            for (int i = 0; i < 8; ++i) s.hdr[32 + i] = uint8_t(t >> (8 * i));
            break;
        }
        case 10: s.hdr[40] = uint8_t(rng.below(3)); break;                // alpha_present
        case 11: s.hdr[41] = uint8_t(rng.below(3)); break;                // color_transform
        case 12: s.hdr[42 + rng.below(20)] = rng.edge_u8(); break;        // reserved
        default: {  // layer_desc
            set32(s.hdr + 16 + 4 * rng.below(4), rng.edge_u32());
            break;
        }
    }
}

inline void mutate_ext(Rng &rng, Stream &s) {
    switch (rng.below(4)) {
        case 0: {  // append a fresh TLV record
            uint16_t len = uint16_t(rng.below(32));
            put16(s.ext, rng.edge_u16());
            put16(s.ext, len);
            for (uint16_t i = 0; i < len; ++i) s.ext.push_back(rng.edge_u8());
            while (s.ext.size() & 3) s.ext.push_back(0);
            break;
        }
        case 1:  // corrupt a record length in place: a record that runs past
                 // the end of the area must be reported malformed, not read
            if (s.ext.size() >= 4) set16(&s.ext[(rng.below(uint32_t(s.ext.size() / 4))) * 4 + 2],
                                         rng.edge_u16());
            break;
        case 2:  // desync ext_len against the bytes actually present
            s.ext_len_override = rng.chance(2) ? int64_t(rng.edge_u16())
                                               : int64_t(s.ext.size() + 1 + rng.below(64));
            break;
        default: mutate_bytes(s.ext, 512, rng); break;
    }
}

inline void mutate_frame_header(Rng &rng, Frame &f) {
    switch (rng.below(9)) {
        case 0: set16(f.hdr, rng.edge_u16()); break;                  // frame_number
        case 1: f.hdr[2 + rng.below(26)] = rng.edge_u8(); break;      // pose (opaque)
        case 2: f.hdr[28] = rng.chance(3) ? uint8_t(rng.below(64)) : rng.edge_u8(); break;
        case 3: f.hdr[29] = rng.edge_u8(); break;                     // chroma_qp_off
        case 4: f.hdr[30] = rng.edge_u8(); break;                     // alpha_qp_off
        case 5: {  // quant_matrix: 0..3 or 255
            static const uint8_t v[] = {0, 1, 2, 3, 4, 128, 254, 255};
            f.hdr[31] = v[rng.below(sizeof(v))];
            if (f.hdr[31] == 255 && f.matrices.size() != kMatrixBytes) {
                f.matrices.resize(kMatrixBytes);
                for (auto &b : f.matrices) b = rng.edge_u8();
            }
            break;
        }
        case 6: {  // tables_present, and the table blobs that must follow it
            f.hdr[32] = rng.edge_u8();
            int want = 0;
            for (int k = 0; k < 8; ++k) want += (f.hdr[32] >> k) & 1;
            while (int(f.tables.size()) < want) {
                std::array<uint8_t, kTableBytes> tb{};
                for (auto &b : tb) b = rng.u8();
                f.tables.push_back(tb);
            }
            f.tables.resize(static_cast<size_t>(want));
            break;
        }
        case 7: f.hdr[34] = rng.edge_u8(); break;                     // flags
        default: f.hdr[33] = rng.edge_u8(); f.hdr[35] = rng.edge_u8(); break;
    }
}

inline void mutate_tile(Rng &rng, Tile &t) {
    switch (rng.below(12)) {
        case 0: t.w0 = set_bits(t.w0, 0, 2, rng.below(4)); break;      // layer
        case 1: t.w0 = set_bits(t.w0, 2, 1, rng.below(2)); break;      // eye
        case 2: t.w0 = set_bits(t.w0, 3, 1, rng.below(2)); break;      // reserved
        case 3: t.set_tile_index(rng.chance(2) ? rng.below(24) : rng.edge_u16() & 0xfff); break;
        case 4: t.w1 = set_bits(t.w1, 0, 3, rng.below(8)); break;      // mode 5..7 illegal
        case 5: t.w1 = set_bits(t.w1, 3, 2, rng.below(4)); break;      // res_level 3 illegal
        case 6: t.w1 = set_bits(t.w1, 5, 1, rng.below(2)); break;      // chroma444
        case 7: t.w1 = set_bits(t.w1, 6, 2, rng.below(4)); break;      // alpha_mode 3 illegal
        case 8: t.w1 = set_bits(t.w1, 8, 6, rng.u8() & 0x3f); break;   // qp_delta
        case 9: t.w1 = set_bits(t.w1, 14, 3, rng.below(8)); break;     // table_set
        case 10: t.w1 = set_bits(t.w1, 17, 3, rng.below(8)); break;    // nsub_log2 6,7 illegal
        default:
            t.w1 = set_bits(t.w1, 20, 1, rng.below(2));                // mv_present
            t.w1 = set_bits(t.w1, 21, 2, rng.below(4));                // ref_sel
            t.w1 = set_bits(t.w1, 23, 1, rng.below(2));                // tskip
            t.w1 = set_bits(t.w1, 24, 2, rng.below(4));                // wgt
            if (rng.chance(6)) t.w1 = set_bits(t.w1, 26, 6, rng.below(64));  // reserved
            break;
    }
}

// One mutation step on a parsed stream.
inline void mutate_one(Rng &rng, Stream &s) {
    // Pick a frame / row / tile to work on, when there is one.
    Frame *f = s.frames.empty() ? nullptr : &s.frames[rng.below(uint32_t(s.frames.size()))];
    Row *row = (f && !f->rows.empty()) ? &f->rows[rng.below(uint32_t(f->rows.size()))] : nullptr;
    Tile *tile = (row && !row->tiles.empty())
                     ? &row->tiles[rng.below(uint32_t(row->tiles.size()))]
                     : nullptr;

    switch (rng.below(16)) {
        case 0:
        case 1: mutate_stream_header(rng, s); break;
        case 2: mutate_ext(rng, s); break;
        case 3: if (f) mutate_frame_header(rng, *f); break;
        case 4:
        case 5: if (tile) mutate_tile(rng, *tile); break;
        case 6:  // payload bytes: the entropy decoder's own input
            if (tile) mutate_bytes(tile->payload, kMaxPayload, rng);
            break;
        case 7:  // payload length surgery, including zero and huge
            if (tile) {
                size_t n;
                switch (rng.below(4)) {
                    case 0: n = 0; break;
                    case 1: n = rng.below(40); break;
                    case 2: n = tile->payload.size() ? tile->payload.size() - 1 : 0; break;
                    default: n = rng.below(uint32_t(kMaxPayload)); break;
                }
                size_t old = tile->payload.size();
                tile->payload.resize(n);
                for (size_t i = old; i < n; ++i) tile->payload[i] = rng.u8();
            }
            break;
        case 8:  // add / duplicate / drop a tile
            if (row) {
                if (rng.chance(3) && !row->tiles.empty()) {
                    row->tiles.erase(row->tiles.begin() + rng.below(uint32_t(row->tiles.size())));
                } else if (row->tiles.size() < kMaxTilesPerRow) {
                    Tile t = tile ? *tile : gen_tile(rng, 0, false, false);
                    t.set_tile_index(uint32_t(row->tiles.size()));
                    row->tiles.push_back(std::move(t));
                }
            }
            break;
        case 9:  // row header fields
            if (row) {
                switch (rng.below(4)) {
                    case 0: row->frame_number = rng.edge_u16(); break;
                    case 1: row->row_index = rng.chance(2) ? uint8_t(rng.below(kMaxRows))
                                                          : rng.edge_u8(); break;
                    case 2:  // skip_bitmap: Phase 1 must reject any nonzero value
                        row->skip_bitmap = rng.chance(2) ? 0 : (rng.next() & 0xffffull);
                        break;
                    default:  // desync tile_count against the tiles present
                        row->count_override = rng.chance(2)
                                                  ? int(rng.below(64))
                                                  : int(row->tiles.size() + 1);
                        break;
                }
            }
            break;
        case 10:  // add / duplicate / drop a row
            if (f) {
                if (rng.chance(3) && !f->rows.empty()) {
                    f->rows.erase(f->rows.begin() + rng.below(uint32_t(f->rows.size())));
                } else if (f->rows.size() < kMaxRows && row) {
                    Row r = *row;
                    r.row_index = uint8_t(f->rows.size());
                    f->rows.push_back(std::move(r));
                }
            }
            break;
        case 11:  // add / duplicate / drop a frame
            if (rng.chance(3) && s.frames.size() > 1) {
                s.frames.erase(s.frames.begin() + rng.below(uint32_t(s.frames.size())));
            } else if (s.frames.size() < kMaxFrames && f) {
                Frame nf = *f;
                set16(nf.hdr, uint16_t(get16(nf.hdr) + 1));
                s.frames.push_back(std::move(nf));
            }
            break;
        case 12:  // desync frame_bytes: the self-delimiting field
            if (f) {
                switch (rng.below(4)) {
                    case 0: f->bytes_override = 0; break;
                    case 1: f->bytes_override = 39; break;
                    case 2: f->bytes_override = int64_t(rng.edge_u32()); break;
                    default: f->bytes_override = -1; break;  // repair it
                }
            }
            break;
        case 13:  // custom quantization matrix bytes (clamped to [1,32] on parse)
            if (f && !f->matrices.empty())
                f->matrices[rng.below(uint32_t(f->matrices.size()))] = rng.edge_u8();
            else if (f && !f->tables.empty()) {
                auto &tb = f->tables[rng.below(uint32_t(f->tables.size()))];
                tb[rng.below(kTableBytes)] = rng.edge_u8();
            }
            break;
        case 14:  // truncate the serialized stream: every length check at once
            s.truncate_to = -1;  // computed by the caller, which knows the size
            break;
        default:
            // Regenerate one frame from scratch, to keep injecting fresh
            // structure into a corpus that has drifted into noise.
            if (f) {
                Rng sub(rng.next());
                Stream fresh = gen_stream(sub);
                if (!fresh.frames.empty()) *f = fresh.frames[0];
            }
            break;
    }
}

// The entry point every stream-shaped target's LLVMFuzzerCustomMutator calls.
inline size_t mutate_stream_bytes(uint8_t *data, size_t size, size_t max_size, unsigned seed) {
    Rng rng(mix_seed(seed, size));
    Stream s;
    if (!parse(data, size, s) || s.frames.empty()) {
        // Nothing usable: synthesize.  This is what pulls a corpus that has
        // been reduced to noise back into the interesting part of the space.
        s = gen_stream(rng);
    }
    int steps = 1 + int(rng.below(4));
    bool truncate = false;
    for (int i = 0; i < steps; ++i) {
        size_t before = s.frames.size();
        mutate_one(rng, s);
        (void)before;
        if (rng.chance(24)) truncate = true;
    }
    std::vector<uint8_t> out = serialize(s);
    if (truncate && !out.empty()) out.resize(1 + rng.below(uint32_t(out.size())));
    if (out.empty()) out = serialize(gen_stream(rng));
    return emit(out, data, max_size);
}

}  // namespace nxf::nxv

#endif  // NXFUZZ_NXVC_STREAM_H
