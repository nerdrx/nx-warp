/* nxvc.h - C ABI for the NX Warp reference codec (nxvc_ref).
 *
 * This is the bit-exact CPU reference implementation of the NX Warp v1
 * bitstream.  The normative syntax is docs/SYNTAX.md; this library IS the
 * specification in executable form.  Scope: intra (Phase 1) and the Phase 2
 * inter path -- pose warp, per-tile motion vectors, a four-slot reference
 * ring, stereo inter-view prediction and deterministic concealment.
 *
 * All integer arithmetic in the normative decode path is int32; no floats,
 * no int64, no division.
 */
#ifndef NXVC_NXVC_H
#define NXVC_NXVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXVC_VERSION 1
#define NXVC_MAX_TILES_PER_ROW 64
#define NXVC_TILE_SIZE 64

/* Minor revision of docs/SYNTAX.md that this build implements.  It is NOT
 * carried in the bitstream: forward compatibility is the `tools` mask plus the
 * TLV area (SYNTAX.md 2.1, 2.3), and a decoder that does not implement a tool
 * refuses the stream at the handshake.  The constant exists so that a build,
 * a conformance-vector set and a spec revision can be pinned to each other;
 * it is bumped whenever a change makes the encoder emit different bytes.
 *
 *   1 : initial v1 syntax
 *   2 : per-tile `wm_id` in tile-header word1 bits 26-27; bit_depth 10 is
 *       reserved-and-rejected; IDCT odd-part rotation specified as an exact
 *       two-word product (no pixel change)
 *   3 : v2 intra tools -- tool bit 17 INTRA_DIR (a nine-mode directional
 *       predictor per 8x8 block, MPM coded, in a per-plane mode unit),
 *       frame-header flag bit 2 (the layered form of it), and tool bit 21
 *       CTX_V2 (16 contexts: dedicated DC-plane CBF/LAST/LEVEL and a mode
 *       context; transmitted table sets grow from 120 to 160 bytes), and
 *       tool bit 22 SIGN_HIDE (the sign at scan position `last` is carried
 *       by the parity of the unit's absolute levels)
 *   4 : Phase 2 inter path -- frame-header flag bit 3 `warp_present` and the
 *       36-byte-per-eye `warp_ext()` that follows the frame header, inter tile
 *       modes WARP_SKIP / STATIC_MV / WARP_MV / STEREO, `eyes == 2` with one
 *       picture per eye and row-major/eye-minor tile rows, the four-slot
 *       reference ring addressed by `ref_sel`, and the 12-bit STEREO
 *       `disparity` field replacing mv_x/mv_y.  See docs/SYNTAX.md 8.
 *   6 : the tournament packages, landed together under one minor bump.
 *       5 is skipped: eight of the ten tournament branches each set 5 for
 *       their own package, and a merged stream is none of those eight.
 *       tool bit 19 XFORM_4X4_SPLIT and 24 INTRA_CFL (the detail package);
 *       25 CTX_V3, the 27-context neighbour-conditioned model, conditioned
 *       per coding unit; 26 TAB_V2, the variable-length transmitted table
 *       set with a per-row "use the built-in default" flag; 27 XFORM_LARGE
 *       and the tile header's two-bit `xform_size`, selecting a 16x16 or
 *       32x32 integer DCT for every plane of the tile instead of the 8x8
 *       one -- the DC plane, the intra predictors, the weighting matrices,
 *       the scans and the entropy contexts all follow the block size by
 *       documented rules, and no new context and no new symbol exists.
 *       28 NEAR_SKIP, a warped tile whose whole residual is a per-plane
 *       DC-plus-ramps correction carried in nine signed bytes in the TILE-ROW
 *       header rather than an entropy-coded payload, gated by a second
 *       per-row bitmap; and 29 QUAD_MV, four motion vectors, one per 32x32
 *       quadrant, as signed nibble deltas from the tile vector over the
 *       tile's own warp corner basis.
 *       See docs/SYNTAX.md 3.3, 6.7, 6.8, 9.4.1, 9.9, 13.9 and 13.10, and
 *       docs/TOOLBITS.md.
 */
#define NXVC_BITSTREAM_MINOR 6

/* "nxvc_ref <major>.<minor> (syntax v1.<minor>)" -- a static string, safe to
 * call before any object exists.  Used by the Python bindings to check that
 * the shared library matches the header they were built against. */
const char *nxvc_version_string(void);

/* ---------------------------------------------------------------- status */
typedef enum nxvc_status {
    NXVC_OK = 0,
    NXVC_ERR_ARG = -1,          /* bad argument from the caller           */
    NXVC_ERR_UNSUPPORTED = -2,  /* legal v1 syntax outside Phase 1 scope  */
    NXVC_ERR_BITSTREAM = -3,    /* malformed / illegal bitstream          */
    NXVC_ERR_TRUNCATED = -4,    /* ran off the end of the buffer          */
    NXVC_ERR_NOMEM = -5,        /* output buffer too small / alloc failed */
    NXVC_ERR_VERSION = -6       /* magic/version/tool mask refused        */
} nxvc_status;

/* ---------------------------------------------------------------- enums  */
typedef enum nxvc_chroma {
    NXVC_CHROMA_420 = 0,
    NXVC_CHROMA_444 = 1
} nxvc_chroma;

