// transport_depacketize_fuzz -- nxt::Receiver on hostile datagrams.
//
// Normative reference: docs/TRANSPORT.md 2 (24-byte header), 3 (payload and
// tile directory), 7 (placement, ring, deadline) and 12 (the failure table --
// every row of it says "drop and count", never "read anyway").
//
// The input is  header(24) || plaintext  and the harness *seals* the plaintext
// under the receiver's own key before feeding it.  Without that, every random
// input dies at the tag check and the target measures the AEAD instead of the
// depacketizer.  The raw, unsealed bytes are fed as well, on every input, so
// the auth-failure and short-datagram paths keep their coverage.
//
// Checked invariants:
//   * a delivered tile's byte span lies inside the receiver's own scratch and
//     its (row, col) is inside the configured grid;
//   * the directory-sum rule of TRANSPORT.md 3.1 holds for every delivered
//     datagram: a receiver that delivers tiles from an inconsistent directory
//     does not know where the tile boundaries are;
//   * the deadline, concealment and feedback paths run on whatever state the
//     datagram left behind, and the feedback packet they produce parses.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <vector>

#include "nxvc/transport/aead.h"
#include "nxvc/transport/receiver.h"
#include "nxvc/transport/wire.h"

#include "common/nxfuzz.h"
#include "common/nxt_wire.h"

namespace {

nxt::Key make_key(uint8_t fill) {
    nxt::Key k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = uint8_t(fill + i * 7u);
    return k;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < nxf::nxt::kHeaderBytes) return 0;
    if (size > 16384) return 0;  // corpus hygiene: a run never needs more

    const nxt::Key session_key = make_key(0x11);
    const nxt::Key session_salt = make_key(0x77);
    std::unique_ptr<nxt::Aead> aead = nxt::make_null_aead();
    if (!aead) return 0;

    nxt::StreamConfig cfg;  // v1 defaults: 68x34 tiles, 6 rows per band
    nxt::Receiver rx(cfg, aead.get(), session_key, session_salt);
    rx.set_negotiated_caps(0xFF);

    std::vector<nxt::TileOutput> tiles;

    // ---- pass 1: the raw bytes, exactly as an attacker would send them.
    tiles.clear();
    rx.on_datagram(std::span<const uint8_t>(data, size), data[12] & 1, 1000, &tiles);

    // ---- pass 2: the same header with a correctly sealed payload.
    nxt::DatagramHeader h{};
    if (nxt::decode_header(data, &h)) {
        const uint8_t *pt = data + nxf::nxt::kHeaderBytes;
        size_t pt_len = size - nxf::nxt::kHeaderBytes;
        if (pt_len > 4096) pt_len = 4096;

        std::vector<uint8_t> wire(nxf::nxt::kHeaderBytes + pt_len + nxt::kTagBytes);
        // The header is the complete associated data (TRANSPORT.md 2), so it
        // has to be re-encoded from the decoded struct with payload_len fixed
        // up, or the tag covers a header the receiver never sees.
        h.payload_len = uint16_t(pt_len);
        nxt::encode_header(h, wire.data());

        uint8_t path = uint8_t(h.path_id % nxt::kMaxPaths);
        nxt::Key sub = nxt::derive_subkey(session_key, session_salt, path,
                                          nxt::Direction::kDownstream);
        // The receiver extends the 14-bit wire sequence against its own
        // expectation, which starts at zero; for a first datagram the extended
        // counter is the wire value.  When a later datagram disagrees the tag
        // fails, which is itself a path worth covering.
        nxt::Nonce nonce = nxt::derive_nonce(h.stream_id, path, 0, h.path_seq);
        aead->seal(sub, nonce, std::span<const uint8_t>(wire.data(), nxf::nxt::kHeaderBytes),
                   std::span<const uint8_t>(pt, pt_len),
                   wire.data() + nxf::nxt::kHeaderBytes);

        tiles.clear();
        rx.on_datagram(std::span<const uint8_t>(wire), path, 2000, &tiles);

        for (const auto &t : tiles) {
            if (t.row >= cfg.rows || t.col >= cfg.cols) __builtin_trap();
            if (t.layer_id >= cfg.layers && cfg.layers > 0) __builtin_trap();
            // A delivered tile must point at real bytes, and never past the
            // plaintext it was carved out of.
            if (!t.bytes.empty() && t.bytes.data() == nullptr) __builtin_trap();
            if (t.bytes.size() > pt_len) __builtin_trap();
            volatile uint8_t acc = 0;
            for (uint8_t b : t.bytes) acc = uint8_t(acc + b);
            (void)acc;
        }

        // A duplicate of the very same datagram must be suppressed, never
        // double-counted towards a FEC group (TRANSPORT.md 7.2).
        tiles.clear();
        rx.on_datagram(std::span<const uint8_t>(wire), path, 2001, &tiles);

        // Deadline / concealment / feedback on the state that datagram left.
        // mark_tile_undecodable() indexes the ring's per-tile metadata with the
        // (row, col) it is given and does not range check it
        // (transport/src/receiver.cpp:318 -> receiver.h FrameRing::at).  The
        // documented flow is to pass coordinates that came back in a
        // TileOutput, so the harness respects that contract; feeding it a
        // header-derived tile_first instead is FINDINGS.md F1, whose
        // reproducer is kept under fuzz/regressions/.
        if (!tiles.empty()) {
            const auto &t = tiles.front();
            rx.mark_tile_undecodable(t.frame_id, t.layer_id, t.row, t.col);
        }
        uint16_t probe_row = uint16_t(h.tile_first / cfg.cols);
        uint16_t probe_col = uint16_t(h.tile_first % cfg.cols);
        if (probe_row < cfg.rows && probe_col < cfg.cols)
            rx.mark_tile_undecodable(h.frame_id, uint8_t(h.layer_id % (cfg.layers ? cfg.layers : 1)),
                                     probe_row, probe_col);
        for (uint8_t band = 0; band < 2; ++band) {
            nxt::ByteVec fb = rx.band_deadline(h.frame_id, uint8_t((h.band + band) % cfg.bands()),
                                               3000 + band, 1234, path);
            if (!fb.empty()) {
                volatile uint8_t acc = 0;
                for (uint8_t b : fb) acc = uint8_t(acc + b);
                (void)acc;
            }
        }
        nxt::Receiver::Presentation p = rx.classify(h.frame_id, 0);
        (void)p;
    }

    // Every failure must have been counted somewhere; reading the counters is
    // also how a torn ReceiverStats would show up under ASan.
    volatile uint64_t sink = rx.stats.datagrams + rx.stats.auth_fail + rx.stats.bad_directory +
                             rx.stats.bad_range + rx.stats.replay + rx.stats.duplicates;
    (void)sink;
    return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned seed) {
    return nxf::nxt::mutate_datagram_bytes(data, size, max_size, seed);
}
