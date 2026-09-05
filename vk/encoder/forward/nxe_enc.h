/* nxe_enc.h -- NX Warp GPU encoder: the E3/E4/E5 contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This header is the ABI between the host, the transform kernel (E3
 * `forward.comp`), the entropy kernel (E4 `rans_encode.comp`) and the
 * packetizer (E5 `packetize.comp`).  Its GLSL mirror is
 * `nxe_enc_common.glsl`.  The two are hand-written copies of one contract and
 * are kept in step by the `vk.encoder.mirror` ctest, which compares every
 * constant they both define -- 53 of them today.  It is a textual check
 * because it has to be: nothing in a C++ translation unit can see a GLSL
 * `#define`, so the static assertion this comment used to promise could not
 * exist and did not.  The stakes are not cosmetic: NXE_LANE_OPS_CAP is a
 * buffer stride that the host takes from this file and the shader takes from
 * the mirror, so a drift there points every rANS lane at another lane's
 * scratch, silently.
 *
 * The syntax it implements is docs/SYNTAX.md at bitstream minor 6, intra only:
 * the 8x8 Loeffler DCT with 7/13 shifts, transform skip, QP 0..63 with a
 * weighting matrix and a per-tile wm_id, the DC-plane intra predictor,
 * directional intra (tool bit 17) behind a specialization constant, and rANS
 * with 8 lanes, 10-bit probabilities and static per-frame tables over 12, 16
 * or 27 contexts (tool bit 25, CTX_V3), with sign data hiding, over the
 * built-in tables or a set trained on the frame (tool bits 6 and 26).
 *
 * Of the minor-6 tools, CTX_V3 and TAB_V2 are implemented here.
 * XFORM_4X4_SPLIT (19), INTRA_CFL (24), XFORM_LARGE (27) and ENTROPY_LITE (30) are
 * not; vk/encoder/README.md says what each would take and why they are in the
 * order they are.
 *
 * ---------------------------------------------------------------------------
 * Room for the merge
 * ---------------------------------------------------------------------------
 * A separate branch is adding large transforms, a 27-context model and inter
 * tools.  Two hooks exist for them and are used consistently below:
 *
 *   * NXE_XFORM_LOG2 is a *specialization constant* in the shaders, defaulting
 *     to 3 (an 8x8 transform).  Every loop bound, LDS extent and scan-table
 *     lookup derives from it rather than from a literal 8, so a 16x16 or 32x32
 *     transform is a second pipeline built from the same source.
 *   * The context count is a *push constant* (`nxe_frame_params::nctx`), never
 *     a compile-time bound.  The table buffer is indexed by `ctx * NXE_NUM_SYM`
 *     with `nctx` rows actually present, so 27 contexts is a bigger upload and
 *     nothing else.  NXE_MAX_CTX below is only the storage bound.
 *
 * Inter tools slot in ahead of E3, but NOT as cheaply as this comment used to
 * claim.  It said E3 "already reads its prediction from a buffer (`pred_src`)
 * rather than deriving it, so an inter tile is a different producer for the
 * same buffer".  There is no such buffer and there never was: `pred_src`
 * appears nowhere else in vk/encoder, `forward.comp` binds params, jobs,
 * source, coefficients and modes and nothing else, and its prediction is
 * `pred_at()`, recomputed from the tile's own reconstructed block means --
 * which that file states plainly ("It is never materialised").
 * `nxe_tile_job::mode` is real, and is INTRA on every tile this pipeline
 * codes.
 *
 * So the work an inter tile actually needs here is a sixth binding carrying
 * the warped predictor Pass W produces, and a branch in the residual path that
 * takes it instead of `pred_at()`.  That is a modest change, but it is a
 * change, and planning against the sentence that was here would have costed it
 * at zero.  See docs/adr/0028-gpu-inter-needs-an-integer-mode-decision.md.
 */

#ifndef NXE_ENC_H
#define NXE_ENC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ syntax */