typedef enum nxvc_color_transform {
    NXVC_CT_NONE = 0,   /* planes are coded as given (YUV in, YUV out)    */
    NXVC_CT_YCOCGR = 1  /* planes are RGB; codec applies YCoCg-R          */
} nxvc_color_transform;

/* What the coded planes mean.  Purely descriptive: the DCT, quantizer and
 * entropy coder are identical for every value.  A YCbCr source (WiVRn's
 * Linux capture path is already VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) is coded
 * as-is with NXVC_CT_NONE; an RGB source goes through YCoCg-R. */
typedef enum nxvc_color_space {
    NXVC_CS_UNSPECIFIED = 0,  /* planes coded as given, range unstated    */
    NXVC_CS_YCBCR_709_LIMITED = 1,
    NXVC_CS_YCBCR_709_FULL = 2,
    NXVC_CS_RGB = 3           /* requires NXVC_CT_YCOCGR                  */
} nxvc_color_space;

/* Encoder effort presets.  Encoder-side only: none of them changes what a
 * stream means or how it decodes, only how long the encoder looks for the
 * cheapest way to say it.
 *
 * This is a LIBRARY concept, not a CLI flag.  A preset that exists only in
 * nxv-enc is not available to anything embedding the encoder, which is most
 * of what the encoder is for.  Every individual knob in nxvc_config still
 * overrides the preset; 0 in a knob means "take the preset's value". */
typedef enum nxvc_preset {
    NXVC_PRESET_MEDIUM = 0,   /* the default */
    NXVC_PRESET_FAST   = 1,
    NXVC_PRESET_SLOW   = 2
} nxvc_preset;

typedef enum nxvc_tile_mode {
    NXVC_MODE_WARP_SKIP = 0,
    NXVC_MODE_STATIC_MV = 1,
    NXVC_MODE_WARP_MV   = 2,   /* "INTER" */
    NXVC_MODE_INTRA     = 3,
    NXVC_MODE_STEREO    = 4
} nxvc_tile_mode;

/* Tool bits of the stream header `tools` u64.  See SYNTAX.md 2.2. */
#define NXVC_TOOL_INTRA_DC_PLANE  (1ull << 0)
#define NXVC_TOOL_TRANSFORM_SKIP  (1ull << 1)
#define NXVC_TOOL_RES_LEVEL       (1ull << 2)
#define NXVC_TOOL_CHROMA444       (1ull << 3)
#define NXVC_TOOL_ALPHA           (1ull << 4)
#define NXVC_TOOL_LOSSLESS        (1ull << 5)
#define NXVC_TOOL_CUSTOM_TABLES   (1ull << 6)
#define NXVC_TOOL_NSUB_VAR        (1ull << 7)
#define NXVC_TOOL_PER_TILE_CHROMA (1ull << 8)
#define NXVC_TOOL_YCOCGR          (1ull << 9)
#define NXVC_TOOL_INTER           (1ull << 10)
#define NXVC_TOOL_WARP            (1ull << 11)
#define NXVC_TOOL_STEREO          (1ull << 12)
#define NXVC_TOOL_LAYERS          (1ull << 13)
#define NXVC_TOOL_BITDEPTH10      (1ull << 14)
#define NXVC_TOOL_ENT_OFFSET_TAB  (1ull << 15)
#define NXVC_TOOL_ENT_BITPLANE    (1ull << 16)
#define NXVC_TOOL_INTRA_DIR       (1ull << 17)
#define NXVC_TOOL_XFORM_WAVELET   (1ull << 18)
#define NXVC_TOOL_XFORM_4X4_SPLIT (1ull << 19)
#define NXVC_TOOL_WM_ID           (1ull << 20)
#define NXVC_TOOL_CTX_V2          (1ull << 21)
#define NXVC_TOOL_SIGN_HIDE       (1ull << 22)
/* Annex D D-5 names the Catmull-Rom selector "tool bit 20".  Bit 20 was
 * already taken by WM_ID in syntax v1.2 (shipped, with conformance vectors),
 * so this reference and docs/SYNTAX.md place it at the first bit that is
 * actually free.  The substance of D-5 is unchanged: it is undefined in
 * version 1 and a v1 decoder MUST reject a stream that sets it. */
#define NXVC_TOOL_FILTER_CATMULLROM (1ull << 23)
/* Syntax v1.5: chroma predicted from the co-located reconstructed luma by a
 * per-block linear model (SYNTAX.md 7.7).  Requires INTRA_DIR and CTX_V2. */
#define NXVC_TOOL_INTRA_CFL       (1ull << 24)
/* The entropy and context package (docs/TOOLBITS.md 2).  CTX_V3 is the
 * 27-context neighbour-conditioned model, conditioned per CODING UNIT -- the
 * 8x8 coefficient group -- never per transform block (MERGE-PLAN 4.5).
 * TAB_V2 is the variable-length transmitted table set with a per-row "use the
 * built-in default" flag.  CTX_V3 requires CTX_V2; TAB_V2 requires
 * CUSTOM_TABLES.  Both ship OFF by default: vk/decoder/passA does not
 * implement them, so a default stream stays decodable by the Vulkan decoder. */
