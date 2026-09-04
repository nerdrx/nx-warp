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
extern const u8 kScan4Split[64];  // four 4x4 sub-blocks, tool bit 19
extern const u8 kZigzag1[1];

inline const u8 *scan_table(int n, bool tskip) {
    if (n == 64) return tskip ? kRaster8 : kZigzag8;
    if (n == 16) return kZigzag4;
    if (n == 4) return kZigzag2;
    return kZigzag1;
}
// The scan of one 64-value residual block.  `split` (tool bit 19) and `tskip`
// are mutually exclusive by syntax, so the three cases do not overlap.
inline const u8 *block_scan(bool tskip, bool split) {
    return split ? kScan4Split : (tskip ? kRaster8 : kZigzag8);
}

// The 4x4 weighting matrix of a split sub-block is the tile's 8x8 matrix
// subsampled by two in each frequency axis: w4[u][v] = w8[2u][2v].  Both
// transforms have the same gain and the same quantiser scale, so no second
// matrix family exists.  docs/SYNTAX.md 6.7.
inline int weight4(const u8 *w8, int i4) { return w8[(i4 >> 2) * 16 + (i4 & 3) * 2]; }

// ---------------------------------------------------------------- contexts
// The v1 model has 12 contexts.  The v2 model (tool bit 21, CTX_V2) adds four:
// dedicated CBF/LAST/LEVEL contexts for the DC plane, whose statistics are
// nothing like an AC block's, and one context for the intra mode symbol.
// The v3 model (tool bit 25, CTX_V3) keeps those and conditions CBF and LAST
// on the lane's previous unit as well; it is laid out below.
// Contexts 0..11 keep their meaning in v1 and v2, but not their *statistics*
// (v2 no longer mixes the DC plane into them), so each model has its own
// built-in table family; v3 renumbers everything and has a third.
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
    kNumSym = 16
};

// ---------------------------------------------------------- unit classes
// The statistical family a coding unit belongs to.  It is a property of the
// unit's position in the tile (which plane, DC plane or residual block), so
// encoder and decoder derive it from the same place and never transmit it.
enum : int {
    kUclsLuma = 0,    // residual blocks of the luma or alpha plane
    kUclsChroma = 1,  // residual blocks of a chroma plane
    kUclsDc = 2,      // a DC-plane unit of any plane
    kNumUcls = 3
};

// ------------------------------------------------- v3 model (tool bit 25)
// The v3 model conditions CBF and LAST on `prev_cbf`: whether the *previous
// coefficient unit the same lane decoded in the same unit class* was coded.
// A lane owns units l, l+N, l+2N, ..., so that unit is always one this lane
// has already finished -- the derivation is causal inside the lane and needs
// no cross-lane communication, whatever the interleaved schedule does.
//
// For the ordinary tile -- res_level 0, nsub_log2 3, so N = 8 lanes over 8x8
// blocks per plane edge -- a lane's units are exactly one column of blocks, so
// `prev_cbf` is the coded flag of the block **directly above**.  SYNTAX.md 9.8.
//
// Richer classes were measured and rejected: splitting on LAST and on mean
// magnitude (4 classes), and giving LEVEL its own neighbour family, each cost
// more in transmitted-table bits than they returned.  ref/RESULTS-ctx-b.md 2.
enum : int {
    kCtxV3CbfBase = 0,                  // + 2 * ucls + prev_cbf  ->  0..5
    kCtxV3LastBase = 2 * kNumUcls,      // + 2 * ucls + prev_cbf  ->  6..11
    kCtxV3LevelBase = 4 * kNumUcls,     // + kLevelCtx[band][prev] -> 12..19
    kCtxV3LevelDc = kCtxV3LevelBase + 8,
    kCtxV3Mode = kCtxV3LevelDc + 1,
    kNumCtxV3 = kCtxV3Mode + 1,         // 22
    kNumCtx = kNumCtxV3   // storage; the coded count is kNumCtxV1/V2/V3
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
    kNumIntraModes = 9,
    // Chroma only, tool bit 24: a linear model of the co-located
    // reconstructed luma, fitted to the block's own reconstructed
    // neighbours.  It extends the chroma mode alphabet to 10 and leaves the
    // luma alphabet at 9.  SYNTAX.md 7.7.
    kIntraCfl = 9,
    kNumIntraModesCfl = 10
};

// ------------------------------------------------------- chroma from luma
// The model is fitted between two neighbour pairs -- the mean of the two with
// the smallest co-located luma and the mean of the two with the largest -- so
// the only division is by their luma difference, which is at most 255.  It is
// a table: kCflRecip[d] = round(2^15 / d) for d in [1, 255], entry 0 unused.
// SYNTAX.md 7.7.  Nothing else in the decoding process divides.
extern const u16 kCflRecip[256];
// The fitted slope is Q8 and clamped to +-8.0, which bounds a * (L - minL) by
// 2048 * 255 = 5.2e5 and keeps the whole predictor inside int32.
constexpr int kCflAlphaBits = 8;
constexpr int kCflAlphaMax = 8 << kCflAlphaBits;   //  2048
// The number of neighbour pairs the fit reads: 8 above and 8 to the left.
constexpr int kCflPairs = 16;

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
// In a split block (tool bit 19) the 64 scan positions are four concatenated
// 4x4 sub-blocks, so the frequency band a position belongs to is its position
// *within its sub-block*, not within the unit.  docs/SYNTAX.md 9.3.
inline int band_pos(int scan_pos, bool split) {
    return split ? (scan_pos & 15) : scan_pos;
}
// band x previous-level class -> one of 8 LEVEL contexts.
extern const u8 kLevelCtx[4][3];
inline int level_ctx(int scan_pos, int prev_class) {
    return kCtxLevelBase + kLevelCtx[band_of(scan_pos)][prev_class];
}
inline int level_class(int magnitude) {
    return magnitude == 0 ? 0 : (magnitude == 1 ? 1 : 2);
}

