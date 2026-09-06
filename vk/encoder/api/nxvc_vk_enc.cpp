/* nxvc_vk_enc.cpp -- the C ABI of the NX Warp Vulkan compute encoder.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A thin, deliberately dull wrapper over nxe::VkEncoder and the frame driver
 * in nxe_host: this file owns no coding logic at all.  Everything it does is
 * translate a caller's configuration into the `nxe::Config` the harness and
 * the acid test already drive, so that the library and `nxvc-vkenc` are the
 * same encoder and the byte-identity the test pins is the byte-identity the
 * library ships.
 *
 * The configuration is FIXED at the values tests/vk-encoder/acid.cmake pins,
 * not merely defaulted to them: there is no way through this ABI to ask for
 * directional intra, a custom table set, or any minor-6 tool.  That is the
 * point.  A caller who wants those wants the reference encoder.
 *
 * This translation unit includes vk/encoder/forward only.  stats/tile_stats.h
 * and forward/nxe_enc.h both define a struct tagged `nxe_frame_params`, with
 * different members, and they are never included together; E0 therefore lives
 * in its own translation unit and talks to this one through plain buffers.
 */

#include <nxvc/nxvc_vk_enc.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nxe_host.h"
#include "nxe_vk.h"

namespace {

/* ------------------------------------------------- the create diagnostic
 * nxvc_vk_encoder_last_error() needs an encoder, and create() is precisely
 * the call that may not produce one: every argument refusal below returns a
 * bare status code, and the VkEncoder failure at the end deletes the object
 * and throws its message away with the comment that there is nowhere to hang
 * it.  There is now.
 *
 * Thread-local, never NULL, never empty; the decoder half of this ABI has the
 * same call for the same reason (nxvc_vk_decoder_last_create_error). */
char *create_err_buf() {
    static thread_local char b[512] = "no error";
    return b;
}
void set_create_err(const char *s) {
    char *b = create_err_buf();
    std::snprintf(b, 512, "%s", s && s[0] ? s : "unspecified failure");
}
nxvc_vke_status createerr(nxvc_vke_status st, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    set_create_err(b);
    return st;
}

/* The tool bits a stream from this encoder carries.  Kept as an explicit
 * literal rather than derived from the stream header so that a change to
 * either one has to be made here too, deliberately. */
constexpr uint64_t kToolsEmitted =
    (1ull << 0) |  /* INTRA_DC_PLANE                                        */
    (1ull << 10) | /* INTER: with create_info::inter; see the note below    */
    (1ull << 11) | /* WARP: travels with INTER, never alone                 */
    (1ull << 6) |  /* CUSTOM_TABLES: tables trained on the frame            */
    (1ull << 21) | /* CTX_V2: the 16-context entropy model                  */
    (1ull << 22) | /* SIGN_HIDE: sign data hiding, exact in E4              */
    (1ull << 25) | /* CTX_V3: the neighbour-conditioned model               */
    (1ull << 26) | /* TAB_V2: the compact transmitted table set             */
    (1ull << 30);  /* ENTROPY_LITE: with create_info::entropy; see below     */

} // namespace

struct nxvc_vk_encoder {
    nxe::Config cfg;
    nxe::Frame frame;
    nxe::VkEncoder vk;
    std::vector<uint8_t> header;
    std::vector<nxvc_vke_tile> tiles;
    std::string err = "no error";
    std::string device_name;
    uint32_t frame_number = 0;
    double last_ms = 0.0;
    double last_upload_ms = 0.0;
    bool created = false;
};

extern "C" const char *nxvc_vk_encoder_status_string(nxvc_vke_status s) {
    switch (s) {
    case NXVC_VKE_OK: return "OK";
    case NXVC_VKE_ERR_ARG: return "bad argument";
    case NXVC_VKE_ERR_UNSUPPORTED: return "unsupported configuration";
    case NXVC_VKE_ERR_VULKAN: return "Vulkan error";
    case NXVC_VKE_ERR_NOMEM: return "out of memory";
    case NXVC_VKE_ERR_NO_DEVICE: return "no usable device";
    case NXVC_VKE_ERR_INTERNAL: return "internal error";
    case NXVC_VKE_ERR_OVERFLOW: return "frame outgrew its tile slots";
    }
    return "unknown status";
}