#define NXVC_TOOL_CTX_V3          (1ull << 25)
#define NXVC_TOOL_TAB_V2          (1ull << 26)

/* Per-tile 16x16 and 32x32 transforms (SYNTAX.md 6.7).  Gates
 * tile-header field `xform_size`; a stream that never sets the bit decodes
 * byte-identically to a v1.4 one. */
#define NXVC_TOOL_XFORM_LARGE     (1ull << 27)

/* Phase 2 inter efficiency (docs/SYNTAX.md 13.9 and 13.10).  Both require
 * INTER; NEAR_SKIP additionally requires WARP only in the sense that its
 * tiles are warped ones, which the mode already gates.  NEAR_SKIP is the one
 * tool in the tournament whose bit gates a TILE-ROW HEADER structure rather
 * than a tile-header field: the correction travels in the row header, named
 * by a second per-row bitmap, so it costs no tile-header bit and its nine
 * bytes are spent only on the tiles the bitmap names.  docs/TOOLBITS.md 4. */
#define NXVC_TOOL_NEAR_SKIP       (1ull << 28)
#define NXVC_TOOL_QUAD_MV         (1ull << 29)

/* ENTROPY_LITE: the table-free, fully parallel entropy tool of SYNTAX.md
 * 9.10.  Mutually exclusive with SIGN_HIDE and CUSTOM_TABLES: both are
 * statements about an arithmetic coder this tool does not have.
 *
 * It ships OFF and is a NEGOTIATED tool: the decoder asks for it from its own
 * measured Pass A time, because whether it is worth +40-50 % bits depends on
 * a number only the decoder knows.  On a Pico 4 it cuts Pass A 7.5x, 138.5 ms
 * to 18.4 ms, and it is the only bitstream-side lever that reaches the Adreno
 * frame budget at all. */
#define NXVC_TOOL_ENTROPY_LITE    (1ull << 30)

/* Tools this reference decoder implements. */
#define NXVC_TOOLS_SUPPORTED                                                  \
    (NXVC_TOOL_INTRA_DC_PLANE | NXVC_TOOL_TRANSFORM_SKIP |                    \
     NXVC_TOOL_RES_LEVEL | NXVC_TOOL_CHROMA444 | NXVC_TOOL_ALPHA |            \
     NXVC_TOOL_LOSSLESS | NXVC_TOOL_CUSTOM_TABLES | NXVC_TOOL_NSUB_VAR |      \
     NXVC_TOOL_PER_TILE_CHROMA | NXVC_TOOL_YCOCGR | NXVC_TOOL_WM_ID |        \
     NXVC_TOOL_INTRA_DIR | NXVC_TOOL_CTX_V2 | NXVC_TOOL_SIGN_HIDE |           \
     NXVC_TOOL_XFORM_4X4_SPLIT | NXVC_TOOL_INTRA_CFL |                        \
     NXVC_TOOL_CTX_V3 | NXVC_TOOL_TAB_V2 | NXVC_TOOL_XFORM_LARGE |            \
     NXVC_TOOL_NEAR_SKIP | NXVC_TOOL_QUAD_MV |                                \
     NXVC_TOOL_INTER | NXVC_TOOL_WARP | NXVC_TOOL_STEREO |                    \
     NXVC_TOOL_ENTROPY_LITE)

/* ---------------------------------------------------------------- images */
/* 8-bit planar image.  plane[0]=Y/R', plane[1]=Co/G', plane[2]=Cg/B',
 * plane[3]=A (optional).  Chroma planes are half size in each dimension for
 * NXVC_CHROMA_420.  Strides are in bytes and may be negative-free only. */
typedef struct nxvc_image {
    uint8_t *plane[4];
    int32_t stride[4];
} nxvc_image;

