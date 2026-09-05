// ENTROPY_LITE (tool bit 30): a fully parallel entropy tool for high-bitrate
// links.  No arithmetic coder, no probability tables, no serial state: a tile
// is four byte-aligned sections whose per-unit offsets are computable with two
// parallel prefix sums, so one lane can decode any unit -- and, in the FIXED
// variant, one thread can decode any single coefficient.
//
// See docs/SYNTAX.md 9.8 for the normative description.
#pragma once
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

// Length in bits of the order-k Exp-Golomb code of v (the RICE magnitude
// field): n = v + 2^k, b = floor(log2 n), j = b - k ones, a zero, b bits.
// Shared by the coder and the encoder's rate model so they cannot disagree.
inline int lite_eg_len(u32 v, int k) {
    u64 n = (u64)v + (1u << k);
    int b = 0;
    while ((n >> (b + 1)) != 0) ++b;
    return 2 * (b - k) + k + 1;
}

// Encoder-side instrumentation only: where a Lite payload's bits went.
// Not normative, not thread-safe, and never read by the decoder.
struct LiteStats {
    u64 cbf, param, sig, mode, mag, sign, pad;
};
extern LiteStats g_lite_stats;

// Encode / decode a tile's unit list.  `variant` is kLiteFixed or kLiteRice.
// The unit list, its `ncoef` values and its `scan` tables are side information
// the decoder derives from the tile header exactly as it does for rANS.
bool lite_encode_units(const Unit *units, int nunits, int variant,
                       std::vector<u8> &out);
bool lite_decode_units(const Unit *units, int nunits, int variant,
                       const u8 *buf, size_t len);

// The exact size in bits of the payload lite_encode_units would produce for
// this unit list -- every section, every byte of padding -- without producing
// it.  This is the encoder's tile rate under the tool: the rANS path's
// `table_set_cost + raw` has no Lite counterpart because Lite has no entropy
// to estimate, only a length to add up.  Returns 0 on a unit list the coder
// would refuse.
size_t lite_payload_bits(const Unit *units, int nunits, int variant);

}  // namespace nxvc
