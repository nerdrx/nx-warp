// transport_loopback.cpp -- a whole frame across a lossy link, in one process.
//
//   Packetize one frame's tiles into tile runs, throw some of the datagrams
//   away, depacketize what survived, run the band deadlines, and print the
//   resulting per-tile concealment map next to the sender's shadow of it.
//
// Run (synthetic tile sizes, no codec needed):
//   nxvc-example-loopback --cols 34 --rows 18 --loss 0.05
//
// Run against a real bitstream (when nxvc_ref is in the build):
//   nxvc-example-loopback --in out.nxv --loss 0.05 --burst 3 --paths 2
//
// There are no sockets here.  nxvc_transport is deliberately socket-free: the
// caller hands it datagram buffers and a clock (docs/TRANSPORT.md, preamble).
// "Sending" in this program is pushing a byte vector from one object into
// another, and "loss" is not pushing it.  That is exactly the seam the real
// integration has, which is why a loopback like this is a genuine test of the
// packetizer, the FEC and the deadline logic rather than a mock of them.
//
// What to look at in the output:
//
//   * `fresh` vs `concealed` in the presentation summary.  A 5 % datagram loss
//     should NOT give 5 % concealed tiles -- class A carries parity (PAPER 4.4)
//     and, with --paths 2, may be duplicated.  If it does, FEC is not working.
//   * the shadow map versus the receiver map.  They must agree tile for tile
//     once feedback has been applied.  PAPER.md 2.11 item 4 calls a divergence
//     here "a permanent artefact until the next refresh"; this program is the
//     smallest thing that can show one.
//   * the datagram count.  Tile-as-packet was rejected in PAPER 4.1 on exactly
//     this arithmetic; the "tiles per datagram" line is that decision measured.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "nxvc/transport/aead.h"
#include "nxvc/transport/receiver.h"
#include "nxvc/transport/sender.h"

#ifdef NXVC_EXAMPLE_HAVE_REF
#include <nxvc/nxvc.h>
#endif

