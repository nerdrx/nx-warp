// nxvc_ref internal common definitions.  See docs/SYNTAX.md.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nxvc/nxvc.h"

namespace nxvc {

// ------------------------------------------------------------------ basics
using i32 = int32_t;
using i64 = int64_t;  // encoder-side only; never in the normative path
using i16 = int16_t;
using i8 = int8_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr int kTile = 64;
constexpr int kBlock = 8;
constexpr int kMaxTilesPerRow = 64;

inline i32 clamp_i32(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline i32 clamp16(i32 v) { return clamp_i32(v, -32768, 32767); }

// ---------------------------------------------------------------- headers
constexpr u32 kMagic = 0x3156584Eu;  // 'N','X','V','1' little endian
constexpr size_t kStreamHeaderBytes = 64;
constexpr size_t kFrameHeaderBytes = 40;
constexpr size_t kTileRowHeaderBytes = 12;
constexpr size_t kTileHeaderBytes = 8;

// ---------------------------------------------------------- quantization
// qstep[qp] = round(16 * 2^(qp/6)), Q4 fixed point (16 == step 1.0).
extern const u16 kQStep[64];

// Built-in weighting matrices, Q4, raster order inside the 8x8 block.
// index 0 flat, 1 luma roll-off, 2 periphery roll-off, 3 chroma.
extern const u8 kWeight[4][64];

// -------------------------------------------------------------- scan order
extern const u8 kZigzag8[64];   // 8x8
extern const u8 kZigzag4[16];   // 4x4
extern const u8 kZigzag2[4];    // 2x2
extern const u8 kRaster8[64];   // transform-skip scan
extern const u8 kZigzag1[1];

inline const u8 *scan_table(int n, bool tskip) {
    if (n == 64) return tskip ? kRaster8 : kZigzag8;
    if (n == 16) return kZigzag4;
    if (n == 4) return kZigzag2;
    return kZigzag1;
}

// ---------------------------------------------------------------- contexts
enum : int {
    kCtxCbfLuma = 0,
    kCtxCbfChroma = 1,
    kCtxLastLuma = 2,
    kCtxLastChroma = 3,
    kCtxLevelBase = 4,
    kNumCtx = 12,
    kNumSym = 16
};

// LAST symbol -> [base, raw_bits]
extern const u8 kLastBase[16];
extern const u8 kLastRawBits[16];
int last_class_of(int pos);  // inverse of the table above

inline int band_of(int scan_pos) {
    if (scan_pos == 0) return 0;
    if (scan_pos < 4) return 1;
    if (scan_pos < 10) return 2;
    return 3;
}
// band x previous-level class -> one of 8 LEVEL contexts.
extern const u8 kLevelCtx[4][3];
inline int level_ctx(int scan_pos, int prev_class) {
    return kCtxLevelBase + kLevelCtx[band_of(scan_pos)][prev_class];
}
inline int level_class(int magnitude) {
    return magnitude == 0 ? 0 : (magnitude == 1 ? 1 : 2);
}

constexpr int kEscSym = 15;   // LEVEL escape symbol
constexpr int kEscOrder = 3;  // Exp-Golomb order
constexpr int kEscMaxPrefix = 16;

// ------------------------------------------------------ probability tables
struct CtxTable {
    u16 freq[kNumSym];
    u16 cum[kNumSym + 1];
    u8 slot2sym[1024];
};
struct TableSet {
    CtxTable ctx[kNumCtx];
};

// Built-in default frequencies: [set][ctx][sym], each row sums to 1024.
extern const u16 kDefaultFreq[8][kNumCtx][kNumSym];

// Deterministic normalization of a 16-entry frequency row to sum 1024,
// every entry >= 1.  Normative (used when parsing custom tables).
void normalize_freqs(u16 f[kNumSym]);
// Build cum + slot2sym from freq.  Returns false if the row is illegal.
bool finalize_ctx(CtxTable &t);
void build_default_set(TableSet &ts, int set_index);

// Custom-table log-domain delta multipliers, Q8 (256 == 1.0):
// kDeltaMul[d + 16] = round(256 * 2^(d/4)) for d in [-16, 15].
extern const u16 kDeltaMul[32];

// ------------------------------------------------------------- tile geometry
struct TileGeom {
    int coded_size;    // luma coded edge: 64 >> res_level
    int chroma_size;   // chroma coded edge
    int alpha_size;    // == coded_size
    int nb_luma;       // blocks per edge, luma
    int nb_chroma;
};
inline TileGeom tile_geom(int res_level, bool chroma444) {
    TileGeom g{};
    g.coded_size = kTile >> res_level;
    g.chroma_size = (chroma444 ? kTile : kTile / 2) >> res_level;
    if (g.chroma_size < 8) g.chroma_size = 8;
    g.alpha_size = g.coded_size;
    g.nb_luma = g.coded_size / 8;
    g.nb_chroma = g.chroma_size / 8;
    return g;
}

}  // namespace nxvc
