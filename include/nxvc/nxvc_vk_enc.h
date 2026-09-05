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
 *   * inter prediction, the pose warp and the reference ring are OPTIONAL,
 *     off unless create_info::inter is set; the mode decision is then the
 *     integer one of docs/adr/0028, and a tile skips, carries a STATIC_MV
 *     vector, or codes intra.  WARP_MV and QUAD_MV are not implemented --
 *     measured, and not worth what they would cost; see create_info's
 *     `coded_vectors`
 *   * no directional intra (DC-plane intra only)
 *   * no rate control of its own, and no per-tile quantiser: one QP codes
 *     every tile of a frame.  That QP is settable between frames --
 *     nxvc_vk_encoder_set_qp() below -- so a host CAN run a rate controller
 *     over this encoder; what the encoder does not have is a controller.
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
 *   nxv-enc --no-rdo --intra-dir off --custom-tables \
 *           --split4x4 off --cfl off --tab v2 --ctx v3 --sign-hide \
 *           --xform 8 --entropy rans
 *
 * and, with create_info::inter set, that command line plus
 *
 *           --inter on --int-decision on --int-coded-vectors off \
 *           --preset fast --me-effort 1 --quad-mv off --near-skip off \
 *           --drift-refresh off --intra-period <T> --poses <track>
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

    /* The quantiser, 0..63, for every tile of the frames that follow.  The
     * encoder runs no rate control of its own, so this is the QP until the
     * caller says otherwise with nxvc_vk_encoder_set_qp(). */
    uint32_t base_qp;

    /* Quantiser weighting matrix, 0..3.  1 is the reference's frame matrix and
     * what `nxv-enc --matrix 1` selects. */
    uint32_t quant_matrix;

    /* --- inter prediction (Phase 2).
     *
     * `inter` turns on the reference ring, the pose warp and the integer mode
     * decision of docs/adr/0028.  It is refused for eyes > 1 and for 4:4:4,
     * which the inter path does not implement yet.
     *
     * `intra_period` is the rolling intra refresh: 1/T of the tiles are forced
     * INTRA every frame and each tile position is refreshed exactly once every
     * T frames, which is the loss-recovery bound PAPER 2.6 states.  0 takes
     * the default 180.  It is the FIXED scheme; the drift-driven one needs an
     * exact client shadow this encoder does not keep, which is why
     * `nxv-enc --drift-refresh off` is part of the configuration this encoder
     * is byte-identical to.
     *
     * A caller that sets `inter` MUST call nxvc_vk_encoder_set_view() before
     * every encode, including the first.  Without a view the warp is the
     * identity, which predicts a still picture correctly and a turning head
     * badly -- it is not an error, it is a worse stream, and nothing else will
     * say so. */
    uint32_t inter;
    uint32_t intra_period;

    /* Which coded-vector mode the inter decision may choose, on top of
     * WARP_SKIP and INTRA.  One of NXVC_VKE_CV_* below; 0 takes the default,
     * which is STATIC because it is free.
     *
     * "Free" is measured, not asserted: on the 1088x1088 head-turn clip at
     * QP 30, STATIC_MV takes the stream from 13446 to 9303 bytes a frame --
     * 2.73x to 3.95x against intra -- and the encode from 4.79 ms to 4.69,
     * because a frame with fewer CODED tiles is cheaper in table training and
     * in E4/E5 than the search costs before E3.  There is no configuration in
     * which turning it off is the better trade, which is why the default is on
     * and `NONE` exists only so a caller can pin the older stream shape.
     *
     * WARP_MV is deliberately absent rather than merely unimplemented: it is
     * 6.2 % fewer bytes and 0.12 dB WORSE than STATIC_MV alone on the same
     * clip, and its predictor is the full homography, which the search cannot
     * evaluate without either nine more Pass W dispatches a frame or a second
     * copy of the warp arithmetic.  vk/encoder/README.md has the route that
     * would make it cheap, and the measurement that says it is not urgent.
     *
     * Refused at create() if `inter` is clear and this is not 0, for the same
     * reason `intra_period` is: a field that cannot take effect should say so
     * rather than be quietly ignored. */
    uint32_t coded_vectors;

    uint32_t flags; /* reserved, pass 0 */
} nxvc_vke_create_info;

