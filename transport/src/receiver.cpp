#include "nxvc/transport/receiver.h"

#include <algorithm>
#include <cstring>

namespace nxt {

// ------------------------------------------------------------------ FrameRing
FrameRing::FrameRing(const StreamConfig& cfg) : cfg_(cfg) {
    for (Slot& s : slots_) {
        s.meta.assign(size_t(cfg_.layers) * cfg_.tiles_per_frame(), TileMeta{});
        s.band_deadline_passed.assign(cfg_.bands(), 0);
        s.band_reports.assign(cfg_.bands(), 0);
    }
}

FrameRing::Slot* FrameRing::acquire(uint16_t frame_id) {
    Slot& s = slots_[frame_id % kRingSlots];
    if (s.used && s.frame_id == frame_id) return &s;
    if (started_ && seq_newer(newest_, frame_id) &&
        uint16_t(newest_ - frame_id) >= kRingSlots)
        return nullptr;  // older than the ring
    // Carry pose_seq and age forward from the previous frame before resetting.
    const Slot& prev = slots_[uint16_t(frame_id - 1) % kRingSlots];
    bool have_prev = prev.used && prev.frame_id == uint16_t(frame_id - 1);
    std::vector<TileMeta> old;
    if (have_prev) old = prev.meta;
    s.used = true;
    s.frame_id = frame_id;
    s.seen_data.clear();
    s.seen_parity.clear();
    s.fec_repaired.clear();
    std::fill(s.band_deadline_passed.begin(), s.band_deadline_passed.end(), uint8_t(0));
    std::fill(s.band_reports.begin(), s.band_reports.end(), uint8_t(0));
    for (size_t i = 0; i < s.meta.size(); ++i) {
        TileMeta m;
        if (have_prev) {
            m.pose_seq = old[i].pose_seq;
            m.age = uint8_t(old[i].age < 255 ? old[i].age + 1 : 255);
        }
        m.state = TileState::kEmpty;
        s.meta[i] = m;
    }
    if (!started_ || seq_newer(frame_id, newest_)) newest_ = frame_id;
    started_ = true;
    return &s;
}

FrameRing::Slot* FrameRing::find(uint16_t frame_id) {
    Slot& s = slots_[frame_id % kRingSlots];
    return (s.used && s.frame_id == frame_id) ? &s : nullptr;
}
const FrameRing::Slot* FrameRing::find(uint16_t frame_id) const {
    const Slot& s = slots_[frame_id % kRingSlots];
    return (s.used && s.frame_id == frame_id) ? &s : nullptr;
}

// --------------------------------------------------------- DeadlineController
void DeadlineController::on_frame(double miss_fraction, int32_t min_margin_us,
                                  double late_fraction) {
    // Climb, never gated by the hold, because a deadline that is too early has
    // to be fixed at once.  Either signal trips it: too much of the frame
    // missing, or any material share of it arriving after the deadline.
    if (miss_fraction > kMissFraction || late_fraction > kLateFraction) {
        if (++consecutive_miss_ >= kClimbFrames && offset_us_ < kMaxOffsetUs) {
            offset_us_ += kClimbStepUs;
            if (offset_us_ > kMaxOffsetUs) offset_us_ = kMaxOffsetUs;
            consecutive_miss_ = 0;
            moved_ = true;
            margins_.clear();  // the window describes the old deadline
            hold_frames_ = 0;
        }
    } else {
        consecutive_miss_ = 0;
    }

    // A frame that missed anything contributes no slack, so one bad frame in
    // the window is enough to block a relax.
    int32_t m = miss_fraction > 0.0 ? kNoMargin : min_margin_us;
    margins_.push_back(m);
    if (int(margins_.size()) > kRelaxWindowFrames) margins_.pop_front();
    if (hold_frames_ < kRelaxWindowFrames) ++hold_frames_;

    // Relax: only when the window is full, the hysteresis hold has expired and
    // even the worst frame in it kept kRelaxMarginUs of slack.
    if (offset_us_ == 0) return;
    if (int(margins_.size()) < kRelaxWindowFrames) return;
    if (hold_frames_ < kRelaxWindowFrames) return;
    if (window_worst_margin_us() < kRelaxMarginUs) return;
    offset_us_ = offset_us_ > kRelaxStepUs ? offset_us_ - kRelaxStepUs : 0;
    moved_ = true;
    hold_frames_ = 0;
    margins_.clear();
}

int32_t DeadlineController::window_worst_margin_us() const {
    int32_t worst = INT32_MAX;
    for (int32_t v : margins_) worst = v < worst ? v : worst;
    return margins_.empty() ? kNoMargin : worst;
}

// ------------------------------------------------------------------- Receiver
Receiver::Receiver(const StreamConfig& cfg, const Aead* aead, const Key& session_key,
                   const Key& session_salt)
    : cfg_(cfg), aead_(aead), ring_(cfg) {
    caps_ = cfg.caps;
    for (uint8_t p = 0; p < kMaxPaths; ++p) {
        subkey_dn_[p] = derive_subkey(session_key, session_salt, p, Direction::kDownstream);
        subkey_up_[p] = derive_subkey(session_key, session_salt, p, Direction::kUpstream);
    }
}

uint64_t Receiver::group_key(const DatagramHeader& h, uint8_t path_id) const {
    return (uint64_t(h.frame_id) << 20) | (uint64_t(h.band & 7) << 17) |
           (uint64_t(h.fec_class & 3) << 15) | (uint64_t(h.layer_id & 15) << 11) |
           (uint64_t(h.fec_group) << 3) | uint64_t(path_id & 3);
}

void Receiver::account_seq(uint8_t path_id, uint64_t ext, uint64_t now_us) {
    PathRx& p = path_[path_id];
    if (!p.started) {
        p.started = true;
        p.highest = ext;
        p.win_first = ext;
        p.win_count = 0;
        p.win_start_us = now_us;
    }
    p.expect = ext + 1;
    if (ext > p.highest) p.highest = ext;
    ++p.win_count;
    if (now_us - p.win_start_us >= 1000000) {
        uint64_t span = p.highest >= p.win_first ? p.highest - p.win_first + 1 : 1;
        p.loss = span > p.win_count ? double(span - p.win_count) / double(span) : 0.0;
        p.win_first = p.highest + 1;
        p.win_count = 0;
        p.win_start_us = now_us;
    }
}

double Receiver::path_loss(uint8_t path_id) const {
    if (path_id >= kMaxPaths) return 0.0;
    const PathRx& p = path_[path_id];
    if (!p.started) return 1.0;  // nothing ever seen: report the path as dead
    uint64_t span = p.highest >= p.win_first ? p.highest - p.win_first + 1 : 0;
    if (span > 0 && p.win_count > 0 && span >= p.win_count)
        return double(span - p.win_count) / double(span);
    return p.loss;
}

bool Receiver::on_datagram(std::span<const uint8_t> wire, uint8_t path_id,
                           uint64_t now_us, std::vector<TileOutput>* tiles) {
    arena_.clear();
    return process(wire, path_id, now_us, false, tiles, 0);
}

bool Receiver::process(std::span<const uint8_t> wire, uint8_t path_id, uint64_t now_us,
                       bool from_fec, std::vector<TileOutput>* tiles, int depth) {
    if (path_id >= kMaxPaths) return false;
    if (wire.size() < kHeaderBytes + kTagBytes) return false;
    DatagramHeader h;
    if (!decode_header(wire.data(), &h)) { ++stats.bad_version; return false; }
    if (h.caps & uint8_t(~caps_)) { ++stats.bad_caps; return false; }
    if (size_t(h.payload_len) + kHeaderBytes + kTagBytes != wire.size()) {
        ++stats.bad_range;
        return false;
    }

    PathRx& pr = path_[path_id];
    uint64_t ext = pr.started ? extend_seq14(pr.expect, h.path_seq) : h.path_seq;
    if (!from_fec) {
        ++stats.datagrams;
        stats.wire_bytes += wire.size();
        stats.path_datagrams[path_id]++;
        stats.path_bytes[path_id] += wire.size();
        if (pr.started && ext + 8192 < pr.highest) { ++stats.replay; return false; }
        account_seq(path_id, ext, now_us);
    }

    arena_.emplace_back(h.payload_len, 0);
    ByteVec& pt = arena_.back();
    Nonce n = derive_nonce(cfg_.stream_id, h.path_id, epoch_, ext);
    size_t got = aead_->open(subkey_dn_[path_id], n,
                             std::span<const uint8_t>(wire.data(), kHeaderBytes),
                             std::span<const uint8_t>(wire.data() + kHeaderBytes,
                                                      wire.size() - kHeaderBytes),
                             pt.data());
    if (got == SIZE_MAX) { ++stats.auth_fail; return false; }
    pt.resize(got);

    FrameRing::Slot* slot = ring_.acquire(h.frame_id);
    if (!slot) { ++stats.stale_frame; return false; }
    frame_ext_ = frame_started_ ? extend_seq16(frame_ext_, h.frame_id) : h.frame_id;
    frame_started_ = true;

    uint8_t band = h.band < cfg_.bands() ? h.band : 0;
    if (!band_rx_seen_[band]) {
        band_rx_seen_[band] = 1;
        band_rx_first_[band] = uint32_t(now_us);
    }
    band_rx_last_[band] = uint32_t(now_us);

    if (h.is_parity()) {
        uint64_t dk = (uint64_t(h.fec_group) << 8) | uint64_t(h.fec_idx) |
                      (uint64_t(h.fec_class) << 24) | (uint64_t(band) << 26) |
                      (uint64_t(path_id) << 30) | (uint64_t(h.layer_id) << 33);
        if (!slot->seen_parity.insert(dk).second) { ++stats.duplicates; return true; }
        ++stats.parity_datagrams;
        if (h.fec_k == 0) return true;
        uint64_t gk = group_key(h, path_id);
        auto it = groups_.find(gk);
        if (it == groups_.end()) {
            GroupState gs;
            gs.dec.reset(h.fec_k, h.fec_m ? h.fec_m : kFecMaxM);
            gs.frame_id = h.frame_id;
            gs.band = band;
            gs.path_id = path_id;
            it = groups_.emplace(gk, std::move(gs)).first;
            ++stats.fec_groups;
        }
        it->second.dec.add_parity(h.fec_idx,
                                  std::span<const uint8_t>(pt.data(), pt.size()));
    } else {
        uint64_t dk = (uint64_t(h.layer_id) << 20) | (uint64_t(h.tile_first) << 4) |
                      uint64_t(h.frag_idx);
        if (from_fec) slot->fec_repaired.insert(dk);
        if (!slot->seen_data.insert(dk).second) {
            ++stats.duplicates;
            if (slot->fec_repaired.erase(dk)) {
                ++stats.fec_recovered_redundant;
                stats.fec_recovered_redundant_bytes += wire.size();
            }
            return true;
        }
        ++stats.data_datagrams;

        size_t off = 0;
        if (h.pose_hdr) {
            if (pt.size() < kPoseHeaderBytes) { ++stats.bad_directory; return false; }
            off = kPoseHeaderBytes;
        }
        size_t dir = size_t(h.tile_count) * kDirEntryBytes;
        if (pt.size() < off + dir) { ++stats.bad_directory; return false; }
        std::vector<TileDirEntry> entries(h.tile_count);
        size_t sum = 0;
        for (uint32_t i = 0; i < h.tile_count; ++i) {
            entries[i] = unpack_dir_entry(rd32(pt.data() + off + i * kDirEntryBytes));
            sum += entries[i].len;
        }
        if (off + dir + sum != pt.size()) { ++stats.bad_directory; return false; }
        if (uint32_t(h.tile_first) + h.tile_count > cfg_.tiles_per_frame()) {
            ++stats.bad_range;
            return false;
        }
        uint16_t row0 = cfg_.row_of(h.tile_first);
        if (cfg_.row_of(uint32_t(h.tile_first) + h.tile_count - 1) != row0) {
            ++stats.bad_range;
            return false;
        }
        if (h.layer_id >= cfg_.layers) { ++stats.bad_range; return false; }

        bool late = band < slot->band_deadline_passed.size() &&
                    slot->band_deadline_passed[band];
        size_t bpos = off + dir;
        for (uint32_t i = 0; i < h.tile_count; ++i) {
            uint32_t ti = uint32_t(h.tile_first) + i;
            TileMeta& m = ring_.at(*slot, h.layer_id, ti);
            m.pose_seq = h.pose_seq;
            m.age = 0;
            m.state = TileState::kDecoded;
            m.late = late;
            m.recovered = from_fec;
            ++stats.tiles_placed;
            if (late) { ++stats.tiles_late; ++band_late_[band]; }
            if (tiles) {
                TileOutput t;
                t.frame_id = h.frame_id;
                t.layer_id = h.layer_id;
                t.row = cfg_.row_of(ti);
                t.col = cfg_.col_of(ti);
                t.cls = TileClass(entries[i].tile_class);
                t.ref_delta = entries[i].ref_delta;
                t.pose_seq = h.pose_seq;
                t.qp = entries[i].qp;
                t.mode = TileMode(entries[i].mode);
                t.res_level = entries[i].res_level;
                t.lossless = entries[i].lossless;
                t.late = late;
                t.recovered = from_fec;
                t.bytes = std::span<const uint8_t>(pt.data() + bpos, entries[i].len);
                tiles->push_back(t);
            }
            stats.tile_bytes += entries[i].len;
            bpos += entries[i].len;
        }

        if (h.fec_k > 0 && !from_fec) {
            uint64_t gk = group_key(h, path_id);
            auto it = groups_.find(gk);
            if (it == groups_.end()) {
                GroupState gs;
                gs.dec.reset(h.fec_k, h.fec_m ? h.fec_m : kFecMaxM);
                gs.frame_id = h.frame_id;
                gs.band = band;
                gs.path_id = path_id;
                it = groups_.emplace(gk, std::move(gs)).first;
                ++stats.fec_groups;
            }
            it->second.dec.add_data(h.fec_idx, wire);
        }
    }

    // Attempt recovery for this group (TRANSPORT.md 6.2).
    if (h.fec_k > 0 && depth < 2) {
        uint64_t gk = group_key(h, path_id);
        auto it = groups_.find(gk);
        // Recovery waits until the group's last parity block has arrived (v2,
        // decision D21).  Parity is sent after its group's data on the same
        // path, so anything missing at that point was lost, not reordered.  A
        // group whose tail was itself lost is retried at the band deadline.
        if (it != groups_.end() && !it->second.closed && !it->second.dec.complete() &&
            it->second.dec.recoverable() &&
            (eager_fec_ || it->second.dec.tail_seen() ||
             it->second.dec.all_blocks_seen())) {
            std::vector<ByteVec> rec;
            if (it->second.dec.recover(&rec)) {
                it->second.closed = true;
                uint8_t rband = it->second.band;
                for (ByteVec& r : rec) {
                    ++stats.fec_recovered;
                    stats.fec_recovered_bytes += r.size();
                    ++band_fec_rec_[rband];
                    process(std::span<const uint8_t>(r.data(), r.size()), path_id, now_us,
                            true, tiles, depth + 1);
                }
            }
        }
    }
    return true;
}

bool Receiver::mark_tile_undecodable(uint16_t frame_id, uint8_t layer, uint16_t row,
                                     uint16_t col) {
    // FINDINGS.md F1: row and col arrive from the decoder, which may have taken
    // them from a header field an attacker controls, so they are checked here
    // rather than trusted.  cfg_.tile_index() is a plain multiply-add and wraps
    // happily, so the check has to be on the coordinates, not on the product.
    if (row >= cfg_.rows || col >= cfg_.cols || layer >= cfg_.layers) {
        ++stats.bad_range;
        return false;
    }
    FrameRing::Slot* s = ring_.find(frame_id);
    if (!s) return false;
    TileMeta* m = ring_.try_at(*s, layer, cfg_.tile_index(row, col));
    if (!m) {
        ++stats.bad_range;
        return false;
    }
    m->state = TileState::kUndecodable;
    return true;
}

ByteVec Receiver::band_deadline(uint16_t frame_id, uint8_t band, uint64_t now_us,
                                uint16_t decode_us, uint8_t path_id) {
    ByteVec out;
    FrameRing::Slot* s = ring_.acquire(frame_id);
    if (!s || band >= cfg_.bands() || path_id >= kMaxPaths) return out;
    if (band < s->band_deadline_passed.size()) s->band_deadline_passed[band] = 1;

    // Last chance to repair this band: a group whose tail parity was itself
    // lost has not been attempted yet.  This runs before the deadline is marked
    // passed, so a repaired tile still counts as received.
    {
        std::vector<ByteVec> rec;
        for (auto& kv : groups_) {
            GroupState& g = kv.second;
            if (g.frame_id != frame_id || g.band != band) continue;
            if (g.closed || g.dec.complete() || !g.dec.recoverable()) continue;
            rec.clear();
            if (!g.dec.recover(&rec)) continue;
            g.closed = true;
            for (ByteVec& r : rec) {
                ++stats.fec_recovered;
                stats.fec_recovered_bytes += r.size();
                ++band_fec_rec_[band];
                process(std::span<const uint8_t>(r.data(), r.size()), g.path_id, now_us,
                        true, nullptr, 1);
            }
        }
    }

    uint16_t first_row = cfg_.first_row_of_band(band);
    uint32_t n = cfg_.tiles_in_band(band);
    BandReport br;
    br.frame_id = frame_id;
    br.band = band;
    br.rx_ts_first = band_rx_first_[band];
    br.rx_ts_last = band_rx_last_[band];
    br.decode_us = decode_us;
    br.late_tiles = uint16_t(band_late_[band]);
    br.fec_recovered = uint8_t(std::min<uint32_t>(255, band_fec_rec_[band]));
    br.fec_failed = uint8_t(std::min<uint32_t>(255, band_fec_fail_[band]));
    br.received.assign(n, 0);

    uint32_t missing = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t row = uint16_t(first_row + i / cfg_.cols);
        uint16_t col = uint16_t(i % cfg_.cols);
        TileMeta& m = ring_.at(*s, 0, cfg_.tile_index(row, col));
        if (m.state == TileState::kDecoded && !m.late) {
            br.received[i] = 1;
        } else {
            if (m.state == TileState::kEmpty) m.state = TileState::kConcealed;
            ++missing;
            ++stats.tiles_concealed;
        }
    }
    br.conceal_tiles = uint16_t(missing);
    br.complete = missing == 0;
    br.deadline_missed = missing > 0;

