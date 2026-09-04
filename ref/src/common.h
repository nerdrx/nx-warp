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
    // The v3 model (tool bit 24, CTX_V3) keeps 0..15 and adds eleven
    // contexts, all selected from state a lane already holds: the class of
    // the last block unit *the same lane* decoded in the same plane (which is
    // the block above it whenever the lane count equals the block-row width),
    // whether a LEVEL sits at scan position LAST, and the DC term of a DC
    // plane.  Nothing here is cross-lane and nothing is a new memory read.
    kCtxCbfLumaN = 16,     // + (nbr_class - 1), nbr_class 1..3
    kCtxCbfChromaN = 19,   // + (nbr_class - 1)
    kCtxLastLumaN = 22,    // LAST, luma/alpha, neighbour coded
    kCtxLastChromaN = 23,  // LAST, chroma, neighbour coded
    kCtxLevelDc0 = 24,     // LEVEL at scan position 0 of a DC plane
    kCtxLevelLastLo = 25,  // LEVEL at scan position LAST, band 0..1
    kCtxLevelLastHi = 26,  // LEVEL at scan position LAST, band 2..3
    kNumCtxV3 = 27,
    // Two further rows belong to VEC_ENT (tool bit 25) and are only coded
    // when a stream can carry a tile vector, so the coded context count --
    // and with it the transmitted table-set size -- grows only where the rows
    // are used.  SYNTAX.md 9.3 states the whole ladder.
    kCtxVecMv = 27,        // motion-vector component magnitude class
    kCtxVecDisp = 28,      // STEREO disparity magnitude class
    kNumCtxV3Vec = 29,
    kNumCtx = 29,   // storage; the coded count is one of the four above
    kNumSym = 16
};

// "no context selected" for Unit::ctx_level / Unit::ctx_mode.  Context 0 is
// kCtxCbfLuma, which is never a legal LEVEL or MODE context, so 0 is an
// unambiguous sentinel -- and, unlike a signed -1, it cannot become 255 and
// index TableSet::ctx out of bounds if a caller forgets to set the field.
constexpr int kCtxNone = 0;

// The number of contexts a stream codes, and therefore the size of a
// transmitted table set (nctx * 16 * 5 bits).  SYNTAX.md 9.3 table.
inline int coded_context_count(bool ctx_v2, bool ctx_v3, bool vec_ent) {
    if (!ctx_v3) return ctx_v2 ? kNumCtxV2 : kNumCtxV1;
    return vec_ent ? kNumCtxV3Vec : kNumCtxV3;
}

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
inline int level_ctx(int scan_pos, int prev_class) {
    return kCtxLevelBase + kLevelCtx[band_of(scan_pos)][prev_class];
}

// ------------------------------------------------ v3 context derivation
// `nbr` is the neighbour class carried by the lane (SYNTAX.md 9.8):
//   0 no neighbour, 1 neighbour had CBF == 0,
//   2 neighbour coded with LAST < 4, 3 neighbour coded with LAST >= 4.
// Class 0 keeps the v2 context, so a lane's first block in a plane codes
// exactly as it did before and the new rows only ever see conditioned data.
inline int cbf_ctx_v3(int base, int nbr) {
    if (nbr == 0) return base;
    return (base == kCtxCbfChroma ? kCtxCbfChromaN : kCtxCbfLumaN) + (nbr - 1);
}
inline int last_ctx_v3(int base, int nbr) {
    if (nbr < 2) return base;
    return base == kCtxLastChroma ? kCtxLastChromaN : kCtxLastLumaN;
}
// LEVEL under v3.  The coefficient at scan position LAST is nonzero by
// construction, so it cannot share a context with positions that may be zero;
// and the DC term of a DC plane is a block mean, not a residual.
inline int level_ctx_v3(int scan_pos, int last, int prev_class, int dc_ctx) {
    if (dc_ctx != kCtxNone) return scan_pos == 0 ? kCtxLevelDc0 : dc_ctx;
    if (scan_pos == last)
        return band_of(scan_pos) < 2 ? kCtxLevelLastLo : kCtxLevelLastHi;
    return level_ctx(scan_pos, prev_class);
}
inline int level_class(int magnitude) {
    return magnitude == 0 ? 0 : (magnitude == 1 ? 1 : 2);
}

// Sign data hiding (tool bit 22): a unit whose LAST is at scan position
// kSdhMinLast or beyond does not code the sign at that position; it is the
// parity of the sum of the unit's absolute levels.  The threshold exists so
// that the encoder always has several coefficients to spend the parity on.
constexpr int kSdhMinLast = 4;

// v3 neighbour class (SYNTAX.md 9.8): a coded block unit is "dense" when its
// LAST is at scan position kNbrDenseLast or beyond.  Same split point as
// kSdhMinLast by measurement, not by construction; they are separate names.
constexpr int kNbrDenseLast = 4;

// ---------------------------------------------------- vector binarisation
// VEC_ENT (tool bit 25): a motion-vector component or a STEREO disparity is a
// magnitude class in a context, `kVecRawBits` raw bypass bits, and -- for a
// vector component -- a sign bit when the magnitude is nonzero.  Class 15 is
// an Exp-Golomb order-3 escape of `magnitude - kVecEscBase`, the same code the
// LEVEL escape uses.  SYNTAX.md 9.9.
extern const u16 kVecBase[16];
extern const u8 kVecRawBits[16];
constexpr int kVecEscSym = 15;
constexpr int kVecEscBase = 256;
constexpr int kMvMagMax = 128;     // mv components are i8: -128 .. +127
constexpr int kDisparityMax = 4095;
int vec_class_of(int magnitude);

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
// The v3 family, retrained with the neighbour, LAST-position and DC-term
// contexts split out.
extern const u16 kDefaultFreqV3[8][kNumCtxV3Vec][kNumSym];

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
