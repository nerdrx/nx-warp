// Test-only rANS encoder for Pass A round-trip testing.
//
// This mirrors ref/src/entropy.cpp (LaneMachine encoding path + encode_ops)
// so that the GPU shader and its CPU model can be exercised without linking
// the reference library.  It is NOT part of the decoder and is never
// compiled into a shipping target.
//
// Encoder order: operations are collected in the interleaved schedule
// (round-robin over the 8 lanes, ascending lane order within a round) and
// then encoded in REVERSE, which is what makes the decoder's forward pass
// work.  The flush writes lane 0's 32-bit state first, little endian.
#ifndef NXWARP_PASSA_TEST_ENCODER_H
#define NXWARP_PASSA_TEST_ENCODER_H

#include <cstdint>
#include <vector>

#include "syntax_constants.h"

namespace nxwarp_passA {
namespace test {

// One coding unit, matching TileCoder::build_units().
struct UnitInfo {
    int ncoef;
    int scan_id;
    int coef_base;  // index into the tile's coefficient array
    int ctx_cbf;
    int ctx_last;
    // [entropy-lite] UNIT_COEF (0) or UNIT_MODE (1).  Mode units exist only
    // when the shape asks for INTRA_DIR, and only the Lite encoder codes
    // them; encode_tile() emits v1 syntax and never sees one.
    int kind = 0;
    int nbx = 0;        // UNIT_MODE: blocks per plane edge
    int mode_base = 0;  // UNIT_MODE: index into the tile's mode array
};

// Tile geometry as it is carried by the 8-byte tile header.
struct TileShape {
    int res_level = 0;
    int chroma444 = 0;
    int alpha_mode = 0;
    int tskip = 0;
    int frame_nplanes = 3;
    int table_set = 0;
    int tile_index = 0;
    // [entropy-lite] Frame-uniform tool bits, Pass A's `tools` push constant.
    // Only kToolFlagIntraDir is honoured here, and only by the Lite path: it
    // adds one mode unit per coded plane to build_units().
    int tools = 0;
};

// Builds the unit list for `shape`; returns the tile's coefficient count.
int build_units(const TileShape &shape, std::vector<UnitInfo> &units);

// scan_pos -> block-local index, for scan id `scan_id`.
int scan_index(int scan_id, int pos);

// A probability table set: freq must sum to kProbScale per context.
struct Tables {
    uint16_t freq[kNumTableSets][kNumCtx][kNumSym];
    uint32_t cum[kNumTableSets][kNumCtx][kNumSym];  // cum[16] == kProbScale
};

// Fills `cum` from `freq` and returns false if any row is illegal.
bool finalize(Tables &t);

// Encodes one tile.  `coef` holds the tile's coefficients in build_units()
// layout.  Output is the 8-byte tile header followed by the rANS payload.
// Returns false if the coefficients cannot be represented.
// `op_count`, when non-null, receives the number of entropy operations the
// tile costs - the numerator of the symbols/pixel figure.
bool encode_tile(const TileShape &shape, const std::vector<UnitInfo> &units,
                 const int16_t *coef, const Tables &tabs,
                 std::vector<uint8_t> &out, uint64_t *op_count = nullptr);

// [entropy-lite] The same, for an ENTROPY_LITE / FIXED payload: the 8-byte
// tile header followed by the five sections of ref/src/entropy_lite.cpp
// lite_encode_units(), which this is byte-identical to for the same units and
// coefficients.  `modes` holds the tile's intra modes, kModesPerPlane per
// plane (so plane p's block b is modes[p * kModesPerPlane + b]); it may be
// null when the shape carries no INTRA_DIR and therefore no mode units.
// `op_count` receives the number of coded BIT FIELDS -- the Lite analogue of
// the rANS entropy-operation count, and not comparable with it.
bool encode_tile_lite(const TileShape &shape,
                      const std::vector<UnitInfo> &units, const int16_t *coef,
                      const uint8_t *modes, std::vector<uint8_t> &out,
                      uint64_t *op_count = nullptr);

}  // namespace test
}  // namespace nxwarp_passA

#endif  // NXWARP_PASSA_TEST_ENCODER_H
