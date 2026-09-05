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
// [v3, docs/SYNTAX.md 9.3] the v2 context model, stream tool bit 21 CTX_V2:
// the DC plane gets its own CBF/LAST/LEVEL contexts (it is a dense
// low-frequency image, nothing like the sparse AC blocks whose statistics it
// used to share) and the intra mode symbol gets one of its own.  Contexts
// 0..11 keep their MEANING in both models but not their statistics, which is
// why ref/src/default_tables.inc carries two built-in families.
NXS_CONST int kNumCtxV1 = 12;
NXS_CONST int kCtxCbfDc = 12;
NXS_CONST int kCtxLastDc = 13;
NXS_CONST int kCtxLevelDc = 14;
NXS_CONST int kCtxMode = 15;
NXS_CONST int kNumCtxV2 = 16;
// [v3, docs/SYNTAX.md 9.9] the v3 context model, stream tool bit 25 CTX_V3
// (which requires CTX_V2).  It keeps v2's sixteen rows and adds eleven: CBF
// and LAST are additionally conditioned on the NEIGHBOUR CLASS this lane
// carries -- 0 nothing to condition on, 1 the previous unit was not coded,
// 2 coded and sparse (LAST < 4), 3 coded and dense -- and LEVEL splits the
// coefficient at scan position LAST (two bands) and the DC term of a DC
// plane.  The class is carried inside one plane's run of block units and
// reset at every plane boundary; a DC-plane unit neither publishes nor
// consumes it.
//
// The conditioning is per CODING UNIT -- the 8x8 coefficient group -- and
// never per transform block, so the kernel never has to know the transform
// size here.  A lane owns units l, l+N, l+2N, ..., so the unit the class
// describes is one the lane has already finished: two registers of per-lane
// state, no extra barrier and no cross-lane read.  For the ordinary tile
// (res_level 0, nsub_log2 3) a lane owns one column of blocks, so the class
// describes the block directly above.
//
//   ucls 0 = luma/alpha residual blocks, 1 = chroma blocks, 2 = the DC plane
//   base_cbf[ucls]  = {0, 1, 12}    base_last[ucls] = {2, 3, 13}
//   CBF   context = nbr == 0 ? base_cbf[ucls]
//                            : (ucls == 1 ? 19 : 16) + (nbr - 1)
//   LAST  context = nbr <  2 ? base_last[ucls] : (ucls == 1 ? 23 : 22)
//   LEVEL context = 24                       (DC plane, scan position 0)
//                 = 14                       (DC plane, elsewhere)
//                 = band < 2 ? 25 : 26       (at scan position LAST)
//                 = 4 + kLevelCtx[band][prev]            (otherwise)
//   MODE  context = 15, v2's row unchanged
NXS_CONST int kCtxCbfLumaN = 16;     // 16..18, + (nbr - 1)
NXS_CONST int kCtxCbfChromaN = 19;   // 19..21, + (nbr - 1)
NXS_CONST int kCtxLastLumaN = 22;
NXS_CONST int kCtxLastChromaN = 23;
NXS_CONST int kCtxLevelDc0 = 24;
NXS_CONST int kCtxLevelLastLo = 25;
NXS_CONST int kCtxLevelLastHi = 26;
NXS_CONST int kNbrDenseLast = 4;
NXS_CONST int kNumCtxV3 = 27;
// A unit's statistical class, derived by both sides from its position in the
// unit list and never transmitted.  [REF] ref/src/common.h kUcls*.
NXS_CONST int kUclsLuma = 0;
NXS_CONST int kUclsChroma = 1;
NXS_CONST int kUclsDc = 2;
// Storage stride of the cumulative-frequency table.  The host uploads one
// layout whichever model the stream selects and contexts past the coded count
// are simply never named, so the stride is the widest model this kernel
// implements -- **kNumCtxV3**, since [minor 6].  That takes the shared
// cumulative-frequency table from 8192 to 13824 bytes, which is the whole
// LDS cost of CTX_V3: the rest of it is two registers of per-lane state and
// the arithmetic below.  It MUST equal ref/src/common.h's kNumCtx, which is
// also the widest model's count; `vk.passA.ref_agreement` checks that.
NXS_CONST int kNumCtx = 27;
NXS_CONST int kNumSym = 16;

// [REF] ref/src/common.h kCtxNone: "no context selected" for a unit's LEVEL or
// MODE context.  Context 0 is kCtxCbfLuma, never a legal LEVEL or MODE
// context, so 0 is an unambiguous sentinel that cannot index out of bounds.
NXS_CONST int kCtxNone = 0;

// Static per-frame probability tables, up to 8 sets (tile header table_set).
NXS_CONST int kNumTableSets = 8;

// ---------------------------------------------------------------------------
// 2b. Directional intra (tool bit 17) and sign data hiding (tool bit 22)
//                                        [docs/SYNTAX.md 7.4, 9.6, 9.7]
// ---------------------------------------------------------------------------

