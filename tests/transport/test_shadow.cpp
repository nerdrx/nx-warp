// Reference eligibility (TRANSPORT.md 9) and the shadow-equivalence property:
// under any loss pattern the sender's per-tile knowledge of a band it has
// feedback for equals the client's real reference state.
#include <algorithm>

#include "nxvc/transport/receiver.h"
#include "nxvc/transport/sender.h"
#include "nxvc/transport/shadow.h"
#include "test_util.h"

using namespace nxt;

namespace {

StreamConfig cfg8() {
    StreamConfig c;
    c.cols = 8;
    c.rows = 6;
    c.band_rows = 2;  // 3 bands of 16 tiles
    c.layers = 1;
    c.caps = kCapFec | kCapPoseHdr | kCapRleFeedback;
    return c;
}

// A feedback packet reporting one band with an explicit bitmap.
FeedbackPacket one_band(const StreamConfig& c, uint16_t frame, uint8_t band,
                        const std::vector<uint8_t>& received) {
    FeedbackPacket fb;
    fb.stream_id = c.stream_id;
    fb.tiles_in_band = uint16_t(c.tiles_in_band(0));
    BandReport br;
    br.frame_id = frame;
    br.band = band;
    br.received = received;
    br.received.resize(fb.tiles_in_band, 1);
    fb.bands.push_back(std::move(br));
    return fb;
}

}  // namespace

static void reference_rule() {
    tt::begin("reference eligibility: newest exact 3x3 neighbourhood");
    StreamConfig c = cfg8();
    ClientShadow sh(c);

    // No feedback at all: everything is intra.
    sh.begin_frame(0);
    TT_EQ(int(sh.reference_choice(0, 2, 3)), int(kRefIntra));

    // Frames 0..3 fully received.
    for (uint16_t f = 0; f <= 3; ++f) {
        sh.begin_frame(f);
        for (uint8_t b = 0; b < c.bands(); ++b)
            sh.apply_feedback(one_band(c, f, b, std::vector<uint8_t>(c.tiles_in_band(b), 1)));
    }
    sh.begin_frame(4);
    TT_EQ(int(sh.reference_choice(4, 2, 3)), 0);  // N-1 == frame 3

    // Frame 3 loses the tile at (2,3) itself: its own 3x3 in frame 3 is not
    // exact unless the concealment source in frame 2 is exact, which it is,
    // so the concealed tile IS exact and N-1 stays usable.
    sh.begin_frame(3);
    {
        std::vector<uint8_t> bm(c.tiles_in_band(1), 1);
        // band 1 covers rows 2..3; tile (2,3) is index 3 in the band.
        bm[3] = 0;
        sh.apply_feedback(one_band(c, 3, 1, bm));
        for (uint8_t b : {0, 2})
            sh.apply_feedback(one_band(c, 3, b, std::vector<uint8_t>(c.tiles_in_band(b), 1)));
    }
    sh.begin_frame(4);
    TT_EQ(int(sh.reference_choice(4, 2, 3)), 0);

    // Now a frame whose band 1 has no feedback at all: UNKNOWN blocks N-1, so
    // the choice falls back to N-2.
    ClientShadow s2(c);
    for (uint16_t f = 0; f <= 2; ++f) {
        s2.begin_frame(f);
        for (uint8_t b = 0; b < c.bands(); ++b)
            s2.apply_feedback(one_band(c, f, b, std::vector<uint8_t>(c.tiles_in_band(b), 1)));
    }
    s2.begin_frame(3);
    for (uint8_t b : {0, 2})
        s2.apply_feedback(one_band(c, 3, b, std::vector<uint8_t>(c.tiles_in_band(b), 1)));
    s2.begin_frame(4);
    TT_EQ(int(s2.reference_choice(4, 2, 3)), 1);   // band 1 unknown in frame 3
    TT_EQ(int(s2.reference_choice(4, 0, 3)), 0);   // band 0 is fine

    // A tile whose 3x3 straddles the unknown band is also pushed to N-2.
    TT_EQ(int(s2.reference_choice(4, 1, 3)), 1);   // row 1 touches row 2

    // Grid edges clip the neighbourhood rather than failing.
    TT_EQ(int(s2.reference_choice(4, 0, 0)), 0);
    tt::end();
}

