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
    /* Accept a tile-row skip bitmap on a stream with NO `INTER` tool bit.
     * Such a stream has no reference ring for a skip to refer to, so the CPU
     * reference refuses a non-zero bitmap and so does this decoder by
     * default; with the flag set the skipped tiles reconstruct
     * deterministically as WARP_SKIP records over a zeroed coefficient slot.
     * [inter] It has nothing to do with an INTER stream, where a skip is
     * ordinary syntax and is always accepted. */
    NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES = 1u << 3,
    /* Use the dense raster-order coefficient layout between Pass A and Pass B
     * instead of the sparse one (PAPER 3.2.5): every tile writes and reads its
     * whole 12.5 KB slot whatever the payload said.  The two produce identical
     * pixels; this exists to measure the difference.  See
     * vk/decoder/passA/syntax_constants.h section 8. */
    NXVC_VKD_FLAG_DENSE_COEF = 1u << 4,
    /* Report the exact coefficient traffic in nxvc_vkd_stats::coef_bytes.  It
     * costs a copy of the per-unit length buffer (264 B per tile) to host
     * memory after every Pass A, so it is off by default; without it
     * coef_bytes reports the reserved slot size, which is what the dense
     * layout actually moves. */
    NXVC_VKD_FLAG_COEF_STATS = 1u << 5,
    /* Write each display format from its own Pass B dispatch instead of
     * writing both from one.  It only makes a difference for a frame that
     * needs two DISPLAY stores: a 4:2:0 stream with a coded alpha plane on
     * the two-plane output.  [inter] The reference-ring slot is also a second
     * store of the same samples, but it is not a display format and is not
     * affected by this flag -- it is written from whichever Pass B dispatch
     * runs first. */
    NXVC_VKD_FLAG_SPLIT_STORES = 1u << 6,

    /* The caller guarantees every submitted frame is complete and contains
     * only independent INTRA/PLANAR tiles. The decoder rejects predictive,
     * concealed, or ATLAS frames and disables its pixel reference ring; the
     * output remains a complete decoded picture. There is no fallback to
     * predictive reconstruction after this opt-in. */
    NXVC_VKD_FLAG_INDEPENDENT_TILES = 1u << 7
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

/* [additive] Human-readable detail for the last nxvc_vk_decoder_create() on
 * THIS THREAD, readable with no handle.  Never NULL; "no error" when the last
 * create on this thread succeeded or none has run.
 *
 * nxvc_vk_decoder_last_error() needs a decoder, and the two failure classes
 * that matter most on a headset do not reliably give the caller one: a failure
 * before the object is allocated has none to hand back, and a caller that does
 * not know the library returns the half-built decoder for exactly this purpose
 * will destroy it and report the bare status name.  That is what happened: an
 * off-by-one descriptor pool made every stream fail at create on the Adreno
 * 650, and the entire report available was "nxvc_vk_decoder_create: vulkan
 * error" -- neither the call nor the VkResult.
 *
 * The string names the failing call and, for a Vulkan failure, the VkResult by
 * its spec name, e.g.
 *
 *   vkAllocateDescriptorSets(d->dev, &da, sets) failed:
 *     VK_ERROR_OUT_OF_POOL_MEMORY (-1000069000)
 *
 * The storage is thread-local and owned by the library.  It is valid until the
 * next nxvc_vk_decoder_create() on the same thread; copy it to keep it. */
const char *nxvc_vk_decoder_last_create_error(void);

/* Device the decoder is running on, for logging.  Never NULL. */
const char *nxvc_vk_decoder_device_name(const nxvc_vk_decoder *dec);

/* [minor 6, additive] The tool bits this decoder implements: exactly the mask
 * a stream's `tools` field must be a subset of, and exactly what a stream
 * setting anything outside is refused against with NXVC_VKD_ERR_VERSION.
 *
 * This is the DECODER's half of the handshake docs/SYNTAX.md 2.3 describes.
 * The tools mask a session ends up using is an intersection of what the
 * encoder can emit and what the receiver offered, and several tools -- 
 * ENTROPY_LITE above all, which buys Pass A time with bits and whose worth
 * only the decoder can judge -- are specified as negotiated for exactly that
 * reason.  Until now this side of it had no name in the ABI and a caller had
 * to try a stream and read the refusal.
 *
 * It is a property of the library build, not of a decoder instance, so it
 * takes no handle and may be called before nxvc_vk_decoder_create().
 * `nxvc_vkd_stream_info::tools` is the other end: what a given stream asks
 * for. */
