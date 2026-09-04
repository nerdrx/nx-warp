/* nxvc_vk.h - C ABI for the NX Warp Vulkan compute decoder (nxvc_vk_decoder).
 *
 * The library turns an .nxv byte stream into decoded images on a Vulkan
 * compute queue.  A frame costs exactly two dispatches:
 *
 *   Pass A  vk/decoder/passA  interleaved rANS entropy decode -> int16
 *                             coefficients + CBF bits, one workgroup per
 *                             group of tiles (docs/PAPER.md 3.2.1)
 *   Pass B  vk/decoder/passB  dequantize, inverse transform, DC-plane intra
 *                             prediction, resample, colour -> output images
 *
 * Everything above the tile payload -- stream header, frame header, quant
 * matrices, probability tables, tile-row headers with their skip bitmaps and
 * the tile headers themselves -- is parsed on the host by this library, which
 * is what produces the per-tile descriptors Pass A needs and the tile records
 * Pass B needs.  See vk/decoder/README.md.
 *
 * The normative specification is docs/SYNTAX.md and the CPU reference in
 * ref/ (C ABI: <nxvc/nxvc.h>).  This decoder reproduces `nxv-dec` output bit
 * for bit; where it cannot, it refuses the stream rather than guessing.
 *
 * Naming: types are `nxvc_vkd_*` and entry points `nxvc_vk_decoder_*`, with
 * the one-shot decode call spelled `nxvc_vk_decode_frame`.  The shared Vulkan
 * runtime in vk/common owns the `nxvc_vk_*` type prefix (<nxvc/vk/nxvc_vk.h>);
 * the two headers can be included in the same translation unit.
 *
 * Threading: an nxvc_vk_decoder is not internally synchronised.  One decoder
 * decodes one stream.
 */
#ifndef NXVC_NXVC_VK_H
#define NXVC_NXVC_VK_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXVC_VK_DECODER_ABI_VERSION 1

/* --------------------------------------------------------------- status */
/* Values 0 and -1..-6 match nxvc_vk_status in <nxvc/vk/nxvc_vk.h>; -7..-9
 * mirror the bitstream errors of nxvc_status in <nxvc/nxvc.h>. */
typedef enum nxvc_vkd_status {
    NXVC_VKD_OK = 0,
    NXVC_VKD_ERR_ARG = -1,         /* bad argument from the caller         */
    NXVC_VKD_ERR_UNSUPPORTED = -2, /* legal syntax outside this decoder    */
    NXVC_VKD_ERR_VULKAN = -3,      /* a VkResult came back non-success     */
    NXVC_VKD_ERR_NOMEM = -4,       /* host or device allocation failed     */
    NXVC_VKD_ERR_NO_DEVICE = -5,   /* no physical device matched           */
    NXVC_VKD_ERR_INTERNAL = -6,
    NXVC_VKD_ERR_BITSTREAM = -7, /* malformed / illegal bitstream          */
    NXVC_VKD_ERR_TRUNCATED = -8, /* ran off the end of the buffer          */
    NXVC_VKD_ERR_VERSION = -9    /* magic / version / tool mask refused    */
} nxvc_vkd_status;

const char *nxvc_vk_decoder_status_string(nxvc_vkd_status s);

/* --------------------------------------------------------------- output */
/* Which image set Pass B writes.  The reconstruction is identical; only the
 * final store differs (vk/decoder/passB/README.md). */
