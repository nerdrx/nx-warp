#include "nxc_frame_ring.h"

#include <algorithm>
#include <cstring>

namespace nxc {

// Lock discipline: mu_ guards every slot, the history and stats_. place() and
// tick() run on the decode thread, snapshot() on the render thread, so the
// metadata cannot be written unguarded. The lock is uncontended in practice (one
// 90 Hz reader against one writer) and is taken exactly once per run and once
// per tick; feedback packets are built under the lock and *sent* outside it, so
// a sendto never stalls a frame.

FrameRing::FrameRing(const AppConfig& cfg, IDepacketizer* depack, IFeedbackSink* fb)
    : cfg_(cfg), depack_(depack), fb_(fb), tiles_(cfg.stream.tiles_per_frame()) {
    // Two frame periods, not one. All bands of frame N share one
    // predicted_display_time (PAPER 4.3), so the whole frame must arrive by
    // that instant minus reproject_budget + runtime_margin. At one frame period
    // that leaves 6.6 ms for a frame that PAPER 4.2 says takes 6.8 ms to
    // deliver on WiFi -- every frame would be born late. The real anchor is the
    // runtime's predicted display time, which sits a compositor phase wait
    // (0 to 11.1 ms, average 5.5) beyond frame completion; two frame periods is
    // the honest stand-in until xrWaitFrame provides it.
    present_latency_us_ = 2 * (1000000u / (cfg.display_hz ? cfg.display_hz : 90));
    slots_.resize(cfg.stream.ring_slots);
    for (auto& s : slots_) {
        s.meta.assign(tiles_, 0);
        s.band_fired.assign(cfg.stream.bands, 0);
        s.band_first_rx.assign(cfg.stream.bands, 0);
        s.band_last_rx.assign(cfg.stream.bands, 0);
        s.band_conceal.assign(cfg.stream.bands, 0);
        s.band_late.assign(cfg.stream.bands, 0);
        s.band_placed.assign(cfg.stream.bands, 0);
    }
    fb_buf_.resize(512);   // 8.2 / D9: the raw worst case is 225 bytes
}

void FrameRing::reset_slot(Slot& s, uint16_t frame_id, uint64_t rx_us) {
    // TRANSPORT.md 7.1: advancing the ring evicts slot frame_id mod 4 and resets
    // its per-tile metadata.
    s.live = true;
    s.frame_id = frame_id;
    s.first_rx_us = rx_us;
    s.last_rx_us = rx_us;
    s.display_anchor_us = rx_us + present_latency_us_;
    s.controller_done = false;
    std::fill(s.meta.begin(), s.meta.end(), 0u);
    std::fill(s.band_fired.begin(), s.band_fired.end(), uint8_t(0));
    std::fill(s.band_first_rx.begin(), s.band_first_rx.end(), uint64_t(0));
    std::fill(s.band_last_rx.begin(), s.band_last_rx.end(), uint64_t(0));
    std::fill(s.band_conceal.begin(), s.band_conceal.end(), uint16_t(0));
    std::fill(s.band_late.begin(), s.band_late.end(), uint16_t(0));
    std::fill(s.band_placed.begin(), s.band_placed.end(), uint32_t(0));
}

FrameRing::Slot& FrameRing::slot_for(uint16_t frame_id) {
    return slots_[frame_id % cfg_.stream.ring_slots];
}

void FrameRing::place(const PlacedRun& run) {
    std::lock_guard<std::mutex> g(mu_);

    Slot& s = slot_for(run.frame_id);

    if (!s.live || s.frame_id != run.frame_id) {
        // A slot holding a different frame. If the incoming frame is older than
        // what the ring already holds there it is behind the ring and dropped
        // (7.1, `stale_frame`); otherwise it advances the ring.
        if (s.live && newer(s.frame_id, run.frame_id)) {
            stats_.stale_frame_drop++;
            return;
        }
        reset_slot(s, run.frame_id, run.rx_ts_us);
        stats_.frames_advanced++;
    }

    if (!have_newest_ || newer(run.frame_id, newest_frame_)) {
        have_newest_ = true;
        newest_frame_ = run.frame_id;
        stats_.frames_seen++;
        stats_.newest_frame = run.frame_id;
    }

    s.last_rx_us = run.rx_ts_us;

    const uint32_t band = (run.band < cfg_.stream.bands)
                              ? run.band
                              : cfg_.stream.band_of_tile(run.tile_first);
    if (s.band_first_rx[band] == 0) s.band_first_rx[band] = run.rx_ts_us;
    s.band_last_rx[band] = run.rx_ts_us;

    // 7.4 item 3: tiles arriving after the deadline are still placed, with
    // late = 1, and their state still becomes DECODED. They remain valid
    // references and better concealment sources.
    const bool late = s.band_fired[band] != 0;
    const Slot& prev = slots_[(uint32_t(run.frame_id) + cfg_.stream.ring_slots - 1) %
                              cfg_.stream.ring_slots];

    uint32_t placed = 0;
    for (uint32_t i = 0; i < run.tile_count; ++i) {
        const uint32_t t = uint32_t(run.tile_first) + i;
        if (t >= tiles_) break;
        const uint32_t len = run.dir[i] & 0xfffu;   // 3.1
        // len == 0 is an explicit skip tile: the position keeps what the
        // previous frame had and ages by one, so it presents as *stale*, not
        // fresh (7.5). A non-empty tile is fresh, age 0.
        uint8_t age = 0;
        if (len == 0 && prev.live) {
            const uint8_t prev_age = meta_age(prev.meta[t]);
            age = prev_age < 255 ? uint8_t(prev_age + 1) : 255;
        }
        s.meta[t] = pack_tile_meta(run.pose_seq, age, kTileDecoded, late, run.recovered);
        ++placed;
    }
    s.band_placed[band] += placed;
    if (late)
        s.band_late[band] = uint16_t(std::min<uint32_t>(65535, s.band_late[band] + placed));

    stats_.tiles_placed += placed;
    if (late) stats_.tiles_late += placed;
}

void FrameRing::tick(uint64_t now_us) {
    // Feedback packets built under the lock, sent after it is released.
    std::vector<std::vector<uint8_t>> pending;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (Slot& s : slots_) {
            if (!s.live) continue;
            for (uint32_t b = 0; b < cfg_.stream.bands; ++b) {
                if (s.band_fired[b]) continue;
                // 7.4: band_deadline = predicted_display - reproject_budget
                //                      - runtime_margin - deadline_offset
                const uint64_t sub = uint64_t(cfg_.reproject_budget_us) +
                                     cfg_.runtime_margin_us + deadline_offset_us_;
                const uint64_t deadline =
                    s.display_anchor_us > sub ? s.display_anchor_us - sub : 0;
                if (now_us < deadline) continue;
                fire_band_deadline_locked(s, b, &pending);
            }
            if (!s.controller_done) {
                bool all = true;
                for (uint32_t b = 0; b < cfg_.stream.bands; ++b)
                    if (!s.band_fired[b]) { all = false; break; }
                if (all) { run_controller_locked(s); s.controller_done = true; }
            }
        }
    }
    if (fb_) {
        for (const auto& p : pending) {
            fb_->send_feedback(p.data(), p.size());
            std::lock_guard<std::mutex> g(mu_);
            stats_.feedback_sent++;
        }
    }
}

