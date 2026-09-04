// Receiver behaviour: the deadline state machine, position-addressed placement
// without a reorder buffer, duplicate suppression across paths, FEC recovery in
// the live path, and the failure-mode counters.  TRANSPORT.md 7, 10, 12.
#include <algorithm>

#include "nxvc/transport/receiver.h"
#include "nxvc/transport/sender.h"
#include "test_util.h"

using namespace nxt;

namespace {

StreamConfig cfg8() {
    StreamConfig c;
    c.cols = 8;
    c.rows = 6;
    c.band_rows = 2;
    c.layers = 1;
    c.caps = kCapFec | kCapMultipath | kCapPoseHdr | kCapRleFeedback;
    return c;
}

struct Pair {
    StreamConfig c = cfg8();
    std::unique_ptr<Aead> aead = make_null_aead();
    Key key{}, salt{};
    Sender tx;
    Receiver rx;
    std::vector<ByteVec> pool;
    Pair() : tx(c, aead.get(), key, salt), rx(c, aead.get(), key, salt) {
        for (int i = 0; i < 16; ++i) pool.emplace_back(20 + i, uint8_t(i + 1));
    }
    std::vector<TileInput> band_tiles(uint16_t frame, uint8_t band, TileClass cls) {
        std::vector<TileInput> v;
        uint16_t r0 = c.first_row_of_band(band);
        for (uint16_t row = r0; row < r0 + c.rows_in_band(band); ++row)
            for (uint16_t col = 0; col < c.cols; ++col) {
                TileInput t;
                t.frame_id = frame;
                t.row = row;
                t.col = col;
                t.cls = cls;
                t.ref_delta = 0;
                t.bytes = pool[(row * c.cols + col) % pool.size()];
                v.push_back(t);
            }
        return v;
    }
};

}  // namespace

static void deadline_state_machine() {
    tt::begin("deadline controller: climb on miss, capped at 4 ms");
    DeadlineController d;
    TT_EQ(d.offset_us(), 0u);
    for (int i = 0; i < 4; ++i) d.on_frame(0.5);
    TT_EQ(d.offset_us(), 0u);
    d.on_frame(0.5);
    TT_EQ(d.offset_us(), 1000u);
    TT_CHECK(d.moved());
    d.clear_moved();
    // A good frame resets the run.
    d.on_frame(0.0, 0);
    for (int i = 0; i < 4; ++i) d.on_frame(0.5);
    TT_EQ(d.offset_us(), 1000u);
    d.on_frame(0.5);
    TT_EQ(d.offset_us(), 2000u);
    for (int i = 0; i < 200; ++i) d.on_frame(0.9);
    TT_EQ(d.offset_us(), 4000u);
    // Exactly 10 % missing does not trip it.
    DeadlineController e;
    for (int i = 0; i < 50; ++i) e.on_frame(0.10);
    TT_EQ(e.offset_us(), 0u);
    tt::end();
}

// The 10 % miss threshold has a dead zone: a band that is systematically late
// is only about 6 % of a frame, so the deadline never moved and those tiles
// stayed concealed forever.  Late tiles are the sharper signal (D24).
static void deadline_climbs_on_late_tiles() {
    tt::begin("deadline climbs on late tiles, not just the miss fraction (D24)");
    // Below the miss threshold, but tiles are arriving after the deadline.
    DeadlineController d;
    for (int i = 0; i < 4; ++i) d.on_frame(0.066, DeadlineController::kNoMargin, 0.056);
    TT_EQ(d.offset_us(), 0u);
    d.on_frame(0.066, DeadlineController::kNoMargin, 0.056);
    TT_EQ(d.offset_us(), 1000u);

    // Loss without lateness must NOT move the deadline: those tiles never
    // arrived, and a later deadline cannot conjure them.
    DeadlineController e;
    for (int i = 0; i < 200; ++i) e.on_frame(0.05, 4000, 0.0);
    TT_EQ(e.offset_us(), 0u);

    // A trickle of late tiles under the threshold is tolerated.
    DeadlineController f;
    for (int i = 0; i < 200; ++i)
        f.on_frame(0.0, 3000, DeadlineController::kLateFraction);
    TT_EQ(f.offset_us(), 0u);
    tt::end();
}