#define NXE_TILE            64
#define NXE_BLOCK           8      /* transform edge at NXE_XFORM_LOG2 == 3 */
#define NXE_MAX_PLANES      3      /* alpha is not coded by this pipeline    */

#define NXE_NUM_SYM         16
#define NXE_MAX_CTX         32     /* storage; the merge's 27 fits           */
#define NXE_NCTX_V1         12
#define NXE_NCTX_V2         16
#define NXE_NCTX_V3         27     /* tool bit 25, CTX_V3                    */

#define NXE_CTX_CBF_LUMA    0
#define NXE_CTX_CBF_CHROMA  1
#define NXE_CTX_LAST_LUMA   2
#define NXE_CTX_LAST_CHROMA 3
#define NXE_CTX_LEVEL_BASE  4
#define NXE_CTX_CBF_DC      12
#define NXE_CTX_LAST_DC     13
#define NXE_CTX_LEVEL_DC    14
#define NXE_CTX_MODE        15
#define NXE_CTX_NONE        0      /* sentinel; see ref/src/common.h         */

/* ------------------------------------------------- the v3 model, bit 25
 *
 * `ref/src/common.h` "v3 context derivation", mirrored.  v3 keeps v2's
 * sixteen rows -- so a unit with nothing to condition on codes exactly as it
 * did under v2 -- and adds eleven that only ever see conditioned data.
 *
 * Three things are conditioned:
 *
 *   * CBF and LAST on the **neighbour class** the rANS lane carries: 0 =
 *     nothing to condition on, 1 = the previous unit was not coded, 2 = coded
 *     and sparse (LAST < NXE_NBR_DENSE_LAST), 3 = coded and dense.
 *   * LEVEL at scan position LAST, split at two bands.  That coefficient is
 *     nonzero by construction, so it cannot share a context with positions
 *     that may be zero.
 *   * the DC term of a DC plane, which is a block mean rather than a residual.
 *
 * The conditioning is per CODING UNIT and never per transform block: a lane
 * owns units l, l+N, l+2N, ... and finishes them in that order, so the unit
 * the class describes is always one this lane has already finished.  The
 * derivation is causal inside the lane, needs no cross-lane read and no
 * barrier.  The class is carried across one plane's run of block units and
 * reset at every plane boundary; DC-plane units neither publish nor consume
 * it.  See docs/SYNTAX.md 9.8 and 9.9.
 */
#define NXE_CTX_CBF_LUMA_N    16   /* 16..18, + (nbr - 1), luma/alpha  */
#define NXE_CTX_CBF_CHROMA_N  19   /* 19..21, + (nbr - 1)              */
#define NXE_CTX_LAST_LUMA_N   22
#define NXE_CTX_LAST_CHROMA_N 23
#define NXE_CTX_LEVEL_DC0     24   /* LEVEL at scan position 0 of a DC plane */
#define NXE_CTX_LEVEL_LAST_LO 25   /* LEVEL at scan position LAST, band 0..1 */
#define NXE_CTX_LEVEL_LAST_HI 26   /* LEVEL at scan position LAST, band 2..3 */

/* A coded unit is "dense" when its LAST is at this scan position or beyond.
 * The same split point as NXE_SDH_MIN_LAST, by measurement rather than by
 * construction, which is why they are separate names. */
#define NXE_NBR_DENSE_LAST  4

/* The statistical family a coding unit belongs to (ref's kUcls*).  It is a
 * property of the unit's position in the tile, so it is never transmitted. */
#define NXE_UCLS_LUMA       0      /* residual blocks of the luma plane */
#define NXE_UCLS_CHROMA     1      /* residual blocks of a chroma plane */
#define NXE_UCLS_DC         2      /* a DC-plane unit of any plane      */

#define NXE_ESC_SYM         15
#define NXE_ESC_ORDER       3
#define NXE_ESC_MAX_PREFIX  16
#define NXE_SDH_MIN_LAST    4

#define NXE_NUM_INTRA_MODES 9
#define NXE_INTRA_DC_PLANE  0