static void recursive_exactness() {
    tt::begin("a concealed tile is exact only if its source is exact");
    StreamConfig c = cfg8();
    ClientShadow sh(c);
    // Frame 0: band 1 lost entirely, with no earlier frame to conceal from.
    sh.begin_frame(0);
    for (uint8_t b = 0; b < c.bands(); ++b) {
        std::vector<uint8_t> bm(c.tiles_in_band(b), b == 1 ? 0 : 1);
        sh.apply_feedback(one_band(c, 0, b, bm));
    }
    TT_CHECK(!sh.exact(0, 2, 3));   // concealed from a frame that does not exist
    TT_CHECK(sh.exact(0, 0, 3));

    // Frame 1 receives everything: exact everywhere.
    sh.begin_frame(1);
    for (uint8_t b = 0; b < c.bands(); ++b)
        sh.apply_feedback(one_band(c, 1, b, std::vector<uint8_t>(c.tiles_in_band(b), 1)));
    TT_CHECK(sh.exact(1, 2, 3));

    // Frame 2 loses (2,3): concealed from frame 1, whose neighbourhood is exact.
    sh.begin_frame(2);
    for (uint8_t b = 0; b < c.bands(); ++b) {
        std::vector<uint8_t> bm(c.tiles_in_band(b), 1);
        if (b == 1) bm[3] = 0;
        sh.apply_feedback(one_band(c, 2, b, bm));
    }
    TT_CHECK(sh.exact(2, 2, 3));

    // Frame 3 loses the same tile again; the source in frame 2 is exact, so it
    // stays exact.  Losing a whole 3x3 source region does break it.
    sh.begin_frame(3);
    for (uint8_t b = 0; b < c.bands(); ++b) {
        std::vector<uint8_t> bm(c.tiles_in_band(b), 1);
        if (b == 1) bm[3] = 0;
        sh.apply_feedback(one_band(c, 3, b, bm));
    }
    TT_CHECK(sh.exact(3, 2, 3));
    tt::end();
}

static void staleness_and_no_feedback() {
    tt::begin("no feedback for four frames drives every tile intra");
    StreamConfig c = cfg8();
    ClientShadow sh(c);
    for (uint16_t f = 0; f < 4; ++f) {
        sh.begin_frame(f);
        for (uint8_t b = 0; b < c.bands(); ++b)
            sh.apply_feedback(one_band(c, f, b, std::vector<uint8_t>(c.tiles_in_band(b), 1)));
    }
    for (uint16_t f = 4; f < 8; ++f) sh.begin_frame(f);  // silence
    TT_EQ(int(sh.reference_choice(8, 2, 3)), int(kRefIntra));
    TT_EQ(int(sh.staleness(7, 2, 3)), 4);
    tt::end();
}