/* ---------------------------------------------------------------- config */
typedef struct nxvc_config {
    uint32_t width;             /* luma samples, 16..4096, multiple of 2   */
    uint32_t height;            /* luma samples, 16..4096, multiple of 2   */
    uint32_t chroma;            /* nxvc_chroma                             */
    uint32_t bit_depth;         /* 8 only in v1                            */
    uint32_t color_transform;   /* nxvc_color_transform                    */
    uint32_t color_space;       /* nxvc_color_space (descriptive)          */
    uint32_t alpha;             /* 0/1: code a 4th plane                   */

    uint32_t base_qp;           /* 0..63                                   */
    int32_t chroma_qp_off;      /* added to tile QP for Co/Cg              */
    int32_t alpha_qp_off;       /* added to tile QP for A                  */
    uint32_t quant_matrix;      /* 0..3 built in, 255 = custom (below)     */
    const uint8_t *custom_matrix; /* 128 bytes: luma[64] then chroma[64],
                                     Q4 weights clamped to 1..32           */

    uint32_t lossless;          /* force QP 0 + transform-skip + 4:4:4     */
    uint32_t transform_skip;    /* 0=off 1=on(all tiles) 2=auto heuristic  */
    uint32_t nsub_log2;         /* 0..5 fixed lane count; 255 = auto       */
    uint32_t tile_chroma420;    /* 1: code 4:2:0 tiles inside a 4:4:4      */
    uint32_t custom_tables;     /* 1: derive+transmit probability tables   */
    uint32_t profile;           /* 0=Lite 1=Full 2=Pro (informative)       */
    uint32_t level;             /* informative                             */
    uint32_t collect_stats;     /* 1: fill nxvc_encode_stats (slower)      */

    /* --- additive since syntax v1.2.  All encoder-side: they change which
     * levels and per-tile parameters are chosen, never how a stream decodes.
     * nxvc_config_default() sets rdo = 1 and leaves the tuning fields 0. */
    uint32_t rdo;               /* 0 = dead-zone only, 1 = RD trellis      */
    uint32_t rdo_lambda_q8;     /* lambda scale, Q8; 0 = built-in default  */
    uint32_t qp_search;         /* per-tile QP offsets searched, 0 = off,
                                   n = try qp_delta in [-n, +n]            */
    uint32_t wm_id;             /* per-tile weighting-matrix id 0..3, or
                                   255 = let the encoder choose per tile   */

    /* --- additive since syntax v1.3.  These two DO change the bitstream:
     * each sets a tool bit in the stream header and a decoder without it
     * refuses the stream at the handshake. */
    uint32_t intra_dir;         /* 0 = off, 1 = directional intra (tool 17) */
    uint32_t intra_dir_layer;   /* 1: the layered form (frame flag bit 2)   */
    uint32_t ctx_v2;            /* 0 = 12 contexts, 1 = 16 (tool 21)        */
    uint32_t intra_dir_cand;    /* modes RD-checked per block, 0 = default  */
    uint32_t sign_hide;         /* 1 = sign data hiding (tool 22)           */

    /* --- additive since syntax v1.4: the Phase 2 inter path.
     * `width`/`height` are PER EYE.  With eyes == 2 the nxvc_image passed to
     * the encoder and filled by the decoder is `eyes * width` samples wide:
     * one picture per eye, side by side, eye 0 first (Annex D D-3). */
    uint32_t eyes;              /* 1 or 2; 0 is read as 1                   */
    uint32_t inter;             /* 1 = inter prediction (tools INTER|WARP)  */
    uint32_t stereo;            /* 1 = allow STEREO on eye 1 (tool 12)      */
    uint32_t intra_period;      /* rolling intra refresh period T in frames,
                                   0 = the default 180 (PAPER 2.6).  Every
                                   frame 1/T of the tiles are forced INTRA by
                                   a fixed pseudo-random permutation.  1 = all
                                   intra every frame.  Under drift_refresh
                                   this is the HARD AGE CAP instead: a tile
                                   position may go at most T frames without an
                                   INTRA, and the loss-recovery bound PAPER 2.6
                                   states is unchanged.                     */
    uint32_t ref_sel;           /* 0..2: reference distance inter tiles ask
                                   for (N-1-ref_sel).  Default 0.           */
    uint32_t mv_range;          /* coarse integer search radius in samples,
                                   0 = the default 16 (PAPER 2.3 step 2)    */
    uint32_t skip_thresh;       /* WARP_SKIP early-out gate, Q8 multiple of
                                   the quantiser's own noise floor
                                   (qstep^2 / 12) per sample; 0 = default   */
    uint32_t mode_lambda_q8;    /* lambda scale of the per-tile MODE
                                   decision, Q8, relative to the trellis's.
                                   0 = the default 256 (the same lambda): the
                                   reference-persistence factor is charged
                                   once, on the skip candidate, and v1.4
                                   charged it here a second time.  Below 256
                                   the decision spends more bits to keep the
                                   reference clean.                         */

    /* --- additive since syntax v1.6.  Bitstream tools, each behind its own
     * tool bit: a stream without the bit decodes byte-identically to v1.4.
     * APPENDED, per JUDGE-detail.md merge item 1: the ABI is additive, so a
     * new field goes at the END of the struct.  detail-a inserted these two
     * mid-struct, which silently moves the offset of every field after them.
     * Every package that follows appends after this block, in merge order. */
    uint32_t split4x4;          /* 1 = per-block 4x4 transform split (19)    */
    uint32_t chroma_from_luma;  /* 1 = the CFL chroma intra mode (24)        */

    /* the entropy and context package (docs/TOOLBITS.md 2) */
    uint32_t ctx_v3;            /* 1 = the 27-context neighbour-conditioned
                                   model (25); implies ctx_v2.  Conditions
                                   per CODING UNIT, never per transform
                                   block -- see docs/SYNTAX.md 9.8          */
    uint32_t tab_v2;            /* 1 = the compact transmitted-table coding:
                                   a per-row "use the built-in default" flag
                                   and a variable-length table area (26).
                                   Requires custom_tables                   */
    uint32_t table_iters;       /* Lloyd iterations refining the eight
                                   per-frame table sets.  Encoder only: it
                                   changes which set each tile names, never
                                   how a stream decodes.  0 = OFF (one
                                   training pass, no reassignment); leave
                                   unset for the default of 3.  Reassigning
                                   without retraining is worse than doing
                                   nothing (-1.8 %), so the two always move
                                   together -- there is no "reassign only"  */
    uint32_t table_iters_set;   /* 1 = table_iters is meaningful even at 0.
                                   Without this a zeroed nxvc_config, which
                                   is how every caller starts, would mean
                                   "off" rather than "default"              */

    /* the inter efficiency package (docs/TOOLBITS.md 2).  APPENDED, not
     * inserted: inter-a placed these before `ref_sel`, which silently moves
     * the offset of every field after them for anything compiled against
     * v1.4 (JUDGE-inter.md merge item 9). */
    uint32_t near_skip;         /* 1 = allow the near-skip correction (28).
                                   A warped tile whose drift is small and
                                   smooth is corrected by nine signed bytes
                                   in the TILE-ROW header instead of a coded
                                   residual.  SYNTAX.md 13.9.              */
    uint32_t quad_mv;           /* 1 = allow four motion vectors per tile,
                                   one per 32x32 quadrant, as nibble deltas
                                   from the tile vector (29).              */
    uint32_t drift_refresh;     /* 1 = drive the refresh from the measured
                                   drift of the encoder's client shadow
                                   instead of the fixed 1-in-T permutation.
                                   Encoder-side only: it changes which tiles
                                   are coded, never how one decodes.  The
                                   drift is measured against the SHADOW --
                                   the loss-aware reconstruction -- not the
                                   encoder's own, which is the only drift
                                   the gate exists to catch.               */
    uint32_t drift_gate_q8;     /* drift_refresh gate, Q8 multiple of the
                                   quantiser's own noise floor (qstep^2 / 12)
                                   per sample.  A tile whose shadow has
                                   drifted further than this from the source
                                   may not skip; 0 = default                */

    /* the transform package (docs/TOOLBITS.md 2).  A nonzero value sets tool
     * bit 27, and a decoder without it refuses the stream at the handshake. */
    uint32_t xform_size;        /* 0 = 8x8 only (and no tool bit), 1 = 16x16
                                   on every tile, 2 = 32x32 on every tile,
                                   255 = let the encoder choose per tile by
                                   rate-distortion.  split4x4 is meaningful
                                   only where this resolves to 8x8; see
                                   docs/SYNTAX.md 4.1                       */

    /* the rate-distortion package (encoder only, no tool bit).  All effort
     * knobs: they change how hard the encoder looks, never what a decoder
     * does.  0 is "the built-in default" for every one of them, so a caller
     * that memsets its config gets the medium preset. */
    uint32_t preset;            /* nxvc_preset.  A LIBRARY concept, not a CLI
                                   flag: an SDK caller that wants "fast" must
                                   be able to say so without going through
                                   nxv-enc.  The fields below override it
                                   individually; 0 means "take the preset's
                                   value" for each of them.                 */
    uint32_t rdoq_effort;       /* 1 = fast (nearest level only), 2 = medium
                                   (the v1.2 candidate set), 3 = full (adds
                                   the level below); 0 = default (medium)  */
    uint32_t me_effort;         /* 1 = fast (no hierarchy, integer only),
                                   2 = medium (hierarchical + quarter-pel
                                   SATD), 3 = full (adds true-RD quarter-pel
                                   refinement); 0 = default (medium)       */
    uint32_t lambda_class_off;  /* 1 = one lambda for every tile; 0 = scale
                                   lambda by the tile's content class
                                   (docs/RATECONTROL.md 3.3)               */
    uint32_t lambda_class_q8[4];/* per-class lambda gain, Q8, in class order
                                   flat, texture, edge, text; 0 = built in */
    uint32_t dc_lambda_q8;      /* lambda gain of the DC plane relative to the
                                   AC planes, Q8; 0 = built-in default      */
    uint32_t dc_rdoq_off;       /* 1 = leave the DC plane on the dead-zone
                                   quantizer, as syntax v1.4 did            */
    uint32_t qp_search_step;    /* spacing of the per-tile QP candidates,
                                   0 = the default 2.  With qp_search = n
                                   the candidates are 0, +-step ... +-n     */
    uint32_t chroma_weight_q8;  /* weight of chroma squared error in the
                                   encoder's distortion, Q8, scaled by the
                                   plane's sample density.  0 = the default
                                   256 (1.0), which is chroma weighted as
                                   the samples fall.  Below 256 buys PSNR-Y
                                   and 6:1:1 at the cost of absolute chroma
                                   fidelity: it is a PERCEPTUAL TUNING KNOB
                                   fitted to a reporting convention, not a
                                   coding gain, and anything quoted with it
                                   must be quoted on both metrics.         */

    /* the entropy-lite tool (docs/TOOLBITS.md 2, bit 30).
     * 0 = interleaved rANS (the default), 1 = Lite/FIXED, 2 = Lite/RICE.
     * A nonzero value sets tool bit 30 and forces sign_hide and
     * custom_tables off; both are meaningless without an arithmetic coder.
     * RICE is defined and reachable but is NOT the variant this merge ships:
     * see docs/SYNTAX.md 9.10. */
    uint32_t entropy_lite;

    /* --- encoder threading (encoder only, no tool bit, no syntax change).
     * The reference encoder codes the frame's 64x64 tiles on `threads`
     * threads.  Tiles are independent by design -- own rANS lanes, no
     * cross-tile prediction -- so the result is BYTE-IDENTICAL to the
     * single-threaded encoder at every setting, and the tile order, the
     * row headers and every per-frame decision are unchanged.
     *
     *   0 = auto: std::thread::hardware_concurrency(), capped at 16
     *   1 = the single-threaded path, byte for byte the code v1.6 shipped
     *   n = n worker slots (the calling thread is one of them)
     *
     * The pool is created once per encoder and joined in
     * nxvc_encoder_destroy(); no thread is created per frame.  ref/README.md
     * "Encoder threading" has the schedule and what stays serial. */
    uint32_t threads;
} nxvc_config;

