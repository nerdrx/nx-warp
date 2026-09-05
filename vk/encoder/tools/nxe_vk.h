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

#include "nxe_host.h"

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
                            uint32_t array_layer, std::string &err);
    void bench(Frame &f, int iters);

private:
    /* The one encode.  `image` null is the host-plane path; non-null is E0
     * reading it.  Everything from E3 on is shared, which is the point: there
     * is one bitstream producer, not two. */
    bool encode_frame_common(Frame &f, uint32_t frame_number, bool check,
                             bool quiet, const VkImage *image,
                             uint32_t array_layer, std::string &err);

    Impl *p_;
};

}  // namespace nxe

#endif /* NXE_VK_H */