// Nine per-8x8-block intra modes; mode 0 IS the v1 DC-plane prediction, which
// is what makes INTRA_DIR a strict superset.  [REF] ref/src/common.h.
NXS_CONST int kIntraDcPlane = 0;
NXS_CONST int kNumIntraModes = 9;
// [minor 6] INTRA_CFL, stream tool bit 24: chroma-from-luma is mode 9, a
// tenth entry in the CHROMA mode alphabet only.  The luma alphabet stays at
// nine, so the mode symbol's alphabet is per-plane.  [REF] ref/src/common.h
// kIntraCfl / kNumIntraModesCfl, docs/SYNTAX.md 7.7.
NXS_CONST int kIntraCfl = 9;
NXS_CONST int kNumIntraModesCfl = 10;

// Mode-unit coding.  Without CTX_V2 a mode is a 1-bit "is MPM" flag plus a
// 3-bit non-MPM index, both bypass; with CTX_V2 it is one symbol in context
// 15 over the alphabet 0..8.  [SYN] 9.6.
NXS_CONST int kModeFlagBits = 1;
NXS_CONST int kModeIdxBits = 3;

// [SYN] 9.7: a unit whose LAST is at scan position kSdhMinLast or beyond does
// not code the sign at that position; it is the parity of the sum of the
// unit's absolute levels.  [REF] ref/src/common.h kSdhMinLast.
NXS_CONST int kSdhMinLast = 4;

// Frame-uniform tool flags, delivered in Pass A's push constants rather than
// as specialisation constants so a frame that turns a tool on does not force a
// pipeline rebuild.  The host derives them from the stream's tool bits.
NXS_CONST uint kToolFlagCtxV2 = 1u;
NXS_CONST uint kToolFlagIntraDir = 2u;
NXS_CONST uint kToolFlagSignHide = 4u;
// [minor 6] CTX_V3, stream tool bit 25: the 27-context model.  The derivation
// is in nxs_v3_ctx_*() below and the per-lane neighbour class is two registers
// in the kernel.
NXS_CONST uint kToolFlagCtxV3 = 8u;
// [minor 6] XFORM_4X4_SPLIT, stream tool bit 19: a block unit whose CBF is 1
// codes a 1-bit split flag; when set the block is four 4x4 sub-blocks and its
// scan is kScan4Split.  Per FRAME the flag says the tool exists; whether a
// given TILE codes flags is tile-header word1 bit 28.
NXS_CONST uint kToolFlagSplit4 = 16u;
// [minor 6] INTRA_CFL, stream tool bit 24: the chroma mode alphabet is 10.
NXS_CONST uint kToolFlagCfl = 32u;
// [minor 6] XFORM_LARGE, stream tool bit 27: tiles may set xform_size != 0.
NXS_CONST uint kToolFlagXformLarge = 64u;
// TAB_V2 (tool bit 26) is a host-side flag only: it changes how the frame's
// transmitted table sets are parsed, never how a symbol is decoded, so the
// kernel receives the same cumulative-frequency upload either way.

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

// [minor 6] A unit of more than 64 coefficients -- a 16x16 or 32x32 block --
// reuses the SAME 64-position LAST class table and the SAME four LEVEL bands
// by coding them over its 64 equal-sized scan GROUPS: the class names the
// group and `last_shift` raw bypass bits name the position inside it, so
// `last = (base[class] << last_shift) + raw` and the LEVEL band of a position
// is the band of `pos >> last_shift`.  No new context and no new symbol exists
// at any transform size, which is what lets one trained set of frequencies
// serve all three (ref/RESULTS-xform-a.md 4.2 measured a per-size family and
// it loses).  [REF] ref/src/common.h last_shift_of(), docs/SYNTAX.md 9.2.1.
NXS_FN int nxs_last_shift_of(int ncoef) {
    int s = 0;
    while ((ncoef >> s) > 64) ++s;
    return s;
}

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
// 4b. ENTROPY_LITE, stream tool bit 24     [ref/src/entropy_lite.{h,cpp}]
// ===========================================================================
// A table-free entropy tool whose tile payload is fully parallel: no
// arithmetic coder, no probability tables, no serial state.  A tile is five
// byte-aligned sections -- H0, H1, P, S, B -- whose per-unit offsets follow
// from three prefix sums, so a lane can decode any unit on its own.
//
// The tile header's `table_set` is repurposed as the variant selector: the
// tool has no probability tables for the field to name.  Only kLiteFixed is
// implemented in Pass A; kLiteRice is rejected as an unsupported header.
NXS_CONST int kLiteFixed = 0;
NXS_CONST int kLiteRice = 1;

// FIXED: the 3-bit per-unit magnitude class -> field width, covering |q| in
// 1 .. 2^bits and coded as |q| - 1.  Class 0 is zero bits wide: a unit whose
// every nonzero is +-1 spends nothing at all on magnitudes.
NXS_ARRAY(int, kLiteMagBits, 8)
    0, 1, 2, 3, 4, 6, 8, 16
NXS_ARRAY_END

// Width of the class field itself, and of the RICE order that replaces it.
NXS_CONST int kLiteParamBits = 3;