/* rANS (docs/SYNTAX.md 9.5). */
#define NXE_RANS_L          (1u << 16)
#define NXE_PROB_BITS       10
#define NXE_MAX_LANES       32

/* ------------------------------------------------------------ op encoding
 *
 * One coding operation, as `entropy.cpp`'s `Op` but packed into a word so the
 * per-lane scratch is a plain uint buffer.
 *
 *   bit  0      kind: 0 = OP_SYM, 1 = OP_BYPASS
 *   bits 1..5   arg:  OP_SYM -> context index (0..31)
 *                     OP_BYPASS -> bit count (1..8)
 *   bits 8..15  value
 *
 * `value` fits in eight bits for every operation the v1 syntax can produce: a
 * LEVEL symbol is 0..15, a LAST class 0..14, a bypass chunk is at most eight
 * bits wide, and the LAST raw field at most four.  That is asserted at the two
 * places a value is packed.
 */
#define NXE_OP_SYM          0u
#define NXE_OP_BYPASS       1u

#define NXE_OP_PACK(kind, arg, value) \
    ((uint32_t)(kind) | ((uint32_t)(arg) << 1) | ((uint32_t)(value) << 8))
#define NXE_OP_KIND(w)      ((w) & 1u)
#define NXE_OP_ARG(w)       (((w) >> 1) & 31u)
#define NXE_OP_VALUE(w)     (((w) >> 8) & 255u)

/* Worst case operations one 64-coefficient coding unit can emit: CBF, LAST and
 * the LAST raw field, then, for each of 64 scan positions, a LEVEL symbol, an
 * escape prefix of up to NXE_ESC_MAX_PREFIX ones plus its terminating zero,
 * up to three suffix chunks, and a sign bit.  Nothing in the syntax can exceed
 * it, so the per-lane scratch below is a hard bound rather than a heuristic
 * and E4 has no overflow path. */
#define NXE_UNIT_MAX_OPS    (3 + 64 * (1 + (NXE_ESC_MAX_PREFIX + 1) + 3 + 1))

/* Per-lane operation scratch in E4.  When a lane's whole operation list fits,
 * the counting pass materialises it once and the two rANS sweeps index it; when
 * it does not, they fall back to regenerating one unit at a time, which is
 * always correct and about three times slower.  2048 covers every measured
 * frame down to QP 0 (a 4:2:0 tile at QP 0 peaks at 1693 operations per lane),
 * and it must be at least NXE_UNIT_MAX_OPS because the fallback path writes one
 * whole unit into the same slot. */
#define NXE_LANE_OPS_CAP    2048

/* ---------------------------------------------------------------- geometry
 *
 * Coefficients are stored per tile in the reference encoder's coding-unit
 * order (ref/src/codec.cpp, TileCoder::build_units): for each plane, the
 * DC plane's nb*nb levels, then nb*nb blocks of 64 levels each in raster block
 * order and block-local raster index.  Two int16 levels per 32-bit word, low
 * half first, exactly as the sample planes E0 writes.
 *
 * The stride is the 4:4:4 worst case so that one buffer serves both chroma
 * modes and a 4:2:0 tile simply leaves the tail of its slot untouched.
 */
#define NXE_PLANE_COEFS_444 (64 + 64 * 64)          /* 4160 */
#define NXE_PLANE_COEFS_420 (16 + 16 * 64)          /* 1040 */
#define NXE_TILE_COEFS_MAX  (3 * NXE_PLANE_COEFS_444)         /* 12480 */
#define NXE_TILE_COEF_WORDS (NXE_TILE_COEFS_MAX / 2)          /*  6240 */

/* Units per tile: one DC unit, optionally one mode unit, and nb*nb block units
 * per plane.  4:4:4 with directional intra is the maximum. */
#define NXE_TILE_UNITS_MAX  (3 * (1 + 1 + 64))                /*   198 */
/* Units one lane can own.  With eight lanes it is 25; with nsub_log2 == 0 the
 * single lane owns every unit, so the bound has to be the unit count itself.
 * The array it sizes lives in global scratch, not LDS, for exactly that
 * reason. */
