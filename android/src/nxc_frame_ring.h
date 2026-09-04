// The 4-slot frame ring, per-tile metadata and the deadline state machine.
//
// Normative: PAPER 4.3 and docs/TRANSPORT.md section 7. Tile (N, t) lands in
// slot N mod 4. There is no reorder buffer: a datagram is placed the moment it
// parses, by (frame_id mod 4, layer_id, tile_index), and arrival order is
// irrelevant (7.1).
//
// WHAT IS SUBSTITUTED HERE. The deadline of 7.4 is anchored on
// `predicted_display_time(N)`, which in the real client comes from the OpenXR
// runtime's xrWaitFrame. This shell has no OpenXR, so the anchor is
// `first_rx_us(N) + present_latency_us` -- the moment the frame's first datagram
// arrived, plus a fixed presentation budget. Every quantity the controller uses
// is still a client-clock quantity, which is the property PAPER 4.11 actually
// requires. When WiVRn integration lands, replace set_display_anchor() with the
// runtime's predicted display time and nothing else changes.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "nxc_config.h"
#include "nxc_transport.h"

namespace nxc {

// TRANSPORT.md 7.3, state field.
enum TileState : uint8_t {
    kTileEmpty = 0, kTileDecoded = 1, kTileConcealed = 2, kTileUndecodable = 3,
};

// TRANSPORT.md 7.3: 4 bytes per tile per slot. This is exactly the layout the
// per-slot metadata SSBO carries to the GPU, so the placeholder decoder and,
// later, the real Pass B read the same words the CPU wrote.
inline uint32_t pack_tile_meta(uint16_t pose_seq, uint8_t age, uint8_t state,
                               bool late, bool recovered) {
    return uint32_t(pose_seq) | (uint32_t(age) << 16) | (uint32_t(state & 3) << 24) |
           (uint32_t(late ? 1 : 0) << 26) | (uint32_t(recovered ? 1 : 0) << 27);
}
inline uint8_t meta_state(uint32_t m) { return uint8_t((m >> 24) & 3); }
inline uint8_t meta_age(uint32_t m)   { return uint8_t((m >> 16) & 0xff); }
inline bool    meta_late(uint32_t m)  { return (m >> 26) & 1; }

// PAPER 4.3 / TRANSPORT.md 7.5, presentation classification.
struct FrameClassification {
    uint32_t fresh = 0, stale = 0, concealed = 0, undecodable = 0, empty = 0;
    uint32_t total = 0;
    bool partial() const { return concealed > 0 || undecodable > 0 || empty > 0; }
};

struct RingStats {
    uint64_t frames_seen      = 0;
    uint64_t frames_advanced  = 0;
    uint64_t tiles_placed     = 0;
    uint64_t tiles_late       = 0;
    uint64_t tiles_concealed  = 0;
    uint64_t stale_frame_drop = 0;   // datagram older than the ring (7.1)
    uint64_t deadlines_fired  = 0;
    uint64_t feedback_sent    = 0;
    uint32_t deadline_offset_us = 0; // the controller's output, 0..4000
    uint32_t consecutive_miss = 0;
    uint32_t clean_frames     = 0;
    uint16_t newest_frame     = 0;
    uint32_t last_decode_us   = 0;
};

// Sink for finished feedback packets. The app implements it by handing the bytes
// to Receiver::send_to_peer.
struct IFeedbackSink {
    virtual ~IFeedbackSink() = default;
    virtual void send_feedback(const uint8_t* data, size_t len) = 0;
};

class FrameRing : public ITileSink {
public:
    FrameRing(const AppConfig& cfg, IDepacketizer* depack, IFeedbackSink* fb);

    // ITileSink. Called on the decode thread only.
    void place(const PlacedRun& run) override;

    // Runs the deadline state machine up to `now_us`. Decode thread only.
    // Fires concealment, feedback and the controller for every band whose
    // deadline has passed since the previous call.
    void tick(uint64_t now_us);

    // Copies the newest presentable slot's metadata for the renderer. Returns
    // false if nothing has arrived yet. Safe from the render thread.
    bool snapshot(std::vector<uint32_t>* out_meta, uint16_t* out_frame_id,
                  FrameClassification* out_class);

    RingStats stats() const;

    // Measured GPU decode time of the last frame, folded into the feedback
    // record's `decode_us` (TRANSPORT.md 8.2). Render thread.
    void set_decode_us(uint32_t us);

    // The presentation budget described in the header comment.
    void set_present_latency_us(uint32_t us) { present_latency_us_ = us; }

private:
    struct Slot {
        bool     live = false;
        uint16_t frame_id = 0;
        uint64_t first_rx_us = 0;
        uint64_t last_rx_us = 0;
        uint64_t display_anchor_us = 0;
        std::vector<uint32_t> meta;          // tiles_per_frame entries
        // Per band: has its deadline fired, and the accounting the feedback
        // record needs (TRANSPORT.md 8.2).
        std::vector<uint8_t>  band_fired;
        std::vector<uint64_t> band_first_rx;
        std::vector<uint64_t> band_last_rx;
        std::vector<uint16_t> band_conceal;
        std::vector<uint16_t> band_late;
        std::vector<uint32_t> band_placed;
        bool controller_done = false;
    };

    // All _locked helpers require mu_ to be held.
    Slot& slot_for(uint16_t frame_id);
    void  reset_slot(Slot& s, uint16_t frame_id, uint64_t rx_us);
    void  fire_band_deadline_locked(Slot& s, uint32_t band,
                                    std::vector<std::vector<uint8_t>>* pending);
    void  build_feedback_locked(const Slot& s, uint32_t band,
                                std::vector<std::vector<uint8_t>>* pending);
    void  run_controller_locked(Slot& s);

    // Newer-than comparison on a 16-bit wrapping frame id (wraps every 12.1 min
    // at 90 Hz, PAPER 4.1).
    static bool newer(uint16_t a, uint16_t b) {
        return int16_t(uint16_t(a - b)) > 0;
    }

    AppConfig      cfg_;
    IDepacketizer* depack_;
    IFeedbackSink* fb_;
    uint32_t       tiles_;
    uint32_t       present_latency_us_;

    std::vector<Slot> slots_;
    bool     have_newest_ = false;
    uint16_t newest_frame_ = 0;

    // Rolling history of the last 3 band feedback records (8: "cumulative over
    // the last 3 bands"), newest at index 0.
    struct BandRecord {
        BandFeedbackInput in{};
        std::vector<uint8_t> bitmap;
    };
    std::vector<BandRecord> history_;
    std::vector<uint8_t>    fb_buf_;

    // Controller state (7.4 / PAPER 4.3).
    uint32_t deadline_offset_us_ = 0;
    uint32_t consecutive_miss_   = 0;
    uint32_t clean_frames_       = 0;
    bool     deadline_moved_     = false;

    mutable std::mutex mu_;      // guards the snapshot copy and stats_
    RingStats stats_;
};

}  // namespace nxc