// Units per group in the two-level coded-unit map (section H0).
NXS_CONST int kLiteCbfGroup = 16;

// [REF] entropy_lite.cpp: the FIXED body codes |q| - 1, and |q| <= 32767.
NXS_CONST uint kLiteMaxMag = 32766u;

// Bits the per-unit LAST field takes, given the unit's coefficient count.
// [REF] entropy_lite.h lite_last_bits().
NXS_FN int nxs_lite_last_bits(int ncoef) {
    int b = 0;
    while ((1 << b) < ncoef) ++b;
    return b;
}

// Round a bit count up to the next byte boundary; every section is padded.
NXS_FN uint nxs_align8(uint bits) { return (bits + 7u) & ~7u; }

// ===========================================================================
// 5. Scan orders                               [ref/src/tables.cpp]
// ===========================================================================

// scan_pos -> block-local coefficient index.
NXS_CONST int kScanZigzag8 = 0;  // 64 coefficients, transform
NXS_CONST int kScanRaster8 = 1;  // 64 coefficients, transform skip
NXS_CONST int kScanZigzag4 = 2;  // 16 coefficients (DC plane, nb == 4)
NXS_CONST int kScanSmall = 3;    // 4 or 1 coefficients, identity
// [minor 6] the 4x4-split scan: four concatenated 4x4 sub-blocks in raster
// sub-block order, each scanned in its own zigzag.  [REF] ref/src/tables.cpp
// kScan4Split, docs/SYNTAX.md 6.8.
NXS_CONST int kScan4Split = 4;
// [minor 6] XFORM_LARGE, tool bit 27: the zigzags of the 16x16 and 32x32
// transforms.  They are NOT tabulated.  A 1024-entry table would be 4 KiB of
// the shared scan array against the 1.25 KiB the four small scans take, on a
// kernel whose LDS is already 13.8 KiB of cumulative frequencies -- and the
// zigzag is a rule (ref/src/common.h build_zigzag), so both directions of it
// are closed-form arithmetic instead.  See nxs_zigzag_*() below.
NXS_CONST int kScanZigzag16 = 5;
NXS_CONST int kScanZigzag32 = 6;
NXS_CONST int kNumScans = 5;   // only the tabulated ones occupy s_scan
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

NXS_ARRAY(int, kScan4SplitTab, 64)
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 25, 18, 11, 19, 26, 27,
     4,  5, 12, 20, 13,  6,  7, 14, 21, 28, 29, 22, 15, 23, 30, 31,
    32, 33, 40, 48, 41, 34, 35, 42, 49, 56, 57, 50, 43, 51, 58, 59,
    36, 37, 44, 52, 45, 38, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
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
// [nxvc_vk_decoder glue, marked edit] wm_id: a per-tile override of the
// frame's weighting matrix, 0 = "use the frame's" (docs/SYNTAX.md 4.1, tool
// bit WM_ID).  It took bits 26-27, which used to be reserved.
NXS_CONST uint kThWmIdShift = 26, kThWmIdMask = 3u;
// [minor 6] word1 bit 28: this tile codes per-block 4x4 split flags.  Gated
// by tool bit 19 and meaningful ONLY at xform_size == 8; a tile with
// xform_size != 8 or tskip and split4x4 set is BITSTREAM (TOOLBITS.md 4.2).
NXS_CONST uint kThSplit4Shift = 28, kThSplit4Mask = 1u;
// [minor 6] word1 bits 29-30: the tile's transform size, 0 = 8x8, 1 = 16x16,
// 2 = 32x32, 3 reserved.  Gated by tool bit 27 XFORM_LARGE.
NXS_CONST uint kThXformSizeShift = 29, kThXformSizeMask = 3u;

NXS_CONST int kMaxResLevel = 2;
NXS_CONST int kMaxMode = 4;
// [SYN] 4.1 tile modes.  Only INTRA is named here, because it is the only one
// the entropy layer has to distinguish: the mode unit of 9.6 exists on an
// INTRA tile and nowhere else ([SYN] 13.3).
NXS_CONST int kTileModeIntra = 3;
NXS_CONST int kMaxNsubLog2 = 5;
NXS_CONST int kAlphaModeConstant = 1;  // a single alpha byte follows
NXS_CONST int kAlphaModeCoded = 2;     // alpha plane is entropy-coded

// Reserved bits that a conforming stream sets to zero (SYNTAX.md 4.1).
NXS_CONST uint kThReservedW0 = 1u << 3;
// [minor 6] word1 bit 28 is split4x4 and 29-30 xform_size.
// [inter] ... and 31 is quad_mv ([SYN] 13.10).  Word1 now has NO reserved bits
// left, which is why the reserved-bit rejection vector r09 moved to word0
// bit 3.  docs/TOOLBITS.md 4.1.
NXS_CONST uint kThReservedW1 = 0u;

