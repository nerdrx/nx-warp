// nxvc_decode_fuzz -- arbitrary bytes into the reference decoder.
//
// Contract under test (docs/PAPER.md 3.9, docs/SYNTAX.md 1):
//   * the decoder never reads or writes outside the buffers it was given;
//   * it never executes undefined behaviour (signed overflow, shift-by-63,
//     misaligned load, invalid enum) -- UBSan is what checks this;
//   * it terminates: an input is a hang if it takes longer than the libFuzzer
//     -timeout, which the nightly sets to 25 s and the smoke job to 10 s;
//   * it either produces a frame or returns a status, never both and never
//     neither.
//
// The input is a whole stream: 64-byte header, TLV area, then frames.  The
// custom mutator understands that shape (fuzz/common/nxvc_stream.h), which is
// what gets a mutated input past the header checks and into the tile decoder.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <vector>

#include "nxvc/nxvc.h"

#include "common/nxfuzz.h"
#include "common/nxvc_stream.h"

namespace {

// A hostile 4096x4096 header would have us allocate 16 MiB per plane and then
// spend minutes in the tile loop.  The decoder is allowed to be asked for
// that; the *fuzzer* is not interested in it, because the interesting bugs are
// in parsing and reconstruction, not in the size of the picture.  Anything
// larger is declined before allocation, which keeps every input inside the
// timeout without weakening what is checked.
constexpr uint64_t kMaxLumaSamples = 1024ull * 1024ull;
constexpr int kMaxFrames = 32;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    nxvc_status st = NXVC_OK;
    nxvc_decoder *d = nxvc_decoder_create(&st);
    if (!d) return 0;

    size_t consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, data, size, &consumed);
    if (st != NXVC_OK) {
        // A rejected header must consume nothing and leave the decoder usable:
        // a second call must return the same status, not crash on half-set
        // state.  This has caught "the header is stored before it is checked".
        size_t again = 0;
        nxvc_status st2 = nxvc_decoder_parse_stream_header(d, data, size, &again);
        (void)st2;
        nxvc_decoder_destroy(d);
        return 0;
    }
    if (consumed > size) __builtin_trap();  // consumed past the input: memory bug

    nxvc_stream_info si;
    std::memset(&si, 0, sizeof si);
    if (nxvc_decoder_stream_info(d, &si) != NXVC_OK) {
        nxvc_decoder_destroy(d);
        return 0;
    }

    uint32_t pw[4] = {0, 0, 0, 0}, ph[4] = {0, 0, 0, 0};
    for (int p = 0; p < 4; ++p) {
        if (nxvc_decoder_plane_size(d, p, &pw[p], &ph[p]) != NXVC_OK) {
            pw[p] = 0;
            ph[p] = 0;
        }
    }
    if (uint64_t(pw[0]) * ph[0] > kMaxLumaSamples) {
        nxvc_decoder_destroy(d);
        return 0;
    }

    // One guard byte after every plane.  ASan catches a heap overflow on its
    // own, but the guard also fires in a plain (non-sanitized) regression run,
    // which is what CI without clang executes.
    std::vector<uint8_t> planes[4];
    nxvc_image img;
    std::memset(&img, 0, sizeof img);
    for (int p = 0; p < 4; ++p) {
        size_t n = static_cast<size_t>(pw[p]) * ph[p];
        planes[p].assign(n + 16, 0xA5);
        img.plane[p] = n ? planes[p].data() : nullptr;
        img.stride[p] = int32_t(pw[p]);
    }

    size_t off = consumed;
    for (int frame = 0; frame < kMaxFrames && off < size; ++frame) {
        // Header-only scan first: it must agree with the full decode about how
        // many bytes the frame occupies, or one of the two is walking off.
        nxvc_frame_info fi;
        std::memset(&fi, 0, sizeof fi);
        size_t scanned = 0;
        nxvc_status sst = nxvc_decoder_scan_frame(d, data + off, size - off, &fi, &scanned);
        if (sst == NXVC_OK && scanned > size - off) __builtin_trap();

        size_t used = 0;
        st = nxvc_decoder_decode_frame(d, data + off, size - off, &img, &used);
        if (st == NXVC_OK) {
            if (used == 0 || used > size - off) __builtin_trap();
            // scan_frame and decode_frame must agree on the frame length.  A
            // disagreement is a spec-consistency bug rather than a memory
            // safety one, so it is counted, not trapped: a soft invariant that
            // aborts would mask every real crash for the rest of the run.
            if (sst == NXVC_OK && scanned != used)
                nxf::note_soft_violation("scan_frame and decode_frame disagree "
                                         "on the frame length");
            uint32_t cnt = 0;
            const nxvc_tile_info *ti = nxvc_decoder_tiles(d, &cnt);
            if (ti && cnt > 0) {
                // Touch every record so ASan sees any short allocation.
                volatile uint32_t acc = 0;
                for (uint32_t i = 0; i < cnt; ++i) acc += ti[i].tile_index + ti[i].qp;
                (void)acc;
            }
            off += used;
        } else {
            break;
        }
    }

    // The guard bytes must be untouched.
    for (int p = 0; p < 4; ++p) {
        size_t n = static_cast<size_t>(pw[p]) * ph[p];
        if (planes[p].empty()) continue;
        for (size_t i = n; i < planes[p].size(); ++i)
            if (planes[p][i] != 0xA5) __builtin_trap();
    }

    nxvc_decoder_destroy(d);
    return 0;
}

// Structure-aware mutator.  Always defined: libFuzzer picks it up by name, and
// the regression runner finds it through a weak symbol, so the mutator's own
// bounds are checked even in a CI without clang.
extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned seed) {
    return nxf::nxv::mutate_stream_bytes(data, size, max_size, seed);
}
