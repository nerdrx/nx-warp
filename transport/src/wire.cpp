#include "nxvc/transport/wire.h"

#include <algorithm>
#include <cstring>

namespace nxt {

void encode_header(const DatagramHeader& h, uint8_t* p) {
    p[0] = uint8_t((h.version & 0x0F) | ((h.flags & 0x0F) << 4));
    p[1] = h.stream_id;
    wr16(p + 2, h.frame_id);
    wr16(p + 4, h.tile_first);
    p[6] = h.tile_count;
    p[7] = uint8_t((h.layer_id & 0x0F) | ((h.ref_delta & 0x03) << 4) |
                   ((h.frag_idx & 0x03) << 6));
    p[8] = uint8_t((h.frag_count & 0x03) | ((h.tile_class & 0x03) << 2) |
                   ((h.band & 0x07) << 4) | (h.pose_hdr ? 0x80 : 0));
    p[9] = h.caps;
    wr16(p + 10, h.pose_seq);
    wr16(p + 12, uint16_t((h.path_seq & 0x3FFF) | (uint16_t(h.path_id & 0x03) << 14)));
    p[14] = h.fec_group;
    p[15] = uint8_t((h.fec_idx & 0x0F) | ((h.fec_k & 0x0F) << 4));
    wr32(p + 16, h.tx_ts);
    wr16(p + 20, h.payload_len);
    wr16(p + 22, h.enc_us);
}

bool decode_header(const uint8_t* p, DatagramHeader* o) {
    o->version = uint8_t(p[0] & 0x0F);
    o->flags = uint8_t(p[0] >> 4);
    o->stream_id = p[1];
    o->frame_id = rd16(p + 2);
    o->tile_first = rd16(p + 4);
    o->tile_count = p[6];
    o->layer_id = uint8_t(p[7] & 0x0F);
    o->ref_delta = uint8_t((p[7] >> 4) & 0x03);
    o->frag_idx = uint8_t((p[7] >> 6) & 0x03);
    o->frag_count = uint8_t(p[8] & 0x03);
    o->tile_class = uint8_t((p[8] >> 2) & 0x03);
    o->band = uint8_t((p[8] >> 4) & 0x07);
    o->pose_hdr = (p[8] & 0x80) != 0;
    o->caps = p[9];
    o->pose_seq = rd16(p + 10);
    uint16_t ps = rd16(p + 12);
    o->path_seq = uint16_t(ps & 0x3FFF);
    o->path_id = uint8_t(ps >> 14);
    o->fec_group = p[14];
    o->fec_idx = uint8_t(p[15] & 0x0F);
    o->fec_k = uint8_t(p[15] >> 4);
    o->tx_ts = rd32(p + 16);
    o->payload_len = rd16(p + 20);
    o->enc_us = rd16(p + 22);
    return o->version == kVersion;
}

uint32_t pack_dir_entry(const TileDirEntry& e) {
    return (uint32_t(e.len) & 0xFFFu) | ((uint32_t(e.qp) & 0x3Fu) << 12) |
           ((uint32_t(e.mode) & 0x7u) << 18) | ((uint32_t(e.res_level) & 0x3u) << 21) |
           (uint32_t(e.lossless) << 23) | (uint32_t(e.chroma444) << 24) |
           (uint32_t(e.alpha) << 25);
}

TileDirEntry unpack_dir_entry(uint32_t v) {
    TileDirEntry e;
    e.len = uint16_t(v & 0xFFFu);
    e.qp = uint8_t((v >> 12) & 0x3Fu);
    e.mode = uint8_t((v >> 18) & 0x7u);
    e.res_level = uint8_t((v >> 21) & 0x3u);
    e.lossless = (v >> 23) & 1u;
    e.chroma444 = (v >> 24) & 1u;
    e.alpha = (v >> 25) & 1u;
    return e;
}

void encode_pose_header(const PoseHeader& h, uint8_t* p) {
    wr16(p, h.pose_seq);
    for (int i = 0; i < 4; ++i) wr16(p + 2 + 2 * i, uint16_t(h.quat[i]));
    for (int i = 0; i < 3; ++i) wr32(p + 10 + 4 * i, uint32_t(h.pos_mm_q8[i]));
    wr32(p + 22, h.render_finish_ts);
}

bool decode_pose_header(const uint8_t* p, PoseHeader* o) {
    o->pose_seq = rd16(p);
    for (int i = 0; i < 4; ++i) o->quat[i] = int16_t(rd16(p + 2 + 2 * i));
    for (int i = 0; i < 3; ++i) o->pos_mm_q8[i] = int32_t(rd32(p + 10 + 4 * i));
    o->render_finish_ts = rd32(p + 22);
    return true;
}

// ------------------------------------------------------------------ feedback
namespace {

// Runs of consecutive missing tiles, capped at 255 tiles per run.
std::vector<std::pair<uint16_t, uint8_t>> missing_runs(const std::vector<uint8_t>& rx) {
    std::vector<std::pair<uint16_t, uint8_t>> runs;
    size_t i = 0;
    while (i < rx.size()) {
        if (rx[i]) { ++i; continue; }
        size_t start = i;
        while (i < rx.size() && !rx[i] && (i - start) < 255) ++i;
        runs.emplace_back(uint16_t(start), uint8_t(i - start));
    }
    return runs;
}

}  // namespace

ByteVec encode_feedback(const FeedbackPacket& fb, bool allow_rle) {
    ByteVec out;
    out.resize(8);
    out[0] = uint8_t((fb.version & 0x0F) | ((fb.flags & 0x0F) << 4));
    out[1] = fb.stream_id;
    uint16_t newest_frame = fb.bands.empty() ? 0 : fb.bands[0].frame_id;
    uint8_t newest_band = fb.bands.empty() ? 0 : fb.bands[0].band;
    wr16(out.data() + 2, newest_frame);
    out[4] = newest_band;
    out[5] = uint8_t(fb.bands.size());
    wr16(out.data() + 6, fb.tiles_in_band);

    for (const BandReport& b : fb.bands) {
        size_t off = out.size();
        out.resize(off + 20);
        uint8_t* r = out.data() + off;
        wr16(r, b.frame_id);
        r[2] = b.band;

        bool any_missing = false;
        for (uint8_t v : b.received)
            if (!v) { any_missing = true; break; }

        BitmapMode mode = BitmapMode::kRaw;
        std::vector<std::pair<uint16_t, uint8_t>> runs;
        size_t raw_bytes = (b.received.size() + 7) / 8;
        if (!any_missing) {
            mode = BitmapMode::kAll;
        } else if (allow_rle) {
            runs = missing_runs(b.received);
            if (runs.size() <= 255 && 1 + runs.size() * 3 < raw_bytes)
                mode = BitmapMode::kRle;
        }
        r[3] = uint8_t(uint8_t(mode) | (b.complete ? 0x04 : 0) |
                       (b.deadline_missed ? 0x08 : 0));
        wr32(r + 4, b.rx_ts_first);
        wr32(r + 8, b.rx_ts_last);
        wr16(r + 12, b.decode_us);
        wr16(r + 14, b.conceal_tiles);
        wr16(r + 16, b.late_tiles);
        r[18] = b.fec_recovered;
        r[19] = b.fec_failed;

        if (mode == BitmapMode::kRaw) {
            size_t bo = out.size();
            out.resize(bo + raw_bytes, 0);
            for (size_t i = 0; i < b.received.size(); ++i)
                if (b.received[i]) out[bo + i / 8] |= uint8_t(1u << (i % 8));
        } else if (mode == BitmapMode::kRle) {
            out.push_back(uint8_t(runs.size()));
            for (auto& [s, l] : runs) {
                size_t bo = out.size();
                out.resize(bo + 3);
                wr16(out.data() + bo, s);
                out[bo + 2] = l;
            }
        }
    }

    size_t off = out.size();
    out.resize(off + 4);
    out[off + 0] = fb.path_loss[0];
    out[off + 1] = fb.path_loss[1];
    out[off + 2] = fb.path_rtt_ms[0];
    out[off + 3] = fb.path_rtt_ms[1];
    return out;
}

bool decode_feedback(std::span<const uint8_t> in, FeedbackPacket* out) {
    if (in.size() < 12) return false;
    const uint8_t* p = in.data();
    out->version = uint8_t(p[0] & 0x0F);
    out->flags = uint8_t(p[0] >> 4);
    if (out->version != kVersion) return false;
    out->stream_id = p[1];
    uint8_t nband = p[5];
    out->tiles_in_band = rd16(p + 6);
    if (nband == 0 || nband > kMaxFeedbackBands) return false;
    if (out->tiles_in_band == 0) return false;
    size_t pos = 8;
    out->bands.clear();
    for (uint8_t i = 0; i < nband; ++i) {
        if (pos + 20 > in.size()) return false;
        const uint8_t* r = p + pos;
        BandReport b;
        b.frame_id = rd16(r);
        b.band = r[2];
        BitmapMode mode = BitmapMode(r[3] & 0x03);
        b.complete = (r[3] & 0x04) != 0;
        b.deadline_missed = (r[3] & 0x08) != 0;
        b.rx_ts_first = rd32(r + 4);
        b.rx_ts_last = rd32(r + 8);
        b.decode_us = rd16(r + 12);
        b.conceal_tiles = rd16(r + 14);
        b.late_tiles = rd16(r + 16);
        b.fec_recovered = r[18];
        b.fec_failed = r[19];
        pos += 20;
        b.received.assign(out->tiles_in_band, 0);
        if (mode == BitmapMode::kAll) {
            std::fill(b.received.begin(), b.received.end(), uint8_t(1));
        } else if (mode == BitmapMode::kRaw) {
            size_t raw = (size_t(out->tiles_in_band) + 7) / 8;
            if (pos + raw > in.size()) return false;
            for (size_t t = 0; t < out->tiles_in_band; ++t)
                b.received[t] = uint8_t((p[pos + t / 8] >> (t % 8)) & 1u);
            pos += raw;
        } else if (mode == BitmapMode::kRle) {
            if (pos >= in.size()) return false;
            uint8_t nruns = p[pos++];
            if (pos + size_t(nruns) * 3 > in.size()) return false;
            std::fill(b.received.begin(), b.received.end(), uint8_t(1));
            for (uint8_t rr = 0; rr < nruns; ++rr) {
                uint16_t s = rd16(p + pos);
                uint8_t l = p[pos + 2];
                pos += 3;
                for (uint32_t t = s; t < uint32_t(s) + l && t < out->tiles_in_band; ++t)
                    b.received[t] = 0;
            }
        } else {
            return false;  // reserved mode
        }
        out->bands.push_back(std::move(b));
    }
    if (pos + 4 > in.size()) return false;
    out->path_loss[0] = p[pos + 0];
    out->path_loss[1] = p[pos + 1];
    out->path_rtt_ms[0] = p[pos + 2];
    out->path_rtt_ms[1] = p[pos + 3];
    return true;
}

}  // namespace nxt
