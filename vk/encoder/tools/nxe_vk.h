/* nxe_vk.h -- the Vulkan backend of nxvc-vkenc: E3, E4 and E5 on the device.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Built on `vk_min`, the throwaway boilerplate the stats harness already uses.
 * Buffers, descriptors and pipelines are created once per stream; a frame is
 * one upload, one command buffer of five dispatches, one readback.  That is
 * not the shape paper 3.6 wants in the compositor (there the source is already
 * a device image and the output buffer is host-cached and read in place), but
 * it is the shape a file-driven harness needs, and the dispatches in the middle
 * are the same.
 */

#ifndef NXE_VK_H
#define NXE_VK_H

#include <string>

#include <vulkan/vulkan.h>

#include <nxvc/nxvc_vk_enc.h>

#include "nxe_atlas.h"
#include "nxe_host.h"
#include "nxe_inter.h"

namespace nxe {

/* Five handles a host already owns, for VkEncoder::create() to adopt instead
 * of creating a device of its own.  All five or none: see vkmin::Device::adopt
 * and nxvc_vk_enc.h. */
struct Adopt {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
};

int vk_list_devices();

class VkEncoder {
public:
    struct Impl;
    VkEncoder();
    ~VkEncoder();
    bool create(const Config &cfg, const Frame &f, std::string &err,
                const Adopt *adopt = nullptr);
    /* Encodes into f.out.  With `check`, every intermediate is diffed against
     * the CPU model and a mismatch is a failure. */
    bool encode_frame(Frame &f, uint32_t frame_number, bool check, bool quiet);

    /* The same encode, with E0 reading the picture out of a compositor image
     * instead of the host laying it out (nxe::load_planes) and uploading it.
     *
     * `image` is a VK_FORMAT_G8_B8R8_2PLANE_420_UNORM image on this encoder's
     * device, created with MUTABLE_FORMAT and a format list that permits
     * R8_UINT and R8G8_UINT plane views, with STORAGE usage (EXTENDED_USAGE
     * where the planar format itself has no storage feature), in
     * VK_IMAGE_LAYOUT_GENERAL and owned by this encoder's queue family.  It
     * must not be written again until this call returns: the encode submits
     * and waits, so returning is the fence.
     *
     * `f.src_packed` is NOT read and NOT written: the plane buffer is filled
     * on the device.  Everything after E0 is the path encode_frame() runs, so
     * the bitstream is the same bitstream. */
    bool encode_frame_image(Frame &f, uint32_t frame_number, VkImage image,
                            uint32_t array_layer, uint32_t layers,
                            std::string &err);
    /* The frame's pose and projection, one per eye, for the frame the NEXT
     * encode() call will code.  Must be called before that encode, and the
     * encoder keeps the history the warp derivation needs. */
    void set_views(const View *v, int eyes, uint32_t frame_number);

    /* Which tiles the client holds, one byte per tile.  A tile it does not
     * hold is coded INTRA on the next frame; an all-zero map is therefore a
     * full reset.  See nxvc_vk_enc.h for why the rule is the blunt one. */
    void set_received_tiles(const uint8_t *received, uint32_t count);

    /* Whether the headset reconstructed a frame it was sent.
     *
     * `false` makes that frame, and every later frame that predicted from it,
     * unusable as a reference; the next inter frame then asks for the newest
     * one that is still usable (ref_sel 0..2) instead of resyncing.
     *
     * `true` is a CONFIRMATION, and from the first one the encoder references
     * confirmed frames only -- which is what makes a refusal impossible rather
     * than merely rarer.  See nxvc_vk_enc.h for the contract and nxe_inter.h
     * for the record. */
    void set_frame_held(uint32_t frame_number, bool held);

    /* Read one ring slot's luma plane back, for the test that pins the
     * encoder's reference against the decoder's.  `out` is filled with
     * `w * h` uint16 samples in the CODED domain.  Returns false if the
     * encoder has no ring (an intra-only stream). */
    bool read_ring_luma(uint32_t slot, uint16_t *out, size_t count);

    /* The DISPLAYED luma picture, tile-major, for the non-normative display
     * of [SYN] 13.12.5 -- which is the only way to price the atlas at equal
     * rate, because under ATLAS the decoder's output image is not the
     * normative object and comparing reconstructions would compare the wrong
     * thing.
     *
     * With ATLAS this warps each atlas tile from its own source pose through
     * its own `C`; without it the displayed picture IS the reconstruction, so
     * it reads the ring slot this frame wrote and re-tiles it.  Either way the
     * result is what a client would put on the panel for `frame_number`.
     *
     * Returns false on a stream with no ring at all. */
    bool read_displayed_luma(uint32_t frame_number,
                             std::vector<uint16_t> &out_tilemajor);

