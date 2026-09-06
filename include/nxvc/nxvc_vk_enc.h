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
 *   * 8-bit 4:2:0 only.  One encoder codes one eye, or BOTH eyes of a
 *     frame as one stereo frame -- create_info::eyes, whose comment has
 *     the input layout
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

    /* Picture geometry, PER EYE, in luma samples ([SYN] 3.3: a picture is one
     * eye, and a stereo frame carries `eyes` pictures rather than one picture
     * of double width). */
    uint32_t width, height;

    /* 1 or 2.  2 codes both eyes of a frame as ONE nxvc stereo frame in one
     * submission, which is the shape the decoder wants: on the Pico 4 the GPU
     * serialises two mono decoders (concurrent/sequential wall 0.977), while
     * one stereo stream costs 49.25 ms of GPU against 68.93 for two mono
     * decodes -- -28.6 %, almost all of it in Pass A, whose cost is a step
     * function of workgroup count and is starved at 289 tiles.
     *
     * THE INPUT IS ONE SIDE-BY-SIDE PICTURE, eye 0 first.  Every plane is
     * `eyes * width` samples wide and `height` tall, the two eyes' sub-pictures
     * laid out left and right in the same rows -- so `nxvc_vke_image::width`
     * and encode_planes()'s strides span the PAIR, while `width` here does not.
     * That is what the reference encoder takes (`nxvc_config::width` is per eye
     * and the image is side-by-side, Annex D D-3), what the ring holds
     * (inter_layout.h: a tile's x origin in a plane is `eye * width + col*64`),
     * and what the WiVRn compositor already produces, so it costs no repack on
     * any of the three sides.  `width` must be a multiple of 64 when `eyes` is
     * 2, so the seam falls on a tile boundary.
     *
     * The tile grid then spans the pair: `cols = eyes * cols_per_eye`, tile
     * rows run row-major and eye-minor, and the linear index is
     * `row * cols + eye * cols_per_eye + index`.  At 2 x 1088x1088 that is 578
     * tiles against a mono 289.
     *
     * An eye is coded independently: prediction, the reference ring and the
     * search all clamp inside one eye's sub-picture and never sample across
     * the seam.  STEREO (tool 12, mode 4, the cross-eye predictor of
     * docs/STEREO.md) is NOT implemented here -- the stream this encoder emits
     * is a valid stereo stream with that tool bit clear, and is byte-identical
     * to `nxv-enc --eyes 2 --stereo off` at the matching flags. */
    uint32_t eyes;
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
     * decision of docs/adr/0028.  It is refused for 4:4:4, which the inter
     * path does not implement yet.  It works for `eyes == 2`: the search,
     * the ring and the reference walk are per eye, and a stereo inter frame
     * is byte-identical to `nxv-enc --eyes 2` at the matching flags.
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

    /* --- the entropy tool (bit 30, ENTROPY_LITE).
     *
     * One of NXVC_VKE_ENTROPY_* below; 0 takes the default, which is
     * interleaved rANS.  ENTROPY_LITE trades bytes for DECODE time and is
     * therefore a negotiated choice, never a default: the decoder's Pass A
     * costs 8-11 ms per eye per frame on the Pico 4's Adreno 650 at 289 tiles
     * because it is latency-bound on the serial rANS round chain, and Lite has
     * no chain -- 0.651 ms to 0.158 ms per 2048 tiles measured on RADV -- for
     * roughly a third more bytes on 4:2:0.
     *
     * A caller must only select it when the client's advertised tool mask has
     * bit 30 (NXVC_TOOL_ENTROPY_LITE); a decoder without it refuses the stream
     * header outright, which is the correct behaviour and a black screen.
     *
     * Selecting it turns three tools OFF, because the syntax does not allow
     * them together and the reference encoder makes the same substitution at
     * create(): SIGN_HIDE (there is no coder parity to spend a sign on),
     * CUSTOM_TABLES and TAB_V2 (there are no probability tables).  So a Lite
     * stream's mask is not a superset of a rANS one, and
     * nxvc_vk_encoder_stream_header() remains the authority on what a
     * particular stream carries. */
    uint32_t entropy;

    /* The reference distance an inter frame asks for FIRST: 0 is frame N-1,
     * and it is the default.  docs/SYNTAX.md 4.1 `ref_sel`, and the same
     * field `nxv-enc --ref-sel` sets.
     *
     * It is a floor, not a fixed choice.  The encoder starts here and walks
     * outwards to 2 until it finds a frame the headset is believed to hold
     * (see nxvc_vk_encoder_set_frame_held), so a caller that never reports a
     * dropped frame gets ref_sel 0 on every frame and the stream this encoder
     * has always produced.  Values above 2 are clamped: ref_sel is two bits
     * and 3 is reserved.
     *
     * Refused at create() if `inter` is clear and this is not 0. */
    uint32_t ref_sel;

    /* The client CONFIRMS the frames it reconstructs, through
     * nxvc_vk_encoder_set_frame_held(enc, f, 1).
     *
     * Setting it makes confirmations REQUIRED from the first frame instead of
     * from the first one that arrives, which is the difference between "a
     * refusal is impossible" and "a refusal is impossible after the first
     * few frames".  Without it the encoder has nothing but the prediction
     * chain to go on until a confirmation lands, and the chain is optimistic
     * for exactly one round trip -- which is the window the frames right after
     * the initial INTRA fall into.
     *
     * The price is INTRA frames until the first confirmation arrives, and
     * INTRA frames whenever nothing within ref_sel's three-frame reach is
     * confirmed.  How often that happens is decided by the CONFIRMATION
     * LATENCY and not by anything in the encoder: measured at 1088x1088 over a
     * client reconstructing every other frame, a confirmation that arrives
     * within the frame costs 3 % more bytes than a client that drops nothing,
     * one frame of latency costs 31 %, and two frames cost 3.9x.  A caller
     * setting this must put the confirmation on a report that goes out at
     * least once per frame.
     *
     * Refused at create() if `inter` is clear and this is not 0. */
    uint32_t ref_confirm;

    uint32_t flags; /* reserved, pass 0 */
} nxvc_vke_create_info;