// Decision D24: the old rule relaxed after 90 consecutive zero-miss frames,
// which a fast link reaches often enough to relax back into the miss region and
// oscillate.  The replacement needs a full window of frames that all kept at
// least kRelaxMarginUs of slack.
static void deadline_relax_rule() {
    tt::begin("deadline controller: relax needs a window of real slack (D24)");
    const int W = DeadlineController::kRelaxWindowFrames;
    DeadlineController d;
    for (int i = 0; i < 5; ++i) d.on_frame(0.5);
    TT_EQ(d.offset_us(), 1000u);
    d.clear_moved();

    // Clean frames that only just make the deadline do NOT relax it, however
    // many there are.  This is the case the old rule got wrong.
    for (int i = 0; i < 4 * W; ++i)
        d.on_frame(0.0, DeadlineController::kRelaxMarginUs - 1);
    TT_EQ(d.offset_us(), 1000u);
    TT_CHECK(!d.moved());

    // A full window with real slack steps down once, by kRelaxStepUs.
    for (int i = 0; i < W; ++i) d.on_frame(0.0, 3000);
    TT_EQ(d.offset_us(), 1000u - DeadlineController::kRelaxStepUs);
    TT_CHECK(d.moved());
    d.clear_moved();

    // Hysteresis: the very next slack frame must not step again.
    d.on_frame(0.0, 3000);
    TT_EQ(d.offset_us(), 1000u - DeadlineController::kRelaxStepUs);
    // Another full window does.
    for (int i = 1; i < W; ++i) d.on_frame(0.0, 3000);
    TT_EQ(d.offset_us(), 1000u - 2 * DeadlineController::kRelaxStepUs);

    // One tight frame anywhere in the window blocks the step.
    DeadlineController f;
    for (int i = 0; i < 5; ++i) f.on_frame(0.5);
    TT_EQ(f.offset_us(), 1000u);
    for (int i = 0; i < W - 1; ++i) f.on_frame(0.0, 3000);
    f.on_frame(0.0, 10);  // one frame with almost no slack
    for (int i = 0; i < W - 1; ++i) f.on_frame(0.0, 3000);
    TT_EQ(f.offset_us(), 1000u);

    // A frame that missed tiles contributes no slack even if a margin is given.
    DeadlineController g;
    for (int i = 0; i < 5; ++i) g.on_frame(0.5);
    for (int i = 0; i < W; ++i) g.on_frame(0.02, 5000);
    TT_EQ(g.offset_us(), 1000u);

    // A climb clears the window: the slack measured against the old, earlier
    // deadline says nothing about the new one.
    DeadlineController h;
    for (int i = 0; i < 5; ++i) h.on_frame(0.5);
    for (int i = 0; i < W - 1; ++i) h.on_frame(0.0, 5000);
    for (int i = 0; i < 5; ++i) h.on_frame(0.5);
    TT_EQ(h.offset_us(), 2000u);
    h.on_frame(0.0, 5000);
    TT_EQ(h.offset_us(), 2000u);

    // It never relaxes below zero.
    DeadlineController z;
    for (int i = 0; i < 20 * W; ++i) z.on_frame(0.0, 5000);
    TT_EQ(z.offset_us(), 0u);
    tt::end();
}


static void placement_is_order_independent() {
    tt::begin("placement is position addressed: no reorder buffer");
    Pair a, b;
    PoseHeader pose;
    a.tx.begin_frame(1, pose, 0, 0);
    b.tx.begin_frame(1, pose, 0, 0);
    auto tiles = a.band_tiles(1, 0, TileClass::kC);
    auto d1 = a.tx.send_band(0, tiles, 100, 10, false);
    auto d2 = b.tx.send_band(0, tiles, 100, 10, false);
    TT_EQ(d1.size(), d2.size());

    std::vector<TileOutput> out;
    for (auto& d : d1)
        a.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                         d.path_id, 1000, &out);
    std::reverse(d2.begin(), d2.end());
    for (auto& d : d2)
        b.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                         d.path_id, 1000, &out);

    auto pa = a.rx.classify(1);
    auto pb = b.rx.classify(1);
    TT_EQ(pa.fresh, pb.fresh);
    TT_EQ(pa.fresh, a.c.tiles_in_band(0));
    tt::end();
}

static void duplicate_suppression() {
    tt::begin("duplicate suppression across paths");
    Pair p;
    p.tx.striper().configure_path(0, 300e6, 3000);
    p.tx.striper().configure_path(1, 900e6, 1000);
    PoseHeader pose;
    p.tx.begin_frame(1, pose, 0, 0);
    auto tiles = p.band_tiles(1, 0, TileClass::kA);  // class A is duplicated
    auto dgs = p.tx.send_band(0, tiles, 100, 10, false);

    int on0 = 0, on1 = 0;
    for (const Datagram& d : dgs) (d.path_id == 0 ? on0 : on1)++;
    TT_CHECK(on0 > 0);
    TT_CHECK(on1 > 0);
    TT_EQ(on0, on1);  // class A duplicated on both paths

    std::vector<TileOutput> out;
    for (auto& d : dgs) {
        out.clear();
        p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                         d.path_id, 1000, &out);
    }
    // Every tile placed once; the second copy of each datagram is suppressed.
    TT_EQ(p.rx.classify(1).fresh, p.c.tiles_in_band(0));
    TT_CHECK(p.rx.stats.duplicates > 0);
    TT_EQ(p.rx.stats.duplicates + p.rx.stats.data_datagrams + p.rx.stats.parity_datagrams -
              p.rx.stats.fec_recovered,
          p.rx.stats.datagrams);
    // Delivering the whole band twice more adds no new tiles.
    uint64_t placed = p.rx.stats.tiles_placed;
    for (auto& d : dgs)
        p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                         d.path_id, 1100, &out);
    TT_EQ(p.rx.stats.tiles_placed, placed);
    tt::end();
}