typedef enum nxvc_vkd_output {
    /* rgba8ui, luma resolution.  Chroma is upsampled to full resolution and
     * YCoCg-R inverted when the stream asks for it.  Alpha in A. */
    NXVC_VKD_OUT_RGBA8 = 0,
    /* rgb10_a2ui, luma resolution.  8-bit samples replicated to 10 bits. */
    NXVC_VKD_OUT_RGB10A2 = 1,
    /* Two-plane 4:2:0 YCbCr passthrough: r8ui luma + rg8ui interleaved CbCr
     * at half resolution.  Requires a 4:2:0 stream with no colour transform.
     * This is what the WiVRn NX client's reprojection shader already samples
     * and it halves reference-slot memory on the headset. */
    NXVC_VKD_OUT_YCBCR420 = 2,
    /* Pick NXVC_VKD_OUT_YCBCR420 for a 4:2:0 stream with no colour
     * transform, NXVC_VKD_OUT_RGBA8 otherwise.  This is the mode that can
     * reproduce `nxv-dec` output for every v1 stream. */
    NXVC_VKD_OUT_AUTO = 3
} nxvc_vkd_output;

/* ---------------------------------------------------------------- flags */
typedef enum nxvc_vkd_create_flags {
    /* Keep a host-visible readback buffer and copy the output images into it
     * at the end of every decode.  Needed by nxvc_vk_decoder_read_planes(). */
    NXVC_VKD_FLAG_READBACK = 1u << 0,
    /* Turn on the Vulkan validation layer when the library creates its own
     * instance.  Ignored for an adopted device. */
    NXVC_VKD_FLAG_VALIDATION = 1u << 1,
    /* Force Pass A's LDS read-pointer fallback instead of the subgroup
     * ballot path.  Both produce identical output; the default picks the
     * ballot whenever the device's subgroups are wide enough. */
    NXVC_VKD_FLAG_LDS_FALLBACK = 1u << 2,
    /* Accept a tile-row skip bitmap.  v1 has no reference frames, so the CPU
     * reference refuses a non-zero bitmap (NXVC_ERR_UNSUPPORTED) and so does
     * this decoder by default.  With the flag set, skipped tiles reconstruct
     * deterministically as WARP_SKIP records over a zeroed coefficient slot,
     * which is the shape the Phase 2 inter predictor plugs into. */
    NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES = 1u << 3
} nxvc_vkd_create_flags;

/* --------------------------------------------------------------- create */
/* Leave `device` NULL to have the library create its own instance, pick a
 * physical device and create a device with one compute queue.  Set all five
 * handles to adopt a device the host already owns (WiVRn's server runs on
 * Monado's VkDevice; the Android client runs on the client's device); the
 * library then creates and destroys nothing it did not allocate itself. */
typedef struct nxvc_vkd_create_info {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;

    /* Device selection when the library creates its own device: a substring
     * matched case-insensitively against VkPhysicalDeviceProperties.
     * deviceName, e.g. "RADV" or "llvmpipe".  NULL takes the first device
     * that can run the decoder. */
    const char *device_name;

    uint32_t output_format; /* nxvc_vkd_output */
    uint32_t flags;         /* nxvc_vkd_create_flags */
} nxvc_vkd_create_info;

void nxvc_vk_decoder_create_info_default(nxvc_vkd_create_info *ci);

typedef struct nxvc_vk_decoder nxvc_vk_decoder;

nxvc_vkd_status nxvc_vk_decoder_create(const nxvc_vkd_create_info *ci,
                                       nxvc_vk_decoder **out);
void nxvc_vk_decoder_destroy(nxvc_vk_decoder *dec);

/* Human-readable detail for the last failure.  Never NULL. */
const char *nxvc_vk_decoder_last_error(const nxvc_vk_decoder *dec);

/* Device the decoder is running on, for logging.  Never NULL. */
const char *nxvc_vk_decoder_device_name(const nxvc_vk_decoder *dec);

/* --------------------------------------------------------------- stream */
typedef struct nxvc_vkd_stream_info {
    uint32_t width, height; /* luma samples                                */
    uint32_t chroma;        /* 0 = 4:2:0, 1 = 4:4:4                        */
    uint32_t color_transform; /* 0 = none, 1 = YCoCg-R                     */
    uint32_t color_space;     /* descriptive, docs/SYNTAX.md 2.2           */
    uint32_t alpha;           /* the stream carries a 4th plane            */
    uint32_t bit_depth, eyes, num_layers, profile, level;
    uint64_t tools;
    uint32_t tiles_x, tiles_y, tile_count;
    uint32_t chroma_width, chroma_height;
    uint32_t ext_len;
    uint32_t output_format; /* resolved nxvc_vkd_output                    */
} nxvc_vkd_stream_info;

