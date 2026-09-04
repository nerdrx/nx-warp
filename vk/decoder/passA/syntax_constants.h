// NX Warp Pass A - every bitstream-syntax-dependent constant, in one place.
//
// This header is compiled BOTH as C++ (host + CPU model) and as GLSL (the
// Pass A compute shader), so that the two can never drift.  Nothing in
// rans_decode.comp / passA_model.cpp may hard-code a syntax value: if a
// number describes the bitstream, it belongs here.
//
// NORMATIVE SOURCE: ref/src/entropy.{h,cpp}, ref/src/common.h,
// ref/src/tables.cpp, ref/src/codec.cpp (pack/unpack_tile_header,
// TileCoder::build_units).  Where docs/SYNTAX.md later disagrees with the
// paper, ref wins; this header is the diff surface.
//
// Sections:
//   1. rANS parameters               ref/src/entropy.h
//   2. Context / symbol alphabet     ref/src/common.h
//   3. LAST classes, level contexts  ref/src/tables.cpp
//   4. Escape coding                 ref/src/entropy.cpp eg3_encode/feed
//   5. Scan orders                   ref/src/tables.cpp
//   6. Tile header (8 bytes)         ref/src/codec.cpp unpack_tile_header
//   7. Tile geometry / unit order    ref/src/common.h, TileCoder::build_units
//   8. Pass A <-> Pass B buffer layout   (this project's own contract)

#ifndef NXWARP_PASSA_SYNTAX_CONSTANTS_H
#define NXWARP_PASSA_SYNTAX_CONSTANTS_H

// ---------------------------------------------------------------------------
// C++ / GLSL common-subset macro layer.
// ---------------------------------------------------------------------------
#if defined(__cplusplus)
#include <cstdint>
namespace nxwarp_passA {
using uint = uint32_t;
#define NXS_CONST static constexpr
#define NXS_FN inline
#define NXS_ARRAY(type, name, n) static constexpr type name[n] = {
#define NXS_ARRAY_END \
    }                 \
    ;
#else
#define NXS_CONST const
#define NXS_FN
#define NXS_ARRAY(type, name, n) const type name[n] = type[n](
#define NXS_ARRAY_END );
#endif

// ===========================================================================
// 1. rANS parameters                              [ref/src/entropy.h]
// ===========================================================================

// Lower bound of the state interval.  A state is always in [L, 2^32).
NXS_CONST uint kRansL = 1u << 16;

// Probability scale: every context table sums to exactly 2^kProbBits.
NXS_CONST uint kProbBits = 10;
NXS_CONST uint kProbScale = 1u << 10;  // == 1 << kProbBits
NXS_CONST uint kProbMask = 1023u;      // == kProbScale - 1

// v1 fixes the substream count at 8 (tile header nsub_log2 == 3).  The
// decoder rejects anything else; the field stays in the syntax for v2.
NXS_CONST uint kLanesLog2 = 3;
NXS_CONST uint kLanes = 8;

// Stream initialisation: kLanes little-endian uint32 states, lane 0 first,
// immediately after the tile header.  Every state must be >= kRansL.
NXS_CONST uint kInitBytesPerLane = 4;

// Renormalisation: one big-endian 16-bit word per renormalising lane, taken
// from a single read pointer shared by the 8 lanes of a tile.  Within one
// scheduling round the lanes consume words in ascending lane order.
NXS_CONST uint kRenormBytes = 2;

// ===========================================================================
// 2. Context layout and symbol alphabet           [ref/src/common.h]
// ===========================================================================

NXS_CONST int kCtxCbfLuma = 0;
NXS_CONST int kCtxCbfChroma = 1;
NXS_CONST int kCtxLastLuma = 2;
NXS_CONST int kCtxLastChroma = 3;
NXS_CONST int kCtxLevelBase = 4;  // 8 LEVEL contexts: 4..11
NXS_CONST int kNumCtx = 12;
NXS_CONST int kNumSym = 16;

// Static per-frame probability tables, up to 8 sets (tile header table_set).
NXS_CONST int kNumTableSets = 8;

// ===========================================================================
// 3. LAST classes and LEVEL context derivation    [ref/src/tables.cpp]
// ===========================================================================

// LAST symbol -> [base position, number of raw (bypass) suffix bits].
// Classes 0..7 code position 0..7 exactly; 8..14 add raw bits; 15 is
// reserved and illegal in v1.
NXS_ARRAY(int, kLastBase, 16)
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32, 48, 64
NXS_ARRAY_END

