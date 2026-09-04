#include "nxvc/transport/packetizer.h"

#include <algorithm>
#include <cstring>

namespace nxt {

void FecPolicy::set_from_loss(double loss) {
    // PAPER 4.4's ladder, restated as ratios of the realised group size:
    // 2/0/0 per 10 below 0.1 %, 3/1/0 nominal, 4/2/1 above 2 %.
    if (loss < 0.001) {
        ratio_pct[0] = 20; ratio_pct[1] = 0; ratio_pct[2] = 0;
        min_parity[0] = 1; min_parity[1] = 0; min_parity[2] = 0;
    } else if (loss > 0.02) {
        ratio_pct[0] = 40; ratio_pct[1] = 20; ratio_pct[2] = 10;
        min_parity[0] = 1; min_parity[1] = 1; min_parity[2] = 0;
    } else {
        ratio_pct[0] = 30; ratio_pct[1] = 10; ratio_pct[2] = 0;
        min_parity[0] = 1; min_parity[1] = 0; min_parity[2] = 0;
    }
}

void FecPolicy::set_from_headroom(double headroom, double loss, bool bc_was_on) {
    double gate = bc_was_on ? kBcHeadroomDrop : kBcHeadroom;
    bool room = headroom >= gate;

    // Class A parity is never switched off: a fovea hole is the one artifact
    // users notice at once (PAPER 4.4), and it measured as a net win at every
    // headroom in transport/RESULTS.md.
    min_parity[0] = 1;
    min_parity[2] = 0;

    if (!room) {
        // No room.  Class B and C are off, and -- the part PAPER 4.4 gets
        // wrong -- the loss escalation is off too.  On a saturated link most
        // measured loss IS congestion loss caused by our own bytes, so
        // answering it with more parity is a positive feedback loop: more
        // parity, more queue drops, more measured loss, more parity.  The
        // ladder may only climb when there is room to absorb the climb.
        ratio_pct[0] = loss < 0.001 ? 20 : 30;
        ratio_pct[1] = 0;
        ratio_pct[2] = 0;
        min_parity[1] = 0;
        return;
    }
    ratio_pct[0] = loss < 0.001 ? 20 : (loss > 0.02 ? 40 : 30);
    ratio_pct[1] = loss > 0.02 ? 20 : 10;
    ratio_pct[2] = loss > 0.02 ? 10 : 0;
    min_parity[1] = loss > 0.02 ? 1 : 0;
}

namespace {

// v2: tile_class and ref_delta live in the tile directory, so a run only has
// to be homogeneous in layer, tile row and the lossless flag (the flag selects
// the fragmentation rules for the whole datagram).
struct RunKey {
    uint8_t layer;
    uint16_t row;
    bool lossless;
    bool operator!=(const RunKey& o) const {
        return layer != o.layer || row != o.row || lossless != o.lossless;
    }
};

RunKey key_of(const TileInput& t) { return RunKey{t.layer_id, t.row, t.lossless}; }

// v1 additionally keyed the run on tile_class and ref_delta.
bool same_run_v1(const TileInput& a, const TileInput& b) {
    return a.cls == b.cls && a.ref_delta == b.ref_delta;
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
        uint8_t run_cls = uint8_t(tiles[i].cls);
        while (i < tiles.size()) {
            const TileInput& t = tiles[i];
            if (key_of(t) != k) break;
            if (t.col != expect_col) break;  // non-contiguous
            if (t.bytes.size() > max_tile) break;  // handled below
            // A class change breaks the run only once it is long enough to be
            // worth ending; a short run absorbs it and takes the stronger
            // protection of the two (TRANSPORT.md decision D20).
            if (v1_) {
                if (!run.empty() && !same_run_v1(*run.front(), t)) break;
            } else if (!run.empty() && uint8_t(t.cls) != run_cls &&
                       run.size() >= class_break_min_) {
                break;
            }
            size_t add = kDirEntryBytes + t.bytes.size();
            if (base + payload + add > budget) break;
            if (run.size() >= kMaxTilesPerRun) break;
            if (uint8_t(t.cls) < run_cls) run_cls = uint8_t(t.cls);
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
                d.hdr.fec_class = uint8_t(t.cls);
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
                e.tile_class = uint8_t(t.cls);
                e.ref_delta = t.ref_delta;
                wr32(d.plaintext.data(), pack_dir_entry(e));
                per_class[d.hdr.fec_class].push_back(std::move(d));
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
                d.hdr.frag_idx = uint8_t(f);
                d.hdr.frag_count = uint8_t(nfrag - 1);
                d.hdr.fec_class = uint8_t(t.cls);
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
                e.tile_class = uint8_t(t.cls);
                e.ref_delta = t.ref_delta;
                wr32(d.plaintext.data(), pack_dir_entry(e));
                std::memcpy(d.plaintext.data() + kDirEntryBytes, t.bytes.data() + off, len);
                per_class[d.hdr.fec_class].push_back(std::move(d));
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
        d.hdr.fec_class = run_cls;
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
            e.tile_class = uint8_t(run[r]->cls);
            e.ref_delta = run[r]->ref_delta;
            wr32(d.plaintext.data() + base + r * kDirEntryBytes, pack_dir_entry(e));
        }
        for (const TileInput* t : run)
            d.plaintext.insert(d.plaintext.end(), t->bytes.begin(), t->bytes.end());
        per_class[run_cls].push_back(std::move(d));
    }

    // Group into FEC units.  Groups never cross band, class, layer or frame
    // (TRANSPORT.md 6, decision D6); layers are already separated because a run
    // is layer-homogeneous and layers are contiguous in the input order.
    for (int c = 0; c < 3; ++c) {
        auto& v = per_class[c];
        if (v.empty()) continue;
        // The parity count depends on the realised group size, so it is decided
        // per group below; a class whose ratio and floor are both zero never
        // forms a group at all.
        int m = cfg_.fec_enabled() ? fec_.parity_for(uint8_t(c), kFecMaxK) : 0;
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
        // Parity blocks are padded to the longest datagram in their group, so
        // grouping datagrams of similar length cuts the padding waste.  It also
        // spreads a group's members across the band in time, which makes a
        // burst less likely to take more than m members of one group
        // (TRANSPORT.md decision D22).  Membership is carried by fec_group /
        // fec_idx, so the receiver does not care about the order.
        if (!v1_)
            std::stable_sort(v.begin(), v.end(),
                             [](const PendingDatagram& a, const PendingDatagram& b) {
                                 return a.plaintext.size() > b.plaintext.size();
                             });
        size_t pos = 0;
        while (pos < v.size()) {
            size_t n = std::min<size_t>(kFecMaxK, v.size() - pos);
            SendUnit u;
            u.cls = TileClass(c);
            u.band = band;
            u.layer = v[pos].hdr.layer_id;
            u.group = next_group_++;
            u.m = fec_.parity_for(uint8_t(c), int(n));
            for (size_t j = 0; j < n; ++j) {
                v[pos + j].hdr.fec_group = u.group;
                v[pos + j].hdr.fec_k = uint8_t(n);
                v[pos + j].hdr.fec_idx = uint8_t(j);
                v[pos + j].hdr.fec_m = uint8_t(u.m);
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
