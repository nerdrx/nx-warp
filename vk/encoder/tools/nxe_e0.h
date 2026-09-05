/* nxe_e0.h -- E0 on the device: a compositor VkImage straight into the packed
 * tile-major plane buffer E3 reads.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The host path (nxe::load_planes) is E0 done on the CPU because a file of
 * planar YUV has no import format.  A compositor does have one: the frame is
 * already a VK_FORMAT_G8_B8R8_2PLANE_420_UNORM image on this device, and E0
 * reads its two planes through UINT storage views.  Running it removes the
 * readback, the de-interleave and the re-upload that the plane entry point
 * costs -- three passes over the picture on the host, per frame, for a picture
 * that never needed to leave the GPU.
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT.  stats/tile_stats.h and
 * forward/nxe_enc.h both define a struct tagged `nxe_frame_params`, with
 * different members, and they must never be included together.  nxe_vk.cpp has
 * the forward one; E0's push constants are the stats one.  So E0's dispatch
 * lives here, behind an interface made of plain integers, exactly as
 * nxvc_vk_enc.cpp keeps its distance from stats/.
 */

#ifndef NXE_E0_H
#define NXE_E0_H

#include <string>

#include <vulkan/vulkan.h>

#include "vk_min.h"

namespace nxe {

/* Everything E0 needs about the picture, in the terms nxe::Frame already has:
 * the plane offsets are word offsets into the packed plane buffer, which is
 * `Frame::plane_base`, and `plane_words` is its total size in words, which is
 * `Frame::src_packed.size() / 2`. */
struct E0Geometry {
    uint32_t width = 0, height = 0; /* luma samples of the picture (one eye) */
    uint32_t tiles_x = 0, tiles_y = 0;
    uint32_t plane_y_off = 0, plane_co_off = 0, plane_cg_off = 0;
    uint32_t plane_words = 0;
};

/* The YCbCr 2-plane 4:2:0 8-bit variant of E0_convert.comp, which is the one
 * the Linux compositor hands over.  10-bit and the RGB import formats have
 * their own variants; the library is 8-bit 4:2:0 only, so this class is too. */
class E0 {
public:
    bool create(vkmin::Device &dev, VkDescriptorPool pool, std::string &err);
    void destroy(vkmin::Device &dev);

    /* Point the pass at one frame's plane views and the output buffer.  Legal
     * only when no dispatch of this pass is in flight, which the encoder
     * guarantees by submitting and waiting inside encode(). */
    void bind(vkmin::Device &dev, VkImageView luma, VkImageView chroma,
              VkBuffer planes);

    /* One dispatch, (tiles_x, tiles_y, 1).  Chroma is passed through with the
     * reference encoder's unsigned convention (NXE_TS_F_CHROMA_RAW), because
     * that is what nxe::load_planes lays down and what the byte-identity with
     * `nxv-enc` is pinned against. */
    void record(VkCommandBuffer cb, const E0Geometry &g);

    bool created() const { return p_.pipe != VK_NULL_HANDLE; }

private:
    vkmin::Pipeline p_{};
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace nxe

#endif /* NXE_E0_H */