void nxvc_vk_encoder_create_info_default(nxvc_vke_create_info *ci);

/* nxvc_vke_create_info::coded_vectors */
#define NXVC_VKE_CV_DEFAULT 0u /* STATIC                                    */
#define NXVC_VKE_CV_NONE    1u /* WARP_SKIP and INTRA only                  */
#define NXVC_VKE_CV_STATIC  2u /* also STATIC_MV: the identity predictor    */

/* nxvc_vke_create_info::entropy */
#define NXVC_VKE_ENTROPY_DEFAULT 0u /* interleaved rANS                      */
#define NXVC_VKE_ENTROPY_RANS    1u /* the same, said out loud               */
#define NXVC_VKE_ENTROPY_LITE    2u /* ENTROPY_LITE / FIXED, tool bit 30     */

typedef struct nxvc_vk_encoder nxvc_vk_encoder;

nxvc_vke_status nxvc_vk_encoder_create(const nxvc_vke_create_info *ci,
                                       nxvc_vk_encoder **out);
void nxvc_vk_encoder_destroy(nxvc_vk_encoder *enc);

/* Human-readable detail for the last failure.  Never NULL. */
const char *nxvc_vk_encoder_last_error(const nxvc_vk_encoder *enc);

/* [additive] Human-readable detail for the last nxvc_vk_encoder_create() on
 * THIS THREAD, readable with no handle.  Never NULL; "no error" when the last
 * create on this thread succeeded or none has run.
 *
 * Every configuration refusal in create() returns before there is an encoder
 * to hang a message on, and the device-creation failure deletes the encoder on
 * its way out, so nxvc_vk_encoder_last_error() cannot reach any of them: a
 * caller got a bare status code and had to guess which field was refused.
 * This names the field and its value, e.g. "bit_depth=10: this encoder codes
 * 8".
 *
 * The storage is thread-local and owned by the library.  It is valid until the
 * next nxvc_vk_encoder_create() on the same thread; copy it to keep it. */
const char *nxvc_vk_encoder_last_create_error(void);

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
 * compositor stores its eyes; pass 0 for a plain 2D image.  With `eyes == 2`
 * the picture in ONE layer is the side-by-side pair, so `width` below is
 * `2 * create_info::width` and `array_layer` still selects a layer, not an
 * eye.  A compositor that keeps its eyes in two array layers instead has to
 * bring them into one side-by-side image itself, or use encode_planes().
 *
 * Everything after E0 is the code encode_planes() runs, so the bitstream is
 * the same bitstream: tests/vk-encoder's api acid test encodes the same
 * picture both ways and requires the two files to be identical. */
/* nxvc_vke_image::flags.
 *
 * EYE_LAYERS: the two eyes are separate ARRAY LAYERS of `image` -- eye 0 at
 * `array_layer`, eye 1 at `array_layer + 1` -- instead of side by side in one
 * layer.  Legal only with create_info::eyes == 2, and then `width` is ONE eye's
 * rather than the pair's, because that is the picture one layer holds.
 *
 * It exists because a compositor's eyes are usually already in separate layers:
 * WiVRn's are, and OpenXR's projection layers generally are.  Without it such a
 * caller has to copy both layers into one side-by-side image before every
 * encode -- a full-frame blit per frame, on the device but not free -- purely to
 * match a layout this pass could just as easily read.  E0's output tile index is
 * pair-wide in both shapes, so the only thing that differs is where a tile
 * READS; the coded planes, and therefore the bitstream, are identical.  The
 * encoder's own tests pin that identity.
 */
#define NXVC_VKE_IMAGE_EYE_LAYERS 0x1u