/* Parse the 64-byte stream header and its TLV area, and size every resource
 * the stream needs.  Must be called before the first decode. */
nxvc_vkd_status nxvc_vk_decoder_parse_stream_header(nxvc_vk_decoder *dec,
                                                    const uint8_t *buf,
                                                    size_t len,
                                                    size_t *consumed);

nxvc_vkd_status nxvc_vk_decoder_stream_info(const nxvc_vk_decoder *dec,
                                            nxvc_vkd_stream_info *out);

/* ---------------------------------------------------------------- decode */
/* Decode exactly one frame unit starting at `bytes`.  Uploads the frame,
 * dispatches Pass A and Pass B, and signals the decoder's timeline
 * semaphore.  `*consumed` returns the frame's byte length so the caller can
 * walk a file.
 *
 * The call is synchronous by default: it waits for the timeline value before
 * returning, so the output images are ready and the stats are filled in.
 * Pass NXVC_VKD_SUBMIT_ASYNC to return right after submission and wait
 * yourself with nxvc_vk_decoder_wait(), or on the timeline semaphore. */
nxvc_vkd_status nxvc_vk_decode_frame(nxvc_vk_decoder *dec,
                                     const uint8_t *bytes, size_t len,
                                     size_t *consumed);

#define NXVC_VKD_SUBMIT_ASYNC 1u
nxvc_vkd_status nxvc_vk_decode_frame_ex(nxvc_vk_decoder *dec,
                                        const uint8_t *bytes, size_t len,
                                        uint32_t submit_flags,
                                        size_t *consumed);

/* Wait for the most recently submitted frame.  UINT64_MAX blocks. */
nxvc_vkd_status nxvc_vk_decoder_wait(nxvc_vk_decoder *dec, uint64_t timeout_ns);

/* The timeline semaphore the decoder signals, and the value the most recent
 * submission signals it with.  A compositor waits on these instead of
 * blocking the CPU. */
VkSemaphore nxvc_vk_decoder_timeline(const nxvc_vk_decoder *dec);
uint64_t nxvc_vk_decoder_timeline_value(const nxvc_vk_decoder *dec);

/* ---------------------------------------------------------------- output */
/* The images Pass B wrote.  They live in VK_IMAGE_LAYOUT_GENERAL and stay
 * valid until the decoder is destroyed; a frame overwrites them in place.
 * `count` is 1 for the RGBA8 and RGB10A2 formats and 2 for the two-plane
 * 4:2:0 format (plane 0 luma r8ui, plane 1 interleaved CbCr rg8ui). */
#define NXVC_VKD_MAX_IMAGES 3
typedef struct nxvc_vkd_images {
    uint32_t count;
    VkImage image[NXVC_VKD_MAX_IMAGES];
    VkImageView view[NXVC_VKD_MAX_IMAGES];
    VkFormat format[NXVC_VKD_MAX_IMAGES];
    uint32_t width[NXVC_VKD_MAX_IMAGES];
    uint32_t height[NXVC_VKD_MAX_IMAGES];
} nxvc_vkd_images;

nxvc_vkd_status nxvc_vk_decoder_images(const nxvc_vk_decoder *dec,
                                       nxvc_vkd_images *out);

/* Copy the decoded frame back into host memory in the reference decoder's
 * planar layout: plane 0 = Y (or R after YCoCg-R), planes 1 and 2 = Co/Cg
 * (or G/B), plane 3 = alpha when the stream carries one.  Chroma planes are
 * half size in each dimension for a 4:2:0 stream.  This is byte for byte
 * what `nxv-dec` writes, which is what makes the conformance test a
 * pixel-for-pixel comparison.
 *
 * Requires NXVC_VKD_FLAG_READBACK.  Pass NULL for a plane to skip it. */
