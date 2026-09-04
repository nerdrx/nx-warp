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

// ------------------------------------------------------------ transform size
// Tile-header field `xform_size` (SYNTAX.md 4.1), gated on tool bit 24
// XFORM_LARGE.  The transform edge a plane actually uses is capped by the
// plane's own coded extent, so no combination of res_level, chroma format and
// xform_size can ask for a block larger than the plane (SYNTAX.md 6.7).
constexpr int kXformSizes = 3;   // 0 = 8x8, 1 = 16x16, 2 = 32x32; 3 reserved
inline int xform_edge(int xform_size) { return 8 << xform_size; }
inline int block_edge_for(int xform_size, int plane_size) {
    int e = xform_edge(xform_size);
    return e < plane_size ? e : plane_size;
}
inline int log2_of(int v) {
    int k = 0;
    while ((1 << k) < v) ++k;
    return k;
}
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
// Every scan is the diagonal zigzag of SYNTAX.md 9.2 over an edge x edge
// block: diagonal `s` ascending, its samples taken from the bottom-left when
// `s` is even and from the top-right when it is odd.  `build_zigzag` is that
// rule; the four small tables below are its output, kept as constants because
// they are pinned by the conformance vectors, and `ref.transform` checks that
// the rule still reproduces them.
extern const u16 kZigzag8[64];    // 8x8
extern const u16 kZigzag4[16];    // 4x4
extern const u16 kZigzag2[4];     // 2x2
extern const u16 kRaster8[64];    // transform-skip scan
extern const u16 kZigzag1[1];

// Fill `out` with the zigzag of an `edge` x `edge` block: diagonal `s` runs
// from (s, 0) upwards when `s` is even and from (0, s) downwards when it is
// odd, clipped to the block, writing the raster index u*edge + v with u
// vertical.
constexpr void build_zigzag(int edge, u16 *out) {
    int p = 0;
    for (int s = 0; s <= 2 * (edge - 1); ++s) {
        const int ulo = s - edge + 1 > 0 ? s - edge + 1 : 0;
        const int uhi = s < edge - 1 ? s : edge - 1;
        if ((s & 1) == 0)
            for (int u = uhi; u >= ulo; --u) out[p++] = (u16)(u * edge + s - u);
        else
            for (int u = ulo; u <= uhi; ++u) out[p++] = (u16)(u * edge + s - u);
    }
}

template <int kEdge>
struct ZigzagTable {
    u16 v[kEdge * kEdge];
    constexpr ZigzagTable() : v{} { build_zigzag(kEdge, v); }
};
// The 16x16 and 32x32 scans are the rule's output, evaluated at compile time.
inline constexpr ZigzagTable<16> kZigzag16{};
inline constexpr ZigzagTable<32> kZigzag32{};

inline const u16 *scan_table(int n, bool tskip) {
    if (n == 64) return tskip ? kRaster8 : kZigzag8;
    if (n == 256) return kZigzag16.v;
    if (n == 1024) return kZigzag32.v;
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

// A unit with more than 64 coefficients (a 16x16 or 32x32 block) reuses the
// 64-position LAST class table and the four LEVEL bands of the 8x8 block by
// coding them over its 64 equal-sized scan *groups*: the class names the
// group and `last_shift` raw bypass bits name the position inside it, so
// `last = (base[class] << last_shift) + raw` and the LEVEL band of a position
// is the band of `pos >> last_shift`.  No new context and no new symbol
// exists at any size; SYNTAX.md 9.2.1 and 9.3.1.
inline int last_shift_of(int ncoef) {
    int s = 0;
    while ((ncoef >> s) > 64) ++s;
    return s;
}

inline int band_of(int scan_pos) {
    if (scan_pos == 0) return 0;
    if (scan_pos < 4) return 1;
    if (scan_pos < 10) return 2;
    return 3;
}
// band x previous-level class -> one of 8 LEVEL contexts.
extern const u8 kLevelCtx[4][3];
// `band_shift` is last_shift_of(ncoef): a 16x16 or 32x32 unit bands its
// scan by group, so the four bands cover the same fraction of the scan at
// every transform size.
inline int level_ctx(int scan_pos, int prev_class, int band_shift) {
    return kCtxLevelBase + kLevelCtx[band_of(scan_pos >> band_shift)][prev_class];
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