extern "C" void nxvc_vk_encoder_create_info_default(nxvc_vke_create_info *ci) {
    if (!ci) return;
    std::memset(ci, 0, sizeof *ci);
    ci->eyes = 1;
    ci->chroma = 0;
    ci->bit_depth = 8;
    ci->base_qp = 28;
    ci->quant_matrix = 1;
}

extern "C" uint64_t nxvc_vk_encoder_tools_supported(void) {
    /* The SUPERSET a stream from this library may carry, which is what a
     * capability handshake wants.  Bits 10 and 11 are in it because the
     * library can code inter, and are absent from an individual stream whose
     * create_info left `inter` clear -- nxvc_vk_encoder_stream_header() is the
     * authority on what one stream actually carries.  The distinction is new:
     * before inter, every stream this library could produce carried the same
     * mask and the two questions had one answer.  ENTROPY_LITE (30) is the
     * same shape of claim and one step stronger: a stream that carries it
     * carries NEITHER 6, 22 nor 26, because the syntax forbids the
     * combination.  So this mask is a superset that no single stream equals,
     * and it is the right answer to "what could you send me", which is the
     * question a handshake asks. */
    return kToolsEmitted;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_create(const nxvc_vke_create_info *ci,
                                                  nxvc_vk_encoder **out) {
    /* Cleared so a caller reading this after a SUCCESSFUL create does not see
     * the last failure of a previous one. */
    set_create_err("no error");
    if (!ci || !out)
        return createerr(NXVC_VKE_ERR_ARG,
                         "nxvc_vk_encoder_create: %s must not be NULL",
                         !ci ? "create_info" : "out");
    *out = nullptr;

    /* Refuse, loudly and at create() time, everything this path cannot code.
     * The alternative -- accepting the field and quietly coding something
     * else -- is the failure mode that costs a day of bisecting a bitstream. */
    if (ci->width == 0 || ci->height == 0)
        return createerr(NXVC_VKE_ERR_ARG,
                         "width=%u height=%u: both must be non-zero",
                         ci->width, ci->height);
    if (ci->eyes != 1 && ci->eyes != 2)
        return createerr(NXVC_VKE_ERR_UNSUPPORTED,
                         "eyes=%u: this encoder codes 1 or 2", ci->eyes);
    /* A stereo frame's tile grid spans the eye pair ([SYN] 3.3), and both the
     * width and the seam have to fall on a tile boundary for an eye's
     * sub-picture to be addressable: `eye * pw` must be even for the ring's
     * uint stores, and `cols_per_eye` must be a whole number of tiles or the
     * skip bitmap's per-eye bound stops meaning anything. */
    if (ci->eyes == 2 && (ci->width % 64) != 0)
        return createerr(NXVC_VKE_ERR_UNSUPPORTED,
                         "eyes=2 needs width=%u a multiple of 64", ci->width);
    if (ci->chroma != 0)
        return createerr(NXVC_VKE_ERR_UNSUPPORTED,
                         "chroma=%u: this encoder codes 4:2:0 (0)",
                         ci->chroma);
    if (ci->bit_depth != 8)
        return createerr(NXVC_VKE_ERR_UNSUPPORTED,
                         "bit_depth=%u: this encoder codes 8", ci->bit_depth);
    if (ci->base_qp > 63)
        return createerr(NXVC_VKE_ERR_ARG, "base_qp=%u: the range is 0..63",
                         ci->base_qp);
    if (ci->quant_matrix > 3)
        return createerr(NXVC_VKE_ERR_ARG,
                         "quant_matrix=%u: the range is 0..3",
                         ci->quant_matrix);
    if (ci->intra_period > 0 && ci->inter == 0)
        return createerr(NXVC_VKE_ERR_ARG,
                         "intra_period=%u needs inter=1",
                         ci->intra_period);
    if (ci->coded_vectors > NXVC_VKE_CV_STATIC)
        return createerr(NXVC_VKE_ERR_ARG,
                         "coded_vectors=%u: the range is 0..%d",
                         ci->coded_vectors, (int)NXVC_VKE_CV_STATIC);
    if (ci->coded_vectors != NXVC_VKE_CV_DEFAULT && ci->inter == 0)
        return createerr(NXVC_VKE_ERR_ARG,
                         "coded_vectors=%u needs inter=1", ci->coded_vectors);
    if (ci->entropy > NXVC_VKE_ENTROPY_LITE)
        return createerr(NXVC_VKE_ERR_ARG,
                         "entropy=%u: the range is 0..%d (rans, lite)",
                         ci->entropy, (int)NXVC_VKE_ENTROPY_LITE);
    if (ci->ref_sel > 2)
        return createerr(NXVC_VKE_ERR_ARG,
                         "ref_sel=%u: the range is 0..2 (3 is reserved)",
                         ci->ref_sel);
    if (ci->ref_sel != 0 && ci->inter == 0)
        return createerr(NXVC_VKE_ERR_ARG, "ref_sel=%u needs inter=1",
                         ci->ref_sel);
    if (ci->ref_confirm != 0 && ci->inter == 0)
        return createerr(NXVC_VKE_ERR_ARG, "ref_confirm=%u needs inter=1",
                         ci->ref_confirm);
    /* ATLAS (31).  [SYN] 2: it requires INTER.  Refusing is the whole point --
     * an ATLAS stream with no temporal reference is not a degraded stream, it
     * is a contradiction, and accepting it would emit a tool bit for a
     * reconstruction process the frames do not use. */
    if (ci->atlas != 0 && ci->inter == 0)
        return createerr(NXVC_VKE_ERR_ARG, "atlas=%u needs inter=1",
                         ci->atlas);
    /* [SYN] 4.1 and 13.12.6: ref_sel SHALL be 0 in every tile header of an
     * ATLAS stream.  A caller that asked for both is refused rather than
     * quietly given one of them: the atlas holds one generation per tile
     * position, so a non-zero ref_sel is a request the model cannot honour and
     * silently honouring the other half would produce a stream the caller did
     * not ask for. */
    if (ci->atlas != 0 && ci->ref_sel != 0)
        return createerr(NXVC_VKE_ERR_ARG,
                         "atlas=1 forces ref_sel to 0 ([SYN] 13.12.6); "
                         "ref_sel=%u was requested",
                         ci->ref_sel);

    const bool adopting = ci->device != VK_NULL_HANDLE;
    if (adopting && (!ci->physical_device || !ci->queue))
        /* all five handles or none */
        return createerr(NXVC_VKE_ERR_ARG,
                         "adopting a device needs all five handles; %s is NULL",
                         !ci->physical_device ? "physical_device" : "queue");

    auto *e = new (std::nothrow) nxvc_vk_encoder();
    if (!e)
        return createerr(NXVC_VKE_ERR_NOMEM,
                         "out of memory allocating the encoder");

    /* Everything below is the acid test's configuration, spelled out.  The
     * fields that are not settable through the ABI are the tools that are off. */
    e->cfg.w = int(ci->width * ci->eyes);
    e->cfg.h = int(ci->height);
    e->cfg.eyes = int(ci->eyes);
    e->cfg.chroma444 = false;
    e->cfg.qp = int(ci->base_qp);
    e->cfg.matrix = int(ci->quant_matrix);
    e->cfg.inter = ci->inter != 0;
    e->cfg.intra_period =
        ci->intra_period ? int(ci->intra_period) : 180;
    /* DEFAULT is STATIC: it is smaller and faster on every measurement, so
     * the caller who says nothing gets it. */
    e->cfg.int_coded_vectors =
        ci->inter != 0 && ci->coded_vectors != NXVC_VKE_CV_NONE;
    e->cfg.ref_sel = ci->inter != 0 ? int(ci->ref_sel) : 0;
    e->cfg.ref_confirm = ci->inter != 0 && ci->ref_confirm != 0;
    e->cfg.atlas = ci->inter != 0 && ci->atlas != 0;
    e->cfg.wm_id = 0;
    e->cfg.chroma_qp_off = 0;
    e->cfg.nsub_log2 = 3; /* eight rANS lanes; paper 6.3 fixes v1 at eight */
    e->cfg.tskip = 0;
    e->cfg.ctx_v2 = true;
    /* The entropy-side tools of bitstream minor 6, all on.  Every one of them
     * is lossless -- the coefficients E3 produces are the same either way --
     * and every one is covered by `vk.encoder.acid.*` against `nxv-enc` at the
     * matching flags.  On the measurement in vk/encoder/README.md they are
     * 9.4 % of the frame at 1088x1088 QP 30, which at ~1 ms of headset decode
     * per kilobyte is frame rate. */
    e->cfg.ctx_v3 = true;
    e->cfg.custom_tables = true;
    e->cfg.tab_v2 = true;
    e->cfg.table_iters = 3;
    e->cfg.sign_hide = true;
    /* ENTROPY_LITE, if the caller asked for it.  The three tools it is
     * incompatible with are left set here and turned off by nxe::setup(),
     * which is the one place that resolves them -- the same substitution
     * `nxvc_encoder_create` makes, in one place rather than two. */
    e->cfg.entropy_lite = ci->entropy == NXVC_VKE_ENTROPY_LITE ? 1 : 0;
    e->cfg.intra_dir = false;
    e->cfg.dir_layer = false;
    e->cfg.dir_mode_seed = 0;
    e->cfg.device = int(ci->device_index);
    e->cfg.quiet = true;

    nxe::setup(e->cfg, e->frame);
    nxe::build_tables(e->cfg, e->frame);
    nxe::fill_modes(e->cfg, e->frame, 0);
    e->header = nxe::stream_header(e->cfg, e->frame);

    nxe::Adopt adopt{};
    if (adopting) {
        adopt.instance = ci->instance;
        adopt.physical_device = ci->physical_device;
        adopt.device = ci->device;
        adopt.queue = ci->queue;
        adopt.queue_family = ci->queue_family;
    }

    std::string err;
    if (!e->vk.create(e->cfg, e->frame, err, adopting ? &adopt : nullptr)) {
        e->err = err;
        const std::string keep = err;
        delete e;
        /* The message is worth more than the object, and until
         * nxvc_vk_encoder_last_create_error() existed there was nowhere to
         * hang it once the handle was gone.  Now there is, so the message
         * survives the delete. */
        set_create_err(keep.c_str());
        return keep.find("device") != std::string::npos ? NXVC_VKE_ERR_NO_DEVICE
                                                        : NXVC_VKE_ERR_VULKAN;
    }
    e->created = true;
    e->device_name = "nxvc_vk_encoder (E0/E3/E4/E5, intra only)";
    *out = e;
    return NXVC_VKE_OK;
}

extern "C" void nxvc_vk_encoder_destroy(nxvc_vk_encoder *e) { delete e; }

extern "C" const char *nxvc_vk_encoder_last_error(const nxvc_vk_encoder *e) {
    return e ? e->err.c_str() : "null encoder";
}

extern "C" const char *nxvc_vk_encoder_last_create_error(void) {
    return create_err_buf();
}

extern "C" const char *nxvc_vk_encoder_device_name(const nxvc_vk_encoder *e) {
    return e ? e->device_name.c_str() : "";
}

extern "C" nxvc_vke_status nxvc_vk_encoder_stream_header(
    const nxvc_vk_encoder *e, uint8_t *buf, size_t cap, size_t *len) {
    if (!e || !len) return NXVC_VKE_ERR_ARG;
    *len = e->header.size();
    if (!buf || cap < e->header.size()) return NXVC_VKE_ERR_ARG;
    std::memcpy(buf, e->header.data(), e->header.size());
    return NXVC_VKE_OK;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_set_qp(nxvc_vk_encoder *e,
                                                  uint32_t qp) {
    if (!e) return NXVC_VKE_ERR_ARG;
    if (qp > 63) {
        e->err = "the quantiser must be 0..63";
        return NXVC_VKE_ERR_ARG;
    }
    if (int(qp) == e->cfg.qp) return NXVC_VKE_OK;
    /* Nothing here is a rebuild.  nxe::set_qp writes the frame parameter
     * record and the job list, both of which nxe_vk.cpp re-uploads on every
     * encode, so the next frame simply carries the new quantiser -- and the
     * stream header, the pipelines, the descriptors and every device
     * allocation are untouched because none of them depends on the QP. */
    nxe::set_qp(e->cfg, e->frame, int(qp));
    return NXVC_VKE_OK;
}

extern "C" uint32_t nxvc_vk_encoder_qp(const nxvc_vk_encoder *e) {
    return e ? uint32_t(e->cfg.qp) : 0u;
}

extern "C" void nxvc_vk_encoder_tile_grid(const nxvc_vk_encoder *e,
                                          uint32_t *cols, uint32_t *rows) {
    if (!e) return;
    if (cols) *cols = e->frame.fp.tiles_x * e->frame.fp.eyes;
    if (rows) *rows = e->frame.fp.tiles_y;
}

extern "C" const nxvc_vke_tile *nxvc_vk_encoder_tiles(const nxvc_vk_encoder *e,
                                                      uint32_t *count) {
    if (!e) {
        if (count) *count = 0;
        return nullptr;
    }
    if (count) *count = uint32_t(e->tiles.size());
    return e->tiles.data();
}

extern "C" double nxvc_vk_encoder_last_encode_ms(const nxvc_vk_encoder *e) {
    return e ? e->last_ms : 0.0;
}

extern "C" double nxvc_vk_encoder_last_upload_ms(const nxvc_vk_encoder *e) {
    return e ? e->last_upload_ms : 0.0;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_set_received_tiles(
    nxvc_vk_encoder *e, const uint8_t *received, uint32_t count) {
    if (!e || !received) return NXVC_VKE_ERR_ARG;
    if (count != e->frame.fp.ntiles) {
        e->err = "the receipt map must have one byte per tile";
        return NXVC_VKE_ERR_ARG;
    }
    /* On an intra stream there is nothing to corrupt, so this is accepted and
     * ignored -- a caller's plumbing does not have to branch on the backend. */
    if (!e->cfg.inter) return NXVC_VKE_OK;
    e->vk.set_received_tiles(received, count);
    return NXVC_VKE_OK;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_set_frame_held(nxvc_vk_encoder *e,
                                                          uint32_t frame_number,
                                                          int held) {
    if (!e) return NXVC_VKE_ERR_ARG;
    /* On an intra stream there is no reference chain to invalidate, so this is
     * accepted and ignored -- a caller's plumbing does not have to branch on
     * the backend, exactly as set_received_tiles() does not make it. */
    if (!e->cfg.inter) return NXVC_VKE_OK;
    e->vk.set_frame_held(frame_number, held != 0);
    return NXVC_VKE_OK;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_set_views(nxvc_vk_encoder *e,
                                                     const nxvc_vke_view *v,
                                                     uint32_t count) {
    if (!e || !v) return NXVC_VKE_ERR_ARG;
    if (count != e->cfg.eyes) {
        e->err = "one view per eye";
        return NXVC_VKE_ERR_ARG;
    }
    if (!e->cfg.inter) return NXVC_VKE_OK;
    nxe::View nv[2];
    for (uint32_t i = 0; i < count && i < 2; ++i) {
        nv[i].qx = v[i].qx; nv[i].qy = v[i].qy;
        nv[i].qz = v[i].qz; nv[i].qw = v[i].qw;
        nv[i].fov_left = v[i].fov_left; nv[i].fov_right = v[i].fov_right;
        nv[i].fov_up = v[i].fov_up;     nv[i].fov_down = v[i].fov_down;
    }
    e->vk.set_views(nv, (int)count, e->frame_number);
    return NXVC_VKE_OK;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_set_view(nxvc_vk_encoder *e,
                                                    const nxvc_vke_view *v) {
    return nxvc_vk_encoder_set_views(e, v, 1);
}

namespace {

/* Per-tile spans, straight out of E5's own layout: the prefix sum over the
 * tile byte counts the GPU reported, run back through the offset function E5
 * itself uses.  This is a read of the layout, not a guess at it.  Shared by
 * both entry points, because a tile record must not depend on where the
 * picture came from. */
void publish_frame(nxvc_vk_encoder *e, const uint8_t **out, size_t *out_len) {
    const nxe_frame_params &fp = e->frame.fp;
    uint32_t run = 0;
    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        e->frame.tile_prefix[t] = run;
        run += e->frame.tile_bytes[t];
    }
    e->tiles.resize(fp.ntiles);
    for (uint32_t t = 0; t < fp.ntiles; ++t) {
        e->tiles[t].index = t;
        e->tiles[t].offset =
            nxe_e5_tile_offset(&fp, t, e->frame.tile_prefix.data());
        e->tiles[t].length = e->frame.tile_bytes[t];
        e->tiles[t].qp = uint8_t(e->cfg.qp);
        e->tiles[t].mode = uint8_t(e->frame.jobs[t].mode);
        e->tiles[t].res_level = 0;
        e->tiles[t].ref_delta = 3; /* no temporal reference */
    }
    e->frame_number++;
    *out = e->frame.out.data();
    *out_len = e->frame.out.size();
}

} // namespace

extern "C" nxvc_vke_status nxvc_vk_encoder_encode_image(
    nxvc_vk_encoder *e, const nxvc_vke_image *img, const uint8_t **out,
    size_t *out_len) {
    if (!e || !img || !out || !out_len) return NXVC_VKE_ERR_ARG;
    *out = nullptr;
    *out_len = 0;
    if (!e->created) return NXVC_VKE_ERR_INTERNAL;
    if (!img->image) return NXVC_VKE_ERR_ARG;
    /* E0 binds the planes as storage images and storage images are read in
     * GENERAL.  Refusing rather than transitioning is deliberate: the
     * transition belongs on the submit that produced the picture, and doing
     * it here would need an ownership claim this library does not have. */
    if (img->layout != VK_IMAGE_LAYOUT_GENERAL) {
        e->err = "the source image must be in VK_IMAGE_LAYOUT_GENERAL";
        return NXVC_VKE_ERR_ARG;
    }
    if (img->flags) return NXVC_VKE_ERR_ARG;
    /* The geometry is fixed at create(): a picture of another size would be
     * silently cropped or read out of bounds, which is worse than an error. */
    if (img->width != uint32_t(e->cfg.w) || img->height != uint32_t(e->cfg.h)) {
        e->err = "the image geometry does not match the one create() was given";
        return NXVC_VKE_ERR_ARG;
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    if (!e->vk.encode_frame_image(e->frame, e->frame_number, img->image,
                                  img->array_layer, err)) {
        e->err = err.empty() ? "the encode pipeline failed; see stderr" : err;
        return NXVC_VKE_ERR_VULKAN;
    }
    const auto t1 = std::chrono::steady_clock::now();
    /* No repack, by construction: the whole point of this entry point is that
     * the number below is zero. */
    e->last_upload_ms = 0.0;
    e->last_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    publish_frame(e, out, out_len);
    return NXVC_VKE_OK;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_encode_planes(
    nxvc_vk_encoder *e, const uint8_t *y, size_t y_stride, const uint8_t *cb,
    const uint8_t *cr, size_t chroma_stride, const uint8_t **out,
    size_t *out_len) {
    if (!e || !y || !cb || !cr || !out || !out_len) return NXVC_VKE_ERR_ARG;
    *out = nullptr;
    *out_len = 0;
    if (!e->created) return NXVC_VKE_ERR_INTERNAL;

    const auto t_up0 = std::chrono::steady_clock::now();
    nxe::load_planes(e->cfg, e->frame, y, y_stride, cb, cr, chroma_stride);
    const auto t_up1 = std::chrono::steady_clock::now();

    if (!e->vk.encode_frame(e->frame, e->frame_number, false, true)) {
        e->err = "the encode pipeline failed; see stderr";
        return NXVC_VKE_ERR_VULKAN;
    }
    const auto t_enc1 = std::chrono::steady_clock::now();
    e->last_upload_ms =
        std::chrono::duration<double, std::milli>(t_up1 - t_up0).count();
    e->last_ms =
        std::chrono::duration<double, std::milli>(t_enc1 - t_up1).count();

    publish_frame(e, out, out_len);
    return NXVC_VKE_OK;
}