/* One eye's view for one frame: the orientation the frame was rendered with
 * and the projection it was rendered through.  The encoder derives the frame's
 * `warp_ext()` matrix from this eye's view of the previous frame and of this
 * frame (docs/WARP.md 4).  This is the ONLY floating-point input the codec
 * takes, it is encoder-side, and its result reaches the decoder already
 * quantised to the nine int32 of `warp_ext()`. */
typedef struct nxvc_view {
    double qx, qy, qz, qw;      /* unit quaternion, OpenXR convention       */
    double fov_left, fov_right; /* radians, left negative (XrFovf)          */
    double fov_up, fov_down;    /* radians, down negative                   */
} nxvc_view;

/* Where the bits went, for the most recent encoded frame. */
typedef struct nxvc_encode_stats {
    uint64_t bytes_total, bytes_frame_header, bytes_tables;
    uint64_t bytes_row_headers, bytes_tile_headers, bytes_payload;
    uint64_t bytes_rans_init;   /* 4 bytes per active lane per tile        */
    uint64_t bits_dc_plane, bits_luma_blocks, bits_chroma_blocks;
    uint64_t bits_alpha_blocks;
    uint64_t tiles, tiles_tskip, tiles_res[3], lanes_total;
    /* The rate the encoder's own model PREDICTED for the payloads it then
     * emitted, in Q10 bits.  The mode decision, the trellis and the per-tile
     * QP search all minimise D + lambda*R against this number; comparing it
     * with `bytes_payload` is how one tells whether they were shown the truth.
     * Added with the v1.5 effort knobs; 0 unless collect_stats is set. */
    uint64_t bits_predicted_q10;
} nxvc_encode_stats;