void FrameRing::fire_band_deadline_locked(Slot& s, uint32_t band,
                                          std::vector<std::vector<uint8_t>>* pending) {
    s.band_fired[band] = 1;

    // 7.4 item 1: every EMPTY tile of the band becomes CONCEALED and inherits
    // pose_seq and age + 1 from slot (N-1) mod 4.
    const Slot& prev = slots_[(uint32_t(s.frame_id) + cfg_.stream.ring_slots - 1) %
                              cfg_.stream.ring_slots];
    const uint32_t first_row = cfg_.stream.band_first_row(band);
    const uint32_t nrows     = cfg_.stream.band_row_count(band);
    const uint32_t cols      = cfg_.stream.cols;

    uint32_t concealed = 0;
    for (uint32_t r = first_row; r < first_row + nrows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            const uint32_t t = r * cols + c;
            if (t >= tiles_) continue;
            if (meta_state(s.meta[t]) != kTileEmpty) continue;
            const uint32_t pm = prev.live ? prev.meta[t] : 0u;
            const uint8_t  prev_age  = meta_age(pm);
            const uint16_t prev_pose = uint16_t(pm & 0xffff);
            s.meta[t] = pack_tile_meta(prev_pose,
                                       prev_age < 255 ? uint8_t(prev_age + 1) : 255,
                                       kTileConcealed, false, false);
            ++concealed;
        }
    }
    s.band_conceal[band] = uint16_t(std::min<uint32_t>(65535, concealed));

    stats_.deadlines_fired++;
    stats_.tiles_concealed += concealed;

    // 7.4 item 2: generate the band's feedback packet.
    if (cfg_.send_feedback) build_feedback_locked(s, band, pending);
}

