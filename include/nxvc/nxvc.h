/* nxvc.h - C ABI for the NX Warp reference codec (nxvc_ref).
 *
 * This is the bit-exact CPU reference implementation of the NX Warp v1
 * bitstream.  The normative syntax is docs/SYNTAX.md; this library IS the
 * specification in executable form.  Phase 1 scope: INTRA only.
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
 */
#define NXVC_BITSTREAM_MINOR 2

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

/* Tools this reference decoder implements. */
#define NXVC_TOOLS_SUPPORTED                                                  \
    (NXVC_TOOL_INTRA_DC_PLANE | NXVC_TOOL_TRANSFORM_SKIP |                    \
     NXVC_TOOL_RES_LEVEL | NXVC_TOOL_CHROMA444 | NXVC_TOOL_ALPHA |            \
     NXVC_TOOL_LOSSLESS | NXVC_TOOL_CUSTOM_TABLES | NXVC_TOOL_NSUB_VAR |      \
     NXVC_TOOL_PER_TILE_CHROMA | NXVC_TOOL_YCOCGR | NXVC_TOOL_WM_ID)

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
} nxvc_config;

/* Where the bits went, for the most recent encoded frame. */
typedef struct nxvc_encode_stats {
    uint64_t bytes_total, bytes_frame_header, bytes_tables;
    uint64_t bytes_row_headers, bytes_tile_headers, bytes_payload;
    uint64_t bytes_rans_init;   /* 4 bytes per active lane per tile        */
    uint64_t bits_dc_plane, bits_luma_blocks, bits_chroma_blocks;
    uint64_t bits_alpha_blocks;
    uint64_t tiles, tiles_tskip, tiles_res[3], lanes_total;
} nxvc_encode_stats;

void nxvc_config_default(nxvc_config *cfg);

/* ---------------------------------------------------------------- layout */
typedef struct nxvc_tile_layout {
    uint32_t tiles_x, tiles_y, tile_count, tile_size;
} nxvc_tile_layout;

void nxvc_tile_layout_get(uint32_t width, uint32_t height,
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