void nxvc_config_default(nxvc_config *cfg);

/* ---------------------------------------------------------------- layout */
typedef struct nxvc_tile_layout {
    uint32_t tiles_x, tiles_y, tile_count, tile_size;
} nxvc_tile_layout;

void nxvc_tile_layout_get(uint32_t width, uint32_t height,
                          nxvc_tile_layout *out);

/* The same for a stereo stream: `width`/`height` are per eye, `tiles_x` comes
 * back as the per-eye column count (`cols_per_eye`) and `tile_count` as
 * `eyes * cols_per_eye * rows`, which is the length of every per-tile array in
 * this API and the transport's linear tile index (Annex D D-3). */
void nxvc_tile_layout_get_ex(uint32_t width, uint32_t height, uint32_t eyes,
                             nxvc_tile_layout *out);

/* Per-tile record exposed after encode/decode, in raster order. */
typedef struct nxvc_tile_info {
    uint16_t tile_index;
    uint16_t payload_len;
    uint8_t layer, eye, mode, res_level;
    uint8_t chroma444, alpha_mode, table_set, nsub_log2;
    uint8_t tskip, wgt, ref_sel, mv_present;
    int8_t qp_delta, mv_x, mv_y;
    uint8_t alpha_value;
    uint8_t qp;                 /* resolved luma QP                        */
    uint8_t wm_id;              /* per-tile weighting matrix, 0 = frame's  */
    uint8_t intra_dir;          /* 1: this tile carries per-block modes    */
    /* --- additive since syntax v1.4 */
    uint8_t skipped;            /* 1: WARP_SKIP via skip_bitmap, not coded  */
    uint8_t concealed;          /* decoder: the tile was reported lost and
                                   was reconstructed by clause 6.11         */
    uint16_t disparity;         /* STEREO: quarter samples, 12 bits used    */
    uint8_t ref_delta;          /* the transport's advisory copy of ref_sel,
                                   with the extra value 3 = "no temporal
                                   reference" for INTRA and STEREO (D-12)   */
    uint16_t age_since_coded;   /* frames since this tile position last
                                   carried a coded residual; 0 on the frame
                                   that coded one, saturating at 65535      */
    /* APPENDED per JUDGE-detail.md merge item 1, then in merge order.
     * The perceptual package appends after the inter one; verified
     * that the three sets of appends are ordered and none was
     * interleaved (docs/MERGE-PLAN.md 4.8). */
    uint8_t split4x4;           /* 1: this tile carries 4x4 split flags    */
    uint8_t xform_size;         /* 0 = 8x8, 1 = 16x16, 2 = 32x32           */
    uint8_t near_skip;          /* 1: this tile's residual is the row
                                   header's DC correction (SYNTAX.md 13.9) */
    uint8_t quad_mv;            /* word1 bit 31: four quadrant vectors      */
    int8_t corr[3][3];          /* near_skip: [plane][dc, gx, gy]           */
    int8_t qmv[4][2];           /* quad_mv: per-quadrant delta, quarter
                                   samples, raster order TL TR BL BR        */
    uint16_t warp_mad_q8;       /* encoder: mean absolute difference per luma
                                   sample between the tile and its WARP_SKIP
                                   predictor (the pose warp plus the tile's
                                   stored vector), in Q8.  This is the
                                   `complexity` input docs/RATECONTROL.md 4.1
                                   asks the rate controller for, measured by
                                   the mode search that builds the predictor
                                   anyway.  NXVC_WARP_MAD_UNMEASURED when the
                                   tile had no eligible reference (the first
                                   frame, a tile-map reset, or a rolling-intra
                                   refresh), which is exactly the set of tiles
                                   whose warped residual does not exist.     */
} nxvc_tile_info;