// Optional bytes between the 8-byte header and the rANS payload, in order:
//   1. i8 mv_x, i8 mv_y   if mv_present   (or u16 disparity when mode is
//                                          STEREO -- the same two bytes)
//   2. 4 quadrant-vector bytes            if quad_mv ([SYN] 13.10)
//   3. u8 alpha_value                     if alpha_mode == 1
NXS_CONST uint kThMvBytes = 2;
NXS_CONST uint kThQuadBytes = 4;
NXS_CONST uint kThAlphaValueBytes = 1;
NXS_CONST uint kThQuadShift = 31, kThQuadMask = 1u;

// ===========================================================================
// 7. Tile geometry and unit order   [ref/src/common.h tile_geom,
//                                    ref/src/codec.cpp build_units]
// ===========================================================================

NXS_CONST int kTileSize = 64;
NXS_CONST int kBlockSize = 8;
// [minor 6] XFORM_LARGE (tool bit 27), tile-header word1 bits 29-30: the
// tile's transform edge is 8 << xform_size, capped by the plane's own coded
// extent so no plane ever carries a block larger than itself.  Value 3 is
// reserved.  [REF] ref/src/common.h xform_edge / block_edge_for,
// docs/SYNTAX.md 4.1 and 6.7.
NXS_CONST int kMaxXformSize = 2;
NXS_FN int nxs_xform_edge(int xform_size) { return 8 << xform_size; }
NXS_FN int nxs_block_edge_for(int xform_size, int plane_size) {
    int e = nxs_xform_edge(xform_size);
    return e < plane_size ? e : plane_size;
}
NXS_FN int nxs_log2_of(int v) {
    int k = 0;
    while ((1 << k) < v) ++k;
    return k;
}
NXS_CONST int kCoefPerBlock = 64;
NXS_CONST int kMaxPlanes = 4;
NXS_CONST int kMaxBlocksPerEdge = 8;   // 64 / 8
NXS_CONST int kMinChromaSize = 8;

// Units of one plane, in coding order: first ONE DC-plane unit carrying
// nb*nb coefficients, then -- when INTRA_DIR is on -- ONE mode unit carrying
// nb*nb intra modes, then nb*nb block units of 64 coefficients each.
// Planes follow in order Y, Co(U), Cg(V), A.  [SYN] 9.1.
NXS_CONST int kUnitsPerPlaneExtra = 1;      // the DC-plane unit
NXS_CONST int kUnitsPerPlaneExtraDir = 2;   // ... plus the mode unit

// Largest possible unit count for one tile: 4 planes x (1 + 1 + 64).
NXS_CONST int kMaxUnitsPerTile = 264;

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

// One CBF bit per unit, bit index == unit index, LSB first.  A mode unit
// codes no CBF and its bit is always 0.
NXS_CONST uint kCbfWordsPerTile = 16;  // 512 bits >= kMaxUnitsPerTile

// ---------------------------------------------------------------------------
// Sparse coefficient transfer                     [PAPER 3.2.5, ADR 0026]
// ---------------------------------------------------------------------------
// The dense layout writes every one of a tile's kCoefStrideMax int16 slots --
// 12.5 KB for a 4:2:0 tile -- whatever the payload said, which is why Pass B's
// cost has no slope against payload size.  The sparse layout changes two
// things and nothing else:
//
//   1. inside a unit, coefficient `k` is stored at slot `k` in SCAN order
//      rather than at `scan_index(scan_id, k)` in raster order.  The unit's
//      base and its reserved width are unchanged, so every unit still lives
//      exactly where the dense layout put it and `coef_stride` does not move;
//   2. Pass A writes one length per unit -- LAST + 1, or 0 for an uncoded
//      unit -- and touches slots [0, LAST] only.  Slots past LAST are never
//      written and never read, so the region is not zeroed either.
//
// Together those make the bytes that cross between the passes proportional to
// the coefficients the stream actually coded.  Nothing about the bitstream,
// the coefficient values or Pass B's output changes: LAST is already in the
// syntax (9.2) and the scan is already normative (5), so the layout is a pure
// re-indexing of the same numbers.
//
// One byte per unit, four units per uint, index == unit index.  The byte is
// LAST + 1 in the unit's scan, so 0..kCoefPerBlock; 0 means CBF == 0.  Lanes
// interleave over units (unit `u` belongs to lane `u % LANES`), so four
// neighbouring units belong to four different lanes and the write must be an
// atomicOr into a pre-zeroed word.
// [minor 6] The field width follows the TILE's transform size, and the region
// does not grow.  A 32x32 unit's LAST + 1 reaches 1024 and would wrap in a
// byte, so a tile with a transform larger than 8x8 packs TWO 16-bit fields to
// a uint instead of four 8-bit ones -- and it can afford to, because the same
// thing that makes its units big makes them few: 18 units per 64-edge plane at
// 16x16 against 66 at 8x8, so at most 72 units against 264.  Widening the
// field unconditionally would instead have taken the shared accumulator from
// 8.4 to 16.9 KiB on top of CTX_V3's 13.8, which does not fit an Adreno 650's
// 32 KiB at 32 tiles per group.  nxs_unit_len_fits() is the header check that
// keeps the bound a rule rather than an assumption.
NXS_CONST uint kUnitLensPerWord = 4;      // 8x8 tiles
NXS_CONST uint kUnitLenBits = 8;
NXS_CONST uint kUnitLenMask = 255u;
NXS_CONST uint kUnitLensPerWordLarge = 2; // 16x16 and 32x32 tiles
NXS_CONST uint kUnitLenBitsLarge = 16;
NXS_CONST uint kUnitLenMaskLarge = 65535u;
NXS_CONST uint kUnitLenWordsPerTile = 66;  // ceil(kMaxUnitsPerTile / 4)
// Units a large-transform tile may have before its 16-bit fields overflow the
// region.  A conforming stream cannot exceed it; the check is there so that
// a malformed one is refused rather than corrupting a neighbour's lengths.
NXS_CONST int kMaxUnitsPerTileLarge = 132;
NXS_FN int nxs_unit_lens_per_word(int xform_size) {
    return xform_size != 0 ? int(kUnitLensPerWordLarge) : int(kUnitLensPerWord);
}
NXS_FN int nxs_unit_len_bits(int xform_size) {
    return xform_size != 0 ? int(kUnitLenBitsLarge) : int(kUnitLenBits);
}
NXS_FN uint nxs_unit_len_mask(int xform_size) {
    return xform_size != 0 ? kUnitLenMaskLarge : kUnitLenMask;
}