// ------------------------------------------------------- encoder dead zone
// The dead-zone quantizer is `q = floor(|c| / step + f)`.  `f` was a flat 1/3
// everywhere; it is now per frequency band, because the two units that still
// use this quantizer rather than the RD trellis -- the DC plane, which is the
// intra predictor and is deliberately not trellised, and every unit when
// --no-rdo is in force -- want different tradeoffs at different frequencies.
// Values are in *forty-eighths* of a step and are indexed by band_of(scan
// position), the same banding the LEVEL contexts use.  Forty-eighths because
// the denominator has to divide 3 and 2 exactly -- 16 is the 1/3 these
// replace and 24 the 1/2 the inter transform-skip path uses, both bit-exact,
// which is what lets this refactor land without touching one conformance
// vector -- while still leaving a fine grid to tune on.  Encoder only:
// nothing in the decoding process depends on them.
constexpr int kDeadZoneUnit = 48;
extern const u8 kDeadZoneDc[4];   // the DC plane
extern const u8 kDeadZoneAc[4];   // residual blocks without the trellis
constexpr u8 kDeadZoneTskipInter = 24;  // 1/2, the inter transform-skip value
// The quantizer's rounding term for a step `t` (Q4) and an offset of `f`
// forty-eighths.  Encoder side, so the division is allowed (SYNTAX.md 6.5).
inline i32 dead_zone(i32 t, int f) { return (t * f) / kDeadZoneUnit; }

// The context the intra mode symbol is coded in, or kCtxNone for the bypass
// binarisation of the v1 model.
inline int mode_context(int nctx) {
    if (nctx >= kNumCtxV3) return kCtxV3Mode;
    return nctx >= kNumCtxV2 ? kCtxMode : kCtxNone;
}

inline int v3_ctx_cbf(int ucls, int prev_cbf) {
    return kCtxV3CbfBase + 2 * ucls + prev_cbf;
}
inline int v3_ctx_last(int ucls, int prev_cbf) {
    return kCtxV3LastBase + 2 * ucls + prev_cbf;
}
// LEVEL is not conditioned on the neighbour: the previous level inside the
// unit already says what the neighbour would (RESULTS-ctx-b.md 2).  The DC
// plane keeps one un-banded context, exactly as v2 gave it.
inline int v3_ctx_level(int ucls, int scan_pos, int prev_class) {
    if (ucls == kUclsDc) return kCtxV3LevelDc;
    return kCtxV3LevelBase + kLevelCtx[band_of(scan_pos)][prev_class];
}

// Sign data hiding (tool bit 22): a unit whose LAST is at scan position
// kSdhMinLast or beyond does not code the sign at that position; it is the
// parity of the sum of the unit's absolute levels.  The threshold exists so
// that the encoder always has several coefficients to spend the parity on.
constexpr int kSdhMinLast = 4;

constexpr int kEscSym = 15;   // LEVEL escape symbol
constexpr int kEscOrder = 3;  // Exp-Golomb order
constexpr int kEscMaxPrefix = 16;

// Lloyd iterations the encoder spends refining the eight per-frame table sets
// (encoder only; see nxvc_config::table_iters).  Three was measured; a fourth
// is worth under 0.1 % and the objective the iteration minimizes does not
// include the transmitted table cost, so it should not be run to convergence.
constexpr int kDefaultTableIters = 3;

// ------------------------------------------------------ probability tables
// Probability precision M = 2^kProbBits.  Every context's frequencies are at
// least 1 and sum to exactly kProbTotal.  SYNTAX.md 9.5.
#ifndef NXVC_PROB_BITS      // development knob; the shipped syntax is 10
#define NXVC_PROB_BITS 10
#endif
constexpr int kProbBits = NXVC_PROB_BITS;
constexpr i32 kProbTotal = 1 << kProbBits;
// Leave room for the other 15 entries when a row is renormalized.
constexpr i32 kProbMax = kProbTotal - (kNumSym - 1);

struct CtxTable {
    u16 freq[kNumSym];
    u16 cum[kNumSym + 1];
    u8 slot2sym[kProbTotal];
};
struct TableSet {
    CtxTable ctx[kNumCtx];
};

// Built-in default frequencies: [set][ctx][sym], each row sums to 1024.
extern const u16 kDefaultFreq[8][kNumCtxV1][kNumSym];
// The v2 family, retrained with the DC plane and the mode symbol split out.
extern const u16 kDefaultFreqV2[8][kNumCtxV2][kNumSym];
// The v3 family, retrained with the neighbour-conditioned contexts.
extern const u16 kDefaultFreqV3[8][kNumCtxV3][kNumSym];

// Deterministic normalization of a 16-entry frequency row to sum 1024,
// every entry >= 1.  Normative (used when parsing custom tables).
void normalize_freqs(u16 f[kNumSym]);
// Build cum + slot2sym from freq.  Returns false if the row is illegal.
bool finalize_ctx(CtxTable &t);
void build_default_set(TableSet &ts, int set_index, int nctx = kNumCtxV1);
// The built-in frequency row for (set, ctx) under a model of `nctx` contexts.
u16 default_freq(int nctx, int set_index, int c, int s);
// The same row at the active probability precision (kProbTotal, not 1024).
void default_row(int nctx, int set_index, int c, u16 f[kNumSym]);
u16 default_row_freq(int nctx, int set_index, int c, int s);

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
