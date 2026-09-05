// NX Warp Pass B -- the Pass A -> Pass B interface.
//
// Shared verbatim by reconstruct.comp, passB_model.cpp and the host harness,
// so there is exactly one description of the layout.  It follows PAPER 3.2.2
// ("coefficients as int16 in block-raster order ... and a 16-byte tile
// record") and the coefficient ordering of the CPU reference
// (ref/src/codec_impl.inc plane_coef_offset / ref/src/codec.cpp
// TileCoder::build_units).
//
// ---------------------------------------------------------------- buffers
//
//  binding 0  Coef      readonly  int16[] packed two per uint, tile stride
//                                 `coefStrideI16` elements (see below)
//  binding 1  TileRecs  readonly  NxvwTileRec[tileCount]
//  binding 2  Weights   readonly  int[512]: four 128-entry sets of 64 luma
//                                 then 64 chroma weights, Q4.  Set 0 is the
//                                 frame's pair; sets 1..3 are the built-in
//                                 pairs a tile's wm_id selects.
//  binding 3  uOutRgba8      writeonly  rgba8ui  storage image
//  binding 4  uOutRgb10a2    writeonly  rgb10_a2ui storage image
//  binding 5  uOutLuma       writeonly  r8ui  storage image
//  binding 6  uOutCbCr       writeonly  rg8ui storage image
//  binding 7  Modes     readonly  [v3] uint[]: kNxvwModeWordsPerTile packed
//                                 4-bit per-8x8-block intra modes per tile,
//                                 written by Pass A (SYNTAX.md 7.4 / 9.6)
//  binding 9  UnitLens  readonly  [sparse] uint[]: one byte per coding unit,
//                                 NXVW_UNIT_LEN_WORDS_PER_TILE uints per
//                                 tile, holding LAST + 1 (0 = not coded)
//  binding 8  TileOrder readonly  uint[]: workgroup index -> tile index.  The
//                                 host may permute it to group tiles of like
//                                 shape into adjacent workgroups; the output
//                                 is identical either way because every write
//                                 address is derived from the tile index.
//
// The inter predictor's WarpRecs buffer (3.2.3 step 5) takes the next free
// binding when it lands; it is unbound in v1.
//
// ------------------------------------------------------- coefficient order
// Inside one tile, for each coded plane p (0=Y/R, 1=Co/G, 2=Cg/B, 3=alpha;
// alpha is present only when alpha_mode == 2):
//
//     [ nb*nb   DC-plane coefficients, raster order over the block grid ]
//     [ nb*nb blocks x 64 coefficients, raster order inside the block,
//       blocks in raster order over the tile                            ]
//
// with nb = coded_size / 8 for that plane and tile (see tile geometry below).
// Planes are concatenated in index order with no padding, exactly as
// plane_coef_offset() computes it.  Note that a plane base can land on an odd
// int16 index (nb == 1 gives 65 elements per plane); the packing helpers below
// therefore address individual int16 elements, never uint pairs.
//
// The per-tile stride is fixed for the whole dispatch and sized for
// res_level 0, so a tile coded at 32 or 16 simply uses a prefix of its slot.
// The host computes it with nxvw_coef_stride_i16() below.
//
// --------------------------------------------------------- tile geometry
// [REF] ref/src/common.h tile_geom():
//     coded_size  = 64 >> res_level
//     chroma_size = max((chroma444 ? 64 : 32) >> res_level, 8)
//     alpha_size  = coded_size
//
// ------------------------------------------------------------ tile record
// 16 bytes.  w0 and w1 are the two tile-header words of PAPER 1.2 copied
// through verbatim by Pass A, which keeps Pass B's field extraction identical
// to the CPU reference's unpack_tile_header().  w2 carries the few values that
// the header does not hold directly.  w3 is the inter/warp hook.

#ifndef NXVW_PASSB_LAYOUT_H
#define NXVW_PASSB_LAYOUT_H