    for (auto it = groups_.begin(); it != groups_.end();) {
        bool same_band = it->second.frame_id == frame_id && it->second.band == band;
        bool too_old = ring_.started() && seq_newer(ring_.newest(), it->second.frame_id) &&
                       uint16_t(ring_.newest() - it->second.frame_id) >= kRingSlots;
        if (same_band || too_old) {
            if (!it->second.closed && !it->second.dec.complete()) {
                ++stats.fec_failed;
                if (same_band) ++band_fec_fail_[band];
            }
            it = groups_.erase(it);
        } else {
            ++it;
        }
    }
    br.fec_failed = uint8_t(std::min<uint32_t>(255, band_fec_fail_[band]));

    // Arrival margin for this band: how far inside its deadline the last
    // datagram landed.  A band that missed tiles or received nothing keeps no
    // slack by definition (decision D24).
    int32_t band_margin = DeadlineController::kNoMargin;
    if (missing == 0 && band_rx_seen_[band])
        band_margin = int32_t(now_us - uint64_t(band_rx_last_[band]));

    if (!acct_valid_ || acct_frame_ != frame_id) {
        if (acct_valid_ && acct_total_)
            deadline_.on_frame(double(acct_missed_) / double(acct_total_),
                               acct_min_margin_,
                               double(acct_late_) / double(acct_total_));
        acct_frame_ = frame_id;
        acct_valid_ = true;
        acct_total_ = 0;
        acct_missed_ = 0;
        acct_late_ = 0;
        acct_min_margin_ = INT32_MAX;
    }
    acct_total_ += n;
    acct_missed_ += missing;
    acct_late_ += band_late_[band];
    if (band_margin < acct_min_margin_) acct_min_margin_ = band_margin;