NXS_ARRAY(int, kLastRawBits, 16)
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 3, 4, 4, 0
NXS_ARRAY_END

NXS_CONST int kLastMaxClass = 14;  // class 15 is illegal

// band x previous-level class -> one of 8 LEVEL contexts.
// Flattened as kLevelCtx[band * 3 + prev_class].
NXS_ARRAY(int, kLevelCtx, 12)
    0, 1, 2,
    3, 4, 2,
    5, 6, 7,
    5, 6, 7
NXS_ARRAY_END

// Scan-position band boundaries: 0 | 1..3 | 4..9 | 10+.
NXS_CONST int kBand1 = 4;
NXS_CONST int kBand2 = 10;

// ===========================================================================
// 4. Escape coding                            [ref/src/entropy.cpp]
// ===========================================================================

// LEVEL symbol 15 escapes to Exp-Golomb order 3 on (magnitude - 15).
// prefix: j one-bits then a zero, each coded as a 1-bit bypass.
// suffix: (j + kEscOrder) bits, sent most-significant chunk first, in
//         chunks of at most 8 bits (the first chunk carries the remainder).
NXS_CONST int kEscSym = 15;
NXS_CONST int kEscOrder = 3;
NXS_CONST int kEscMaxPrefix = 16;
NXS_CONST int kEscMaxValue = 32752;  // escape payload upper bound
NXS_CONST int kEscChunkBits = 8;

// Magnitudes 0..14 are coded directly by the LEVEL symbol.
NXS_CONST int kLevelMaxDirect = 14;

// Sign is a single bypass bit, 1 == negative, sent after the magnitude.

// ===========================================================================
// 5. Scan orders                               [ref/src/tables.cpp]
// ===========================================================================

// scan_pos -> block-local coefficient index.
NXS_CONST int kScanZigzag8 = 0;  // 64 coefficients, transform
NXS_CONST int kScanRaster8 = 1;  // 64 coefficients, transform skip
NXS_CONST int kScanZigzag4 = 2;  // 16 coefficients (DC plane, nb == 4)
NXS_CONST int kScanSmall = 3;    // 4 or 1 coefficients, identity
NXS_CONST int kNumScans = 4;
NXS_CONST int kScanStride = 64;  // slots per scan table in the shared array

NXS_ARRAY(int, kZigzag8, 64)
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
NXS_ARRAY_END

NXS_ARRAY(int, kZigzag4, 16)
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
NXS_ARRAY_END

// kRaster8 and kZigzag2/kZigzag1 are the identity permutation and are
// generated rather than tabulated; see scan_index().

// ===========================================================================
// 6. Tile header - 8 bytes, two little-endian uint32
//                                       [ref/src/codec.cpp]
// ===========================================================================

NXS_CONST uint kTileHeaderBytes = 8;

// word 0
NXS_CONST uint kThLayerShift = 0, kThLayerMask = 3u;
NXS_CONST uint kThEyeShift = 2, kThEyeMask = 1u;
NXS_CONST uint kThTileIndexShift = 4, kThTileIndexMask = 0xfffu;
NXS_CONST uint kThPayloadLenShift = 16, kThPayloadLenMask = 0xffffu;
// word 1
NXS_CONST uint kThModeShift = 0, kThModeMask = 7u;
NXS_CONST uint kThResLevelShift = 3, kThResLevelMask = 3u;
NXS_CONST uint kThChroma444Shift = 5, kThChroma444Mask = 1u;
NXS_CONST uint kThAlphaModeShift = 6, kThAlphaModeMask = 3u;
NXS_CONST uint kThQpDeltaShift = 8, kThQpDeltaMask = 0x3fu;  // signed, 6-bit
NXS_CONST uint kThTableSetShift = 14, kThTableSetMask = 7u;
NXS_CONST uint kThNsubLog2Shift = 17, kThNsubLog2Mask = 7u;
NXS_CONST uint kThMvPresentShift = 20, kThMvPresentMask = 1u;
NXS_CONST uint kThRefSelShift = 21, kThRefSelMask = 3u;
NXS_CONST uint kThTskipShift = 23, kThTskipMask = 1u;
NXS_CONST uint kThWgtShift = 24, kThWgtMask = 3u;

NXS_CONST int kMaxResLevel = 2;
NXS_CONST int kMaxMode = 4;
NXS_CONST int kMaxNsubLog2 = 5;
NXS_CONST int kAlphaModeConstant = 1;  // a single alpha byte follows
NXS_CONST int kAlphaModeCoded = 2;     // alpha plane is entropy-coded