nxvc_vkd_status nxvc_vk_decoder_read_planes(nxvc_vk_decoder *dec,
                                            uint8_t *const plane[4],
                                            const int32_t stride[4]);

/* Byte size of one plane in the layout read_planes writes. */
nxvc_vkd_status nxvc_vk_decoder_plane_size(const nxvc_vk_decoder *dec,
                                           int plane, uint32_t *w,
                                           uint32_t *h);

/* ----------------------------------------------------------------- stats */
typedef struct nxvc_vkd_stats {
    /* Wall clock on the host. */
    double parse_ms;  /* container / header parse                          */
    double submit_ms; /* record + submit                                   */
    double total_ms;  /* the whole nxvc_vk_decode_frame call               */
    /* Device timestamps, 0 when the device has no timestamp support. */
    double pass_a_ms;
    double pass_b_ms;
    double gpu_ms; /* first to last timestamp                              */

    uint64_t frame_bytes;   /* the frame unit, header included             */
    uint64_t payload_bytes; /* entropy-coded tile payloads only            */
    uint64_t coef_bytes;    /* coefficient SSBO traffic: written by Pass A
                             * and read back by Pass B, so device traffic is
                             * about twice this                            */
    uint32_t tiles;
    uint32_t tiles_skipped; /* tiles covered by a row skip bitmap          */
    uint32_t tiles_tskip;   /* tiles that skipped the transform            */
    uint32_t lane_groups;   /* Pass A dispatches: distinct nsub_log2 values*/
    uint32_t dispatches;    /* Pass A + Pass B dispatches this frame       */
    uint32_t frames;        /* frames decoded so far                       */
} nxvc_vkd_stats;

nxvc_vkd_status nxvc_vk_decoder_stats(const nxvc_vk_decoder *dec,
                                      nxvc_vkd_stats *out);

/* ---------------------------------------------------------------- tuning
 * Two knobs that exist for measurement, not for normal use.  Both may be set
 * at any time and take effect on the next decoded frame.
 */

/* The directional-intra wavefront schedule Pass B is compiled with
 * (docs/SYNTAX.md 7.4 and 7.6), as a bit mask:
 *
 *   0  the normative derivation: left, above and above-right references.
 *      22 wavefront steps for a res_level-0 luma plane.  THE DEFAULT, and the
 *      only value a conformant stream may be decoded with today.
 *   1  drop the above-right reference.        15 steps, +0.24 % rate
 *   2  confine the dependency to 32x32 sub-tiles.  10 steps, +1.6 % rate
 *   3  both.                                   7 steps, +1.8 % rate
 *
 * This is a BITSTREAM property, not a performance option: values 1..3 decode
 * a stream encoded under the matching restriction bit-exactly and decode a
 * normal stream to different pixels.  ref/ produces such streams only when it
 * is built with -DNXVC_DIR_SCHED_EXPERIMENT.  Returns NXVC_VKD_ERR_ARG for a
 * value above 3.
 */
nxvc_vkd_status nxvc_vk_decoder_set_dir_sched(nxvc_vk_decoder *dec,
                                              uint32_t sched);
uint32_t nxvc_vk_decoder_dir_sched(const nxvc_vk_decoder *dec);

/* Group Pass B's workgroups by tile shape (mode, res_level, chroma444,
 * alpha_mode, tskip) instead of dispatching them in raster order.  Host-side
 * reordering only: the decoded image is bit-identical either way, because
 * every write address is derived from the tile index rather than from the
 * workgroup index.  Off by default. */
nxvc_vkd_status nxvc_vk_decoder_set_tile_sort(nxvc_vk_decoder *dec,
                                              uint32_t on);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NXVC_NXVC_VK_H */
