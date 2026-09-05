/* nxvc-vkenc-api.cpp -- drive the encoder through its C ABI and nothing else.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `nxvc-vkenc` drives nxe::VkEncoder directly, so it proves the kernels but
 * not the library: the ABI's own configuration mapping -- which tools it turns
 * off, which quantiser matrix it picks, what it puts in the stream header --
 * is code the harness never executes.  This tool executes only that.  It
 * writes the same .nxv `nxvc-vkenc` would, so the acid test's comparison
 * against `nxv-enc` applies to it unchanged.
 *
 * It also prints the per-frame timing the ABI reports, which is where the
 * encode-time numbers in the integration notes come from.
 *
 * `--image` drives the OTHER entry point: it creates a Vulkan device of its
 * own, adopts it into the encoder the way WiVRn's server adopts Monado's,
 * builds the two-plane 4:2:0 image a Linux compositor hands over -- mutable
 * format, a format list naming the UINT plane views, storage usage through
 * EXTENDED_USAGE -- uploads each frame into it and encodes from the image.
 * The two modes must write byte-identical files, which is what
 * tests/vk-encoder/api_acid.cmake checks; without that this entry point would
 * be a second bitstream producer nobody compares against the first.
 *
 * Exit 77 when no usable Vulkan device is present, so ctest reports a skip.
 */

#include <nxvc/nxvc_vk_enc.h>

#include "vk_min.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


namespace {

/* The compositor's image, built here so that the library's image entry point
 * can be driven from a file of planar YUV.
 *
 * Everything in the create info is what <nxvc/nxvc_vk_enc.h> requires of a
 * caller, spelled out once: MUTABLE_FORMAT plus a format list that names the
 * UINT plane views, and STORAGE usage reached through EXTENDED_USAGE because
 * G8_B8R8_2PLANE_420_UNORM has no storage feature of its own on any driver
 * this has been run on.  Two array layers, and the encode reads layer 1, so
 * the layer plumbing is exercised rather than merely present -- WiVRn's
 * compositor keeps its eyes in layers of one image. */
struct PlanarSource {
    vkmin::Device dev;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkmin::Buffer stage{};
    uint32_t w = 0, h = 0;
    static const uint32_t layer = 1;
    bool first = true;

    bool create(uint32_t width, uint32_t height, uint32_t device_index,
                std::string &err) {
        w = width;
        h = height;
        if (!dev.create(device_index, false, err)) return false;

        const VkFormat list[5] = {
            VK_FORMAT_R8_UNORM,  VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8_UINT,
            VK_FORMAT_R8G8_UINT, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM};
        VkImageFormatListCreateInfo fl{};
        fl.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        fl.viewFormatCount = 5;
        fl.pViewFormats = list;

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.pNext = &fl;
        ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                   VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = layer + 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(dev.handle(), &ci, nullptr, &img);
        if (r != VK_SUCCESS) {
            err = std::string("vkCreateImage: ") + vkmin::result_str(r);
            return false;
        }

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(dev.handle(), img, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex =
            dev.find_memory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) {
            err = "no device-local memory type for the source image";
            return false;
        }
        r = vkAllocateMemory(dev.handle(), &ai, nullptr, &mem);
        if (r != VK_SUCCESS) {
            err = std::string("vkAllocateMemory: ") + vkmin::result_str(r);
            return false;
        }
        r = vkBindImageMemory(dev.handle(), img, mem, 0);
        if (r != VK_SUCCESS) {
            err = std::string("vkBindImageMemory: ") + vkmin::result_str(r);
            return false;
        }

        const VkDeviceSize bytes =
            (VkDeviceSize)w * h + (VkDeviceSize)((w + 1) / 2) * ((h + 1) / 2) * 2;
        return dev.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                                 stage, err);
    }