uint64_t nxvc_vk_decoder_tools_supported(void);

/* The tool bits THIS decoder, on THIS device, will accept -- which can be less
 * than the build implements.  A device that decodes a legal stream wrong, or
 * hangs on one, must not advertise the tool that reaches it: the Adreno 650
 * wedges on the 4:4:4 32x32 conformance vector, so a decoder there clears
 * XFORM_LARGE (bit 27).  This is the number a capability handshake must send;
 * nxvc_vk_decoder_tools_supported() is the build-wide superset and is what to
 * report when there is no device yet.  NULL returns the build-wide mask. */
uint64_t nxvc_vk_decoder_tools(const nxvc_vk_decoder *dec);

/* The same per-device mask, for a caller that has the device's PROPERTIES but
 * no decoder yet.
 *
 * That caller is the capability handshake, and it is the normal case rather
 * than an odd one: a receiver has to tell the sender what it can decode BEFORE
 * a stream exists, and a decoder is not created until the stream description
 * arrives.  Without this entry point such a caller has two choices, and both
 * are wrong -- send nxvc_vk_decoder_tools_supported(), which over-promises on
 * exactly the device the subtraction exists for, or repeat the vendor test in
 * its own tree, which is one rule in two places and drifts.  WiVRn's client did
 * the second, with a runtime comparison to catch the drift; this is what
 * replaces it.
 *
 * `vendor_id` is VkPhysicalDeviceProperties::vendorID and `device_name` its
 * deviceName (NULL is allowed).  Both are used: the vendor id is the reliable
 * half, the name is what catches a Qualcomm part behind a translation layer
 * reporting someone else's id.
 *
 * nxvc_vk_decoder_tools(dec) on a decoder created for that same device returns
 * this same value, and the toolmask test pins that.
 *
 * The macro is the feature test.  An integrator that has to build against both
 * this header and an older one -- which is every integrator during a rollout --
 * needs something to ask, and a declaration is not something the preprocessor
 * can see. */
#define NXVC_VK_DECODER_TOOLS_FOR 1
uint64_t nxvc_vk_decoder_tools_for(uint32_t vendor_id, const char *device_name);