static void fec_recovery_in_the_live_path() {
    tt::begin("live FEC: a dropped class A datagram is recovered");
    Pair p;
    PoseHeader pose;
    p.tx.begin_frame(1, pose, 0, 0);
    auto tiles = p.band_tiles(1, 0, TileClass::kA);  // 3 parity per group
    auto dgs = p.tx.send_band(0, tiles, 100, 10, false);
    int data = 0, parity = 0;
    for (const Datagram& d : dgs) {
        DatagramHeader h;
        decode_header(d.bytes.data(), &h);
        (h.is_parity() ? parity : data)++;
    }
    // v2: parity scales with the realised k (30 % for class A, floor 1), so a
    // two-datagram class A group gets one parity block, not three.
    TT_CHECK(parity >= 1);
    FecPolicy pol;
    TT_EQ(pol.parity_for(0, data), parity);
    TT_EQ(pol.parity_for(0, 10), 3);
    TT_EQ(pol.parity_for(0, 2), 1);
    TT_EQ(pol.parity_for(1, 10), 1);
    TT_EQ(pol.parity_for(1, 3), 0);
    TT_EQ(pol.parity_for(2, 10), 0);

    std::vector<TileOutput> out;
    int dropped = 0;
    for (auto& d : dgs) {
        DatagramHeader h;
        decode_header(d.bytes.data(), &h);
        if (!h.is_parity() && dropped == 0) { ++dropped; continue; }  // lose one
        out.clear();
        p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                         d.path_id, 1000, &out);
    }
    TT_CHECK(p.rx.stats.fec_recovered >= 1);
    TT_EQ(p.rx.classify(1).fresh, p.c.tiles_in_band(0));
    tt::end();
}

static void concealment_and_feedback() {
    tt::begin("deadline conceals, feedback reports, ages advance");
    Pair p;
    PoseHeader pose;
    // Frame 1: everything arrives.
    p.tx.begin_frame(1, pose, 0, 0);
    std::vector<TileOutput> out;
    for (uint8_t b = 0; b < p.c.bands(); ++b) {
        auto dgs = p.tx.send_band(b, p.band_tiles(1, b, TileClass::kC), 100, 10,
                                  b + 1 == p.c.bands());
        for (auto& d : dgs)
            p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                             d.path_id, 1000, &out);
        ByteVec fb = p.rx.band_deadline(1, b, 2000, 40, 0);
        TT_CHECK(!fb.empty());
    }
    TT_EQ(p.rx.classify(1).fresh, p.c.tiles_per_frame());
    TT_EQ(p.rx.classify(1).concealed, 0u);

    // Frame 2: band 1 is lost entirely.
    p.tx.begin_frame(2, pose, 0, 0);
    for (uint8_t b = 0; b < p.c.bands(); ++b) {
        auto dgs = p.tx.send_band(b, p.band_tiles(2, b, TileClass::kC), 100, 10,
                                  b + 1 == p.c.bands());
        if (b != 1)
            for (auto& d : dgs)
                p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                                 d.path_id, 3000, &out);
        p.rx.band_deadline(2, b, 4000, 40, 0);
    }
    auto pr = p.rx.classify(2);
    TT_EQ(pr.concealed, p.c.tiles_in_band(1));
    TT_EQ(pr.fresh, p.c.tiles_per_frame() - p.c.tiles_in_band(1));
    TT_CHECK(pr.partial());
    tt::end();
}

