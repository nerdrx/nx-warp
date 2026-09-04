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
 */
#define NXVC_BITSTREAM_MINOR 4

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
/* --- syntax v1.5 */
#define NXVC_TOOL_INTRA_CFL       (1ull << 24)

/* Tools this reference decoder implements. */
#define NXVC_TOOLS_SUPPORTED                                                  \
    (NXVC_TOOL_INTRA_DC_PLANE | NXVC_TOOL_TRANSFORM_SKIP |                    \
     NXVC_TOOL_RES_LEVEL | NXVC_TOOL_CHROMA444 | NXVC_TOOL_ALPHA |            \
     NXVC_TOOL_LOSSLESS | NXVC_TOOL_CUSTOM_TABLES | NXVC_TOOL_NSUB_VAR |      \
     NXVC_TOOL_PER_TILE_CHROMA | NXVC_TOOL_YCOCGR | NXVC_TOOL_WM_ID |        \
     NXVC_TOOL_INTRA_DIR | NXVC_TOOL_CTX_V2 | NXVC_TOOL_SIGN_HIDE |           \
     NXVC_TOOL_INTER | NXVC_TOOL_WARP | NXVC_TOOL_STEREO |                    \
     NXVC_TOOL_XFORM_4X4_SPLIT | NXVC_TOOL_INTRA_CFL)

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

    /* --- additive since syntax v1.5.  Both change the bitstream. */
    uint32_t split4;            /* 1 = per-block 4x4 transform split (19)   */
    uint32_t cfl;               /* 1 = chroma from luma (tool 24); needs
                                 * intra_dir and ctx_v2                     */

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
                                   intra every frame.                       */
    uint32_t ref_sel;           /* 0..2: reference distance inter tiles ask
                                   for (N-1-ref_sel).  Default 0.           */
    uint32_t mv_range;          /* coarse integer search radius in samples,
                                   0 = the default 16 (PAPER 2.3 step 2)    */
    uint32_t skip_thresh;       /* WARP_SKIP early-out gate, Q8 multiple of
                                   the quantiser's own noise floor
                                   (qstep^2 / 12) per sample; 0 = default   */
    uint32_t mode_lambda_q8;    /* lambda scale of the per-tile MODE
                                   decision, Q8, relative to the trellis's.
                                   Below 256 the decision spends more bits to
                                   keep the reference clean, which is what an
                                   all-reference stream wants; 0 = default  */
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
    /* --- additive since syntax v1.5.  How often the two new tools fire:
     * `blocks_coded` counts every 8x8 residual block of an INTRA tile and
     * `blocks_chroma` the subset in planes 1 and 2; `blocks_split4` is the
     * blocks coded as four 4x4 transforms and `blocks_cfl` the chroma blocks
     * predicted from luma (mode 9), so the natural denominator of the first
     * is `blocks_coded` and of the second `blocks_chroma`. */
    uint64_t blocks_coded, blocks_chroma, blocks_split4, blocks_cfl;
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
} nxvc_tile_info;

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