#ifdef __cplusplus
#include <cstdint>
namespace nxvw {
using uint = uint32_t;
#endif

struct NxvwTileRec {
    uint w0;  // layer(2) eye(1) rsvd(1) tile_index(12) payload_len(16)
    uint w1;  // mode(3) res_level(2) chroma444(1) alpha_mode(2) qp_delta(6)
              // table_set(3) nsub_log2(3) mv_present(1) ref_sel(2) tskip(1)
              // wgt(2)
    uint w2;  // alpha_value(8) | present(1)<<8 ; bits 9..31 reserved, zero
    uint w3;  // reserved for the warp predictor: index into WarpRecs, or
              // 0xffffffff for "no warp record".  Ignored in v1.
};

// ---------------------------------------------------------- field accessors
// (functions, not macros, so both compilers type-check them)

#ifdef __cplusplus
#define NXVW_FN inline int
#define NXVW_FNU inline uint
#else
#define NXVW_FN int
#define NXVW_FNU uint
#endif

NXVW_FN nxvw_rec_mode(uint w1) { return int(w1 & 7u); }
NXVW_FN nxvw_rec_res_level(uint w1) { return int((w1 >> 3) & 3u); }
NXVW_FN nxvw_rec_chroma444(uint w1) { return int((w1 >> 5) & 1u); }
NXVW_FN nxvw_rec_alpha_mode(uint w1) { return int((w1 >> 6) & 3u); }
NXVW_FN nxvw_rec_qp_delta(uint w1) {
    int q = int((w1 >> 8) & 0x3fu);
    return q >= 32 ? q - 64 : q;
}
NXVW_FN nxvw_rec_tskip(uint w1) { return int((w1 >> 23) & 1u); }
// [minor 6] word1 bit 28 `split4x4` and bits 29-30 `xform_size`.  Pass B reads
// the split flags themselves out of the mode region, but it needs the
// transform size directly: it sets the block grid, and it also selects how
// wide a unit-length field is (see NXVW_UNIT_LEN_BITS_LARGE).
NXVW_FN nxvw_rec_xform_size(uint w1) { return int((w1 >> 29) & 3u); }
// [nxvc_vk_decoder glue, marked edit] per-tile weighting-matrix override,
// docs/SYNTAX.md 4.1 bits 26-27: 0 means "the frame's matrices", 1..3 name a
// built-in pair for this tile alone (the degradation ladder's step 1).
NXVW_FN nxvw_rec_wm_id(uint w1) { return int((w1 >> 26) & 3u); }
NXVW_FN nxvw_rec_alpha_value(uint w2) { return int(w2 & 0xffu); }
NXVW_FN nxvw_rec_present(uint w2) { return int((w2 >> 8) & 1u); }

// ------------------------------------------------------------ tile geometry
NXVW_FN nxvw_coded_size(int res_level) { return 64 >> res_level; }
NXVW_FN nxvw_chroma_size(int res_level, int chroma444) {
    int s = (chroma444 != 0 ? 64 : 32) >> res_level;
    return s < 8 ? 8 : s;
}
// Coded edge of plane p for this tile.
NXVW_FN nxvw_plane_size(int p, int res_level, int chroma444) {
    return (p == 1 || p == 2) ? nxvw_chroma_size(res_level, chroma444)
                              : nxvw_coded_size(res_level);
}
// Full (display) edge of plane p inside the 64x64 tile.  Pass B always
// resamples every plane up to 64x64 because it writes a packed display image,
// which is where it differs from the reference's planar 4:2:0 output.
NXVW_FN nxvw_plane_full(int p) { return 64; }

// Number of int16 elements one plane occupies, given its coded edge.
NXVW_FN nxvw_plane_coef_count(int size) {
    int nb = size >> 3;
    return nb * nb * 65;
}

// ---------------------------------------------------------- push constants
// All scalars so that the std430 / push-constant layout is trivially the same
// on both sides.
struct NxvwPassBPush {
    int imageW;        // luma pixels
    int imageH;
    int tilesX;
    int baseQp;        // frame header base_qp, 0..63
    int chromaQpOff;   // frame header chroma_qp_off
    int alphaQpOff;    // frame header alpha_qp_off
    int coefStrideI16; // int16 elements per tile slot in the Coef buffer
    int colorTransform;// 0 = none (planes are display planes), 1 = YCoCg-R
    int chroma420;     // 1 = stream chroma is 4:2:0
    int alphaPresent;  // 1 = the stream has an alpha plane
    int planeWords0;   // uint offset of plane p inside the shared sample store
    int planeWords1;   // (host-computed so the shader needs no divides)
    int planeWords2;
    int planeWords3;
    // [v3] stream tool bit 17: every 8x8 block carries an intra mode and the
    // blocks of a plane are reconstructed in raster order (SYNTAX.md 7.4).
    int intraDir;
    // [v3] frame-header flags bit 2: the layered form, in which the modes
    // predict the DC-plane residual rather than the samples (SYNTAX.md 7.5).
    int dirLayer;
    // [sparse] 1 = scan-order slots plus the per-unit lengths at binding 9;
    // 0 = the dense raster-order layout with no lengths.  Must match Pass A's
    // `sparse` push constant for the same frame.  The GPU kernel takes it as
    // specialization constant 4 rather than from here -- it sits inside the
    // innermost coefficient loop -- so this field is what the CPU model reads
    // and what the host mirrors into the specialization info.
    int sparse;
};

// ------------------------------------------------------------- intra modes
// [v3] Pass A writes one 4-bit mode per 8x8 block, 8 per uint, with each
// plane's region starting on a uint boundary.  These MUST equal the
// kMode* constants in passA/syntax_constants.h; the two files are the two
// sides of the same buffer.
#define NXVW_MODE_BITS 4u
#define NXVW_MODE_MASK 15u
#define NXVW_MODES_PER_UINT 8u
#define NXVW_MODE_WORDS_PER_PLANE 8
#define NXVW_MODE_WORDS_PER_TILE 32

// [minor 6] XFORM_4X4_SPLIT (tool bit 19): the per-block split flags share the
// same per-tile region, one BIT per block, 32 blocks to a uint, two uints per
// plane, immediately after the mode words.  A split flag exists whether or not
// INTRA_DIR does, so it cannot live inside the mode field.  These MUST equal
// the kSplit* constants in passA/syntax_constants.h.
#define NXVW_SPLIT_WORDS_PER_PLANE 2
#define NXVW_SPLIT_WORDS_PER_TILE 8
#define NXVW_MODE_REGION_UINTS 40

// ------------------------------------------------------------- unit lengths
// [sparse] Pass A stores a unit's coefficients in SCAN order at slots
// [0, LAST] of the same reserved region the dense layout used, and publishes
// LAST + 1 here -- one byte per unit, four units per uint, index == unit
// index.  0 means the unit was not coded at all.  These MUST equal the
// kUnitLen* constants in passA/syntax_constants.h.
//
// Unit index, [SYN] 9.1: planes in order, and inside a plane one DC-plane
// unit, then (with INTRA_DIR) one mode unit, then nb*nb block units.  A mode
// unit carries no coefficients and its length stays 0.
// [minor 6] The field width follows the TILE's transform size: eight bits and
// four to a uint at 8x8, sixteen bits and two to a uint at 16x16 and 32x32,
// where LAST + 1 reaches 1024.  The region does not grow, because the same
// thing that makes those units big makes them few.  These MUST equal the
// kUnitLen* constants in passA/syntax_constants.h.
#define NXVW_UNIT_LENS_PER_WORD 4u
#define NXVW_UNIT_LEN_BITS 8u
#define NXVW_UNIT_LEN_MASK 255u
#define NXVW_UNIT_LENS_PER_WORD_LARGE 2u
#define NXVW_UNIT_LEN_BITS_LARGE 16u
#define NXVW_UNIT_LEN_MASK_LARGE 65535u
#define NXVW_UNIT_LEN_WORDS_PER_TILE 66

#ifdef __cplusplus
// Per-tile slot size for a whole dispatch: worst case over res_level, which is
// res_level 0.  Rounded up to an even number of int16 so every tile slot
// starts on a uint boundary.
inline int nxvw_coef_stride_i16(int chroma420, int alphaPresent) {
    int chroma444 = chroma420 ? 0 : 1;
    int n = nxvw_plane_coef_count(nxvw_plane_size(0, 0, chroma444)) +
            2 * nxvw_plane_coef_count(nxvw_plane_size(1, 0, chroma444));
    if (alphaPresent) n += nxvw_plane_coef_count(nxvw_plane_size(3, 0, chroma444));
    return (n + 1) & ~1;
}
// uint size of one plane's slot in the shared sample store (int16 samples at
// the plane's res_level-0 coded size, two per uint).
inline int nxvw_plane_store_words(int p, int chroma420) {
    int s = nxvw_plane_size(p, 0, chroma420 ? 0 : 1);
    return (s * s) / 2;
}
}  // namespace nxvw
#endif

#undef NXVW_FN
#undef NXVW_FNU

#endif  // NXVW_PASSB_LAYOUT_H