void FrameRing::build_feedback_locked(const Slot& s, uint32_t band,
                                      std::vector<std::vector<uint8_t>>* pending) {
    const uint32_t nband     = cfg_.stream.tiles_in_band(band);
    const uint32_t bmbytes   = (nband + 7u) / 8u;
    const uint32_t first_row = cfg_.stream.band_first_row(band);
    const uint32_t cols      = cfg_.stream.cols;

    BandRecord rec;
    rec.bitmap.assign(bmbytes, 0);
    uint32_t received = 0;
    for (uint32_t i = 0; i < nband; ++i) {
        const uint32_t t = (first_row + i / cols) * cols + (i % cols);
        if (t >= tiles_) continue;
        // 8.2: the bit is set only if the datagram decrypted AND the tile
        // decoded without error. With the placeholder decoder, DECODED is the
        // strongest claim available; the real decoder will clear bits through
        // mark_tile_undecodable() before the deadline.
        if (meta_state(s.meta[t]) == kTileDecoded) {
            rec.bitmap[i >> 3] |= uint8_t(1u << (i & 7));
            ++received;
        }
    }

    rec.in.frame_id        = s.frame_id;
    rec.in.band            = uint8_t(band);
    rec.in.tiles_in_band   = uint16_t(nband);
    rec.in.complete        = (received == nband);
    rec.in.deadline_missed = (received != nband);
    rec.in.rx_ts_first     = uint32_t(s.band_first_rx[band]);
    rec.in.rx_ts_last      = uint32_t(s.band_last_rx[band]);
    rec.in.decode_us       = uint16_t(std::min<uint32_t>(65535, stats_.last_decode_us));
    rec.in.conceal_tiles   = s.band_conceal[band];
    rec.in.late_tiles      = s.band_late[band];
    rec.in.fec_recovered   = 0;   // stub: no FEC
    rec.in.fec_failed      = 0;

    history_.insert(history_.begin(), std::move(rec));
    if (history_.size() > 3) history_.resize(3);

    // Bitmap pointers are fixed up here because the records were moved.
    std::vector<BandFeedbackInput> ins(history_.size());
    for (size_t i = 0; i < history_.size(); ++i) {
        ins[i] = history_[i].in;
        ins[i].received_bitmap = history_[i].bitmap.data();
        ins[i].bitmap_bytes    = uint32_t(history_[i].bitmap.size());
    }

    FeedbackFlags flags;
    flags.deadline_moved = deadline_moved_;
    deadline_moved_ = false;

    const auto& c = depack_->counters();
    uint8_t loss_q8[2] = {0, 0};
    for (int p = 0; p < 2; ++p) {
        const uint64_t rx = c.path_rx[p], lost = c.path_lost[p];
        const uint64_t tot = rx + lost;
        loss_q8[p] = tot ? uint8_t((lost * 255 + tot / 2) / tot) : 0;
    }
    const uint8_t rtt[2] = {0, 0};   // no ping/pong in this shell

    const size_t n = depack_->build_feedback(ins.data(), ins.size(), flags, loss_q8, rtt,
                                             fb_buf_.data(), fb_buf_.size());
    if (n) pending->emplace_back(fb_buf_.begin(), fb_buf_.begin() + n);
}

void FrameRing::run_controller_locked(Slot& s) {
    // 7.4 / PAPER 4.3. miss(N) is the fraction of the frame's tiles whose state
    // at their band deadline was not DECODED.
    uint32_t missed = 0;
    for (uint32_t t = 0; t < tiles_; ++t)
        if (meta_state(s.meta[t]) != kTileDecoded) ++missed;
    const double miss = tiles_ ? double(missed) / double(tiles_) : 0.0;

    if (miss > 0.10) ++consecutive_miss_; else consecutive_miss_ = 0;
    if (consecutive_miss_ >= 5 && deadline_offset_us_ < 4000) {
        deadline_offset_us_ += 1000;
        consecutive_miss_ = 0;
        deadline_moved_ = true;
    }
    // D8: consecutive frames with ZERO missing tiles, reset (not decremented).
    if (missed == 0) {
        ++clean_frames_;
        if (clean_frames_ >= cfg_.display_hz) {   // "relaxes 0.2 ms per clean second"
            clean_frames_ = 0;
            deadline_offset_us_ = deadline_offset_us_ > 200 ? deadline_offset_us_ - 200 : 0;
            deadline_moved_ = true;
        }
    } else {
        clean_frames_ = 0;
    }

    stats_.deadline_offset_us = deadline_offset_us_;
    stats_.consecutive_miss = consecutive_miss_;
    stats_.clean_frames = clean_frames_;
}

bool FrameRing::snapshot(std::vector<uint32_t>* out_meta, uint16_t* out_frame_id,
                         FrameClassification* out_class) {
    std::lock_guard<std::mutex> g(mu_);
    if (!have_newest_) return false;
    const Slot& s = slots_[newest_frame_ % cfg_.stream.ring_slots];
    if (!s.live) return false;

    out_meta->resize(tiles_);
    std::memcpy(out_meta->data(), s.meta.data(), size_t(tiles_) * sizeof(uint32_t));
    *out_frame_id = s.frame_id;

    // 7.5, presentation classification.
    FrameClassification k;
    k.total = tiles_;
    for (uint32_t t = 0; t < tiles_; ++t) {
        const uint32_t m = (*out_meta)[t];
        switch (meta_state(m)) {
            case kTileDecoded:     (meta_age(m) == 0 ? k.fresh : k.stale)++; break;
            case kTileConcealed:   k.concealed++; break;
            case kTileUndecodable: k.undecodable++; break;
            default:               k.empty++; break;
        }
    }
    *out_class = k;
    return true;
}

void FrameRing::set_decode_us(uint32_t us) {
    std::lock_guard<std::mutex> g(mu_);
    stats_.last_decode_us = us;
}

RingStats FrameRing::stats() const {
    std::lock_guard<std::mutex> g(mu_);
    return stats_;
}

}  // namespace nxc