// [v3] Per-block intra modes, the second thing Pass A produces for Pass B.
// One 4-bit field per block (modes are 0..8), 8 fields per uint.  A plane's
// region is a fixed kModesPerPlane slots so that each plane starts on a uint
// boundary and no two planes ever share a word -- which is what lets the one
// lane that owns a plane's mode unit read-modify-write its words with no
// atomic and no cross-lane ordering.
NXS_CONST uint kModeBits = 4;
NXS_CONST uint kModeMask = 15u;
NXS_CONST uint kModesPerUint = 8;
NXS_CONST uint kModesPerPlane = 64;   // nb*nb <= 8*8
NXS_CONST uint kModeWordsPerPlane = 8;   // kModesPerPlane / kModesPerUint
NXS_CONST uint kModeWordsPerTile = 32;   // kMaxPlanes * kModeWordsPerPlane

// [minor 6] The per-block 4x4 SPLIT flags ride in the same per-tile region,
// immediately after the mode words: one BIT per block, 32 blocks to a uint,
// two uints per plane.  They are one bit rather than a fifth mode field
// because a split flag exists whether or not INTRA_DIR does -- nothing in the
// syntax ties tool bit 19 to tool bit 17 -- so it cannot live inside a
// structure the mode unit owns.
//
// Unlike a mode word, a split word IS shared between lanes: block `b` of a
// plane is decoded by lane `b % LANES`, so all 8 (or 32) lanes write bits of
// the same word.  The write is an atomicOr into a pre-zeroed word, exactly as
// the unit-length words are, and for the same reason.
NXS_CONST uint kSplitWordsPerPlane = 2;  // 64 blocks, 32 to a uint
NXS_CONST uint kSplitWordsPerTile = 8;   // kMaxPlanes * kSplitWordsPerPlane
NXS_CONST uint kModeRegionUints = 40;    // kModeWordsPerTile + kSplitWordsPerTile

// Per-tile descriptor handed to the shader (uints).
//   0: byte offset of the 8-byte tile header inside the bitstream buffer
//   1: byte length of header + payload
//   2: int16 index of this tile's coefficient region
//   3: uint index of this tile's CBF words
//   4: uint index of this tile's intra-mode words        [v3]
//   5: uint index of this tile's unit-length words       [sparse]
//   6..7: reserved, zero.  The descriptor is padded to a power of two so the
//         shader addresses it with a shift.
NXS_CONST uint kTileDescUints = 8;
NXS_CONST uint kTdBitsOffset = 0;
NXS_CONST uint kTdBitsLength = 1;
NXS_CONST uint kTdCoefOffset = 2;
NXS_CONST uint kTdCbfOffset = 3;
NXS_CONST uint kTdModeOffset = 4;
NXS_CONST uint kTdUnitLenOffset = 5;  // uint index of the tile's length words

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
// [entropy-lite] 3: which entropy tool the dispatch decodes.  main() branches
// on it at the very top, so the branch is dynamically uniform and each path's
// barriers stay in uniform control flow -- the same discipline READ_PTR_MODE
// follows.  Specialisation constant 2 is LANES.
NXS_CONST uint kSpecIdEntropyMode = 3;
// [minor 6] 4: the stride of the shared cumulative-frequency table, kNumCtxV2
// or kNumCtxV3.  It SIZES the array, so a v1 or v2 frame keeps the 8192-byte
// table it always had instead of paying CTX_V3's 13824 for a model it does
// not use -- which on an Adreno 650 is the difference between two resident
// workgroups and one.
NXS_CONST uint kSpecIdCtxStride = 4;
// [minor 6] 5: does the frame set XFORM_LARGE?  It gates the two computed
// zigzags, which the DENSE coefficient layout needs and nothing else does --
// and which, left in the binary, cost 844 instructions of Pass A and about
// 10 ms of it on an Adreno 650, on every stream, for a path no conforming
// sparse-layout stream can reach.  Dead code is not free on this part.
NXS_CONST uint kSpecIdXformLarge = 5;