    recent_bands_.push_front(br);
    while (recent_bands_.size() > kMaxFeedbackBands) recent_bands_.pop_back();

    FeedbackPacket fb;
    fb.stream_id = cfg_.stream_id;
    fb.tiles_in_band = uint16_t(cfg_.tiles_in_band(0));
    if (deadline_.moved()) { fb.flags |= kFbDeadlineMoved; deadline_.clear_moved(); }
    for (const BandReport& b : recent_bands_) {
        BandReport c = b;
        // Re-derive the bitmap from the live ring so tiles that arrived after
        // their deadline are reported as received in this cumulative packet
        // (PAPER 4.3 item 5, TRANSPORT.md D15).
        FrameRing::Slot* bs = ring_.find(c.frame_id);
        if (bs && c.band < bs->band_reports.size() &&
            bs->band_reports[c.band] < 255)
            ++bs->band_reports[c.band];
        if (bs && c.band < cfg_.bands()) {
            uint16_t fr = cfg_.first_row_of_band(c.band);
            uint32_t bn = cfg_.tiles_in_band(c.band);
            c.received.assign(bn, 0);
            for (uint32_t i = 0; i < bn; ++i) {
                uint16_t row = uint16_t(fr + i / cfg_.cols);
                uint16_t col = uint16_t(i % cfg_.cols);
                const TileMeta& m =
                    bs->meta[cfg_.tile_index(row, col)];
                // TRANSPORT.md D17: a tile that arrived after its band deadline
                // is decoded for display but is NOT acknowledged, so the sender's
                // shadow stays exactly equal to the client's reference state.
                c.received[i] = (m.state == TileState::kDecoded && !m.late) ? 1 : 0;
            }
        }
        c.received.resize(fb.tiles_in_band, 1);  // short final band: pad as received
        fb.bands.push_back(std::move(c));
    }
    for (uint8_t i = 0; i < kMaxPaths; ++i) {
        fb.path_loss[i] = uint8_t(std::min(255.0, path_loss(i) * 255.0 + 0.5));
        fb.path_rtt_ms[i] = uint8_t(std::min<uint32_t>(255, path_[i].rtt_us / 1000));
    }

