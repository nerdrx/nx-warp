// NX Warp decoder, ATLAS: the host bookkeeping the atlas needs, Vulkan-free.
//
// Three questions the decoder has to answer on every arriving run of tiles, and
// none of them can be answered by reading the device:
//
//   1. is this tile SUPERSEDED?  A tile of frame N that arrives after a later
//      frame already coded that position is dropped.  Arrivals are not in frame
//      order and the atlas is a per-position latest-wins store, so this is the
//      whole of ordering under tile streaming.
//   2. which entries must be ADVANCED before this run is decoded, and how far?
//      The lazy advance moves an entry only when a coded tile is about to read
//      it -- plus any entry about to fall out of the `H` ring, because the steps
//      to advance it will no longer exist.
//   3. which `H` slots are still live?
//
// It is host state and not device state because the answers are host-known:
// `src_frame` changes only when the host applies a coded tile or a base-layer
// import, and `advanced_to` changes only when the host dispatches an advance.
// Reading either back from the device would put a stall in every frame, which
// is the one thing tile streaming exists to remove.
//
// What is NOT here, deliberately: `valid`.  Validity is decided by the envelope
// check inside the composition, which runs on the device, so the host cannot
// know it without either a readback or a second implementation of 13.12.2.
// [SYN] 13.12.4's "a coded non-INTRA tile whose entry is invalid is BITSTREAM"
// is therefore reported by the kernel through a status word and refused once
// the frame completes -- deferred, not absent.  See atlas_tiles.comp.
//
// NORMATIVE SOURCE: docs/SYNTAX.md 13.12.3 and docs/ATLAS-DECODER.md.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_ATLAS_STATE_H
#define NXVW_ATLAS_STATE_H

#include <cstdint>
#include <vector>

#include "atlas_layout.h"

namespace nxvw {

// What happened to one arriving run of tiles.
struct AtlasApply {
    std::vector<uint32_t> accepted;    // table indices to decode, in order
    std::vector<uint32_t> superseded;  // table indices dropped, in order
};

class AtlasHostState {
  public:
    // `tile_map_reset` zeroes the whole table, which makes every entry invalid
    // and the atlas pixels undefined ([SYN] 13.12.1).  `src_frame` starts at
    // -1 and NOT at 0, because 0 is a real frame number: an entry that has
    // never been coded must accept frame 0, and `0 >= 0` would drop it.
    void reset(uint32_t entries) {
        src_frame_.assign(entries, -1);
        advanced_to_.assign(entries, 0);
        for (uint32_t k = 0; k < NXVW_ATLAS_HRING; ++k) live_[k] = false;
        newest_ = -1;
    }

    uint32_t entries() const { return (uint32_t)src_frame_.size(); }

    // Retain one frame's per-eye homography.  `warp_present == 0` is recorded
    // too: such a frame contributes NO step at all -- not a composition and not
    // a `gen` increment -- and the advance still has to walk PAST it, so the
    // slot has to exist.
    void record_frame(uint32_t frame) {
        live_[frame % NXVW_ATLAS_HRING] = true;
        if ((int64_t)frame > newest_) newest_ = (int64_t)frame;
    }

    // Is frame `f`'s `H` still in the ring at frame `now`?
    bool h_live(uint32_t now, uint32_t f) const {
        if ((int64_t)f > (int64_t)now) return false;
        return (uint64_t)(now - f) < (uint64_t)NXVW_ATLAS_HRING &&
               live_[f % NXVW_ATLAS_HRING];
    }

    // [ATLAS-DECODER.md] the monotonicity rule.  A tile of frame `frame` at a
    // position whose `src_frame` is already >= `frame` is DROPPED and reported
    // `superseded`.  It is a report and not a loss: the position already holds
    // a newer generation than the dropped tile, so the encoder must not answer
    // it with a refresh.
    //
    // `>=` and not `>`.  Two tiles of the SAME frame at the same position is
    // not something a well-formed stream does, but a retransmit is, and
    // applying the second one would re-run the write-back over an entry that
    // has already been reset -- which is harmless for the metadata and wrong
    // for the pixels the moment a NEAR_SKIP correction has landed on top.
    AtlasApply apply(const uint32_t *tiles, uint32_t count, uint32_t frame) {
        AtlasApply r;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t t = tiles[i];
            if (t >= src_frame_.size()) continue;
            if (src_frame_[t] >= (int64_t)frame) r.superseded.push_back(t);
            else r.accepted.push_back(t);
        }
        return r;
    }

    // Commit the accepted tiles: the write-back of 13.12.3 step 3 has run, so
    // the position now holds frame `frame` and is advanced to it.
    void commit(const uint32_t *tiles, uint32_t count, uint32_t frame) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t t = tiles[i];
            if (t >= src_frame_.size()) continue;
            src_frame_[t] = (int64_t)frame;
            advanced_to_[t] = frame;
        }
    }

    // Which entries the compose dispatch must cover before `frame`'s tiles are
    // decoded: the tiles about to be read, plus every entry whose `advanced_to`
    // is about to fall out of the ring.
    //
    // The second set is what keeps the ring from ever being the reason a tile
    // is invalidated.  It is NOT what makes the lazy advance safe -- the
    // per-step envelope check is -- but without it an entry that goes untouched
    // for 64 frames would have to be invalidated for a reason the atlas cannot
    // state.
    void select_for_advance(const uint32_t *tiles, uint32_t count,
                            uint32_t frame, std::vector<uint32_t> &out) const {
        out.clear();
        // The empty case is spelled out rather than left to the bounds test:
        // GCC cannot see through the inlined `tiles[i] < size()` guard and
        // warns about a write into a zero-sized region otherwise.
        const size_t n = src_frame_.size();
        if (n == 0) return;
        std::vector<uint8_t> named(n, 0u);
        for (uint32_t i = 0; i < count; ++i)
            if ((size_t)tiles[i] < n) named[tiles[i]] = 1u;
        for (uint32_t t = 0; t < (uint32_t)n; ++t) {
            if (advanced_to_[t] == frame) continue;
            const bool falling =
                (uint64_t)(frame - advanced_to_[t]) >= NXVW_ATLAS_HRING - 1u;
            if (named[t] || falling) out.push_back(t);
        }
    }

    // The FLUSH: every entry advanced to `frame`.  The normative atlas is the
    // flushed state, and the frame-complete path flushes every frame, which is
    // exactly the eager form.
    void select_for_flush(uint32_t frame, std::vector<uint32_t> &out) const {
        out.clear();
        for (uint32_t t = 0; t < (uint32_t)src_frame_.size(); ++t)
            if (advanced_to_[t] != frame) out.push_back(t);
    }

    void mark_advanced(const std::vector<uint32_t> &tiles, uint32_t frame) {
        for (uint32_t t : tiles)
            if (t < advanced_to_.size()) advanced_to_[t] = frame;
    }
    void mark_all_advanced(uint32_t frame) {
        for (auto &a : advanced_to_) a = frame;
    }

    int64_t src_frame(uint32_t t) const { return src_frame_[t]; }
    uint32_t advanced_to(uint32_t t) const { return advanced_to_[t]; }

  private:
    std::vector<int64_t> src_frame_;    // -1 = never coded
    std::vector<uint32_t> advanced_to_;
    bool live_[NXVW_ATLAS_HRING] = {};
    int64_t newest_ = -1;
};

}  // namespace nxvw

#endif  // NXVW_ATLAS_STATE_H