NXS_CONST uint kReadPtrBallot = 0;
NXS_CONST uint kReadPtrLdsFallback = 1;

NXS_CONST uint kEntropyRans = 0;       // interleaved rANS, docs/SYNTAX.md 9
NXS_CONST uint kEntropyLiteFixed = 1;  // ENTROPY_LITE, kLiteFixed variant

#ifndef NXVW_PASSA_TILES_PER_GROUP
#define NXVW_PASSA_TILES_PER_GROUP 32
#endif

// [entropy-lite] Dispatch shape of the Lite path: ONE workgroup of
// kWorkgroupSize threads per tile, unit `u` handled by thread `u %
// kWorkgroupSize`.  The per-unit LDS arrays are padded to a whole number of
// units per thread so the workgroup scan can give each thread one contiguous
// block; kLiteUnitsPad must be >= kMaxUnitsPerTile.
//
// Both numbers FOLLOW the workgroup shape and must not be written out: they
// were the literals 5 and 320, correct at kWorkgroupSize == 64, and stayed
// behind when NXVW_PASSA_TPG went to 32 tiles per group and the workgroup to
// 256 threads.  lite_scan() then indexed `tid * 5 + k` up to 1280 into an
// array of 320 and every Lite decode segfaulted in the CPU model and read out
// of bounds on the GPU.  Same lesson as nxs_desc_slots(): derive the bound
// from the shape rather than remember it.
// (kWorkgroupSize itself is declared below, with the rest of the dispatch
// shape; the macro it comes from is the one thing available this early.)
NXS_CONST int kLiteWgSize = NXVW_PASSA_TILES_PER_GROUP * 8;
NXS_CONST int kLiteUnitsPerThread =
    (kMaxUnitsPerTile + kLiteWgSize - 1) / kLiteWgSize;
NXS_CONST int kLiteUnitsPad = kLiteWgSize * kLiteUnitsPerThread;
// Groups of kLiteCbfGroup units in section H0: ceil(kMaxUnitsPerTile / 16).
NXS_CONST int kLiteMaxGroups = 17;

// Dispatch shape: one workgroup is always kWorkgroupSize threads and handles
// TILES_PER_GROUP tiles of LANES lanes each, with TILES_PER_GROUP * LANES <=
// kWorkgroupSize and TILES_PER_GROUP <= kMaxSlots.  kTilesPerGroup is the
// build's default shape; LANES follows the tile's nsub_log2.
//
// [nxvc_vk_decoder glue, marked edit] LANES is specialisation constant 2 and
// no longer fixed at 8; see rans_decode.comp.  nxs_tiles_per_group() is the
// one place that derives the workgroup shape from a lane count.
// The workgroup shape is a build constant so it can be measured.  It was
// 8 tiles x 8 lanes = 64 threads, one wave64.  The kernel's LDS is dominated
// by two per-workgroup tables that do not grow with the tile count -- the 8 KB
// cumulative-frequency sets and the 1 KB scan tables -- so widening the
// workgroup amortises them and is the only lever on occupancy this kernel has:
// at 64 threads and 12 KB, an Adreno 650 SP with 32 KB of LDS holds two
// workgroups, which is two waves and no latency hiding at all on a kernel
// whose inner loop is a dependent chain of shared-memory reads.
//
// Measured on a Pico 4, 2048 tiles, best of 12 with a cooldown either side:
// 8 -> 16 -> 32 tiles per group is 153.7 -> 102.5 -> 79.8 ms at QP 24 and
// 26.7 -> 12.8 -> 10.1 at QP 36.  32 was blocked until the descriptor array
// was sized from this number (nxs_desc_slots) instead of a fixed allowance.
// 64 hangs the device and is not a supported value.
#ifndef NXVW_PASSA_TILES_PER_GROUP
#define NXVW_PASSA_TILES_PER_GROUP 32
#endif
NXS_CONST uint kTilesPerGroup = NXVW_PASSA_TILES_PER_GROUP;
NXS_CONST uint kWorkgroupSize = NXVW_PASSA_TILES_PER_GROUP * 8u;
NXS_CONST uint kMaxSlots = NXVW_PASSA_TILES_PER_GROUP;
NXS_CONST uint kMaxLanes = 32;       // nsub_log2 <= 5

NXS_FN uint nxs_tiles_per_group(uint lanes) {
    uint t = kWorkgroupSize / lanes;
    return t > kMaxSlots ? kMaxSlots : t;
}

