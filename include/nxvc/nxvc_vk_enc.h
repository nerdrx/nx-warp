/* nxvc_vk_enc.h - C ABI for the NX Warp Vulkan compute encoder
 * (nxvc_vk_encoder).
 *
 * The mirror of <nxvc/nxvc_vk.h>: that header turns an .nxv stream into
 * images, this one turns images into an .nxv stream.  The passes are
 * vk/encoder/README.md's E0..E5; a frame is one command buffer.
 *
 * WHAT THIS ENCODER IS FOR, and what it is not.  It exists so that a
 * compositor can encode NX Warp at frame rate: the CPU reference encoder
 * (<nxvc/nxvc.h>) is docs/SYNTAX.md in executable form and costs hundreds of
 * milliseconds a frame, which is a slideshow on a headset.  It is NOT a
 * superset of the reference.  It implements the intra half of the v1
 * bitstream and refuses -- explicitly, at create() -- everything else:
 *
 *   * no inter prediction, no pose warp, no reference ring
 *   * no directional intra (DC-plane intra only)
 *   * no rate control: the QP is fixed for the life of the stream
 *   * no resolution levels, no alpha plane, no custom probability tables
 *   * eight rANS lanes exactly (paper 6.3 fixes v1 at eight)
 *   * 8-bit 4:2:0 only, one eye per encoder
 *
 * The tools mask it emits is `nxvc_vk_encoder_tools_supported()`, and it is
 * the mask a stream from this encoder actually carries.  Every bitstream
 * minor-6 tool -- INTRA_CFL, XFORM_4X4_SPLIT, CTX_V3, TAB_V2, ENTROPY_LITE,
 * XFORM_LARGE -- is OFF.  This is not a limitation being papered over: a
 * stream this encoder produces is byte-identical to
 *
 *   nxv-enc --no-rdo --intra-dir off --no-custom-tables \
 *           --split4x4 off --cfl off --tab v1 --xform 8 --entropy rans
 *
 * for the same picture and the same QP, and tests/vk-encoder/acid.cmake pins
 * exactly that.  Byte-identity with the reference at the same settings is the
 * contract; being able to reach every setting is not.
 *
 * Threading: an nxvc_vk_encoder is not internally synchronised, and it submits
 * on the queue the caller adopted it with.  One encoder encodes one stream,
 * from one thread, and the caller serialises that queue against its own use.
 */
#ifndef NXVC_NXVC_VK_ENC_H
#define NXVC_NXVC_VK_ENC_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXVC_VK_ENCODER_ABI_VERSION 1

/* --------------------------------------------------------------- status */
/* 0 and -1..-6 match nxvc_vk_status in <nxvc/vk/nxvc_vk.h> and the decoder's
 * nxvc_vkd_status, so a host that already maps one can map this one. */
typedef enum nxvc_vke_status {
    NXVC_VKE_OK = 0,
    NXVC_VKE_ERR_ARG = -1,         /* bad argument from the caller          */
    NXVC_VKE_ERR_UNSUPPORTED = -2, /* legal configuration this cannot code  */
    NXVC_VKE_ERR_VULKAN = -3,      /* a VkResult came back non-success      */
    NXVC_VKE_ERR_NOMEM = -4,       /* host or device allocation failed      */
    NXVC_VKE_ERR_NO_DEVICE = -5,   /* no physical device matched            */
    NXVC_VKE_ERR_INTERNAL = -6,
    NXVC_VKE_ERR_OVERFLOW = -10    /* the frame outgrew its tile slots      */
} nxvc_vke_status;

const char *nxvc_vk_encoder_status_string(nxvc_vke_status s);

/* --------------------------------------------------------------- create */
/* Leave `device` NULL to have the library create its own instance and pick a
 * physical device.  Set all five handles to adopt a device the host already
 * owns -- WiVRn's server runs on Monado's VkDevice, which is where the
 * compositor's image already lives -- and the library then creates and
 * destroys nothing it did not allocate itself.  All five or none.
 *
 * `queue` must be from a compute-capable family; create() checks and returns
 * NXVC_VKE_ERR_ARG if it is not. */
typedef struct nxvc_vke_create_info {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;

    /* Device selection when the library creates its own device: an index into
     * vkEnumeratePhysicalDevices order. */
    uint32_t device_index;

    /* Picture geometry, per eye, in luma samples. */
    uint32_t width, height;
    uint32_t eyes;      /* 1; a WiVRn stream is one eye                     */
    uint32_t chroma;    /* 0 = 4:2:0.  4:4:4 is refused.                    */
    uint32_t bit_depth; /* 8                                                */

    /* The fixed quantiser, 0..63, for every tile of every frame.  There is no
     * rate control on this path; see the header comment. */
    uint32_t base_qp;

    /* Quantiser weighting matrix, 0..3.  1 is the reference's frame matrix and
     * what `nxv-enc --matrix 1` selects. */
    uint32_t quant_matrix;

    uint32_t flags; /* reserved, pass 0 */
} nxvc_vke_create_info;

void nxvc_vk_encoder_create_info_default(nxvc_vke_create_info *ci);

typedef struct nxvc_vk_encoder nxvc_vk_encoder;

nxvc_vke_status nxvc_vk_encoder_create(const nxvc_vke_create_info *ci,
                                       nxvc_vk_encoder **out);
