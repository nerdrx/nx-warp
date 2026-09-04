#include "nxvc/transport/packetizer.h"

#include <algorithm>
#include <cstring>

namespace nxt {

void FecPolicy::set_from_loss(double loss) {
    // PAPER 4.4: 2/0/0 below 0.1 %, 3/1/0 nominal, 4/2/1 above 2 %.
    if (loss < 0.001) { parity[0] = 2; parity[1] = 0; parity[2] = 0; }
    else if (loss > 0.02) { parity[0] = 4; parity[1] = 2; parity[2] = 1; }
    else { parity[0] = 3; parity[1] = 1; parity[2] = 0; }
}

namespace {

struct RunKey {
    uint8_t layer, ref_delta, cls;
    uint16_t row;
    bool lossless;
    bool operator!=(const RunKey& o) const {
        return layer != o.layer || ref_delta != o.ref_delta || cls != o.cls ||
               row != o.row || lossless != o.lossless;
    }
};

RunKey key_of(const TileInput& t) {
    return RunKey{t.layer_id, t.ref_delta, uint8_t(t.cls), t.row, t.lossless};
}

}  // namespace

Packetizer::Status Packetizer::packetize_band(uint8_t band,
                                              std::span<const TileInput> tiles,
                                              const FrameContext& ctx,
                                              std::vector<SendUnit>* out) {
    const size_t budget = cfg_.run_payload_budget();
    const size_t max_tile = cfg_.max_tile_bytes();
    const bool pose_first = ctx.pose && (cfg_.caps & kCapPoseHdr);

    // Per class, the datagrams produced by this band, in order.
    std::vector<PendingDatagram> per_class[3];
    bool pose_placed = false;

    size_t i = 0;
    while (i < tiles.size()) {
        RunKey k = key_of(tiles[i]);
        // Collect a maximal contiguous, homogeneous run that fits the budget.
        std::vector<const TileInput*> run;
        size_t payload = 0;
        bool want_pose = pose_first && !pose_placed;
        size_t base = want_pose ? kPoseHeaderBytes : 0;
        uint16_t expect_col = tiles[i].col;
        while (i < tiles.size()) {
            const TileInput& t = tiles[i];
            if (key_of(t) != k) break;
            if (t.col != expect_col) break;  // non-contiguous
            if (t.bytes.size() > max_tile) break;  // handled below
            size_t add = kDirEntryBytes + t.bytes.size();
            if (base + payload + add > budget) break;
            if (run.size() >= kMaxTilesPerRun) break;
            run.push_back(&t);
            payload += add;
            ++expect_col;
            ++i;
        }

        if (run.empty()) {
            // The tile at i does not fit any run by itself: it is oversize.
            const TileInput& t = tiles[i];
            if (t.bytes.size() <= max_tile) return Status::kBadInput;
            ++oversize_tiles_;
            if (policy_ == OversizePolicy::kReject) return Status::kOversizeTile;
            if (policy_ == OversizePolicy::kDropTile) {
                TileInput drop = t;
                drop.bytes = {};
                drop.mode = TileMode::kWarpSkip;
                // Emit a single-tile run with len 0.
                PendingDatagram d;
                d.hdr.stream_id = cfg_.stream_id;
                d.hdr.frame_id = ctx.frame_id;
                d.hdr.tile_first = uint16_t(cfg_.tile_index(t.row, t.col));
                d.hdr.tile_count = 1;
                d.hdr.layer_id = t.layer_id;
                d.hdr.ref_delta = t.ref_delta;
                d.hdr.tile_class = uint8_t(t.cls);
                d.hdr.band = band;
                d.hdr.caps = cfg_.caps;
                d.hdr.pose_seq = ctx.pose_seq;
                d.hdr.tx_ts = ctx.tx_ts;
                d.hdr.enc_us = ctx.enc_us;
                if (ctx.partial_frame) d.hdr.flags |= kFlagPartialFrame;
                d.plaintext.resize(kDirEntryBytes);
                TileDirEntry e;
                e.len = 0;
                e.qp = t.qp;
                e.mode = uint8_t(TileMode::kWarpSkip);
                wr32(d.plaintext.data(), pack_dir_entry(e));
                per_class[uint8_t(t.cls)].push_back(std::move(d));
                ++i;
                continue;
            }
            // kFragment: legal only for lossless tiles with CAP_FRAGMENT.
            if (!t.lossless || !(cfg_.caps & kCapFragment)) return Status::kOversizeTile;
            size_t chunk = budget - kDirEntryBytes;
            size_t nfrag = (t.bytes.size() + chunk - 1) / chunk;
            if (nfrag > 4) return Status::kOversizeTile;
            for (size_t f = 0; f < nfrag; ++f) {
                size_t off = f * chunk;
                size_t len = std::min(chunk, t.bytes.size() - off);
                PendingDatagram d;
                d.hdr.stream_id = cfg_.stream_id;
                d.hdr.frame_id = ctx.frame_id;
                d.hdr.tile_first = uint16_t(cfg_.tile_index(t.row, t.col));
                d.hdr.tile_count = 1;
                d.hdr.layer_id = t.layer_id;
                d.hdr.ref_delta = t.ref_delta;
                d.hdr.frag_idx = uint8_t(f);
                d.hdr.frag_count = uint8_t(nfrag - 1);
                d.hdr.tile_class = uint8_t(t.cls);
                d.hdr.band = band;
                d.hdr.caps = cfg_.caps;
                d.hdr.pose_seq = ctx.pose_seq;
                d.hdr.tx_ts = ctx.tx_ts;
                d.hdr.enc_us = ctx.enc_us;
                d.hdr.flags |= kFlagLossless;
                d.plaintext.resize(kDirEntryBytes + len);
                TileDirEntry e;
                e.len = uint16_t(len);
                e.qp = t.qp;
                e.mode = uint8_t(t.mode);
                e.res_level = t.res_level;
                e.lossless = true;
                e.chroma444 = t.chroma444;
                e.alpha = t.alpha;
                wr32(d.plaintext.data(), pack_dir_entry(e));
                std::memcpy(d.plaintext.data() + kDirEntryBytes, t.bytes.data() + off, len);
                per_class[uint8_t(t.cls)].push_back(std::move(d));
            }
            ++i;
            continue;
        }

        // Emit the run.
        PendingDatagram d;
        const TileInput& t0 = *run.front();
        d.hdr.stream_id = cfg_.stream_id;
        d.hdr.frame_id = ctx.frame_id;
        d.hdr.tile_first = uint16_t(cfg_.tile_index(t0.row, t0.col));
        d.hdr.tile_count = uint8_t(run.size());
        d.hdr.layer_id = t0.layer_id;
        d.hdr.ref_delta = t0.ref_delta;
        d.hdr.tile_class = uint8_t(t0.cls);
        d.hdr.band = band;
        d.hdr.caps = cfg_.caps;
        d.hdr.pose_seq = ctx.pose_seq;
        d.hdr.tx_ts = ctx.tx_ts;
        d.hdr.enc_us = ctx.enc_us;
        if (ctx.partial_frame) d.hdr.flags |= kFlagPartialFrame;
        if (t0.lossless) d.hdr.flags |= kFlagLossless;
        bool all_intra = true;
        for (const TileInput* t : run)
            if (t->mode != TileMode::kIntra) { all_intra = false; break; }
        if (all_intra) d.hdr.flags |= kFlagKeyframeRun;
        if (want_pose) {
            d.hdr.pose_hdr = true;
            pose_placed = true;
        }
        d.plaintext.assign(base + run.size() * kDirEntryBytes, 0);
        d.plaintext.reserve(base + payload);
        if (want_pose) encode_pose_header(*ctx.pose, d.plaintext.data());
        for (size_t r = 0; r < run.size(); ++r) {
            TileDirEntry e;
            e.len = uint16_t(run[r]->bytes.size());
            e.qp = run[r]->qp;
            e.mode = uint8_t(run[r]->mode);
            e.res_level = run[r]->res_level;
            e.lossless = run[r]->lossless;
            e.chroma444 = run[r]->chroma444;
            e.alpha = run[r]->alpha;
            wr32(d.plaintext.data() + base + r * kDirEntryBytes, pack_dir_entry(e));
        }
        for (const TileInput* t : run)
            d.plaintext.insert(d.plaintext.end(), t->bytes.begin(), t->bytes.end());
        per_class[uint8_t(t0.cls)].push_back(std::move(d));
    }

    // Group into FEC units.  Groups never cross band, class, layer or frame
    // (TRANSPORT.md 6, decision D6); layers are already separated because a run
    // is layer-homogeneous and layers are contiguous in the input order.
    for (int c = 0; c < 3; ++c) {
        auto& v = per_class[c];
        if (v.empty()) continue;
        int m = fec_.parity[c];
        if (!cfg_.fec_enabled()) m = 0;
        if (m == 0) {
            for (auto& d : v) {
                SendUnit u;
                u.cls = TileClass(c);
                u.band = band;
                u.layer = d.hdr.layer_id;
                u.m = 0;
                d.hdr.fec_k = 0;
                d.hdr.fec_idx = 0;
                u.data.push_back(std::move(d));
                out->push_back(std::move(u));
            }
            continue;
        }
        size_t pos = 0;
        while (pos < v.size()) {
            size_t n = std::min<size_t>(kFecMaxK, v.size() - pos);
            SendUnit u;
            u.cls = TileClass(c);
            u.band = band;
            u.layer = v[pos].hdr.layer_id;
            u.group = next_group_++;
            u.m = m;
            for (size_t j = 0; j < n; ++j) {
                v[pos + j].hdr.fec_group = u.group;
                v[pos + j].hdr.fec_k = uint8_t(n);
                v[pos + j].hdr.fec_idx = uint8_t(j);
                u.data.push_back(std::move(v[pos + j]));
            }
            out->push_back(std::move(u));
            pos += n;
        }
    }

    if (ctx.last_band && !out->empty()) {
        SendUnit& last = out->back();
        if (!last.data.empty()) last.data.back().hdr.flags |= kFlagLastRunOfFrame;
    }
    return Status::kOk;
}

}  // namespace nxt