namespace {

// One tile the sender is about to hand to the transport.
struct Tile {
    uint16_t row = 0, col = 0;
    nxt::TileClass cls = nxt::TileClass::kA;
    uint8_t qp = 28;
    uint8_t res_level = 0;
    nxt::TileMode mode = nxt::TileMode::kIntra;
    std::vector<uint8_t> bytes;
};

// Eccentricity classes as PAPER 4.2 / TRANSPORT.md 1 define them: A is the
// fovea, B the mid ring, C the periphery.  A real encoder gets these from the
// foveation map; a loopback can compute them from the grid.
nxt::TileClass class_of(uint16_t row, uint16_t col, uint16_t rows, uint16_t cols) {
    const double dy = (double(row) + 0.5) / rows - 0.5;
    const double dx = (double(col) + 0.5) / cols - 0.5;
    const double r = std::sqrt(dx * dx + dy * dy) * 2.0;  // 0 centre, ~1 corner
    if (r < 0.30) return nxt::TileClass::kA;
    if (r < 0.60) return nxt::TileClass::kB;
    return nxt::TileClass::kC;
}

char state_char(nxt::TileState s) {
    switch (s) {
        case nxt::TileState::kDecoded: return '#';
        case nxt::TileState::kConcealed: return 'c';
        case nxt::TileState::kUndecodable: return 'x';
        case nxt::TileState::kEmpty: return '.';
    }
    return '?';
}

void usage() {
    std::fprintf(stderr,
                 "usage: nxvc-example-loopback [--in FILE.nxv] [--cols N] [--rows N]\n"
                 "       [--loss F] [--burst N] [--paths 1|2] [--mtu N] [--seed N]\n"
                 "       [--no-fec] [--band-rows N]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string in_path;
    int cols = 34, rows = 18, band_rows = 6, paths = 1, burst = 1;
    double loss = 0.05;
    size_t mtu = nxt::kDefaultMtu;
    unsigned seed = 1;
    bool fec = true;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto take = [&](const char* n) { return a == n && v && ++i; };
        if (take("--in")) in_path = v;
        else if (take("--cols")) cols = std::atoi(v);
        else if (take("--rows")) rows = std::atoi(v);
        else if (take("--band-rows")) band_rows = std::atoi(v);
        else if (take("--paths")) paths = std::atoi(v);
        else if (take("--burst")) burst = std::atoi(v);
        else if (take("--mtu")) mtu = size_t(std::atoi(v));
        else if (take("--seed")) seed = unsigned(std::atoi(v));
        else if (take("--loss")) loss = std::atof(v);
        else if (a == "--no-fec") fec = false;
        else { usage(); return 2; }
    }
    if (paths < 1 || paths > nxt::kMaxPaths || cols < 1 || rows < 1 || band_rows < 1) {
        usage();
        return 2;
    }

    // ------------------------------------------------------------ the tiles
    std::vector<Tile> tiles;
    std::mt19937 rng(seed);

    if (!in_path.empty()) {
#ifdef NXVC_EXAMPLE_HAVE_REF
        // Real tile payload *lengths* from a real bitstream.  The transport is
        // codec agnostic -- a tile is an opaque blob -- so the loopback only
        // needs the sizes and the descriptors to be honest.  The bytes
        // themselves are filled with a per-tile pattern so a misplacement is
        // visible rather than silently plausible.
        std::vector<uint8_t> data;
        if (std::FILE* f = std::fopen(in_path.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            long n = std::ftell(f);
            std::rewind(f);
            if (n > 0) { data.resize(size_t(n)); if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear(); }
            std::fclose(f);
        }
        if (data.empty()) { std::fprintf(stderr, "cannot read %s\n", in_path.c_str()); return 1; }

        nxvc_status st{};
        nxvc_decoder* dec = nxvc_decoder_create(&st);
        size_t off = 0, consumed = 0;
        if (!dec || nxvc_decoder_parse_stream_header(dec, data.data(), data.size(),
                                                     &consumed) != NXVC_OK) {
            std::fprintf(stderr, "%s: not an nxv stream\n", in_path.c_str());
            return 1;
        }
        off += consumed;
        nxvc_stream_info si;
        nxvc_decoder_stream_info(dec, &si);
        nxvc_tile_layout tl;
        nxvc_tile_layout_get(si.width, si.height, &tl);
        cols = int(tl.tiles_x);
        rows = int(tl.tiles_y);

        std::vector<std::vector<uint8_t>> planes(si.alpha ? 4 : 3);
        nxvc_image img{};
        for (size_t p = 0; p < planes.size(); p++) {
            uint32_t pw = 0, ph = 0;
            nxvc_decoder_plane_size(dec, int(p), &pw, &ph);
            planes[p].resize(size_t(pw) * ph);
            img.plane[p] = planes[p].data();
            img.stride[p] = int32_t(pw);
        }
        if (nxvc_decoder_decode_frame(dec, data.data() + off, data.size() - off, &img,
                                      &consumed) != NXVC_OK) {
            std::fprintf(stderr, "%s: first frame does not decode\n", in_path.c_str());
            return 1;
        }
        uint32_t count = 0;
        const nxvc_tile_info* ti = nxvc_decoder_tiles(dec, &count);
        for (uint32_t i = 0; i < count; i++) {
            Tile t;
            t.row = uint16_t(ti[i].tile_index / tl.tiles_x);
            t.col = uint16_t(ti[i].tile_index % tl.tiles_x);
            t.cls = class_of(t.row, t.col, uint16_t(rows), uint16_t(cols));
            t.qp = ti[i].qp;
            t.res_level = ti[i].res_level;
            t.mode = nxt::TileMode(ti[i].mode);
            t.bytes.assign(ti[i].payload_len ? ti[i].payload_len : 1, uint8_t(i & 0xFF));
            tiles.push_back(std::move(t));
        }
        nxvc_decoder_destroy(dec);
        std::printf("loaded %u tiles from %s (%ux%u picture, %ux%u grid)\n", count,
                    in_path.c_str(), si.width, si.height, tl.tiles_x, tl.tiles_y);
#else
        std::fprintf(stderr,
                     "--in needs the reference codec; this build has no nxvc_ref. "
                     "Run without --in for synthetic tile sizes.\n");
        return 77;  // ctest's "skipped"
#endif
    } else {
        // Synthetic sizes with a fovea bias, so the class split is not a lie:
        // an A tile is roughly 4x a C tile, which is the shape foveation gives.
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                Tile t;
                t.row = uint16_t(r);
                t.col = uint16_t(c);
                t.cls = class_of(t.row, t.col, uint16_t(rows), uint16_t(cols));
                const int base = t.cls == nxt::TileClass::kA   ? 320
                                 : t.cls == nxt::TileClass::kB ? 160
                                                               : 80;
                std::uniform_int_distribution<int> jitter(-base / 3, base / 2);
                t.bytes.assign(size_t(std::max(8, base + jitter(rng))),
                               uint8_t((r * cols + c) & 0xFF));
                tiles.push_back(std::move(t));
            }
        std::printf("synthesised %zu tiles on a %dx%d grid\n", tiles.size(), cols, rows);
    }

    // ----------------------------------------------------------- the stream
    nxt::StreamConfig cfg;
    cfg.cols = uint16_t(cols);
    cfg.rows = uint16_t(rows);
    cfg.band_rows = uint16_t(band_rows);
    cfg.mtu = mtu;
    cfg.caps = nxt::kCapPoseHdr | nxt::kCapRleFeedback |
               (fec ? nxt::kCapFec : 0) |
               (paths > 1 ? nxt::kCapMultipath : 0);

    // The library never generates keys (TRANSPORT.md 4): the integration hands
    // it a session key and salt from the WiVRn NX handshake.  A loopback that
    // is not a link uses the NullAead, which is keyed and detects corruption
    // but is NOT cryptography.  Never ship this backend on a real path.
    auto aead = nxt::make_null_aead();
    nxt::Key key{}, salt{};
    for (size_t i = 0; i < key.size(); i++) { key[i] = uint8_t(i); salt[i] = uint8_t(0xA0 + i); }

    nxt::Sender sender(cfg, aead.get(), key, salt);
    nxt::Receiver receiver(cfg, aead.get(), key, salt);
    receiver.set_negotiated_caps(cfg.caps);

    // Oversize tiles cannot be carried without fragmentation; dropping them is
    // the honest loopback behaviour and the counter is printed at the end.
    sender.packetizer().set_policy(nxt::Packetizer::OversizePolicy::kDropTile);
    for (int p = 0; p < paths; p++)
        sender.striper().configure_path(uint8_t(p), 150e6 / paths, 8000);

    nxt::PoseHeader pose{};
    pose.pose_seq = 1;
    pose.quat[3] = 32767;  // identity rotation in Q15
    const uint16_t frame_id = 1;
    sender.begin_frame(frame_id, pose, /*render_finish_us=*/0,
                       /*frame_bit_budget=*/150000000u / 90u);

    // ----------------------------------------------------- send, drop, receive
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    int in_burst = 0;
    size_t sent = 0, dropped = 0, wire_bytes = 0;
    std::vector<nxt::TileOutput> delivered;
    size_t delivered_tiles = 0;
    uint64_t now_us = 0;

    const uint8_t nbands = cfg.bands();
    for (uint8_t band = 0; band < nbands; band++) {
        // Tiles of one band, ordered (layer, row, col) as packetize_band requires.
        std::vector<nxt::TileInput> band_tiles;
        for (const Tile& t : tiles) {
            if (cfg.band_of_row(t.row) != band) continue;
            nxt::TileInput ti;
            ti.frame_id = frame_id;
            ti.layer_id = 0;
            ti.row = t.row;
            ti.col = t.col;
            ti.cls = t.cls;
            ti.ref_delta = nxt::kRefIntra;  // Phase 1: every tile is intra
            ti.qp = t.qp;
            ti.mode = t.mode;
            ti.res_level = t.res_level;
            ti.bytes = std::span<const uint8_t>(t.bytes.data(), t.bytes.size());
            band_tiles.push_back(ti);
        }
        std::sort(band_tiles.begin(), band_tiles.end(),
                  [](const nxt::TileInput& a, const nxt::TileInput& b) {
                      if (a.layer_id != b.layer_id) return a.layer_id < b.layer_id;
                      if (a.row != b.row) return a.row < b.row;
                      return a.col < b.col;
                  });

        now_us += cfg.frame_period_us / nbands;
        auto dgrams = sender.send_band(band, band_tiles, uint32_t(now_us),
                                       uint16_t(cfg.frame_period_us / nbands),
                                       band + 1 == nbands);

        for (auto& d : dgrams) {
            sent++;
            wire_bytes += d.bytes.size();
            // Loss model: Bernoulli with a burst extension, because real WiFi
            // loss is correlated and an independent model flatters FEC.
            bool drop;
            if (in_burst > 0) { drop = true; in_burst--; }
            else if (coin(rng) < loss) { drop = true; in_burst = burst - 1; }
            else drop = false;
            if (drop) { dropped++; continue; }

            delivered.clear();
            receiver.on_datagram(d.bytes, d.path_id, now_us + 4000, &delivered);
            delivered_tiles += delivered.size();
        }

        // The band deadline is what turns "not arrived" into "concealed" and
        // produces the feedback the sender's shadow is driven by.
        auto fb = receiver.band_deadline(frame_id, band, now_us + 6000,
                                         /*decode_us=*/500, /*path_id=*/0);
        if (!fb.empty()) sender.on_feedback(fb, 0, now_us + 9000);
    }

    // --------------------------------------------------------------- report
    const auto pres = receiver.classify(frame_id, 0);
    const uint32_t total = cfg.tiles_per_frame();

    std::printf("\nlink        %d path(s), %.1f%% datagram loss, burst %d, mtu %zu, "
                "FEC %s\n",
                paths, loss * 100.0, burst, mtu, fec ? "on" : "off");
    std::printf("sender      %llu datagrams (%llu data, %llu parity), %zu bytes on "
                "the wire\n",
                (unsigned long long)sender.stats.datagrams,
                (unsigned long long)sender.stats.data_datagrams,
                (unsigned long long)sender.stats.parity_datagrams, wire_bytes);
    std::printf("            %llu tiles in %llu runs = %.1f tiles/datagram "
                "(PAPER 4.1)\n",
                (unsigned long long)sender.stats.tiles,
                (unsigned long long)sender.stats.runs,
                sender.stats.runs ? double(sender.stats.tiles) / double(sender.stats.runs)
                                  : 0.0);
    if (sender.stats.oversize_tiles)
        std::printf("            %llu tile(s) too large for the MTU and dropped "
                    "(raise --mtu or lower QP)\n",
                    (unsigned long long)sender.stats.oversize_tiles);
    std::printf("            overhead: %llu header + %llu directory + %llu tag + "
                "%llu parity bytes on %llu tile bytes\n",
                (unsigned long long)sender.stats.header_bytes,
                (unsigned long long)sender.stats.dir_bytes,
                (unsigned long long)sender.stats.tag_bytes,
                (unsigned long long)sender.stats.parity_bytes,
                (unsigned long long)sender.stats.tile_bytes);
    std::printf("channel     %zu sent, %zu dropped (%.1f%%)\n", sent, dropped,
                sent ? 100.0 * double(dropped) / double(sent) : 0.0);
    std::printf("receiver    %zu tiles delivered, %llu recovered by FEC "
                "(%llu group(s), %llu unrecoverable)\n",
                delivered_tiles, (unsigned long long)receiver.stats.fec_recovered,
                (unsigned long long)receiver.stats.fec_groups,
                (unsigned long long)receiver.stats.fec_failed);
    std::printf("presentation fresh %u  stale %u  concealed %u  undecodable %u  "
                "empty %u  of %u  -> frame is %s\n",
                pres.fresh, pres.stale, pres.concealed, pres.undecodable, pres.empty,
                total, pres.partial() ? "PARTIAL" : "complete");

    // The map.  '#' decoded, 'c' concealed by the deterministic warp, 'x' the
    // decoder rejected it, '.' never arrived and the deadline has not passed.
    std::printf("\nreceiver tile map  ('#' decoded  'c' concealed  'x' undecodable"
                "  '.' empty)\n");
    if (const nxt::FrameRing::Slot* slot = receiver.ring().find(frame_id)) {
        for (uint16_t r = 0; r < cfg.rows; r++) {
            std::printf("  %s|", cfg.band_of_row(r) % 2 ? " " : ">");
            for (uint16_t c = 0; c < cfg.cols; c++)
                std::putchar(state_char(slot->meta[cfg.tile_index(r, c)].state));
            std::printf("|\n");
        }
    } else {
        std::printf("  (frame is no longer in the 4-slot ring)\n");
    }

    // And the sender's belief about the same tiles.  These two maps disagreeing
    // is the Phase 2 kill condition of PAPER.md 2.11 item 4.
    std::printf("\nsender shadow map  ('#' believed received  'c' believed concealed"
                "  '?' no feedback yet)\n");
    size_t divergent = 0;
    for (uint16_t r = 0; r < cfg.rows; r++) {
        std::printf("   |");
        for (uint16_t c = 0; c < cfg.cols; c++) {
            const nxt::ShadowState s = sender.shadow().state(frame_id, r, c);
            std::putchar(s == nxt::ShadowState::kReceived    ? '#'
                         : s == nxt::ShadowState::kConcealed ? 'c'
                                                             : '?');
            if (const nxt::FrameRing::Slot* slot = receiver.ring().find(frame_id)) {
                const auto rs = slot->meta[cfg.tile_index(r, c)].state;
                const bool rx_ok = rs == nxt::TileState::kDecoded;
                if (s == nxt::ShadowState::kReceived && !rx_ok) divergent++;
                if (s == nxt::ShadowState::kConcealed && rx_ok) divergent++;
            }
        }
        std::printf("|\n");
    }
    std::printf("\nshadow divergence: %zu tile(s)%s\n", divergent,
                divergent ? "  <-- BUG: the encoder would predict from pixels the "
                            "client does not have"
                          : "  (encoder and client agree)");
    return divergent ? 1 : 0;
}