    ByteVec ptext = encode_feedback(fb, (caps_ & kCapRleFeedback) != 0);
    uint64_t counter = extend_seq16(frame_ext_, frame_id) * cfg_.bands() + band;
    Nonce nn = derive_nonce(cfg_.stream_id, path_id, epoch_, counter);
    out.resize(ptext.size() + kTagBytes);
    std::memcpy(out.data(), ptext.data(), 8);
    aead_->seal(subkey_up_[path_id], nn, std::span<const uint8_t>(ptext.data(), 8),
                std::span<const uint8_t>(ptext.data() + 8, ptext.size() - 8),
                out.data() + 8);
    ++stats.feedback_packets;
    stats.feedback_bytes += out.size();
    up_seq_ = counter + 1;

    band_rx_seen_[band] = 0;
    band_rx_first_[band] = 0;
    band_rx_last_[band] = 0;
    band_late_[band] = 0;
    band_fec_rec_[band] = 0;
    band_fec_fail_[band] = 0;
    return out;
}

Receiver::Presentation Receiver::classify(uint16_t frame_id, uint8_t layer) const {
    Presentation p;
    const FrameRing::Slot* s = ring_.find(frame_id);
    if (!s || layer >= cfg_.layers) return p;
    size_t base = size_t(layer) * cfg_.tiles_per_frame();
    for (uint32_t i = 0; i < cfg_.tiles_per_frame(); ++i) {
        const TileMeta& m = s->meta[base + i];
        switch (m.state) {
            case TileState::kDecoded: (m.age == 0 ? p.fresh : p.stale)++; break;
            case TileState::kConcealed: ++p.concealed; break;
            case TileState::kUndecodable: ++p.undecodable; break;
            case TileState::kEmpty: ++p.empty; break;
        }
    }
    return p;
}

}  // namespace nxt