// Descriptor slots a frame of `ntiles` tiles can occupy.  The tiles are sorted
// into one group per distinct nsub_log2 and each group is aligned up to its
// own tiles-per-group so vkCmdDispatchBase can address it, so a frame that
// uses all six lane counts pays up to tpg-1 padding slots per group.  The
// descriptor and status buffers are indexed by descriptor slot, not by tile,
// and must be sized from this rather than from the tile count; it grows with
// the workgroup shape (76 slots of slack at 16 tiles per group, 152 at 32),
// which is what a hard-coded allowance got wrong.
NXS_FN uint nxs_desc_slack(void) {
    uint n = 0u;
    for (uint ns = 0u; ns <= 5u; ++ns) n += nxs_tiles_per_group(1u << ns) - 1u;
    return n;
}
NXS_FN uint nxs_desc_slots(uint ntiles) { return ntiles + nxs_desc_slack(); }

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

// ---------------------------------------------------- v3 context derivation
// [REF] ref/src/common.h v3_ctx_cbf / v3_ctx_last / v3_ctx_level.  These three
// are the ONLY places a v3 context is chosen, and each is arithmetic over the
// unit's class and the lane's neighbour class rather than a ladder on what the
// v2 context happened to be.
//
// `nbr` is the class this lane's PREVIOUS coefficient unit in the same group
// published: 0 nothing to condition on, 1 uncoded, 2 coded and sparse, 3 coded
// and dense.  It is per-lane state, written once per unit and read once per
// unit, so there is no cross-lane read and no extra barrier.
NXS_FN int nxs_v3_ctx_cbf(int ucls, int nbr) {
    if (nbr == 0) return ucls == kUclsDc ? kCtxCbfDc
                         : (ucls == kUclsChroma ? kCtxCbfChroma : kCtxCbfLuma);
    return (ucls == kUclsChroma ? kCtxCbfChromaN : kCtxCbfLumaN) + (nbr - 1);
}
// LAST splits coded from not-coded only: the sparse/dense distinction pays on
// CBF, where it says how likely a coefficient is at all, and not on LAST,
// where the unit's own magnitudes already say it.
NXS_FN int nxs_v3_ctx_last(int ucls, int nbr) {
    if (nbr < 2) return ucls == kUclsDc ? kCtxLastDc
                        : (ucls == kUclsChroma ? kCtxLastChroma : kCtxLastLuma);
    return ucls == kUclsChroma ? kCtxLastChromaN : kCtxLastLumaN;
}

// The neighbour class a finished coefficient unit publishes to its lane.
NXS_FN int nxs_nbr_class_of(int cbf, int last) {
    if (cbf == 0) return 1;
    return last < kNbrDenseLast ? 2 : 3;
}

// ref/src/common.h level_ctx() / level_class().
NXS_FN int nxs_level_class(int magnitude) {
    return magnitude == 0 ? 0 : (magnitude == 1 ? 1 : 2);
}
// [minor 6] ref/src/common.h band_pos(): in a 4x4-split block the 64 scan
// positions are four concatenated sub-blocks, so a position's frequency band
// is its position WITHIN its sub-block.
NXS_FN int nxs_band_pos(int scan_pos, int split) {
    return split != 0 ? (scan_pos & 15) : scan_pos;
}
NXS_FN int nxs_level_ctx(int scan_pos, int prev_class) {
    return kCtxLevelBase + kLevelCtx[nxs_band_of(scan_pos) * 3 + prev_class];
}

// [REF] ref/src/common.h v3_ctx_level().  LEVEL is NOT conditioned on the
// neighbour -- the previously decoded level inside the same unit already
// carries that, and about this unit rather than the one before it.  It does
// split the coefficient at scan position LAST, which is nonzero by
// construction, and it gives the DC term of a DC plane its own row.
// `band_scan_pos` is the position after the band mappings of 6.8 and 9.3.1;
// `scan_pos` and `last` are raw positions in the unit.
NXS_FN int nxs_v3_ctx_level(int ucls, int scan_pos, int band_scan_pos, int last,
                            int prev_class) {
    if (ucls == kUclsDc) return scan_pos == 0 ? kCtxLevelDc0 : kCtxLevelDc;
    if (scan_pos == last)
        return nxs_band_of(band_scan_pos) < 2 ? kCtxLevelLastLo
                                              : kCtxLevelLastHi;
    return nxs_level_ctx(band_scan_pos, prev_class);
}

// ref/src/tables.cpp last_class_of().
NXS_FN int nxs_last_class_of(int pos) {
    for (int c = kLastMaxClass; c >= 0; --c)
        if (pos >= kLastBase[c]) return c;
    return 0;
}