void nxvc_vk_encoder_create_info_default(nxvc_vke_create_info *ci);

/* nxvc_vke_create_info::coded_vectors */
#define NXVC_VKE_CV_DEFAULT 0u /* STATIC                                    */
#define NXVC_VKE_CV_NONE    1u /* WARP_SKIP and INTRA only                  */
#define NXVC_VKE_CV_STATIC  2u /* also STATIC_MV: the identity predictor    */

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

/* ------------------------------------------------------------ quantiser */
/* Change the quantiser, 0..63, for the frames that follow.  This is the whole
 * of the rate-control surface: the encoder never moves the QP on its own, and
 * a host that wants a bitrate runs its own controller and calls this between
 * frames.  Cheap and synchronous -- nothing is recreated, no pipeline is
 * rebuilt, no allocation is made -- so calling it every frame is the intended
 * use, and calling it with the QP it already has is a no-op.
 *
 * Byte identity holds ACROSS the change, which is the property that makes it
 * usable: after set_qp(q), every frame this encoder codes is byte for byte the
 * frame an encoder CREATED at q would have coded at that frame number, and so
 * (by the acid tests' claim at a fixed QP) the frame `nxv-enc --qp q` codes at
 * that frame number.  Nothing of the previous quantiser survives the call.
 * tests/vk-encoder/qp_switch.cmake pins exactly that, frame by frame, against
 * a mixture of quantisers.
 *
 * Must not be called while an encode() is in flight; like the rest of this
 * ABI it is not internally synchronised.  Returns NXVC_VKE_ERR_ARG for a QP
 * above 63.
 *
 * The stream header does NOT depend on the QP, so a decoder that parsed it
 * before the first frame stays correct: a frame carries its own base_qp. */
nxvc_vke_status nxvc_vk_encoder_set_qp(nxvc_vk_encoder *enc, uint32_t qp);

/* The quantiser the next frame will be coded at. */
uint32_t nxvc_vk_encoder_qp(const nxvc_vk_encoder *enc);

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
 * This is the portable entry point and it costs an upload of the picture --
 * a repack on the host, a staging write and a copy.  A compositor that
 * already has the frame in a VkImage on this device should prefer
 * nxvc_vk_encoder_encode_image() below, which costs none of the three. */
nxvc_vke_status nxvc_vk_encoder_encode_planes(nxvc_vk_encoder *enc,
                                              const uint8_t *y, size_t y_stride,
                                              const uint8_t *cb,
                                              const uint8_t *cr,
                                              size_t chroma_stride,
                                              const uint8_t **out,
                                              size_t *out_len);

/* ---------------------------------------------------- the image entry point
 *
 * Encode one frame straight out of a compositor image, with no host copy of
 * the picture anywhere: E0 reads the image's two planes through UINT storage
 * views and writes the tile-major planes E3 consumes, on the device.  This is
 * the entry point paper 3.6 describes and the one a compositor should use;
 * encode_planes() is the portable fallback for a host that has pixels rather
 * than an image.
 *
 * What the image must be, all of it checked by the caller and none of it by
 * this library (there is no way to interrogate a VkImage for how it was
 * created):
 *
 *   * VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, at least `width` x `height`, on the
 *     VkDevice this encoder adopted;
 *   * created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and a
 *     VkImageFormatListCreateInfo that names R8_UINT and R8G8_UINT as well as
 *     the plane formats -- a list that omits them makes the plane views
 *     invalid, and a driver is entitled to refuse them;
 *   * VK_IMAGE_USAGE_STORAGE_BIT, with VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
 *     where the planar format has no storage feature of its own, which on
 *     every driver worth naming it does not;
 *   * in VK_IMAGE_LAYOUT_GENERAL and owned by this encoder's queue family
 *     when the call is made.  A layout transition or a queue-family
 *     acquisition is the caller's to record, on the submit that produced the
 *     picture; this library submits only its own passes.
 *
 * The call submits and waits, so the image is free again the moment it
 * returns -- and must not be written before that.
 *
 * `array_layer` selects a layer of an array image, which is how WiVRn's
 * compositor stores its eyes; pass 0 for a plain 2D image.
 *
 * Everything after E0 is the code encode_planes() runs, so the bitstream is
 * the same bitstream: tests/vk-encoder's api acid test encodes the same
 * picture both ways and requires the two files to be identical. */
