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
constexpr int kBlock = 8;          // the coefficient group, and the v1 block
constexpr int kMaxTilesPerRow = 64;

// ------------------------------------------------------------ transform size
// Tool bit 24 XFORM_LARGE lets a tile pick a transform edge of 8, 16 or 32
// samples (tile-header field `xform`, SYNTAX.md 4.1).  `xform` is the log2 of
// the edge over 8, so 0 is the version 1 transform exactly.
constexpr int kMaxXform = 32;
constexpr int kMaxXformLog2 = 2;   // the largest legal `xform`
inline int xform_edge(int xform) { return kBlock << xform; }
inline int xform_log2(int n) { return n == 8 ? 3 : (n == 16 ? 4 : 5); }

// A plane whose coded edge is smaller than the tile's transform uses the
// largest size it can hold: `xform` is capped at log2(size / 8).  There is no
// syntax for this -- it is derived from the tile header alone, so a 4:2:0
// chroma plane inside a 32x32-transform tile needs no separate signal.
inline int plane_xform(int xform, int size) {
    int cap = 0;
    while (cap < kMaxXformLog2 && (kBlock << (cap + 1)) <= size) ++cap;
    return xform < cap ? xform : cap;
}

// Position of coefficient (u, v) of an n x n transform block inside that
// block's coefficient-group storage: (n/8)^2 groups of 64 in raster order,
// each group holding its 64 coefficients in 8x8 raster order.  For n == 8
// this is the identity, so the version 1 layout is unchanged.
// SYNTAX.md 6.7.
inline int group_pos(int n, int u, int v) {
    return ((u >> 3) * (n >> 3) + (v >> 3)) * 64 + (u & 7) * 8 + (v & 7);
}

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

// The weight of coefficient (u, v) of an n x n block, n = 8 << xform.  The
// larger sizes have no matrices of their own: the 8x8 matrix is sampled at
// the same *spatial* frequency, w_n[u][v] = w_8[u >> xform][v >> xform]
// (SYNTAX.md 6.5).  Values stay in [1, 32], so the dequantizer's range bound
// is the version 1 one at every size.
inline int weight_at(const u8 *w8, int xform, int u, int v) {
    return w8[((u >> xform) << 3) + (v >> xform)];
}

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
// The v1 model has 12 contexts.  The v2 model (tool bit 21, CTX_V2) adds four:
// dedicated CBF/LAST/LEVEL contexts for the DC plane, whose statistics are
// nothing like an AC block's, and one context for the intra mode symbol.
// Contexts 0..11 keep their meaning in both models, but their *statistics*
// differ (v2 no longer mixes the DC plane into them), so the two models have
// separate built-in table families.
enum : int {
    kCtxCbfLuma = 0,
    kCtxCbfChroma = 1,
    kCtxLastLuma = 2,
    kCtxLastChroma = 3,
    kCtxLevelBase = 4,
    kNumCtxV1 = 12,
    kCtxCbfDc = 12,
    kCtxLastDc = 13,
    kCtxLevelDc = 14,
    kCtxMode = 15,
    kNumCtxV2 = 16,
    kNumCtx = 16,   // storage; the coded count is kNumCtxV1 or kNumCtxV2
    kNumSym = 16
};

// "no context selected" for Unit::ctx_level / Unit::ctx_mode.  Context 0 is
// kCtxCbfLuma, which is never a legal LEVEL or MODE context, so 0 is an
// unambiguous sentinel -- and, unlike a signed -1, it cannot become 255 and
// index TableSet::ctx out of bounds if a caller forgets to set the field.
constexpr int kCtxNone = 0;

// ------------------------------------------------------------ intra modes
// Per 8x8 block when tool bit 17 (INTRA_DIR) is on.  Mode 0 reproduces the v1
// predictor exactly, which makes directional intra a strict superset: a tile
// that wants v1 behaviour codes mode 0 everywhere.
enum : int {
    kIntraDcPlane = 0,  // the bilinear DC-plane prediction (v1)
    kIntraDc = 1,       // mean of the top and left neighbours
    kIntraPlanar = 2,   // HEVC-style planar
    kIntraH = 3,        // horizontal
    kIntraV = 4,        // vertical
    kIntraDdl = 5,      // diagonal down-left (45 deg)
    kIntraDdr = 6,      // diagonal down-right (45 deg)
    kIntraVr = 7,       // vertical-right (26.6 deg)
    kIntraHd = 8,       // horizontal-down (63.4 deg)
    kNumIntraModes = 9
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
// `band_min` is a coding unit's band floor: the LEVEL band of scan position
// p is max(band_of(p), band_min).  It is 0 for every version 1 unit, which
// makes it inert there, and it is how the larger transforms reuse the four
// existing bands without adding a context (SYNTAX.md 9.3).
//
// For a large transform block the floor is per coefficient group: the group
// holding the block's DC keeps the version 1 bands, every other group is high
// frequency by construction and floors at band 3.  Measured against floors 0
// and 2 in ref/RESULTS-xform-b.md.
inline int group_band_min(int gi) { return gi == 0 ? 0 : 3; }

inline int level_ctx(int scan_pos, int prev_class, int band_min) {
    int band = band_of(scan_pos);
    if (band < band_min) band = band_min;
    return kCtxLevelBase + kLevelCtx[band][prev_class];
}
inline int level_class(int magnitude) {
    return magnitude == 0 ? 0 : (magnitude == 1 ? 1 : 2);
}

// Sign data hiding (tool bit 22): a unit whose LAST is at scan position
// kSdhMinLast or beyond does not code the sign at that position; it is the
// parity of the sum of the unit's absolute levels.  The threshold exists so
// that the encoder always has several coefficients to spend the parity on.
constexpr int kSdhMinLast = 4;

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
extern const u16 kDefaultFreq[8][kNumCtxV1][kNumSym];
// The v2 family, retrained with the DC plane and the mode symbol split out.
extern const u16 kDefaultFreqV2[8][kNumCtxV2][kNumSym];

// Deterministic normalization of a 16-entry frequency row to sum 1024,
// every entry >= 1.  Normative (used when parsing custom tables).
void normalize_freqs(u16 f[kNumSym]);
// Build cum + slot2sym from freq.  Returns false if the row is illegal.
bool finalize_ctx(CtxTable &t);
void build_default_set(TableSet &ts, int set_index, int nctx = kNumCtxV1);
// The built-in frequency row for (set, ctx) under a model of `nctx` contexts.
u16 default_freq(int nctx, int set_index, int c, int s);

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
