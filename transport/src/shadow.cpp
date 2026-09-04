#include "nxvc/transport/shadow.h"

#include <algorithm>

namespace nxt {

ClientShadow::ClientShadow(const StreamConfig& cfg)
    : cfg_(cfg), tiles_(cfg.tiles_per_frame()) {
    ring_.resize(kShadowFrames);
    for (Frame& f : ring_) {
        f.state.assign(tiles_, ShadowState::kUnknown);
        f.band_known.assign(cfg_.bands(), 0);
    }
    cache_.assign(size_t(kShadowFrames) * tiles_, -1);
    in_progress_.assign(size_t(kShadowFrames) * tiles_, 0);
}

void ClientShadow::begin_frame(uint16_t frame_id) {
    Frame& f = ring_[frame_id % kShadowFrames];
    f.used = true;
    f.frame_id = frame_id;
    std::fill(f.state.begin(), f.state.end(), ShadowState::kUnknown);
    std::fill(f.band_known.begin(), f.band_known.end(), uint8_t(0));
    std::fill(cache_.begin(), cache_.end(), int8_t(-1));
}

const ClientShadow::Frame* ClientShadow::find(uint16_t frame_id) const {
    const Frame& f = ring_[frame_id % kShadowFrames];
    return (f.used && f.frame_id == frame_id) ? &f : nullptr;
}

ClientShadow::Frame* ClientShadow::find(uint16_t frame_id) {
    Frame& f = ring_[frame_id % kShadowFrames];
    return (f.used && f.frame_id == frame_id) ? &f : nullptr;
}

void ClientShadow::apply_feedback(const FeedbackPacket& fb) {
    for (const BandReport& br : fb.bands) {
        Frame* f = find(br.frame_id);
        if (!f) continue;  // outside the 8-frame history
        if (br.band >= cfg_.bands()) continue;
        uint16_t first_row = cfg_.first_row_of_band(br.band);
        uint32_t n = cfg_.tiles_in_band(br.band);
        for (uint32_t i = 0; i < n && i < br.received.size(); ++i) {
            uint16_t row = uint16_t(first_row + i / cfg_.cols);
            uint16_t col = uint16_t(i % cfg_.cols);
            uint32_t idx = cfg_.tile_index(row, col);
            if (br.received[i]) {
                f->state[idx] = ShadowState::kReceived;
            } else if (f->state[idx] != ShadowState::kReceived) {
                f->state[idx] = ShadowState::kConcealed;
            }
        }
        f->band_known[br.band] = 1;
    }
    std::fill(cache_.begin(), cache_.end(), int8_t(-1));
}

bool ClientShadow::band_known(uint16_t frame_id, uint8_t band) const {
    const Frame* f = find(frame_id);
    return f && band < f->band_known.size() && f->band_known[band];
}

ShadowState ClientShadow::state(uint16_t frame_id, uint16_t row, uint16_t col) const {
    const Frame* f = find(frame_id);
    if (!f) return ShadowState::kUnknown;
    return f->state[cfg_.tile_index(row, col)];
}

bool ClientShadow::exact_uncached(const Frame& f, uint32_t idx) const {
    ShadowState s = f.state[idx];
    if (s == ShadowState::kReceived) return true;
    if (s == ShadowState::kUnknown) return false;
    // Concealed: exact only if the 3x3 source neighbourhood in the previous frame
    // is itself exact (TRANSPORT.md decision D10).
    return neighbourhood_exact(uint16_t(f.frame_id - 1), cfg_.row_of(idx),
                               cfg_.col_of(idx));
}

bool ClientShadow::exact(uint16_t frame_id, uint16_t row, uint16_t col) const {
    const Frame* f = find(frame_id);
    if (!f) return false;
    uint32_t idx = cfg_.tile_index(row, col);
    size_t slot = size_t(frame_id % kShadowFrames) * tiles_ + idx;
    if (cache_[slot] >= 0) return cache_[slot] != 0;
    if (in_progress_[slot]) return false;  // defensive; the recursion is acyclic
    in_progress_[slot] = 1;
    bool r = exact_uncached(*f, idx);
    in_progress_[slot] = 0;
    cache_[slot] = int8_t(r);
    return r;
}

bool ClientShadow::neighbourhood_exact(uint16_t frame_id, uint16_t row,
                                       uint16_t col) const {
    if (!find(frame_id)) return false;
    int r0 = std::max(0, int(row) - 1);
    int r1 = std::min(int(cfg_.rows) - 1, int(row) + 1);
    int c0 = std::max(0, int(col) - 1);
    int c1 = std::min(int(cfg_.cols) - 1, int(col) + 1);
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c)
            if (!exact(frame_id, uint16_t(r), uint16_t(c))) return false;
    return true;
}

uint8_t ClientShadow::reference_choice(uint16_t frame_id, uint16_t row,
                                       uint16_t col) const {
    for (uint8_t d = 0; d < 3; ++d) {
        // At session start the wrapped frame ids simply are not in the ring, so
        // find() returns null and the neighbourhood is not exact.
        uint16_t m = uint16_t(frame_id - 1 - d);
        if (neighbourhood_exact(m, row, col)) return d;
    }
    return kRefIntra;
}

uint8_t ClientShadow::staleness(uint16_t newest_frame, uint16_t row,
                                uint16_t col) const {
    for (uint8_t d = 0; d < kShadowFrames; ++d) {
        uint16_t m = uint16_t(newest_frame - d);
        if (exact(m, row, col)) return d;
    }
    return kShadowFrames;
}

}  // namespace nxt
