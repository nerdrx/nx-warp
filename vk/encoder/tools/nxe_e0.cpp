/* nxe_e0.cpp -- see nxe_e0.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nxe_e0.h"

#include <cstdio>
#include <vector>

#include "tile_stats.h" /* carries its own extern "C" */

#include "E0_convert_ycbcr8_420.spv.h"

namespace nxe {

bool E0::create(vkmin::Device &dev, VkDescriptorPool pool, std::string &err) {
    const std::vector<VkDescriptorType> types = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  /* 0: luma plane, r8ui       */
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, /* 1: packed planes out      */
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  /* 2: chroma plane, rg8ui    */
    };
    if (!dev.create_pipeline(E0_convert_ycbcr8_420_spv,
                             sizeof E0_convert_ycbcr8_420_spv, types,
                             sizeof(nxe_e0_push), p_, err))
        return false;
    set_ = dev.allocate_set(pool, p_.dsl);
    if (!set_) {
        err = "E0: descriptor set allocation failed";
        dev.destroy_pipeline(p_);
        return false;
    }
    return true;
}

void E0::destroy(vkmin::Device &dev) {
    if (p_.pipe) dev.destroy_pipeline(p_);
    p_ = {};
    set_ = VK_NULL_HANDLE;
}

void E0::bind(vkmin::Device &dev, VkImageView luma, VkImageView chroma,
              VkBuffer planes) {
    VkDescriptorImageInfo ii[2]{};
    ii[0].imageView = luma;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ii[1].imageView = chroma;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo bi{planes, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set_;
        w[i].descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[0].pImageInfo = &ii[0];
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &bi;
    w[2].dstBinding = 2;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo = &ii[1];
    vkUpdateDescriptorSets(dev.handle(), 3, w, 0, nullptr);
}

void E0::record(VkCommandBuffer cb, const E0Geometry &g) {
    nxe_e0_push pc{};
    pc.f.width = g.width;
    pc.f.height = g.height;
    pc.f.tiles_x = g.tiles_x;
    pc.f.tiles_y = g.tiles_y;
    pc.f.plane_y_off = g.plane_y_off;
    pc.f.plane_co_off = g.plane_co_off;
    pc.f.plane_cg_off = g.plane_cg_off;
    /* CHROMA_420 and YCBCR describe the source; CHROMA_RAW is the one that
     * changes what E0 writes, and it must be on: nxe::load_planes stores the
     * compositor's chroma codes unshifted, and the stream the acid test pins
     * is the one that convention produces. */
    pc.f.flags = NXE_TS_F_CHROMA_420 | NXE_TS_F_YCBCR | NXE_TS_F_CHROMA_RAW;
    pc.plane_words = g.plane_words;

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p_.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p_.layout, 0, 1,
                            &set_, 0, nullptr);
    vkCmdPushConstants(cb, p_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof pc, &pc);
    vkCmdDispatch(cb, g.tiles_x, g.tiles_y, 1);
}

} // namespace nxe
