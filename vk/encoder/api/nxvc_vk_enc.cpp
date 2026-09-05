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
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nxe_host.h"
#include "nxe_vk.h"

namespace {

/* The tool bits a stream from this encoder carries.  Kept as an explicit
 * literal rather than derived from the stream header so that a change to
 * either one has to be made here too, deliberately. */
constexpr uint64_t kToolsEmitted =
    (1ull << 0) |  /* INTRA_DC_PLANE: the only prediction this encoder has  */
    (1ull << 21) | /* CTX_V2: the 16-context entropy model                  */
    (1ull << 22);  /* SIGN_HIDE: sign data hiding, exact in E4              */

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
    return kToolsEmitted;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_create(const nxvc_vke_create_info *ci,
                                                  nxvc_vk_encoder **out) {
    if (!ci || !out) return NXVC_VKE_ERR_ARG;
    *out = nullptr;

    /* Refuse, loudly and at create() time, everything this path cannot code.
     * The alternative -- accepting the field and quietly coding something
     * else -- is the failure mode that costs a day of bisecting a bitstream. */
    if (ci->width == 0 || ci->height == 0) return NXVC_VKE_ERR_ARG;
    if (ci->eyes != 1) return NXVC_VKE_ERR_UNSUPPORTED;
    if (ci->chroma != 0) return NXVC_VKE_ERR_UNSUPPORTED;
    if (ci->bit_depth != 8) return NXVC_VKE_ERR_UNSUPPORTED;
    if (ci->base_qp > 63) return NXVC_VKE_ERR_ARG;
    if (ci->quant_matrix > 3) return NXVC_VKE_ERR_ARG;

    const bool adopting = ci->device != VK_NULL_HANDLE;
    if (adopting && (!ci->physical_device || !ci->queue))
        return NXVC_VKE_ERR_ARG; /* all five handles or none */

    auto *e = new (std::nothrow) nxvc_vk_encoder();
    if (!e) return NXVC_VKE_ERR_NOMEM;

    /* Everything below is the acid test's configuration, spelled out.  The
     * fields that are not settable through the ABI are the tools that are off. */
    e->cfg.w = int(ci->width * ci->eyes);
    e->cfg.h = int(ci->height);
    e->cfg.eyes = int(ci->eyes);
    e->cfg.chroma444 = false;
    e->cfg.qp = int(ci->base_qp);
    e->cfg.matrix = int(ci->quant_matrix);
    e->cfg.wm_id = 0;
    e->cfg.chroma_qp_off = 0;
    e->cfg.nsub_log2 = 3; /* eight rANS lanes; paper 6.3 fixes v1 at eight */
    e->cfg.tskip = 0;
    e->cfg.ctx_v2 = true;
    e->cfg.ctx_v3 = false;
    e->cfg.sign_hide = true;
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
        /* The message is worth more than the object; there is nowhere to hang
         * it once the handle is gone, so the caller gets the code and the
         * status string.  A create failure is a bring-up failure, not a
         * runtime one. */
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
    nxvc_vk_encoder *e, const uint8_t *, uint32_t) {
    /* Accepted and ignored; see the header.  An all-intra stream has no
     * prediction for a lost tile to corrupt. */
    return e ? NXVC_VKE_OK : NXVC_VKE_ERR_ARG;
}

extern "C" nxvc_vke_status nxvc_vk_encoder_set_views(nxvc_vk_encoder *e,
                                                     const nxvc_vke_view *,
                                                     uint32_t) {
    /* Accepted and ignored; see the header.  An intra frame's warp matrix is
     * the identity because there is no reference to warp. */
    return e ? NXVC_VKE_OK : NXVC_VKE_ERR_ARG;
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
