// Packetizer size and structure invariants, oversize policy hooks, and the
// sender's end-to-end datagram size discipline.  TRANSPORT.md 3, 5.
#include <algorithm>

#include "nxvc/transport/packetizer.h"
#include "nxvc/transport/sender.h"
#include "test_util.h"

using namespace nxt;

namespace {

StreamConfig small_cfg() {
    StreamConfig c;
    c.cols = 68;
    c.rows = 34;
    c.band_rows = 6;
    c.layers = 1;
    c.caps = kCapFec | kCapPoseHdr | kCapRleFeedback;
    return c;
}

struct Band {
    std::vector<TileInput> tiles;
    std::vector<ByteVec> store;
};

// `vary_class`: 0 = one class, 1 = random per tile (worst case for run packing),
// 2 = contiguous column regions, which is what a foveation map actually looks like.
Band make_band(const StreamConfig& c, uint8_t band, tt::Rng& r, size_t mean_bytes,
               int vary_class, bool vary_ref) {
    Band b;
    uint16_t r0 = c.first_row_of_band(band);
    for (uint16_t row = r0; row < r0 + c.rows_in_band(band); ++row)
        for (uint16_t col = 0; col < c.cols; ++col) {
            TileInput t;
            t.row = row;
            t.col = col;
            t.cls = vary_class == 0   ? TileClass::kB
                    : vary_class == 1 ? TileClass(r.u32(3))
                                      : TileClass(col < c.cols / 3        ? 0
                                                  : col < 2 * c.cols / 3 ? 1
                                                                         : 2);
            t.ref_delta = vary_ref ? uint8_t(r.u32(4)) : 0;
            t.qp = uint8_t(r.u32(64));
            t.mode = TileMode(r.u32(5));
            size_t n = 1 + r.u32(uint32_t(mean_bytes * 2));
            b.store.emplace_back(n, uint8_t(r.u32(256)));
            b.tiles.push_back(t);
        }
    for (size_t i = 0; i < b.tiles.size(); ++i) b.tiles[i].bytes = b.store[i];
    return b;
}

void check_units(const StreamConfig& c, const std::vector<SendUnit>& units) {
    const size_t budget = c.run_payload_budget();
    for (const SendUnit& u : units) {
        TT_CHECK(u.data.size() <= size_t(kFecMaxK));
        for (const PendingDatagram& d : u.data) {
            // Size invariant: the whole datagram fits the MTU with room for the
            // parity datagram's own overhead.
            TT_CHECK(d.plaintext.size() <= budget);
            TT_CHECK(kHeaderBytes + d.plaintext.size() + kTagBytes <= c.mtu);
            TT_CHECK(d.hdr.tile_count >= 1);
            TT_EQ(int(d.hdr.band), int(u.band));
            TT_EQ(int(d.hdr.fec_class), int(uint8_t(u.cls)));
            TT_EQ(int(d.hdr.layer_id), int(u.layer));
            // Directory consistency: the lengths sum to the plaintext exactly.
            size_t off = d.hdr.pose_hdr ? kPoseHeaderBytes : 0;
            size_t dir = size_t(d.hdr.tile_count) * kDirEntryBytes;
            TT_CHECK(d.plaintext.size() >= off + dir);
            size_t sum = 0;
            for (uint32_t i = 0; i < d.hdr.tile_count; ++i)
                sum += unpack_dir_entry(
                           rd32(d.plaintext.data() + off + i * kDirEntryBytes))
                           .len;
            TT_EQ(off + dir + sum, d.plaintext.size());
            // Run homogeneity: a run never crosses a tile row.
            uint32_t first = d.hdr.tile_first;
            uint32_t last = first + d.hdr.tile_count - 1;
            TT_EQ(int(c.row_of(first)), int(c.row_of(last)));
        }
        if (u.m > 0) {
            for (size_t i = 0; i < u.data.size(); ++i) {
                TT_EQ(int(u.data[i].hdr.fec_idx), int(i));
                TT_EQ(int(u.data[i].hdr.fec_k), int(u.data.size()));
                TT_EQ(int(u.data[i].hdr.fec_group), int(u.group));
            }
        }
    }
}

}  // namespace