typedef struct nxvc_vke_image {
    VkImage image;
    VkImageLayout layout; /* must be VK_IMAGE_LAYOUT_GENERAL             */
    uint32_t array_layer;
    uint32_t width, height; /* the picture in it; must match create()    */
    uint32_t flags;         /* reserved, pass 0                          */
} nxvc_vke_image;

nxvc_vke_status nxvc_vk_encoder_encode_image(nxvc_vk_encoder *enc,
                                             const nxvc_vke_image *img,
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
/* Which tiles of the last frame the client actually holds: `count` bytes, one
 * per tile in raster order, nonzero for "the client has it".
 *
 * On an intra stream this is accepted and ignored -- there is no prediction
 * for a lost tile to corrupt.  On an inter stream it is the loss-recovery
 * contract, and the rule is deliberately the blunt one:
 *
 *   a tile the client does NOT hold is coded INTRA on the next frame.
 *
 * That is stronger than it has to be and weaker than the reference's.  The
 * reference keeps an exact client shadow and replays concealment into it, so
 * it can often repair a lost tile with a cheap coded residual instead; this
 * encoder does not keep a shadow, so the only thing it can say honestly about
 * a tile the client is missing is that predicting from it is unsound.  Coding
 * it fresh is always correct and costs one tile.
 *
 * **An all-zero map is therefore a full reset**, and that is the intended way
 * to express one: the client holds nothing, so every tile is coded INTRA on
 * the next frame and the client resynchronises from it.  A resumed session
 * calls exactly that.
 *
 * The effect lasts ONE frame.  A tile coded INTRA is a tile the client can
 * hold again, so the encoder does not keep forcing it; a caller whose client
 * is still missing tiles says so again.
 *
 * Returns NXVC_VKE_ERR_ARG if `count` is not the tile count. */
nxvc_vke_status nxvc_vk_encoder_set_received_tiles(nxvc_vk_encoder *enc,
                                                   const uint8_t *received,
                                                   uint32_t count);

/* The frame's pose and projection, for the frame the NEXT encode() codes.
 *
 * The fields are OpenXR's: a unit quaternion in the convention docs/WARP.md
 * 2.1 fixes, and an XrFovf whose left and down angles are negative.  They are
 * `nxwarp_codec_view`'s fields exactly, so a WiVRn backend passes its own
 * struct through unchanged.
 *
 * The encoder keeps the view that went with each reference-ring slot and
 * derives warp_ext() from that slot's view and this frame's, which is what
 * makes the matrix describe the motion BETWEEN the two pictures rather than
 * the absolute pose of either.  So the call has to be made for every frame,
 * including the first: a gap in the track is not detected and produces a
 * confident prediction of the wrong place.
 *
 * On an intra stream it is accepted and ignored -- there is no reference to
 * warp. */
typedef struct nxvc_vke_view {
    double qx, qy, qz, qw;
    double fov_left, fov_right, fov_up, fov_down;
} nxvc_vke_view;

nxvc_vke_status nxvc_vk_encoder_set_views(nxvc_vk_encoder *enc,
                                          const nxvc_vke_view *views,
                                          uint32_t count);

/* The single-eye form, which is what a WiVRn stream wants: one encoder codes
 * one eye, so `count` is always 1 and the array is ceremony.  Identical to
 * nxvc_vk_encoder_set_views(enc, view, 1). */
nxvc_vke_status nxvc_vk_encoder_set_view(nxvc_vk_encoder *enc,
                                         const nxvc_vke_view *view);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NXVC_NXVC_VK_ENC_H */