// [minor 6] The zigzag of an `edge` x `edge` block, as a rule rather than a
// table.  [REF] ref/src/common.h build_zigzag(): diagonal `s` runs from
// (s, 0) upwards when `s` is even and from (0, s) downwards when it is odd,
// clipped to the block, writing the raster index u * edge + v with u vertical.
//
// Both directions are needed and both are closed form.  Diagonal `s` holds
// `hi - lo + 1` positions with lo = max(s - edge + 1, 0) and
// hi = min(s, edge - 1), so the number of positions before it is
//
//     s <= edge - 1 :  s * (s + 1) / 2
//     otherwise     :  edge * edge - (2 * edge - 1 - s) * (2 * edge - s) / 2
//
// which is the triangle from either end.  Raster -> scan needs nothing else;
// scan -> raster inverts the count, which is one short loop over the
// diagonals and is only ever walked by the DENSE coefficient layout (the
// sparse one stores a coefficient at its scan position and never asks).
NXS_FN int nxs_zigzag_before(int edge, int s) {
    if (s <= edge - 1) return s * (s + 1) / 2;
    int t = 2 * edge - 1 - s;
    return edge * edge - t * (t + 1) / 2;
}
// Raster index (u, v) of an edge x edge block -> its scan position.
NXS_FN int nxs_zigzag_raster_to_pos(int edge, int raster) {
    int u = raster / edge, v = raster - u * edge;
    int s = u + v;
    int lo = s - edge + 1 > 0 ? s - edge + 1 : 0;
    int hi = s < edge - 1 ? s : edge - 1;
    int idx = (s & 1) == 0 ? hi - u : u - lo;
    return nxs_zigzag_before(edge, s) + idx;
}
// ... and its inverse, scan position -> raster index, by walking the
// diagonals.  A closed form was written and measured -- the counts are
// triangles from either end, so one float square root and four integer
// corrections invert them exactly -- and on an Adreno 650 it was *worse*:
// 1587 instructions of Pass A against the walk's 844, and 26.3 ms against
// 22.0.  The walk stays.
//
// What actually mattered is that neither spelling is compiled at all unless
// the frame sets XFORM_LARGE; see XFORM_LARGE_TOOL in rans_decode.comp.
NXS_FN int nxs_zigzag_pos_to_raster(int edge, int pos) {
    int s = 0;
    // At most 2 * edge - 1 diagonals; the loop is bounded and side-effect free.
    for (int k = 0; k <= 2 * (edge - 1); ++k)
        if (nxs_zigzag_before(edge, k) <= pos) s = k;
    int lo = s - edge + 1 > 0 ? s - edge + 1 : 0;
    int hi = s < edge - 1 ? s : edge - 1;
    int idx = pos - nxs_zigzag_before(edge, s);
    int u = (s & 1) == 0 ? hi - idx : lo + idx;
    return u * edge + (s - u);
}

// ref/src/common.h scan_table(): which scan a unit of `ncoef` uses.
//
// [minor 6] The 256- and 1024-coefficient units of XFORM_LARGE take the two
// computed zigzags; every other unit takes a tabulated one.  Transform skip is
// mutually exclusive with a transform size other than 8x8, so the tskip arm
// only ever sees 64.
NXS_FN int nxs_scan_id(int ncoef, int tskip) {
    if (ncoef == 1024) return kScanZigzag32;
    if (ncoef == 256) return kScanZigzag16;
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
    if (((w1 >> kThQuadShift) & kThQuadMask) != 0u) o += kThQuadBytes;
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

// ---------------------------------------------------------------------------
// Directional-intra helpers.  [REF] ref/src/entropy.cpp mpm_of/nonmpm_mode.
// ---------------------------------------------------------------------------

// The most probable mode from the already-decoded left and above neighbours.
// Both live in the SAME mode unit, so the derivation only ever reads values
// this lane has already produced, whatever the interleaved lane schedule does
// with the other units ([SYN] 9.1, and why the modes are a unit of their own).
NXS_FN int nxs_mpm(int left, int above) {
    return left == above ? left : (left < above ? left : above);
}

// The `idx`-th of the eight modes OTHER than `mpm`, in ascending mode order.
NXS_FN int nxs_nonmpm_mode_n(int mpm, int idx, int nmodes) {
    int n = 0;
    for (int m = 0; m < nmodes; ++m) {
        if (m == mpm) continue;
        if (n == idx) return m;
        ++n;
    }
    return kIntraDcPlane;
}
NXS_FN int nxs_nonmpm_mode(int mpm, int idx) {
    int n = 0;
    for (int m = 0; m < kNumIntraModes; ++m) {
        if (m == mpm) continue;
        if (n == idx) return m;
        ++n;
    }
    return kIntraDcPlane;
}

// Packed mode array addressing: word and shift of block `b` of plane `p`
// inside a tile's kModeWordsPerTile-uint region.
NXS_FN int nxs_mode_word(int p, int b) {
    return p * int(kModeWordsPerPlane) + (b / int(kModesPerUint));
}
NXS_FN int nxs_mode_shift(int b) {
    return (b % int(kModesPerUint)) * int(kModeBits);
}

// [minor 6] Split-flag addressing inside the same per-tile region.
NXS_FN int nxs_split_word(int p, int b) {
    return int(kModeWordsPerTile) + p * int(kSplitWordsPerPlane) + (b / 32);
}
NXS_FN int nxs_split_shift(int b) { return b % 32; }

// Number of entropy-coded planes in a tile.
NXS_FN int nxs_coded_planes(int frame_nplanes, int alpha_mode) {
    if (alpha_mode != kAlphaModeCoded) return frame_nplanes > 3 ? 3 : frame_nplanes;
    return frame_nplanes;
}

#if defined(__cplusplus)
}  // namespace nxwarp_passA
#endif

#endif  // NXWARP_PASSA_SYNTAX_CONSTANTS_H