    /* Planar in, two-plane out: this is the NV12 interleave a compositor does
     * on the way to the image, done here so the file and the image carry the
     * same picture. */
    bool upload(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                std::string &err) {
        const size_t cw = (w + 1) / 2, chh = (h + 1) / 2;
        uint8_t *p = (uint8_t *)stage.map;
        std::memcpy(p, y, (size_t)w * h);
        uint8_t *c = p + (size_t)w * h;
        for (size_t i = 0; i < cw * chh; ++i) {
            c[i * 2 + 0] = cb[i];
            c[i * 2 + 1] = cr[i];
        }

        VkCommandBuffer cbuf = dev.begin();
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkAccessFlags src, VkAccessFlags dst,
                           VkPipelineStageFlags sstage,
                           VkPipelineStageFlags dstage) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = src;
            b.dstAccessMask = dst;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                  layer + 1};
            vkCmdPipelineBarrier(cbuf, sstage, dstage, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
        };
        barrier(first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        first = false;

        VkBufferImageCopy rg[2]{};
        rg[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, layer, 1};
        rg[0].imageExtent = {w, h, 1};
        rg[1].bufferOffset = (VkDeviceSize)w * h;
        rg[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, layer, 1};
        rg[1].imageExtent = {(uint32_t)cw, (uint32_t)chh, 1};
        vkCmdCopyBufferToImage(cbuf, stage.buf, img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, rg);

        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        return dev.submit_and_wait(cbuf, err);
    }

    void destroy() {
        if (stage.buf) dev.destroy_buffer(stage);
        if (img) vkDestroyImage(dev.handle(), img, nullptr);
        if (mem) vkFreeMemory(dev.handle(), mem, nullptr);
        img = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;
        dev.destroy();
    }
};

} // namespace