/* `warp_mad_q8` when the tile had no warped predictor to measure. */
#define NXVC_WARP_MAD_UNMEASURED 0xFFFFu

typedef struct nxvc_stream_info {
    uint32_t magic, version, profile, level, tile_size;
    uint32_t width, height, eyes, bit_depth, num_layers;
    uint32_t chroma, color_transform, color_space, alpha;
    uint64_t tools;
    uint32_t layer_desc[4];
    uint32_t ext_len;
    uint32_t ext_tlv_count;
    uint32_t ext_unknown_count;
} nxvc_stream_info;

typedef struct nxvc_frame_info {
    uint32_t frame_number;
    uint32_t base_qp;
    int32_t chroma_qp_off, alpha_qp_off;
    uint32_t quant_matrix, tables_present, ref_slots, flags;
    uint32_t frame_bytes;
    uint8_t pose[26];
    uint32_t tile_count;
    /* --- additive since syntax v1.4 */
    uint32_t warp_present;      /* frame flags bit 3                        */
    int32_t warp[2][9];         /* warp_ext(), per eye, h00..h22            */
} nxvc_frame_info;

/* ---------------------------------------------------------------- encoder */
typedef struct nxvc_encoder nxvc_encoder;

nxvc_encoder *nxvc_encoder_create(const nxvc_config *cfg, nxvc_status *st);
void nxvc_encoder_destroy(nxvc_encoder *enc);

/* Serialize the 64-byte stream header (+ TLV area) into buf. */
nxvc_status nxvc_encoder_stream_header(nxvc_encoder *enc, uint8_t *buf,
                                       size_t cap, size_t *out_len);

/* Attach an opaque TLV to the stream header (type >= 0x8000 = private).
 * Must be called before nxvc_encoder_stream_header. */
nxvc_status nxvc_encoder_add_tlv(nxvc_encoder *enc, uint16_t type,
                                 const uint8_t *data, uint16_t len);

/* Set the 26 pose bytes copied verbatim into the next frame header. */
void nxvc_encoder_set_pose(nxvc_encoder *enc, const uint8_t pose[26]);

/* Set the per-eye view of the NEXT frame to be encoded.  `count` must be the
 * configured `eyes`.  Call it before every nxvc_encoder_encode_frame; the
 * encoder keeps the previous frame's views itself.  Without it the encoder
 * emits an identity warp, which is legal and reduces WARP_MV to STATIC_MV. */
nxvc_status nxvc_encoder_set_views(nxvc_encoder *enc, const nxvc_view *views,
                                   uint32_t count);

/* Tiles per frame, i.e. the length of the per-tile arrays of this API. */
uint32_t nxvc_encoder_tile_count(const nxvc_encoder *enc);

/* Ask for tiles to be coded as WARP_SKIP in the NEXT frame
 * (docs/RATECONTROL.md 8.7).  `skip[t] != 0` is applied AFTER the mode search
 * and pins `mode = WARP_SKIP`, producing a tile indistinguishable from one the
 * RD search chose to skip: no new syntax, no new decoder path.
 *
 * The encoder OVERRIDES the request wherever correctness requires a coded
 * tile, and the caller is expected to rely on that rather than to replicate
 * the conditions:
 *
 *   - the tile is due for rolling intra refresh this frame;
 *   - there is no eligible reference (the first frame, or a tile-map reset,
 *     which is also how a scene cut reaches the encoder);
 *   - the stream codes an alpha plane, which a skipped tile cannot carry.
 *
 * The flags are consumed by one nxvc_encoder_encode_frame call.  Which tiles
 * were actually skipped is readable afterwards from nxvc_encoder_tiles():
 * `skipped`, `age_since_coded` and `ref_delta`. */
nxvc_status nxvc_encoder_set_skip_map(nxvc_encoder *enc, const uint8_t *skip,
                                      uint32_t count);

/* Per-tile weighting-matrix id for the NEXT frame: the `wm_id` of
 * docs/RATECONTROL.md 4.4, which the degradation ladder needs per tile while
 * `nxvc_config::wm_id` only sets it per stream.
 *
 * `wm[t]` is 0..3 and lands verbatim in tile-header word1 bits 26-27 (syntax
 * v1.2, tool bit NXVC_TOOL_WM_ID), so this changes NO syntax: it is a second
 * way to choose a field the bitstream already carries.  0 means "the frame's
 * matrix", as it does in the header.
 *
 * The stream must already declare the tool, i.e. the encoder was created with
 * `cfg.wm_id != 0`, a built-in `quant_matrix` and no `lossless`; otherwise
 * the call is rejected with NXVC_ERR_ARG rather than silently emitting a
 * stream whose tile headers a conforming decoder would refuse.  A map
 * overrides `cfg.wm_id`, including the `wm_id == 255` per-tile search.
 *
 * The map is consumed by one nxvc_encoder_encode_frame call. */