    /* ------------------------------------------------- the normative output
     *
     * [SYN] 13.12 says the normative output under ATLAS is the atlas -- its
     * per-tile table and its pixels -- and NOT the picture.  Byte-identity of
     * the STREAM against nxv-enc is therefore necessary and not sufficient:
     * two encoders can emit the same bytes while disagreeing about the
     * reference they think the client now holds, and that disagreement shows
     * up frames later as drift rather than as a broken frame.
     *
     * `atlas_table()` returns the 64-byte records of 13.12.1 in the tile order
     * of Annex D D-3, written little-endian by field rather than memcpy'd, so
     * the wire form does not depend on this compiler's struct layout.
     * `atlas_pixel_digest()` is the FNV-1a of every atlas plane in the same
     * form nxv-enc and nxv-dec use, because the atlas is megabytes and a
     * conformance vector should not be.
     *
     * Both return false when the stream is not an ATLAS stream. */
    bool atlas_table(std::vector<uint8_t> &out) const;
    bool atlas_pixel_digest(uint8_t out[32]);

    /* How many skips `atlas_disp_margin` refused over the clip so far: the
     * FORCED REFRESH the displacement bound costs.  Zero when the bound is
     * off, which is the default. */
    uint64_t atlas_disp_forced() const;

    /* The layout `atlas_write_tiles()` consumes, filled from the SAME
     * `RingLayout` the copies are built from -- see nxvc_vk_enc.h for why a
     * caller must not re-derive it.  False on a non-ATLAS stream. */
    bool atlas_layout(nxvc_vke_atlas_layout &out) const;

    /* Build a patch buffer from `atlas_layout()` ALONE, copy a checkerboard of
     * tiles through the production region builder, and verify every sample of
     * every plane came back where the accessor said it would.  This is the one
     * property a caller depends on and cannot check itself: that the reported
     * layout addresses the samples the encoder's own copies do.  False and
     * `err` on the first sample that disagrees. */
    bool atlas_layout_roundtrip(std::string &err);

    /* Fill a contiguous run of atlas tile positions from the BASE LAYER
     * ([SYN] 13.12.9).  `src` is DEVICE memory on this encoder's device,
     * already in the atlas's own plane layout -- the same strided u16 planes a
     * ring slot holds -- so a patch is the same set of row regions a rollback
     * is and needs no per-tile address arithmetic on either side.
     *
     * `first_tile` and `count` are within `eye`, in that eye's row-major
     * order; the pair-wide index of Annex D D-3 is derived here, because a run
     * that is contiguous in one eye is not contiguous pair-wide.
     *
     * Each written position gets 13.12.9's metadata block, `base_sourced`
     * (flags bit 2) included -- normative in v1 and compared by conformance.
     *
     * SUPERSEDE.  A position whose `src_frame` is not advanced by this write
     * is DROPPED rather than written, per 13.12.9's ordering rule, and the
     * call still succeeds: the base arrives through a hardware decoder with
     * its own latency, so being overtaken by a coded tile is ordinary.
     * `applied` and `superseded` (either may be null) count how the run split.
     *
     * The copy is QUEUED, not submitted: it is recorded at the top of the next
     * encode, in the same command buffer as the rollback restore and one
     * barrier ahead of the E-stages, which is the only ordering that is
     * correct (after the previous frame's Pass B, before this frame's Pass W
     * reads the atlas) and is not one the caller can arrange.  The table,
     * however, is updated immediately, because the next encode's decision pass
     * has to see it.
     *
     * Returns false and fills `err` on a run that leaves the eye, on a null
     * buffer, or on a non-ATLAS stream.  A superseded tile is NOT an error. */
    bool atlas_write_tiles(uint32_t eye, uint32_t first_tile, uint32_t count,
                           VkBuffer src, uint64_t src_offset,
                           uint32_t src_frame, uint32_t *applied,
                           uint32_t *superseded, std::string &err);

    void bench(Frame &f, int iters);

private:
    /* The one encode.  `image` null is the host-plane path; non-null is E0
     * reading it.  Everything from E3 on is shared, which is the point: there
     * is one bitstream producer, not two. */
    bool encode_frame_common(Frame &f, uint32_t frame_number, bool check,
                             bool quiet, const VkImage *image,
                             uint32_t array_layer, uint32_t src_layers,
                             std::string &err);

    Impl *p_;
};

}  // namespace nxe

#endif /* NXE_VK_H */