static void size_invariants() {
    tt::begin("packetizer size and structure invariants");
    StreamConfig c = small_cfg();
    tt::Rng r(4242);
    for (size_t mean : {8u, 40u, 90u, 400u, 650u}) {
        Packetizer p(c);
        for (uint8_t band = 0; band < c.bands(); ++band) {
            Band b = make_band(c, band, r, mean, 1, true);
            PoseHeader pose;
            FrameContext ctx;
            ctx.frame_id = 5;
            ctx.pose = &pose;
            std::vector<SendUnit> units;
            TT_CHECK(p.packetize_band(band, b.tiles, ctx, &units) ==
                     Packetizer::Status::kOk);
            check_units(c, units);
            // Exactly one pose header per band.
            int poses = 0;
            for (const SendUnit& u : units)
                for (const PendingDatagram& d : u.data) poses += d.hdr.pose_hdr ? 1 : 0;
            TT_EQ(poses, 1);
            // Every tile of the band is carried exactly once.
            size_t carried = 0;
            for (const SendUnit& u : units)
                for (const PendingDatagram& d : u.data) carried += d.hdr.tile_count;
            TT_EQ(carried, b.tiles.size());
        }
    }
    tt::end();
}

static void runs_are_homogeneous() {
    tt::begin("runs are homogeneous in layer and row; class/ref per tile");
    StreamConfig c = small_cfg();
    tt::Rng r(7);
    Packetizer p(c);
    Band b = make_band(c, 0, r, 60, 1, true);
    PoseHeader pose;
    FrameContext ctx;
    ctx.pose = &pose;
    std::vector<SendUnit> units;
    TT_CHECK(p.packetize_band(0, b.tiles, ctx, &units) == Packetizer::Status::kOk);
    for (const SendUnit& u : units)
        for (const PendingDatagram& d : u.data) {
            uint32_t first = d.hdr.tile_first;
            size_t off = d.hdr.pose_hdr ? kPoseHeaderBytes : 0;
            uint8_t best = 3;
            for (uint32_t i = 0; i < d.hdr.tile_count; ++i) {
                const TileInput& t = b.tiles[first + i];
                TileDirEntry e = unpack_dir_entry(
                    rd32(d.plaintext.data() + off + i * kDirEntryBytes));
                // v2: the per-tile class and reference live in the directory.
                TT_EQ(int(e.ref_delta), int(t.ref_delta));
                TT_EQ(int(e.tile_class), int(uint8_t(t.cls)));
                TT_EQ(int(t.layer_id), int(d.hdr.layer_id));
                if (uint8_t(t.cls) < best) best = uint8_t(t.cls);
            }
            // The datagram takes the strongest protection class it carries.
            TT_EQ(int(d.hdr.fec_class), int(best));
        }
    tt::end();
}

// v2: with the class out of the run key, a run of average tiles must fill the
// payload budget rather than stopping at a foveation boundary.
static void runs_pack_to_the_mtu() {
    tt::begin("runs pack to the MTU (v2): 90-byte tiles give >= 13 per run");
    StreamConfig c = small_cfg();
    tt::Rng r(1234);
    Packetizer p(c);
    size_t tiles = 0, runs = 0;
    for (uint8_t band = 0; band < c.bands(); ++band) {
        Band b = make_band(c, band, r, 45, 2, true);  // contiguous class regions
        PoseHeader pose;
        FrameContext ctx;
        ctx.pose = &pose;
        std::vector<SendUnit> units;
        TT_CHECK(p.packetize_band(band, b.tiles, ctx, &units) == Packetizer::Status::kOk);
        for (const SendUnit& u : units)
            for (const PendingDatagram& d : u.data) {
                tiles += d.hdr.tile_count;
                ++runs;
            }
    }
    double per_run = double(tiles) / double(runs);
    TT_CHECK(per_run >= 13.0);
    tt::end();
}