nxvc_status nxvc_encoder_set_wm_map(nxvc_encoder *enc, const uint8_t *wm,
                                    uint32_t count);

/* --- the loss/concealment hooks (PAPER 2.6, 2.7; docs/TRANSPORT.md 8).
 *
 * Tell the encoder which tiles of the frame it JUST encoded the client holds.
 * `received[t] == 0` means the client did not get tile t, so the encoder
 * replays clause 6.11 concealment on its shadow copy of that frame -- exactly
 * what nxvc_decoder_set_lost_tiles makes the decoder do -- and predicts the
 * next frame from the result.  Call it after encode_frame and before the next
 * one.  With no call every tile counts as received. */
nxvc_status nxvc_encoder_set_received_tiles(nxvc_encoder *enc,
                                            const uint8_t *received,
                                            uint32_t count);

/* The encoder's shadow reconstruction of the most recently encoded frame,
 * after any concealment replay.  Written in the same layout the decoder
 * writes, so a test can compare the two byte for byte. */
nxvc_status nxvc_encoder_shadow_image(const nxvc_encoder *enc, nxvc_image *img);

/* Encode one frame.  qp_map/res_map are per-tile arrays of tile_count bytes
 * in raster order, or NULL for "use base_qp" / "res_level 0". */
nxvc_status nxvc_encoder_encode_frame(nxvc_encoder *enc,
                                      const nxvc_image *img,
                                      const uint8_t *qp_map,
                                      const uint8_t *res_map,
                                      uint8_t *buf, size_t cap,
                                      size_t *out_len);

/* Tile records of the most recent encoded frame. */
const nxvc_tile_info *nxvc_encoder_tiles(const nxvc_encoder *enc,
                                         uint32_t *count);

nxvc_status nxvc_encoder_stats(const nxvc_encoder *enc,
                               nxvc_encode_stats *out);

/* ---------------------------------------------------------------- decoder */
typedef struct nxvc_decoder nxvc_decoder;

nxvc_decoder *nxvc_decoder_create(nxvc_status *st);
void nxvc_decoder_destroy(nxvc_decoder *dec);

/* Parse the stream header.  Consumes 64 + ext_len bytes. */
nxvc_status nxvc_decoder_parse_stream_header(nxvc_decoder *dec,
                                             const uint8_t *buf, size_t len,
                                             size_t *consumed);

nxvc_status nxvc_decoder_stream_info(const nxvc_decoder *dec,
                                     nxvc_stream_info *out);

/* Decode one frame.  `img` planes must be large enough for the stream
 * geometry (see nxvc_decoder_plane_size).  *consumed returns the frame's
 * byte length. */
nxvc_status nxvc_decoder_decode_frame(nxvc_decoder *dec, const uint8_t *buf,
                                      size_t len, nxvc_image *img,
                                      size_t *consumed);

/* Parse only the headers of a frame (for nxv-info). */
nxvc_status nxvc_decoder_scan_frame(nxvc_decoder *dec, const uint8_t *buf,
                                    size_t len, nxvc_frame_info *fi,
                                    size_t *consumed);

nxvc_status nxvc_decoder_plane_size(const nxvc_decoder *dec, int plane,
                                    uint32_t *w, uint32_t *h);

/* Mark tiles of the NEXT frame as not received.  `lost[t] != 0` makes the
 * decoder ignore whatever the bitstream carries for tile t and reconstruct it
 * by clause 6.11: the WARP_SKIP predictor with the tile's stored `last_mv` and
 * no residual.  The flags are consumed by one decode_frame call.  This is the
 * decoder half of the shadow contract; the encoder half is
 * nxvc_encoder_set_received_tiles. */
nxvc_status nxvc_decoder_set_lost_tiles(nxvc_decoder *dec, const uint8_t *lost,
                                        uint32_t count);

uint32_t nxvc_decoder_tile_count(const nxvc_decoder *dec);

const nxvc_tile_info *nxvc_decoder_tiles(const nxvc_decoder *dec,
                                         uint32_t *count);
nxvc_status nxvc_decoder_frame_info(const nxvc_decoder *dec,
                                    nxvc_frame_info *out);

const char *nxvc_status_string(nxvc_status st);

/* ------------------------------------------------------- utility (tests) */
/* Normative YCoCg-R forward/inverse on interleaved planes.  Chroma is
 * stored biased by +(1 << bit_depth) in a uint16 plane. */
void nxvc_ycocgr_forward(const uint8_t *r, const uint8_t *g, const uint8_t *b,
                         uint8_t *y, uint16_t *co, uint16_t *cg, size_t n);
void nxvc_ycocgr_inverse(const uint8_t *y, const uint16_t *co,
                         const uint16_t *cg, uint8_t *r, uint8_t *g,
                         uint8_t *b, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NXVC_NXVC_H */