#define NXE_TILE_UNIT_SLOTS NXE_TILE_UNITS_MAX

/* Bounded per-tile output slot, paper 3.6: "two bytes per coefficient plus
 * header".  The tile header and the rANS flush states sit at the front; the
 * renormalisation words sit at the *end*, one whole word each, emission e at
 * word (slot_end - 1 - e).
 *
 * A word per 16-bit emission rather than a half word costs twice the scratch
 * and buys the whole second sweep.  E4 produces bytes back to front, so their
 * final offsets are only known once the emission count is, which forced a
 * counting pass and then a placing pass over the same operations.  Anchored at
 * the end instead, the position of emission e is known the moment it is
 * produced -- and reading that region forwards yields exactly the order the
 * bitstream wants, because the sweep produces emissions in descending global
 * order.  It also removes the read-modify-write: two emissions can no longer
 * share a word, so there are no atomics and nothing to pre-zero. */
/* The largest a tile's *output* can be: header, rANS flush, two bytes per
 * coefficient.  The CPU model writes that contiguously; the GPU slot below is
 * a scratch layout, not the same thing. */
#define NXE_TILE_BYTES_MAX  (8 + 4 * 8 + 2 * NXE_TILE_COEFS_MAX)  /* 25000 */
/* Two header words, ONE optional-field word, the eight rANS flush states, and
 * the emission words.  The field word is reserved on every tile even though
 * only a coded-vector tile uses it: one formula for the byte layout is worth
 * four bytes a tile, and E5 would otherwise need the field size to find the
 * flush states. */
#define NXE_TILE_SLOT_WORDS (2 + 1 + 8 + NXE_TILE_COEFS_MAX)      /* 12491 */
#define NXE_TILE_SLOT_BYTES (NXE_TILE_SLOT_WORDS * 4)             /* 49960 */

/* Header sizes, ref/src/common.h. */
#define NXE_STREAM_HEADER_BYTES 64
/* Tile modes, nxvc_tile_mode's numbering (SYNTAX.md 6.2).  This pipeline
 * codes INTRA and WARP_SKIP; the coded-vector modes are named so the skip
 * test is a comparison against a name rather than against 0. */
#define NXE_MODE_WARP_SKIP      0
#define NXE_MODE_STATIC_MV      1
#define NXE_MODE_WARP_MV        2
#define NXE_MODE_INTRA          3
#define NXE_MODE_STEREO         4

#define NXE_FRAME_HEADER_BYTES  40
/* The transmitted probability tables (SYNTAX.md 9.4) sit between the frame
 * header and the first tile-row header.  The largest area the syntax can
 * produce is eight sets of the 27-context model, each row a 1-bit `row_coded`
 * flag (TAB_V2) plus sixteen 5-bit deltas: 8 * 27 * 81 bits = 2187 bytes.
 * Rounded up to a word so E5 can copy it as words. */
#define NXE_TABLE_AREA_MAX      2188
#define NXE_ROW_HEADER_BYTES    12
#define NXE_TILE_HEADER_BYTES   8

/* Workgroup shapes. */
#define NXE_E3_WG           64    /* one tile; 8 blocks x 8 rows per step */
#define NXE_E4_TILES_PER_WG 8
#define NXE_E4_WG           (NXE_E4_TILES_PER_WG * 8)   /* 64 */
#define NXE_E5_WG           256

/* ------------------------------------------------------------- frame params
 *
 * One record per frame, bound as a uniform buffer.  Everything the kernels
 * need that is not per tile.  std140-compatible: scalars only, 4-byte aligned,
 * arrays of uint.
 */