// Reserved bits that a conforming stream sets to zero (SYNTAX.md 4.1).
NXS_CONST uint kThReservedW0 = 1u << 3;
NXS_CONST uint kThReservedW1 = 0xfc000000u;  // bits 26..31

// Optional bytes between the 8-byte header and the rANS payload, in order:
//   1. i8 mv_x, i8 mv_y   if mv_present
//   2. u8 alpha_value     if alpha_mode == 1
NXS_CONST uint kThMvBytes = 2;
NXS_CONST uint kThAlphaValueBytes = 1;

// ===========================================================================
// 7. Tile geometry and unit order   [ref/src/common.h tile_geom,
//                                    ref/src/codec.cpp build_units]
// ===========================================================================

NXS_CONST int kTileSize = 64;
NXS_CONST int kBlockSize = 8;
NXS_CONST int kCoefPerBlock = 64;
NXS_CONST int kMaxPlanes = 4;
NXS_CONST int kMaxBlocksPerEdge = 8;   // 64 / 8
NXS_CONST int kMinChromaSize = 8;

// Units of one plane, in coding order: first ONE DC-plane unit carrying
// nb*nb coefficients, then nb*nb block units of 64 coefficients each.
// Planes follow in order Y, Co(U), Cg(V), A.
NXS_CONST int kUnitsPerPlaneExtra = 1;  // the DC-plane unit

// Largest possible unit count for one tile: 4 planes x (1 + 64).
NXS_CONST int kMaxUnitsPerTile = 260;

// ===========================================================================
// 8. Pass A output layout (Pass A <-> Pass B contract)
// ===========================================================================

// Coefficients are written exactly in the reference's TileCoder::coef order
// so Pass B can feed reconstruct_plane() directly:
//   for each coded plane p:  [ nb*nb DC-plane coefficients ]
//                            [ nb*nb blocks x 64 coefficients ]
// The per-tile region is padded to kCoefStrideMax int16 entries.
// 4:2:0, res 0, 3 planes  -> (64 + 64*64) + 2*(16 + 16*64) = 6240
// 4:4:4, res 0, 4 planes  -> 4 * (64 + 64*64)              = 16640
NXS_CONST uint kCoefStrideMax = 16640;

// One CBF bit per unit, bit index == unit index, LSB first.
NXS_CONST uint kCbfWordsPerTile = 16;  // 512 bits >= kMaxUnitsPerTile

// Per-tile descriptor handed to the shader (uints).
//   0: byte offset of the 8-byte tile header inside the bitstream buffer
//   1: byte length of header + payload
//   2: int16 index of this tile's coefficient region
//   3: uint index of this tile's CBF words
NXS_CONST uint kTileDescUints = 4;
NXS_CONST uint kTdBitsOffset = 0;
NXS_CONST uint kTdBitsLength = 1;
NXS_CONST uint kTdCoefOffset = 2;
NXS_CONST uint kTdCbfOffset = 3;

// Per-tile status codes written to the status SSBO.
NXS_CONST uint kStatusOk = 0;
NXS_CONST uint kStatusTruncated = 1;      // ran out of payload bytes
NXS_CONST uint kStatusBadSymbol = 2;      // illegal symbol for the phase
NXS_CONST uint kStatusBadHeader = 3;      // unsupported header field
NXS_CONST uint kStatusRoundOverflow = 4;  // scheduling round limit hit

// Safety bound on scheduling rounds; a legal tile can never reach it.
NXS_CONST uint kMaxRounds = 1u << 20;

// Specialisation-constant IDs used by rans_decode.comp.
NXS_CONST uint kSpecIdReadPtrMode = 0;  // 0 = subgroup ballot, 1 = LDS fallback
NXS_CONST uint kSpecIdWorkgroupTiles = 1;

NXS_CONST uint kReadPtrBallot = 0;
NXS_CONST uint kReadPtrLdsFallback = 1;

// Dispatch shape: one workgroup is always kWorkgroupSize threads and handles
// TILES_PER_GROUP tiles of LANES lanes each, with TILES_PER_GROUP * LANES <=
// kWorkgroupSize and TILES_PER_GROUP <= kMaxSlots.  kTilesPerGroup is the
// v1-default shape (8 tiles x 8 lanes).
//
// [nxvc_vk_decoder glue, marked edit] LANES is specialisation constant 2 and
// no longer fixed at 8; see rans_decode.comp.  nxs_tiles_per_group() is the
// one place that derives the workgroup shape from a lane count.
NXS_CONST uint kTilesPerGroup = 8;
NXS_CONST uint kWorkgroupSize = 64;  // kTilesPerGroup * kLanes
NXS_CONST uint kMaxSlots = 8;        // shared-array slots per workgroup
NXS_CONST uint kMaxLanes = 32;       // nsub_log2 <= 5