void nxvc_vk_encoder_destroy(nxvc_vk_encoder *enc);

/* Human-readable detail for the last failure.  Never NULL. */
const char *nxvc_vk_encoder_last_error(const nxvc_vk_encoder *enc);

/* Device the encoder is running on, for logging.  Never NULL. */
const char *nxvc_vk_encoder_device_name(const nxvc_vk_encoder *enc);

/* The tool bits a stream from this encoder carries.  A property of the library
 * build, not of an instance, so it takes no handle.  The decoder's half of the
 * same handshake is nxvc_vk_decoder_tools_supported(). */
uint64_t nxvc_vk_encoder_tools_supported(void);

/* --------------------------------------------------------------- stream */
/* The stream header: magic, geometry, tool mask, TLV area.  Constant for the
 * life of the encoder, and the decoder must parse it before the first frame.
 * Returns NXVC_VKE_ERR_ARG if `cap` is too small; `*len` is always set to the
 * length needed. */
nxvc_vke_status nxvc_vk_encoder_stream_header(const nxvc_vk_encoder *enc,
                                              uint8_t *buf, size_t cap,
                                              size_t *len);

/* Tile grid, per frame.  `cols` is over the eye pair when eyes > 1, matching
 * the transport's column count. */
void nxvc_vk_encoder_tile_grid(const nxvc_vk_encoder *enc,
                               uint32_t *cols, uint32_t *rows);

/* ---------------------------------------------------------------- tiles */
/* One coded tile of the frame that encode() just produced.
 *
 * `offset` and `length` are the tile's OWN bytes within that frame, which the
 * reference codec's C ABI cannot report (nxvc_tile_info carries a length but
 * no offset).  A transport that has them can put one coded tile in one
 * datagram and lose one tile when one datagram is lost; without them it has to
 * cut the frame into fixed chunks and a single loss costs the whole frame.
 * They are exact: E5 computes the layout, so this is a read of it and not a
 * reconstruction. */
typedef struct nxvc_vke_tile {
    uint32_t index;  /* raster order within the frame */
    uint32_t offset; /* byte offset into the frame bitstream */
    uint32_t length; /* bytes */
    uint8_t qp;
    uint8_t mode;      /* nxvc_tile_mode numbering */
    uint8_t res_level; /* always 0 on this path */
    uint8_t ref_delta; /* always 3 (no temporal reference) on this path */
} nxvc_vke_tile;

/* --------------------------------------------------------------- encode */
/* Encode one frame from planar 8-bit 4:2:0 host memory.  `cb` and `cr` are
 * half size in both axes.  On success `*out` points at the frame's bytes,
 * valid until the next encode() or destroy(), and `*out_len` is their length.
 *
 * This is the portable entry point and it costs an upload of the picture.  A
 * compositor that already has the frame in a VkImage on this device should
 * prefer nxvc_vk_encoder_encode_image(). */
nxvc_vke_status nxvc_vk_encoder_encode_planes(nxvc_vk_encoder *enc,
                                              const uint8_t *y, size_t y_stride,
                                              const uint8_t *cb,
                                              const uint8_t *cr,
                                              size_t chroma_stride,
                                              const uint8_t **out,
                                              size_t *out_len);

/* Per-tile records of the frame encode() just produced.  Valid until the next
 * encode() or destroy().  `*count` is the tile count. */
const nxvc_vke_tile *nxvc_vk_encoder_tiles(const nxvc_vk_encoder *enc,
                                           uint32_t *count);

/* Wall time of the last encode(), in milliseconds, measured around the queue
 * submit and its wait.  It is what the caller paid, not what the GPU was busy
 * for -- the two differ by the submit and the fence wait -- which is the
 * number a frame budget is actually spent from. */
double nxvc_vk_encoder_last_encode_ms(const nxvc_vk_encoder *enc);

/* Host time inside the last encode() spent laying the caller's planes out in
 * the tile-major order the kernels read, in milliseconds.  Zero for the image
 * entry point.  Reported separately because it is the cost the image path
 * exists to remove. */
double nxvc_vk_encoder_last_upload_ms(const nxvc_vk_encoder *enc);

/* -------------------------------------------------------------- feedback */
/* Which tiles of the last frame the client actually holds.  Accepted and
 * IGNORED on this path: it exists so a caller's plumbing does not have to
 * branch on the backend.  Every frame this encoder produces is all-intra, so
 * there is no prediction for a lost tile to corrupt and nothing for the
 * encoder to replay on a shadow copy.  It becomes meaningful when inter
 * prediction lands.  Returns NXVC_VKE_OK. */
nxvc_vke_status nxvc_vk_encoder_set_received_tiles(nxvc_vk_encoder *enc,
                                                   const uint8_t *received,
                                                   uint32_t count);

/* The frame's pose and projection.  Accepted and IGNORED on this path, for the
 * same reason: the warp matrix an intra frame carries is the identity, because
 * there is no reference to warp.  Returns NXVC_VKE_OK. */
typedef struct nxvc_vke_view {
    double qx, qy, qz, qw;
    double fov_left, fov_right, fov_up, fov_down;
} nxvc_vke_view;

nxvc_vke_status nxvc_vk_encoder_set_views(nxvc_vk_encoder *enc,
                                          const nxvc_vke_view *views,
                                          uint32_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NXVC_NXVC_VK_ENC_H */