/* --------------------------------------------------------------- stream */
typedef struct nxvc_vkd_stream_info {
    uint32_t width, height; /* luma samples                                */
    uint32_t chroma;        /* 0 = 4:2:0, 1 = 4:4:4                        */
    uint32_t color_transform; /* 0 = none, 1 = YCoCg-R                     */
    uint32_t color_space;     /* descriptive, docs/SYNTAX.md 2.2           */
    uint32_t alpha;           /* the stream carries a 4th plane            */
    uint32_t bit_depth, eyes, num_layers, profile, level;
    uint64_t tools;
    /* [inter] `width`, `height`, `chroma_width`, `chroma_height` and
     * `tiles_x` are PER EYE -- docs/SYNTAX.md 3.3: a picture is one eye.  The
     * transport's column count over the eye pair is `eyes * tiles_x`, and a
     * linear tile index is `row * (eyes * tiles_x) + eye * tiles_x + index`,
     * which is what nxvc_vk_decoder_mark_missing() takes.  `tile_count` is
     * already over the pair.  nxvc_vk_decoder_plane_size() reports the
     * readback layout, which spans the pair. */
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
/* Also signal the binary semaphore nxvc_vk_decoder_binary_semaphore() returns.
 *
 * It exists for the driver that advertises VK_KHR_timeline_semaphore and then
 * refuses to create one -- the Pico 4's Adreno 650, where binary semaphores
 * work and timeline semaphores do not.  Without it a client that has to
 * consume the decoded images on the GPU has no synchronisation object at all
 * and must round-trip through the host (nxvc_vk_decoder_wait) between the
 * decode and its own submission; with it the two pipeline.
 *
 * A binary semaphore is single-use, so the contract is strict: pass this flag
 * only when the very next submission on the same queue WILL wait on the
 * semaphore, exactly once.  Signalling one nobody waits on leaves it signalled
 * and the next frame's submit deadlocks.  It is ignored when the decoder has a
 * real timeline (wait on that instead) and requires NXVC_VKD_SUBMIT_ASYNC,
 * since the synchronous path has already waited on the host by the time it
 * returns and would leave the semaphore signalled. */
#define NXVC_VKD_SUBMIT_SIGNAL_BINARY 2u
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

/* The binary semaphore NXVC_VKD_SUBMIT_SIGNAL_BINARY signals.  VK_NULL_HANDLE
 * when the decoder has a working timeline semaphore, which is the object to
 * use there, or when the device would not create a binary one either. */
VkSemaphore nxvc_vk_decoder_binary_semaphore(const nxvc_vk_decoder *dec);

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
/* ------------------------------------------------- [ATLAS] the atlas (13.12)
 * Under tool bit 31 the decoder's normative output is the ATLAS -- its pixels
 * and its per-tile table -- and NOT a picture.  These read it back, and they
 * are what conformance compares against the reference's
 * nxvc_decoder_atlas_table() / nxvc_decoder_atlas_plane().
 *
 * Both wait for the frame in flight.  Both return NXVC_VKD_ERR_ARG on a
 * decoder whose stream does not set tool bit 31.
 */

/* [SYN] 13.12.9: fill a contiguous run of atlas tile positions from the BASE
 * LAYER instead of coding them.  The client HEVC-decodes the base picture,
 * converts it into the atlas's own coded sample domain with its own kernel,
 * and hands the decoder the buffer to import.
 *
 * `first_tile` and `count` are within `eye`, in that eye's own row-major
 * order.  A contiguous RUN is the primary form because row strips are what the
 * writes coalesce to -- measured at 3.43 us per scattered tile against a full
 * refresh at 0.071 ms when the same bytes go as full-width strips, which is
 * 28x -- so the run is turned into strips inside the decoder rather than
 * passed through as one region per tile.
 *
 * `src` is a buffer already in the atlas's own SLOT-SHAPED layout: the layout
 * nxvc_vk_decoder_atlas_plane() reports, u16 samples, both eyes side by side
 * within each plane.  An image source is not accepted -- 13.12.9 warns that a
 * base picture sampled through an external format can yield
 * (.r,.g,.b) == (Cr,Y,Cb), and a conformance matrix without a
 * non-identity-swizzle device cannot catch getting that wrong.  A buffer
 * carries no such risk: the caller has already done the mapping.
 *
 * SUPERSEDE IS NOT AN ERROR.  A write whose `src_frame` does not ADVANCE the
 * position is dropped rather than applied -- the ordinary case, because the
 * base arrives through a hardware decoder with its own latency while coded
 * tiles come down the usual path, so the two interleave.  Those positions are
 * skipped, the rest of the run is applied, and the call SUCCEEDS.  `applied`
 * and `superseded` (either may be NULL) report how the run split; a caller
 * that needs to know a patch landed must read them and not the status.
 *
 * The entry gets 13.12.9's metadata: identity `C`, `src_frame` as given,
 * `gen` 0, valid, NEVER static, `res_level` 0, and `base_sourced` (flags
 * bit 2) SET.  A later coded tile at the same position clears it.
 *
 * The copy is recorded on the DECODER's command buffer, so it and
 * nxvc_vk_decode_frame() serialise by submission order and the caller needs no
 * fence of its own.
 *
 * Returns NXVC_VKD_ERR_UNSUPPORTED on a non-ATLAS stream, NXVC_VKD_ERR_ARG on
 * a run that leaves the eye or a null buffer.  A `count` of 0 is a no-op, and
 * a fully superseded run is a success with `*applied == 0`. */
/* The decoder's Vulkan handles, for a caller that did NOT adopt a device.
 *
 * nxvc_vk_atlas_write_tiles() takes a VkBuffer on the decoder's device, and a
 * caller that let the decoder create that device had no way to allocate one --
 * which made the entry point callable only by a client that already owned the
 * device (WiVRn does) and untestable by anything that did not.  Any pointer
 * may be NULL.  The handles are owned by the decoder and are valid until
 * nxvc_vk_decoder_destroy(). */
/* [timing] The two numbers every GPU duration this decoder reports is built
 * from: `timestampPeriod` in NANOSECONDS PER TICK, and how many bits of the
 * counter the decoder's queue family actually drives.  Bits above
 * `valid_bits` are UNDEFINED per spec and are masked off before any
 * subtraction; `valid_bits == 0` means the family has no timestamps and every
 * `*_ms` field stays 0.
 *
 * Exposed because a wrong tick rate is invisible in a duration and obvious in
 * the pair -- a bench that prints them alongside a GPU/wall ratio can say
 * WHICH of the two is wrong, and this decoder shipped GPU times about 1.57x
 * high on one device for want of exactly that. */
nxvc_vkd_status nxvc_vk_decoder_timestamp_info(const nxvc_vk_decoder *dec,
                                               float *period_ns,
                                               uint32_t *valid_bits);

nxvc_vkd_status nxvc_vk_decoder_vk_handles(const nxvc_vk_decoder *dec,
                                           VkInstance *instance,
                                           VkPhysicalDevice *physical_device,
                                           VkDevice *device, VkQueue *queue,
                                           uint32_t *queue_family);

typedef struct nxvc_vkd_atlas_src {
    VkBuffer buffer;      /* the patch source; required          */
    VkDeviceSize offset;  /* where the slot-shaped image starts  */
    VkImage image;        /* reserved; must be VK_NULL_HANDLE    */
} nxvc_vkd_atlas_src;

nxvc_vkd_status nxvc_vk_atlas_write_tiles(nxvc_vk_decoder *dec, uint32_t eye,
                                          uint32_t first_tile, uint32_t count,
                                          const nxvc_vkd_atlas_src *src,
                                          uint32_t src_frame,
                                          uint32_t submit_flags,
                                          uint32_t *applied,
                                          uint32_t *superseded);

/* ------------------------------------------ [ATLAS] the display view (13.12.5)
 * The atlas is an SSBO of u16 pairs because that is the layout Pass W reads the
 * reference through.  A client's display pass wants a SAMPLER, so the sampled
 * view is produced BESIDE the atlas, and these select and expose it.
 *
 * NOTHING HERE IS NORMATIVE.  The u16 layout stays what conformance compares;
 * 13.12.5's display warp is not compared at all.  The two forms carry
 * IDENTICAL samples and differ only in how many taps a display pass spends:
 *
 *   R16  three R16_UINT planes -- luma, Cb, Cr.  2.124 ms/pair on a Pico 4
 *        over a full 2176x1088 display pass.
 *   R8   NV12-shaped: R8_UNORM luma at full resolution and R8G8_UNORM chroma
 *        at half, one luma tap plus one chroma tap.  1.086 ms/pair.
 *
 * The gap is the TAP COUNT, not the format: 16-bit to 8-bit at the same tap
 * count is worth 0.24 ms of the 1.04 ms.  Both are kept so the device
 * measurement stays an A/B rather than a claim.
 *
 * THE 8-BIT CONVERSION RULE.  For a `CT_NONE` stream the atlas holds the
 * stream's own YCbCr at the stream's own bit depth (13.12.1), so a sample of
 * an 8-bit stream is already a byte: the R8 view stores its VALUE UNCHANGED as
 * a UNORM byte and the R16 view stores the same value as an integer.  The two
 * views are therefore the same picture, and `vk.atlas.view` asserts exactly
 * that, sample for sample.
 *
 * The R8 view is REFUSED on a stream whose colour transform is not `CT_NONE`:
 * under `CT_YCOCGR` the chroma planes carry the extra bit that transform
 * produces -- 9 bits for an 8-bit stream -- which an 8-bit UNORM cannot hold,
 * and truncating it silently would be a wrong picture rather than a cheaper
 * one. */
typedef enum nxvc_vkd_atlas_view {
    NXVC_VKD_ATLAS_VIEW_NONE = 0, /* no view produced (the default)         */
    NXVC_VKD_ATLAS_VIEW_R16 = 1,  /* three R16_UINT planes                  */
    NXVC_VKD_ATLAS_VIEW_R8 = 2    /* one-tap 8-bit, NV12-shaped; CT_NONE    */
} nxvc_vkd_atlas_view;

/* Takes effect on the next decoded frame.  Returns NXVC_VKD_ERR_UNSUPPORTED on
 * a non-ATLAS stream, or for R8 on a stream with a colour transform. */
nxvc_vkd_status nxvc_vk_decoder_set_atlas_view(nxvc_vk_decoder *dec,
                                               nxvc_vkd_atlas_view view);
nxvc_vkd_atlas_view nxvc_vk_decoder_atlas_view(const nxvc_vk_decoder *dec);

/* The images the view was rendered into, for a client that binds them.
 * `image[0]` is luma; under R8 `image[1]` is the interleaved CbCr and
 * `image[2]` is VK_NULL_HANDLE, under R16 `image[1]` and `image[2]` are Cb and
 * Cr.  Extents are over the eye PAIR, with eye `e` at column `e * (w / eyes)`.
 * Handles are decoder-owned and invalidated by destroy, stream-header reparse,
 * or changing the atlas view mode. Contents change on subsequent decodes.
 * Wait for decode completion before reading/copying and finish that read before
 * the next decode; asynchronous display consumers must snapshot or serialize. */
typedef struct nxvc_vkd_atlas_images {
    VkImage image[3];
    VkImageView view[3];
    VkFormat format[3];
    uint32_t width[3], height[3];
} nxvc_vkd_atlas_images;

/* Optional borrowed R8 display target. The caller owns storage-capable images
 * and views on the decoder's adopted VkDevice,
 * keeps them retired until the decode submission completes, and supplies them
 * in an undefined/discardable layout; the decoder never frees or retains
 * ownership. Only CT_NONE 8-bit 4:2:0 R8 (Y + CbCr) targets are accepted.
 * Reset the target before reusing it after a caller layout transition.
 * atlas_view_read() is unavailable while a borrowed target is active; the caller
 * performs readback with its own image usage flags and synchronization. */
#define NXVC_VKD_ATLAS_BORROWED_TARGET 1
/* Set only between completed decode submissions; changing it while a decode is
 * in flight is rejected. Passing NULL clears the target and restores the
 * decoder-owned atlas images. atlas_images() reports the active target. */
nxvc_vkd_status nxvc_vk_decoder_set_atlas_borrowed_target(
    nxvc_vk_decoder *dec, const nxvc_vkd_atlas_images *target);

/* Opt-in persistent-target catchup.  `generation` is a nonzero value unique
 * for the lifetime of this decoder and must change whenever the caller
 * destroys/recreates the target storage.  Generation zero disables caching
 * and has the same full-refresh semantics as the legacy setter above.
 * `initial_layout` describes both target planes before this decode; a newly
 * allocated generation is discarded and fully populated. Reused targets must
 * retain their prior pixels. The caller must finish all earlier sampling of
 * the target before decoding into it. Output layout is GENERAL. */
nxvc_vkd_status nxvc_vk_decoder_set_atlas_borrowed_target_generation(
    nxvc_vk_decoder *dec, const nxvc_vkd_atlas_images *target,
    uint64_t generation, VkImageLayout initial_layout);

nxvc_vkd_status nxvc_vk_decoder_atlas_images(const nxvc_vk_decoder *dec,
                                             nxvc_vkd_atlas_images *out);

/* Read one view plane back, as bytes: 1 byte per sample for R8 luma, 2
 * interleaved for R8 chroma, 2 (little-endian u16) for an R16 plane.  For
 * inspection and for the conformance A/B; a client samples the images. */
nxvc_vkd_status nxvc_vk_decoder_atlas_view_read(nxvc_vk_decoder *dec, int plane,
                                                uint8_t *out, size_t cap,
                                                uint32_t *w, uint32_t *h,
                                                uint32_t *bytes_per_sample);

/* Byte size of the per-tile table: 64 * tile_count, over the eye pair.  0 if
 * this is not an atlas stream. */
size_t nxvc_vk_decoder_atlas_table_size(const nxvc_vk_decoder *dec);

#define NXVC_VK_DECODER_ATLAS_TABLE_BUFFER 1
/* Borrow the normative GPU table without a readback. No synchronization is
 * performed. Wait for decode completion before reading/copying; finish that
 * read before the next decode or atlas patch mutates the table. The buffer is
 * decoder-owned and invalidated by stream-header reparse or destruction.
 * A queued display consumer must snapshot it or serialize its use. */
nxvc_vkd_status nxvc_vk_decoder_atlas_table_buffer(const nxvc_vk_decoder *dec,
                                                  VkBuffer *out,
                                                  VkDeviceSize *bytes);

/* The whole table, [SYN] 13.12.1's layout exactly, including the 20 reserved
 * bytes a v1 decoder zeroes -- conformance compares all 64. */
nxvc_vkd_status nxvc_vk_decoder_atlas_table(nxvc_vk_decoder *dec, uint8_t *out,
                                            size_t cap);

/* One plane of the atlas PIXELS, in the coded sample domain, as u16 samples.
 * The layout is nxvw_ring_layout()'s: both eyes side by side within each
 * plane, eye `e` beginning at column `e * (*w)`, rows `*stride` samples apart.
 *
 * Note the two EYE CONVENTIONS in one feature, which is the thing most likely
 * to be got wrong: the PIXELS are side by side and the TABLE is interleaved
 * per row ([SYN] 3.3, eye-minor).  */
nxvc_vkd_status nxvc_vk_decoder_atlas_plane(nxvc_vk_decoder *dec, int plane,
                                            uint16_t *out, size_t cap,
                                            uint32_t *w, uint32_t *h,
                                            uint32_t *stride);

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
    double gpu_ms; /* first to last timestamp; 0 means unavailable         */

    uint64_t frame_bytes;   /* the frame unit, header included             */
    uint64_t payload_bytes; /* entropy-coded tile payloads only            */
    uint64_t coef_bytes;    /* coefficient SSBO traffic: written by Pass A
                             * and read back by Pass B, so device traffic is
                             * about twice this.  Exact only with
                             * NXVC_VKD_FLAG_COEF_STATS; otherwise the
                             * reserved slot size.                         */
    uint64_t coef_slot_bytes; /* what the dense layout would have moved     */
    uint32_t tiles;
    uint32_t tiles_skipped; /* tiles covered by a row skip bitmap          */
    uint32_t tiles_tskip;   /* tiles that skipped the transform            */
    uint32_t lane_groups;   /* Pass A dispatches: distinct nsub_log2 values*/
    uint32_t dispatches;    /* Pass A + Pass B dispatches this frame       */
    uint32_t frames;        /* frames decoded so far                       */
    /* --- [inter] APPENDED, not inserted, so a caller built against the
     * older layout keeps reading the same offsets for every field above. */
    uint32_t tiles_concealed; /* tiles nxvc_vk_decoder_mark_missing named  */
    /* Pass W, the predictor dispatch.  Measured around the FIRST Pass W of
     * the frame, which is the whole of it for every frame that does not carry
     * a STEREO tile; a stereo frame runs the pair once per eye and this then
     * covers eye 0 only.  0 on a frame with no inter tile, and on a device
     * with no timestamp support.                                          */
    double pass_w_ms;
    /* --- [SYN] 3.1.2 APPENDED.  Tile-row structures this frame whose
     * `row_present` bit was 0, so their 12-byte header, near-skip records and
     * tile structures were not sent at all.  0 on every frame that does not
     * set frame flags bit 4, which is every stream without tool bit 32.
     *
     * It is reported because "the bytes were elided" is otherwise invisible:
     * an elided row decodes identically to a transmitted all-skipped one, so
     * a test that does not read this cannot tell whether it exercised the
     * tool or merely re-ran the ordinary skip path.                        */
    uint32_t rows_elided;
    /* --- [passb] APPENDED, same rule as above.
     *
     * Pass B's three dispatch segments, broken out.  `pass_b_ms` is the whole
     * of the Pass A -> Pass B window and therefore contains Pass W as well as
     * all three of these, which is a genuine trap for anyone reading it as
     * "the reconstruction": on a live inter stream the warp of the skipped
     * tiles is most of it.  These make that split visible without the caller
     * having to know the dispatch order.
     *
     *   pass_b_skip_ms   WARP_SKIP tiles -- the reconstruct_skip_store module,
     *                    which runs the normative integer pose warp itself
     *   pass_b_coded_ms  every other non-INTRA tile
     *   pass_b_dir_ms    INTRA tiles on the directional-intra wavefront module
     *
     * Measured around eye pass 0, the same convention `pass_w_ms` already
     * uses: a frame with a STEREO tile runs the segments once per eye and
     * these then cover eye 0 only.  A segment with no tiles reports 0 and its
     * tile count says why.  All zero on a device with no timestamp support,
     * and on one whose query pool is shorter than 12 (`ts_count`).
     *
     * These are timestamps around dispatches, so they include the pipeline
     * drain between segments and do not sum to `pass_b_ms`.  The gap is real
     * -- it is what the segment split costs -- and is left visible rather
     * than distributed.                                                    */
    double pass_b_skip_ms;
    double pass_b_coded_ms;
    double pass_b_dir_ms;
    uint32_t tiles_skip_seg;  /* tiles in the WARP_SKIP segment, eye pass 0 */
    uint32_t tiles_coded_seg; /* tiles in the other-non-INTRA segment       */
    uint32_t tiles_dir_seg;   /* tiles on the directional-intra module      */
    /* --- [SYN] 13.12.6 APPENDED.  Coded tiles this frame DROPPED because the
     * position already held a generation from this frame or a later one --
     * which a base-layer patch can produce, since 13.12.9 lets one carry a
     * `src_frame` ahead of the stream.  It is a REPORT and not a loss: the
     * position holds something newer, and the encoder must not answer it with
     * a refresh.                                                           */
    uint32_t tiles_superseded;
    /* --- [passb] APPENDED, same rule again.
     *
     * The copy segment: skip tiles whose prediction is their reference
     * unchanged, decided on the HOST (warp_tile_is_copy()) and dispatched to a
     * module with no coordinate pipeline.  Both are 0 on a frame where no tile
     * qualifies, which is every frame on an encoder that does not snap a
     * near-identity pose to the identity -- so a run reporting
     * tiles_identity_seg == 0 has NOT exercised the path.               */
    double pass_b_identity_ms;
    uint32_t tiles_identity_seg;
    /* --- [ATLAS] APPENDED, same rule as every block above: after the
     * existing fields, never inserted, so a caller built against an older
     * header keeps every offset it was compiled with.  Guarded by
     * NXVC_VK_DECODER_ATLAS_STATS.
     *
     * These exist for a client wiring the atlas up (WiVRn) that has to show
     * what the decoder is doing without a readback of the 37 kB table every
     * frame. */

    /* Which of [SYN] 13.12.11's two modes this frame took:
     *   0  not an ATLAS stream at all (tool bit 31 clear)
     *   1  an ATLAS frame -- 13.12 as written; skipped tiles untouched
     *   2  a PICTURE frame -- frame flags bit 5; the ordinary picture model
     *      against a reference assembled from the atlas, and the atlas then
     *      rebuilt from the reconstruction
     * Derived from the tools word and the frame header, so it is what the
     * WIRE said and not what the decoder decided.                         */
    uint32_t frame_mode;

    /* Atlas entries whose `valid` bit is set AFTER this frame -- the advance's
     * invalidations and this frame's write-backs both accounted for.
     *
     * It cannot be derived on the host: validity is decided by 3.1.1's
     * envelope check INSIDE the composition, on the device.  So the compose
     * dispatch counts what survived its advance and the write-back counts what
     * it newly validated, and the two are summed here -- exact, and with no
     * per-frame readback of the table, which is the one thing tile streaming
     * exists to remove.  On a PICTURE frame it is the tile count by
     * construction, because 13.12.11 step 3 validates every position.
     *
     * Like the timestamps, it describes the most recently COMPLETED frame: an
     * async caller reading it mid-flight gets the previous frame's number
     * rather than a stall.                                                 */
    uint32_t atlas_entries_valid;

    /* Tile positions re-posed by a PICTURE frame's assembly (13.12.11 step 1),
     * which is every position or none -- so this is the tile count on a
     * PICTURE frame and 0 on an ATLAS one.  Kept as a count rather than a flag
     * because it is the term that scales the mode's cost.                  */
    uint32_t tiles_assembled;

    /* The tiles the WARP_SKIP module warped, which is the same number as
     * `tiles_skip_seg` above and is deliberately duplicated under a name that
     * says what it MEANS rather than which dispatch segment produced it.
     * `tiles_skip_seg` is eye pass 0's segment population; this is "how many
     * tiles cost a pose warp this frame", which is the question a client
     * budgeting a frame is actually asking.  Under ATLAS it is 0, because a
     * skipped tile is not reconstructed at all.                            */
    uint32_t tiles_warped_skip;

    /* PICTURE frames decoded since the decoder was created -- a RUNNING total,
     * for a caller that differences it across an interval rather than
     * sampling every frame.  `frames` above is the matching denominator.   */
    uint32_t picture_frames;

    /* `pass_b_identity_ms` and `tiles_identity_seg` are ABOVE, from
     * passb-adreno: they landed in main first and keep the offsets their
     * callers were built against, and these six follow them.  They were
     * deliberately never duplicated here -- one number at two offsets is a
     * struct conflict waiting at every future merge.                       */
} nxvc_vkd_stats;

