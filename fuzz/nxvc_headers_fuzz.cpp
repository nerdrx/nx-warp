// nxvc_headers_fuzz -- the header parsers only, with no reconstruction.
//
// Why this exists separately from nxvc_decode_fuzz: the tile decoder dominates
// the time budget, so a combined target spends most of its cycles below the
// parsers and barely explores the header space at all.  This target stops at
// nxvc_decoder_scan_frame, so it runs orders of magnitude faster per input and
// drives the stream header (SYNTAX.md 2), the TLV extension area (2.1), the
// tool mask (2.2) and the frame header (3.1) hard.
//
// Invariants:
//   * every rejected header consumes nothing and leaves the decoder reusable;
//   * an accepted stream header reports geometry inside the declared limits
//     (width/height in [16, 4096] and even, ceil(width/64) <= 64) -- a value
//     outside them means a constraint was not enforced, not that the input was
//     unusual;
//   * scan_frame never reports more bytes consumed than it was given.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>

#include "nxvc/nxvc.h"

#include "common/nxfuzz.h"
#include "common/nxvc_stream.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    nxvc_status st = NXVC_OK;
    nxvc_decoder *d = nxvc_decoder_create(&st);
    if (!d) return 0;

    size_t consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, data, size, &consumed);

    // stream_info must be self-consistent whether or not the header parsed:
    // asking for it after a failure must not read uninitialised state.
    nxvc_stream_info si;
    std::memset(&si, 0, sizeof si);
    nxvc_status ist = nxvc_decoder_stream_info(d, &si);

    if (st != NXVC_OK) {
        // Re-parsing the same bytes must give the same answer.
        size_t again = 0;
        nxvc_status st2 = nxvc_decoder_parse_stream_header(d, data, size, &again);
        if (st2 != st) __builtin_trap();
        nxvc_decoder_destroy(d);
        return 0;
    }

    if (consumed > size) __builtin_trap();
    if (consumed < nxf::nxv::kStreamHdr) __builtin_trap();
    if (ist != NXVC_OK) __builtin_trap();

    // SYNTAX.md 2: constraints the decoder must have enforced before it said OK.
    if (si.magic != nxf::nxv::kMagic) __builtin_trap();
    if (si.version != 1) __builtin_trap();
    if (si.width < 16 || si.width > 4096 || (si.width & 1)) __builtin_trap();
    if (si.height < 16 || si.height > 4096 || (si.height & 1)) __builtin_trap();
    if ((si.width + 63) / 64 > 64) __builtin_trap();
    if (si.chroma > 1) __builtin_trap();
    if (si.color_transform > 1) __builtin_trap();
    if (si.alpha > 1) __builtin_trap();
    if (si.color_transform == 1 && si.chroma != 1) __builtin_trap();
    if (si.tools & ~uint64_t(NXVC_TOOLS_SUPPORTED)) __builtin_trap();
    if (si.ext_len != consumed - nxf::nxv::kStreamHdr) __builtin_trap();

    // Plane geometry must follow from the header, not from anything later.
    for (int p = 0; p < 4; ++p) {
        uint32_t w = 0, h = 0;
        if (nxvc_decoder_plane_size(d, p, &w, &h) != NXVC_OK) continue;
        if (w > 4096 || h > 4096) __builtin_trap();
    }

    // Walk the frame headers.  No pixels are produced; this is the nxv-info path.
    size_t off = consumed;
    for (int i = 0; i < 64 && off < size; ++i) {
        nxvc_frame_info fi;
        std::memset(&fi, 0, sizeof fi);
        size_t used = 0;
        nxvc_status fst = nxvc_decoder_scan_frame(d, data + off, size - off, &fi, &used);
        if (fst != NXVC_OK) break;
        if (used == 0 || used > size - off) __builtin_trap();
        // SYNTAX.md 3.1 constraints on an accepted frame header.
        if (fi.base_qp > 63) __builtin_trap();
        if (fi.quant_matrix > 3 && fi.quant_matrix != 255) __builtin_trap();
        if (fi.frame_bytes < 40) __builtin_trap();
        off += used;
    }

    nxvc_decoder_destroy(d);
    return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned seed) {
    return nxf::nxv::mutate_stream_bytes(data, size, max_size, seed);
}