typedef struct nxvc_vke_image {
    VkImage image;
    VkImageLayout layout; /* must be VK_IMAGE_LAYOUT_GENERAL             */
    /* Eye 0's array layer.  With NXVC_VKE_IMAGE_EYE_LAYERS eye 1 is the layer
     * after it, so the image needs `array_layer + 2` layers. */
    uint32_t array_layer;
    /* The picture in ONE LAYER, by its height.  Without
     * NXVC_VKE_IMAGE_EYE_LAYERS that is `eyes * create()'s width` -- the
     * side-by-side pair, since one layer holds both eyes -- and with it, one
     * eye's, since each layer holds one. */
    uint32_t width, height;
    uint32_t flags;         /* NXVC_VKE_IMAGE_EYE_LAYERS, or 0           */
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

/* Whether the headset reconstructed a frame it was sent.
 *
 * This is the FRAME-level counterpart of set_received_tiles(), and the two
 * answer different questions.  A receipt map says which tiles arrived, which
 * is about the transport; this says whether the decoder actually produced a
 * picture, which is about the decoder -- a frame whose every datagram landed
 * is still not held if the client dropped it from a decode queue it could not
 * keep up with, and that is the common case on a headset, not the rare one.
 *
 * Why it exists.  An inter tile names its reference with `ref_sel`, and
 * docs/SYNTAX.md 4.1 makes a tile naming a picture the decoder does not hold
 * a BITSTREAM error -- ref/src/codec_impl.inc `ref_for` refuses it, so the
 * frame is refused whole, not concealed.  Before this call the only way to
 * say "the client lost a frame" was an all-zero receipt map, which codes the
 * NEXT frame entirely INTRA: correct, and expensive enough that a headset
 * dropping one frame in four spends a fifth of its bitrate on resyncs.  With
 * this call the encoder instead asks for the newest frame the headset still
 * holds -- ref_sel 1 or 2 -- and codes an ordinary inter frame.
 *
 * `frame_number` is the value the encoder put in that frame's header, which
 * is the same number the client reports.  `held` is 0 for "did not
 * reconstruct it".
 *
 * A negative report is transitive: the frame is unusable as a reference, and
 * so is every later frame that predicted from it, however cleanly that one
 * arrived.  The encoder tracks the whole chain, so one call is enough and
 * repeating it is harmless.  A frame coded with no temporal reference at all
 * is unaffected, which is what lets an all-INTRA resync end the cascade.
 *
 * A positive report -- `held == 1` -- is a CONFIRMATION, and it is what makes
 * a refusal structurally impossible rather than merely rarer.
 *
 * The negative report alone cannot do that, and the reason is timing, not
 * logic: it is negative, so silence means "held", and silence is exactly what
 * a frame that was dropped a moment ago also produces.  The encoder codes
 * frame N believing N-1 is held, and only learns otherwise after N has already
 * been refused.  That window is one round trip and no amount of chain
 * reasoning closes it.
 *
 * A confirmation has no window.  A frame the headset says it reconstructed is
 * one it can predict from, whatever happened before or after, so an inter
 * frame that references only confirmed frames cannot be refused.  The cost is
 * a reference one confirmation-latency older -- ref_sel 1 or 2 instead of 0 --
 * and, when nothing within the three-frame reach is confirmed, an INTRA frame,
 * which is decodable where the inter frame it replaces was not.
 *
 * The two are not alternatives.  The negative report stays the fast path: it
 * reaches the encoder on its own message as soon as the headset knows, where a
 * confirmation waits for the next periodic feedback, and it is what stops the
 * encoder spending a frame predicting from something already known to be gone.
 *
 * Until the FIRST confirmation arrives the encoder uses the chain-derived
 * record, so a caller that never sends one behaves exactly as before and needs
 * no flag to say so.  From that point on it uses confirmations only, and there
 * is deliberately no timeout back the other way: a client that has stopped
 * confirming is a client whose held set is unknown, and coding INTRA for it is
 * correct where guessing is not -- and the INTRA frame it does reconstruct
 * starts the confirmations again.
 *
 * The history is sixteen frames deep -- about 180 ms at 90 Hz.  A report for
 * a frame older than that is accepted and has no effect, which is sound:
 * ref_sel reaches three frames back, so nothing that old can be referenced.
 *
 * Falling back to an all-INTRA frame still happens, but only when NONE of the
 * three candidate references is held.  On an intra stream the call is accepted
 * and ignored. */
nxvc_vke_status nxvc_vk_encoder_set_frame_held(nxvc_vk_encoder *enc,
                                               uint32_t frame_number,
                                               int held);

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

/* The single-eye form.  `count` must equal create_info::eyes, so this is the
 * call for a mono stream and a stereo one must use set_views() with two
 * entries, eye 0 first -- one view per eye, because the two eyes have
 * different poses and each gets its own `warp_ext()` record ([SYN] 3.1.1
 * carries `36 * eyes` bytes).  Identical to
 * nxvc_vk_encoder_set_views(enc, view, 1). */
nxvc_vke_status nxvc_vk_encoder_set_view(nxvc_vk_encoder *enc,
                                         const nxvc_vke_view *view);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NXVC_NXVC_VK_ENC_H */