static void oversize_policies() {
    tt::begin("oversize policy: reject, drop, fragment");
    StreamConfig c = small_cfg();
    ByteVec big(c.max_tile_bytes() + 200, 0xAB);
    std::vector<ByteVec> store;
    std::vector<TileInput> tiles;
    for (uint16_t col = 0; col < 4; ++col) {
        TileInput t;
        t.row = 0;
        t.col = col;
        t.cls = TileClass::kA;
        t.ref_delta = 0;
        store.emplace_back(50, 1);
        tiles.push_back(t);
    }
    tiles[2].bytes = big;
    for (size_t i = 0; i < tiles.size(); ++i)
        if (i != 2) tiles[i].bytes = store[i];
    PoseHeader pose;
    FrameContext ctx;
    ctx.pose = &pose;

    {
        Packetizer p(c, Packetizer::OversizePolicy::kReject);
        std::vector<SendUnit> u;
        TT_CHECK(p.packetize_band(0, tiles, ctx, &u) == Packetizer::Status::kOversizeTile);
    }
    {
        Packetizer p(c, Packetizer::OversizePolicy::kDropTile);
        std::vector<SendUnit> u;
        TT_CHECK(p.packetize_band(0, tiles, ctx, &u) == Packetizer::Status::kOk);
        check_units(c, u);
        size_t carried = 0;
        bool found_zero = false;
        for (const SendUnit& su : u)
            for (const PendingDatagram& d : su.data) {
                carried += d.hdr.tile_count;
                size_t off = d.hdr.pose_hdr ? kPoseHeaderBytes : 0;
                for (uint32_t i = 0; i < d.hdr.tile_count; ++i) {
                    TileDirEntry e = unpack_dir_entry(
                        rd32(d.plaintext.data() + off + i * kDirEntryBytes));
                    if (e.len == 0) {
                        found_zero = true;
                        TT_EQ(int(e.mode), int(uint8_t(TileMode::kWarpSkip)));
                    }
                }
            }
        TT_EQ(carried, tiles.size());
        TT_CHECK(found_zero);
        TT_EQ(p.oversize_tiles(), size_t(1));
    }
    {
        StreamConfig cf = c;
        cf.caps |= kCapFragment;
        tiles[2].lossless = true;
        Packetizer p(cf, Packetizer::OversizePolicy::kFragment);
        std::vector<SendUnit> u;
        TT_CHECK(p.packetize_band(0, tiles, ctx, &u) == Packetizer::Status::kOk);
        check_units(cf, u);
        int frags = 0;
        for (const SendUnit& su : u)
            for (const PendingDatagram& d : su.data)
                if (d.hdr.frag_count > 0) {
                    ++frags;
                    TT_CHECK(d.hdr.flags & kFlagLossless);
                }
        TT_EQ(frags, 2);
    }
    tt::end();
}

static void sender_datagrams_fit_mtu() {
    tt::begin("sender: every datagram on the wire fits the MTU");
    StreamConfig c = small_cfg();
    auto aead = make_null_aead();
    Key k{}, salt{};
    Sender tx(c, aead.get(), k, salt);
    tt::Rng r(31337);
    PoseHeader pose;
    tx.begin_frame(1, pose, 0, 0);
    size_t total = 0;
    for (uint8_t band = 0; band < c.bands(); ++band) {
        Band b = make_band(c, band, r, 120, 2, false);
        auto dgs = tx.send_band(band, b.tiles, 1000, 500, band + 1 == c.bands());
        TT_CHECK(!dgs.empty());
        for (const Datagram& d : dgs) {
            TT_CHECK(d.bytes.size() <= c.mtu);
            TT_CHECK(d.bytes.size() >= kHeaderBytes + kTagBytes);
            DatagramHeader h;
            TT_CHECK(decode_header(d.bytes.data(), &h));
            TT_EQ(size_t(h.payload_len) + kHeaderBytes + kTagBytes, d.bytes.size());
            TT_EQ(int(h.caps), int(c.caps));
            total += d.bytes.size();
        }
    }
    TT_CHECK(total > 0);
    // The last data datagram of the frame carries LAST_RUN_OF_FRAME.
    tt::end();
}

static void budget_arithmetic() {
    tt::begin("budget arithmetic matches TRANSPORT.md 5");
    StreamConfig c = small_cfg();
    TT_EQ(c.run_payload_budget(), size_t(1316));
    TT_EQ(c.max_tile_bytes(), size_t(1312));
    StreamConfig nofec = c;
    nofec.caps &= uint8_t(~kCapFec);
    TT_EQ(nofec.run_payload_budget(), size_t(1360));
    TT_EQ(nofec.max_tile_bytes(), size_t(1356));
    // A parity datagram over the largest legal data datagram still fits.
    TT_CHECK(kHeaderBytes + 2 + (kHeaderBytes + 1316 + kTagBytes + 2) + kTagBytes <= c.mtu);
    tt::end();
}

int main() {
    budget_arithmetic();
    size_invariants();
    runs_are_homogeneous();
    runs_pack_to_the_mtu();
    oversize_policies();
    sender_datagrams_fit_mtu();
    return tt::report("transport.packetizer");
}
