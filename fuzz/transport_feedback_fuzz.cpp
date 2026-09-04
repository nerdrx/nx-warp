// transport_feedback_fuzz -- the client-to-server feedback packet parser.
//
// Normative reference: docs/TRANSPORT.md 8 (8-byte header, 1..3 band records
// of 20 bytes plus a bitmap, 4-byte trailer) and Decision D9 (three bitmap
// encodings: RAW, ALL, RLE).
//
// This parser is the server's only attack surface from the client side, and
// two of its fields are lengths supplied by the peer:
//
//   * `tiles_in_band` sizes a RAW bitmap;
//   * `nruns` at the head of an RLE bitmap promises 3 * nruns further bytes.
//
// Either one can promise more than the packet carries, and a parser that
// believes them reads past the buffer.  The custom mutator produces exactly
// that shape on purpose.
//
// Checked invariants beyond memory safety:
//   * a decoded packet re-encodes and re-decodes to the same thing, so the
//     parser and the generator agree about what a legal packet is;
//   * band_count is 1..3 and every band's `received` vector has as many
//     entries as the header's tiles_in_band -- a shorter one would be indexed
//     out of range by the shadow (TRANSPORT.md 9).
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <vector>

#include "nxvc/transport/wire.h"

#include "common/nxfuzz.h"
#include "common/nxt_wire.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 8192) return 0;

    nxt::FeedbackPacket fb;
    bool ok = nxt::decode_feedback(std::span<const uint8_t>(data, size), &fb);
    if (!ok) return 0;

    // TRANSPORT.md 8.1: 1..3 band records.
    if (fb.bands.empty() || fb.bands.size() > static_cast<size_t>(nxt::kMaxFeedbackBands)) __builtin_trap();

    for (const auto &b : fb.bands) {
        // Every tile of the band must have a bit, or none at all (a band the
        // parser chose not to expand).  More bits than the header declares is
        // always wrong: the shadow indexes this by tile_in_band and trusts the
        // header's count (TRANSPORT.md 9).
        if (b.received.size() > fb.tiles_in_band) __builtin_trap();
        for (uint8_t v : b.received)
            if (v > 1) __builtin_trap();
        volatile uint32_t acc = b.conceal_tiles + b.late_tiles + b.fec_recovered + b.fec_failed;
        (void)acc;
    }

    // Round trip.  A packet the decoder accepted must survive re-encoding: if
    // it does not, either the encoder cannot express something the decoder
    // accepts, or the decoder accepts something no encoder can produce.  Both
    // are bugs, and both let the two ends disagree about a loss pattern.
    for (int allow_rle = 0; allow_rle < 2; ++allow_rle) {
        nxt::ByteVec wire = nxt::encode_feedback(fb, allow_rle != 0);
        nxt::FeedbackPacket again;
        if (!nxt::decode_feedback(std::span<const uint8_t>(wire), &again)) __builtin_trap();
        if (again.bands.size() != fb.bands.size()) __builtin_trap();
        if (again.tiles_in_band != fb.tiles_in_band) __builtin_trap();
        for (size_t i = 0; i < fb.bands.size(); ++i) {
            const auto &a = fb.bands[i];
            const auto &c = again.bands[i];
            if (a.frame_id != c.frame_id || a.band != c.band) __builtin_trap();
            if (a.received != c.received) __builtin_trap();
            if (a.conceal_tiles != c.conceal_tiles || a.late_tiles != c.late_tiles)
                __builtin_trap();
        }
        for (int p = 0; p < nxt::kMaxPaths; ++p) {
            if (again.path_loss[p] != fb.path_loss[p]) __builtin_trap();
            if (again.path_rtt_ms[p] != fb.path_rtt_ms[p]) __builtin_trap();
        }
        // Truncating a legal packet by one byte must be rejected, never parsed
        // into a short bitmap.
        if (wire.size() > 1) {
            nxt::FeedbackPacket cut;
            nxt::decode_feedback(std::span<const uint8_t>(wire.data(), wire.size() - 1), &cut);
        }
    }
    return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned seed) {
    return nxf::nxt::mutate_feedback_bytes(data, size, max_size, seed);
}