/* The feature test for the six atlas fields above, for an integrator building
 * against both this header and an older one during a rollout -- a struct field
 * is not something the preprocessor can see. */
#define NXVC_VK_DECODER_ATLAS_STATS 1

/* The feature test for the six pass_b_*_ms / tiles_*_seg fields, for an
 * integrator building against both this header and an older one during a
 * rollout -- a struct field is not something the preprocessor can see.
 *
 * `rows_elided` sits before them because it was on main first and keeps the
 * offset its callers were built against; appending ours after it is what makes
 * this merge ABI-safe in both directions. */
#define NXVC_VK_DECODER_PASSB_SEGMENTS 1
/* The identity/copy segment above, which arrived after the other three. */
#define NXVC_VK_DECODER_PASSB_IDENTITY 1

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

/* ------------------------------------------------------- loss concealment
 * [inter] Mark the tiles the client did NOT receive, by linear tile index
 * (docs/SYNTAX.md 3.3: `tile = row * cols + eye * cols_per_eye + index`).
 *
 * The marks apply to the NEXT frame decoded and are consumed by it, whether
 * or not that frame is accepted; a second frame starts clean.  This is
 * docs/TRANSPORT.md 8's "the decoder needs an API to mark tiles not received
 * so concealment replays exactly": each marked tile is reconstructed by
 * running the WARP_SKIP predictor with the tile's stored `last_mv` and no
 * residual -- exactly a legitimately skipped tile, which is why there is no
 * separate concealment path to test and why the encoder can replay it
 * (docs/SYNTAX.md 13.6).  A marked tile's bytes are still parsed, so the
 * frame stays self-delimiting, and then discarded; its prediction state does
 * not advance, and a near-skip correction naming it is NOT applied, because
 * the correction travelled in a row header the transport does not replicate.
 *
 * It is the mirror of the reference decoder's
 * `nxvc_decoder_set_lost_tiles()`, and the two produce byte-identical
 * pictures for the same marks -- which is what
 * `tests/vk-decoder/conformance`'s loss test asserts over 100 frames.
 *
 * Passing count == 0 clears any pending marks.  An index at or above the
 * stream's tile count is NXVC_VKD_ERR_ARG and nothing is marked.  Requires a
 * parsed stream header.
 */
nxvc_vkd_status nxvc_vk_decoder_mark_missing(nxvc_vk_decoder *dec,
                                             const uint32_t *tile_ids,
                                             uint32_t count);

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