// FINDINGS.md F1: mark_tile_undecodable() took (row, col) on trust and indexed
// the ring's per-tile metadata with them.  A decoder that derives them from a
// header field walks off the end; reproducer
// fuzz/regressions/transport_depacketize_fuzz/F1-mark_tile_undecodable-oob-tile_first-32768.bin
static void mark_tile_undecodable_bounds() {
    tt::begin("mark_tile_undecodable rejects off-grid coordinates (F1)");
    Pair p;
    PoseHeader pose;
    p.tx.begin_frame(0, pose, 0, 0);
    std::vector<TileOutput> out;
    for (auto& d : p.tx.send_band(0, p.band_tiles(0, 0, TileClass::kC), 100, 10, false))
        p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                         d.path_id, 1000, &out);

    // In range: accepted, and the tile stops counting as received.
    TT_CHECK(p.rx.mark_tile_undecodable(0, 0, 0, 0));
    TT_EQ(p.rx.classify(0).undecodable, 1u);

    // The exact reproducer coordinates: tile_first 32768 on a 68-column grid.
    const uint16_t bad_row = 481, bad_col = 60;
    TT_EQ(uint32_t(bad_row) * 68u + bad_col, 32768u);
    uint64_t before = p.rx.stats.bad_range;
    TT_CHECK(!p.rx.mark_tile_undecodable(0, 0, bad_row, bad_col));
    TT_EQ(p.rx.stats.bad_range, before + 1);

    // Every axis, including the first value past each edge and the extremes a
    // 16-bit header field can carry.
    struct { uint16_t row, col; uint8_t layer; } bad[] = {
        {p.c.rows, 0, 0},        {0, p.c.cols, 0},        {p.c.rows, p.c.cols, 0},
        {0, 0, uint8_t(p.c.layers)}, {65535, 65535, 0},   {65535, 0, 0},
        {0, 65535, 0},           {uint16_t(p.c.rows + 1), uint16_t(p.c.cols + 1), 3},
    };
    for (const auto& b : bad) {
        before = p.rx.stats.bad_range;
        TT_CHECK(!p.rx.mark_tile_undecodable(0, b.layer, b.row, b.col));
        TT_EQ(p.rx.stats.bad_range, before + 1);
    }
    // Nothing but the one legal call above was written.
    TT_EQ(p.rx.classify(0).undecodable, 1u);

    // A frame that is not in the ring is rejected without touching bad_range:
    // the coordinates were fine, the frame simply aged out.
    before = p.rx.stats.bad_range;
    TT_CHECK(!p.rx.mark_tile_undecodable(4242, 0, 0, 0));
    TT_EQ(p.rx.stats.bad_range, before);
    tt::end();
}

static void failure_modes() {
    tt::begin("failure modes: version, caps, tag, truncation, stale frame");
    Pair p;
    PoseHeader pose;
    p.tx.begin_frame(5, pose, 0, 0);
    auto dgs = p.tx.send_band(0, p.band_tiles(5, 0, TileClass::kC), 100, 10, false);
    TT_CHECK(!dgs.empty());
    std::vector<TileOutput> out;

    ByteVec bad = dgs[0].bytes;
    bad[0] = uint8_t((bad[0] & 0xF0) | 0x3);  // version 3
    TT_CHECK(!p.rx.on_datagram(std::span<const uint8_t>(bad.data(), bad.size()), 0, 1, &out));
    TT_EQ(p.rx.stats.bad_version, 1u);

    bad = dgs[0].bytes;
    bad[9] = 0xFF;  // caps the receiver did not negotiate
    TT_CHECK(!p.rx.on_datagram(std::span<const uint8_t>(bad.data(), bad.size()), 0, 1, &out));
    TT_EQ(p.rx.stats.bad_caps, 1u);

    bad = dgs[0].bytes;
    bad[bad.size() - 1] ^= 0x01;  // tag bit flip
    TT_CHECK(!p.rx.on_datagram(std::span<const uint8_t>(bad.data(), bad.size()), 0, 1, &out));
    TT_EQ(p.rx.stats.auth_fail, 1u);

    bad = dgs[0].bytes;
    bad.resize(bad.size() - 3);  // payload_len no longer matches
    TT_CHECK(!p.rx.on_datagram(std::span<const uint8_t>(bad.data(), bad.size()), 0, 1, &out));
    TT_CHECK(p.rx.stats.bad_range >= 1u);

    // Advance the ring past frame 5 and replay an old datagram.
    for (auto& d : dgs)
        p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()), 0, 1, &out);
    for (uint16_t f = 6; f <= 10; ++f) {
        p.tx.begin_frame(f, pose, 0, 0);
        auto nd = p.tx.send_band(0, p.band_tiles(f, 0, TileClass::kC), 100, 10, false);
        for (auto& d : nd)
            p.rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()), 0, 1,
                             &out);
    }
    uint64_t before = p.rx.stats.stale_frame;
    p.rx.on_datagram(std::span<const uint8_t>(dgs[0].bytes.data(), dgs[0].bytes.size()), 0,
                     1, &out);
    TT_CHECK(p.rx.stats.stale_frame > before || p.rx.stats.replay > 0);
    tt::end();
}

int main() {
    deadline_state_machine();
    deadline_relax_rule();
    deadline_climbs_on_late_tiles();
    placement_is_order_independent();
    duplicate_suppression();
    fec_recovery_in_the_live_path();
    concealment_and_feedback();
    mark_tile_undecodable_bounds();
    failure_modes();
    return tt::report("transport.receiver");
}