NXS_FN uint nxs_tiles_per_group(uint lanes) {
    uint t = kWorkgroupSize / lanes;
    return t > kMaxSlots ? kMaxSlots : t;
}

// ---------------------------------------------------------------------------
// Derived helpers, valid in both languages.
// ---------------------------------------------------------------------------

// Scan-position band, ref/src/common.h band_of().
NXS_FN int nxs_band_of(int scan_pos) {
    if (scan_pos == 0) return 0;
    if (scan_pos < kBand1) return 1;
    if (scan_pos < kBand2) return 2;
    return 3;
}

// ref/src/common.h level_ctx() / level_class().
NXS_FN int nxs_level_class(int magnitude) {
    return magnitude == 0 ? 0 : (magnitude == 1 ? 1 : 2);
}
NXS_FN int nxs_level_ctx(int scan_pos, int prev_class) {
    return kCtxLevelBase + kLevelCtx[nxs_band_of(scan_pos) * 3 + prev_class];
}

// ref/src/tables.cpp last_class_of().
NXS_FN int nxs_last_class_of(int pos) {
    for (int c = kLastMaxClass; c >= 0; --c)
        if (pos >= kLastBase[c]) return c;
    return 0;
}

// ref/src/common.h scan_table(): which scan a unit of `ncoef` uses.
NXS_FN int nxs_scan_id(int ncoef, int tskip) {
    if (ncoef == 64) return tskip != 0 ? kScanRaster8 : kScanZigzag8;
    if (ncoef == 16) return kScanZigzag4;
    return kScanSmall;  // 4 or 1: identity
}

// ref/src/common.h tile_geom(): coded edge of plane `p`.
NXS_FN int nxs_plane_size(int p, int res_level, int chroma444) {
    if (p == 1 || p == 2) {
        int s = (chroma444 != 0 ? kTileSize : kTileSize / 2) >> res_level;
        return s < kMinChromaSize ? kMinChromaSize : s;
    }
    return kTileSize >> res_level;  // luma and alpha
}

// Byte offset of the rANS payload, given the tile header's byte offset and
// word1.  SYNTAX.md 4.1: optional MV and alpha bytes sit between the header
// and the payload.  v1 intra streams set neither, so this reduces to
// hdr_off + kTileHeaderBytes there.
NXS_FN uint nxs_tile_payload_offset(uint hdr_off, uint w1) {
    uint o = hdr_off + kTileHeaderBytes;
    if (((w1 >> kThMvPresentShift) & kThMvPresentMask) != 0u) o += kThMvBytes;
    if (((w1 >> kThAlphaModeShift) & kThAlphaModeMask) ==
        uint(kAlphaModeConstant))
        o += kThAlphaValueBytes;
    return o;
}

// Reserved-field validation, SYNTAX.md 4.1.  Returns 0 when the header is
// conforming.  (A bool return keeps GLSL and C++ in step less cleanly than
// an int, so this reports 1 for "reject".)
NXS_FN int nxs_tile_header_reserved_bad(uint w0, uint w1) {
    if ((w0 & kThReservedW0) != 0u) return 1;
    if ((w1 & kThReservedW1) != 0u) return 1;
    if (int((w1 >> kThModeShift) & kThModeMask) > kMaxMode) return 1;
    if (int((w1 >> kThResLevelShift) & kThResLevelMask) > kMaxResLevel) return 1;
    if (int((w1 >> kThAlphaModeShift) & kThAlphaModeMask) == 3) return 1;
    if (int((w1 >> kThNsubLog2Shift) & kThNsubLog2Mask) > kMaxNsubLog2) return 1;
    return 0;
}

// Number of entropy-coded planes in a tile.
NXS_FN int nxs_coded_planes(int frame_nplanes, int alpha_mode) {
    if (alpha_mode != kAlphaModeCoded) return frame_nplanes > 3 ? 3 : frame_nplanes;
    return frame_nplanes;
}

#if defined(__cplusplus)
}  // namespace nxwarp_passA
#endif

#endif  // NXWARP_PASSA_SYNTAX_CONSTANTS_H
