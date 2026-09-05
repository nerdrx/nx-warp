// ENTROPY_LITE (tool bit 30): a fully parallel entropy tool for high-bitrate
// links.  No arithmetic coder, no probability tables, no serial state: a tile
// is four byte-aligned sections whose per-unit offsets are computable with two
// parallel prefix sums, so one lane can decode any unit -- and, in the FIXED
// variant, one thread can decode any single coefficient.
//
// See docs/SYNTAX.md 9.8 for the normative description.
#pragma once
#include <atomic>
#include "common.h"
#include "entropy.h"

namespace nxvc {

// Tile-header `table_set` is repurposed as the variant selector under
// ENTROPY_LITE: the tool has no probability tables for the field to name.
enum LiteVariant : int { kLiteFixed = 0, kLiteRice = 1, kLiteNumVariants = 2 };

// FIXED: 3-bit per-unit magnitude class -> field width.  The width covers
// |q| in 1 .. 2^bits, coded as |q| - 1, so class 7 spans the whole int16
// range and the variant needs no escape at all.  Class 0 is *zero* bits wide:
// a unit whose every nonzero is +-1 spends nothing at all on magnitudes,
// which is the common case above QP 16.
constexpr int kLiteMagBits[8] = {0, 1, 2, 3, 4, 6, 8, 16};
// RICE: 3-bit per-unit Exp-Golomb order, plus a 12-bit body length in bits.
constexpr int kLiteRiceKBits = 3;
constexpr int kLiteLenBits = 12;

// Units per group in the two-level coded-unit map.  At QP 20 and above nine
// coefficient units in ten are empty, and a group of 16 clears most of them
// for one bit instead of sixteen.
constexpr int kLiteCbfGroup = 16;

// Bits the per-unit LAST field takes, given the unit's coefficient count.
inline int lite_last_bits(int ncoef) {
    int b = 0;
    while ((1 << b) < ncoef) ++b;
    return b;
}

// Encoder-side instrumentation only: where a Lite payload's bits went.
// Not normative, not thread-safe, and never read by the decoder.
// Diagnostic bit accounting.  The encoder codes tiles on a thread pool, so
// every field is atomic: `+=` is a relaxed fetch-add and the dump at exit
// reads a consistent total.
struct LiteStats {
    std::atomic<u64> cbf{0}, param{0}, sig{0}, mode{0}, mag{0}, sign{0}, pad{0};
};
extern LiteStats g_lite_stats;

// Encode / decode a tile's unit list.  `variant` is kLiteFixed or kLiteRice.
// The unit list, its `ncoef` values and its `scan` tables are side information
// the decoder derives from the tile header exactly as it does for rANS.
bool lite_encode_units(const Unit *units, int nunits, int variant,
                       std::vector<u8> &out);
bool lite_decode_units(const Unit *units, int nunits, int variant,
                       const u8 *buf, size_t len);

}  // namespace nxvc