int main(int argc, char **argv) {
    std::string in, out;
    uint32_t w = 0, h = 0, qp = 26, frames = 8, matrix = 1;
    bool timing = false, use_image = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--in") in = next();
        else if (a == "--out") out = next();
        else if (a == "--w") w = (uint32_t)std::atoi(next());
        else if (a == "--h") h = (uint32_t)std::atoi(next());
        else if (a == "--qp") qp = (uint32_t)std::atoi(next());
        else if (a == "--frames") frames = (uint32_t)std::atoi(next());
        else if (a == "--matrix") matrix = (uint32_t)std::atoi(next());
        else if (a == "--timing") timing = true;
        else if (a == "--image") use_image = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (in.empty() || out.empty() || !w || !h) {
        std::fprintf(stderr,
                     "usage: nxvc-vkenc-api --in f.yuv --w W --h H --out f.nxv\n"
                     "                      [--qp N] [--frames N] [--matrix N] [--timing]\n"
                     "                      [--image]\n");
        return 2;
    }

    nxvc_vke_create_info ci;
    nxvc_vk_encoder_create_info_default(&ci);
    ci.width = w;
    ci.height = h;
    ci.base_qp = qp;
    ci.quant_matrix = matrix;

    /* The image path needs a device the caller owns: the image has to live on
     * the encoder's device, and a device the library created is one this tool
     * has no handle to. */
    PlanarSource src;
    if (use_image) {
        std::string err;
        if (!src.create(w, h, 0, err)) {
            std::fprintf(stderr, "source image: %s\n", err.c_str());
            /* No ICD and no device are a skip, exactly as create() is. */
            return 77;
        }
        ci.instance = src.dev.instance();
        ci.physical_device = src.dev.phys();
        ci.device = src.dev.handle();
        ci.queue = src.dev.queue();
        ci.queue_family = src.dev.queue_family();
    }

    nxvc_vk_encoder *enc = nullptr;
    nxvc_vke_status st = nxvc_vk_encoder_create(&ci, &enc);
    if (st != NXVC_VKE_OK) {
        std::fprintf(stderr, "nxvc_vk_encoder_create: %s\n",
                     nxvc_vk_encoder_status_string(st));
        /* No device is a skip, not a failure: this runs on CI boxes with no
         * ICD at all. */
        return (st == NXVC_VKE_ERR_NO_DEVICE || st == NXVC_VKE_ERR_VULKAN) ? 77 : 1;
    }

    std::FILE *fi = std::fopen(in.c_str(), "rb");
    if (!fi) { std::perror("open input"); nxvc_vk_encoder_destroy(enc); return 1; }
    std::FILE *fo = std::fopen(out.c_str(), "wb");
    if (!fo) { std::perror("open output"); std::fclose(fi);
               nxvc_vk_encoder_destroy(enc); return 1; }

    size_t hlen = 0;
    nxvc_vk_encoder_stream_header(enc, nullptr, 0, &hlen);
    std::vector<uint8_t> hdr(hlen);
    if (nxvc_vk_encoder_stream_header(enc, hdr.data(), hdr.size(), &hlen) != NXVC_VKE_OK) {
        std::fprintf(stderr, "stream header failed\n");
        return 1;
    }
    std::fwrite(hdr.data(), 1, hdr.size(), fo);

    const size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<uint8_t> Y((size_t)w * h), U(cw * ch), V(cw * ch);

    uint32_t n = 0;
    double sum_ms = 0, max_ms = 0, sum_up = 0;
    size_t total_bytes = 0;
    int rc = 0;
    while (n < frames) {
        if (std::fread(Y.data(), 1, Y.size(), fi) != Y.size()) break;
        if (std::fread(U.data(), 1, U.size(), fi) != U.size()) break;
        if (std::fread(V.data(), 1, V.size(), fi) != V.size()) break;

        const uint8_t *bytes = nullptr;
        size_t len = 0;
        if (use_image) {
            std::string err;
            if (!src.upload(Y.data(), U.data(), V.data(), err)) {
                std::fprintf(stderr, "upload frame %u: %s\n", n, err.c_str());
                rc = 1;
                break;
            }
            nxvc_vke_image im{};
            im.image = src.img;
            im.layout = VK_IMAGE_LAYOUT_GENERAL;
            im.array_layer = PlanarSource::layer;
            im.width = w;
            im.height = h;
            st = nxvc_vk_encoder_encode_image(enc, &im, &bytes, &len);
        } else {
            st = nxvc_vk_encoder_encode_planes(enc, Y.data(), w, U.data(),
                                               V.data(), cw, &bytes, &len);
        }
        if (st != NXVC_VKE_OK) {
            std::fprintf(stderr, "encode frame %u: %s (%s)\n", n,
                         nxvc_vk_encoder_status_string(st),
                         nxvc_vk_encoder_last_error(enc));
            rc = 1;
            break;
        }
        std::fwrite(bytes, 1, len, fo);
        total_bytes += len;

        const double ms = nxvc_vk_encoder_last_encode_ms(enc);
        sum_ms += ms;
        sum_up += nxvc_vk_encoder_last_upload_ms(enc);
        if (ms > max_ms) max_ms = ms;

        /* The per-tile spans must tile the frame exactly: every tile's bytes
         * inside the frame, and no two overlapping.  It is the claim the
         * transport would rely on, so check it here rather than trust it. */
        uint32_t tc = 0;
        const nxvc_vke_tile *tiles = nxvc_vk_encoder_tiles(enc, &tc);
        uint32_t prev_end = 0;
        for (uint32_t t = 0; t < tc; ++t) {
            if (size_t(tiles[t].offset) + tiles[t].length > len) {
                std::fprintf(stderr,
                             "tile %u span [%u,+%u) runs past the %zu-byte frame\n",
                             t, tiles[t].offset, tiles[t].length, len);
                rc = 1;
            }
            /* Ascending and non-overlapping, and the last one ends exactly at
             * the frame end.  A bounds check alone would pass an offset that
             * ignored a whole region of the frame -- which is precisely what a
             * transmitted table area (SYNTAX.md 9.4) inserts between the frame
             * header and the first tile row. */
            if (tiles[t].offset < prev_end) {
                std::fprintf(stderr,
                             "tile %u starts at %u, before the previous tile "
                             "ended at %u\n", t, tiles[t].offset, prev_end);
                rc = 1;
            }
            prev_end = tiles[t].offset + tiles[t].length;
        }
        if (tc && prev_end != len) {
            std::fprintf(stderr,
                         "the last tile ends at %u, not at the frame end %zu\n",
                         prev_end, len);
            rc = 1;
        }
        ++n;
    }
    std::fclose(fo);
    std::fclose(fi);

    if (timing && n) {
        std::printf("%u frames, %ux%u QP %u: encode mean %.3f ms, max %.3f ms, "
                    "repack mean %.3f ms, %zu bytes/frame\n",
                    n, w, h, qp, sum_ms / n, max_ms, sum_up / n,
                    total_bytes / n);
    }
    nxvc_vk_encoder_destroy(enc);
    if (use_image) src.destroy();
    if (rc) return rc;
    return n > 0 ? 0 : 1;
}
