// CPU model of rans_decode.comp.
//
// This is deliberately NOT idiomatic C++: it is a line-for-line transcription
// of the shader, with the same function names, the same per-thread "register"
// variables and the same workgroup/round structure, so that a divergence
// between GPU and CPU shows up as a diff between two files that otherwise
// read identically.  The GPU and this model must agree bit for bit.
#ifndef NXWARP_PASSA_MODEL_H
#define NXWARP_PASSA_MODEL_H

#include <cstddef>
#include <cstdint>

#include "syntax_constants.h"

namespace nxwarp_passA {

// Mirrors the per-tile descriptor SSBO (kTileDescUints uints per tile).
struct TileDesc {
    uint32_t bits_offset;  // byte offset of the 8-byte tile header
    uint32_t bits_length;  // header + payload length in bytes
    uint32_t coef_offset;  // int16 index of the tile's coefficient region
    uint32_t cbf_offset;   // uint index of the tile's CBF words
};

// Mirrors the shader's bindings and push constants.
struct Inputs {
    const uint8_t *bits = nullptr;
    size_t bits_size = 0;
    const TileDesc *tiles = nullptr;
    uint32_t num_tiles = 0;
    // Cumulative frequencies, cum[set][ctx][sym]: kNumTableSets*kNumCtx*kNumSym.
    const uint32_t *tables = nullptr;
    uint32_t frame_nplanes = 3;
    uint32_t coef_stride = kCoefStrideMax;
    uint32_t cbf_words = kCbfWordsPerTile;
    // Specialisation constant: kReadPtrBallot or kReadPtrLdsFallback.  Both
    // must give identical output; the model implements the emulated ballot.
    uint32_t read_ptr_mode = kReadPtrBallot;
    // [nxvc_vk_decoder glue, marked edit] rANS lanes per tile, 1 << nsub_log2.
    // Matches specialisation constant 2 (LANES) of rans_decode.comp.  One
    // decode() call covers the tiles of one lane count; the host groups the
    // frame's tiles by nsub_log2 and issues one dispatch per group.
    uint32_t lanes = kLanes;
};

struct Outputs {
    int16_t *coef = nullptr;    // num_tiles * coef_stride
    uint32_t *cbf = nullptr;    // num_tiles * cbf_words
    uint32_t *status = nullptr; // num_tiles
};

// Runs every workgroup of the dispatch.  Equivalent to
// vkCmdDispatch(ceil(num_tiles / kTilesPerGroup), 1, 1).
void decode(const Inputs &in, const Outputs &out);

// Number of workgroups the host must dispatch for `num_tiles` at `lanes`
// rANS lanes per tile.
inline uint32_t group_count(uint32_t num_tiles, uint32_t lanes = kLanes) {
    uint32_t tpg = nxs_tiles_per_group(lanes);
    return (num_tiles + tpg - 1) / tpg;
}

}  // namespace nxwarp_passA

#endif  // NXWARP_PASSA_MODEL_H