// ---------------------------------------------------------------- equivalence
static void equivalence_fuzz() {
    tt::begin("shadow == receiver state under fuzzed loss (200 seeds)");
    StreamConfig c = cfg8();
    auto aead = make_null_aead();
    Key key{}, salt{};
    for (size_t i = 0; i < kKeyBytes; ++i) { key[i] = uint8_t(i + 3); salt[i] = uint8_t(i * 5); }

    for (int seed = 1; seed <= 200; ++seed) {
        tt::Rng r(uint64_t(seed) * 2654435761u + 1);
        Sender tx(c, aead.get(), key, salt);
        Receiver rx(c, aead.get(), key, salt);
        double loss = r.u01() * 0.6;
        bool lose_feedback = (seed % 3) == 0;
        bool late_delivery = (seed % 5) == 0;

        // truth[frame][tile] = usable as a reference on the client
        std::vector<std::vector<uint8_t>> truth(16,
                                                std::vector<uint8_t>(c.tiles_per_frame(), 0));
        std::vector<std::vector<uint8_t>> known(16,
                                                std::vector<uint8_t>(c.tiles_per_frame(), 0));
        std::vector<ByteVec> pool;
        for (int i = 0; i < 32; ++i) pool.emplace_back(1 + r.u32(60), uint8_t(i));

        uint64_t now = 0;
        for (uint16_t f = 0; f < 24; ++f) {
            PoseHeader pose;
            pose.pose_seq = f;
            tx.begin_frame(f, pose, uint32_t(now), 0);
            auto& t_now = truth[f % 16];
            auto& k_now = known[f % 16];
            std::fill(t_now.begin(), t_now.end(), uint8_t(0));
            std::fill(k_now.begin(), k_now.end(), uint8_t(0));

            for (uint8_t b = 0; b < c.bands(); ++b) {
                std::vector<TileInput> tiles;
                uint16_t r0 = c.first_row_of_band(b);
                for (uint16_t row = r0; row < r0 + c.rows_in_band(b); ++row)
                    for (uint16_t col = 0; col < c.cols; ++col) {
                        TileInput t;
                        t.frame_id = f;
                        t.row = row;
                        t.col = col;
                        t.cls = TileClass(r.u32(3));
                        t.ref_delta = tx.reference_choice(f, row, col);
                        t.mode = t.ref_delta == kRefIntra ? TileMode::kIntra : TileMode::kWarpMv;
                        t.bytes = pool[r.u32(uint32_t(pool.size()))];
                        tiles.push_back(t);
                    }
                now += 500;
                auto dgs = tx.send_band(b, tiles, uint32_t(now), 100, b + 1 == c.bands());

                std::vector<Datagram> deferred;
                std::vector<TileOutput> out;
                for (Datagram& d : dgs) {
                    if (r.u01() < loss) continue;
                    if (late_delivery && r.u01() < 0.1) { deferred.push_back(std::move(d)); continue; }
                    out.clear();
                    rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                                   d.path_id, now, &out);
                    for (const TileOutput& t : out)
                        if (!t.late) truth[t.frame_id % 16][c.tile_index(t.row, t.col)] = 1;
                }
                now += 100;
                ByteVec fb = rx.band_deadline(f, b, now, 50, 0);
                for (uint16_t row = r0; row < r0 + c.rows_in_band(b); ++row)
                    for (uint16_t col = 0; col < c.cols; ++col)
                        k_now[c.tile_index(row, col)] = 1;
                // Tiles arriving after the deadline: decoded for display, never
                // acknowledged (TRANSPORT.md D17).
                for (Datagram& d : deferred) {
                    out.clear();
                    rx.on_datagram(std::span<const uint8_t>(d.bytes.data(), d.bytes.size()),
                                   d.path_id, now, &out);
                    for (const TileOutput& t : out)
                        if (!t.late) truth[t.frame_id % 16][c.tile_index(t.row, t.col)] = 1;
                }
                if (!fb.empty() && !(lose_feedback && r.u01() < 0.3))
                    tx.on_feedback(std::span<const uint8_t>(fb.data(), fb.size()), 0, now);
            }

            // Check the frame that is now safely out of the feedback window.
            if (f >= 3) {
                uint16_t chk = uint16_t(f - 2);
                const auto& tr = truth[chk % 16];
                const auto& kn = known[chk % 16];
                for (uint32_t t = 0; t < c.tiles_per_frame(); ++t) {
                    if (!kn[t]) continue;
                    ShadowState s = tx.shadow().state(chk, c.row_of(t), c.col_of(t));
                    if (s == ShadowState::kUnknown) continue;  // feedback was lost
                    TT_EQ(int(s == ShadowState::kReceived), int(tr[t] != 0));
                }
            }
        }
    }
    tt::end();
}

int main() {
    reference_rule();
    recursive_exactness();
    staleness_and_no_feedback();
    equivalence_fuzz();
    return tt::report("transport.shadow");
}