typedef struct nxe_frame_params {
    uint32_t tiles_x;        /* per eye */
    uint32_t tiles_y;
    uint32_t eyes;
    uint32_t ntiles;         /* eyes * tiles_x * tiles_y */

    uint32_t chroma420;      /* 1 = 4:2:0 source, chroma tile edge 32 */
    uint32_t base_qp;
    int32_t  chroma_qp_off;
    uint32_t nctx;           /* 12 or 16 today; 27 after the merge */

    uint32_t sdh;            /* sign data hiding (tool bit 22) */
    uint32_t intra_dir;      /* tool bit 17; also an E3 spec constant */
    uint32_t dir_layer;      /* frame flag bit 2 */
    uint32_t nsub_log2;      /* rANS lanes, log2; 3 = 8 lanes */

    uint32_t frame_number;
    uint32_t frame_flags;
    uint32_t quant_matrix;
    uint32_t tables_present;

    uint32_t width;          /* per eye */
    uint32_t height;
    /* NXVC_CT_YCOCGR: chroma is 9-bit signed offset by 256, so maxval and the
     * DC offset differ from the YCbCr passthrough path (ref Geometry::maxval,
     * ::dc_offset).  Carried as a flag rather than as the two derived values
     * so the shader derives them the same way the reference does. */
    uint32_t ycocgr;
    /* Bytes of transmitted probability table between the frame header and the
     * first row header (SYNTAX.md 9.4).  0 when `tables_present` is 0, which
     * is every stream without CUSTOM_TABLES. */
    uint32_t table_bytes;

    /* Bytes of `warp_ext()`: 36 per eye on an inter frame that carries a pose
     * matrix, 0 otherwise.  It sits BEFORE the table area -- SYNTAX.md 12 has
     * `frame := frame_header [warp_ext] [custom_matrices] [table_set]*
     * tile_row*` -- so the two are added together for the offsets and written
     * in that order. */
    uint32_t warp_bytes;

    /* Frame-header byte 33.  `1 << (frame_number & 3)` on an inter stream, 0
     * otherwise -- a MASK naming the ring slot this frame writes, not an
     * index; the decoder rejects any other value (SYNTAX.md 3.1). */
    uint32_t ref_slots;

    /* int16 elements one tile occupies in the WPred buffer Pass W writes, and
     * which E3 subtracts for a coded inter tile.  0 on an intra stream, where
     * the buffer is a 4-byte placeholder and nothing reads it. */
    uint32_t wpred_stride;
    uint32_t pad1;

    /* Frame weighting matrices, Q4, raster order in the 8x8 block.  wm_id 0
     * on a tile selects these; 1..3 select a built-in pair (kWeight). */
    uint32_t wm_luma[64];
    uint32_t wm_chroma[64];
} nxe_frame_params;

/* ------------------------------------------------------------- the tile job
 *
 * One record per tile: everything the mode decision (E1c plus the host rate
 * controller) settled, and the fields E4 and E5 write back.  18 words.
 */
typedef struct nxe_tile_job {
    uint32_t tile;           /* linear tile index, row-major eye-minor */
    uint32_t col;
    uint32_t row;
    uint32_t eye;

    int32_t  qp_delta;       /* -32..31 */
    uint32_t table_set;      /* 0..7 */
    uint32_t tskip;
    uint32_t wm_id;          /* 0..3 */

    uint32_t chroma444;
    uint32_t res_level;      /* 0 only in this pipeline */
    uint32_t mode;           /* NXVC_MODE_*; INTRA (0) only in this pipeline */
    uint32_t nsub_log2;

    uint32_t payload_len;    /* written by E4 */
    uint32_t tile_bytes;     /* written by E4: 8 + payload_len */
    uint32_t nunits;         /* written by E3 */
    uint32_t flags;

    /* The tile's motion vector, quarter LUMA samples, two int8 packed low
     * byte first: mv_x in bits 0-7, mv_y in 8-15.  Zero unless the mode is a
     * coded-vector one, and written by E1c rather than by the host, because
     * it is the decision's own output. */
    uint32_t mv;
    uint32_t pad_job;
} nxe_tile_job;

#define NXE_JOB_F_OK        1u

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* NXE_ENC_H */
