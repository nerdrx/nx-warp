// external_ahb.cpp - AHardwareBuffer import, docs/PAPER.md 3.5 step 2.
//
// MediaCodec decodes the HEVC base layer into an AImageReader surface; each
// acquired AImage carries an AHardwareBuffer in a vendor-tiled YCbCr format
// (UBWC NV12 on XR2 Gen 1).  There is no VkFormat for that, so the image is
// created with VkExternalFormatANDROID and can only ever be read through a
// sampler carrying an immutable VkSamplerYcbcrConversion built from the same
// external format.  Everything about such an image is constrained:
//
//   * the view must use the same conversion,
//   * the sampler must be immutable in the descriptor set layout,
//   * usage is SAMPLED only, tiling and format come from the driver,
//   * a dedicated allocation is mandatory.
//
// 3.5 also says the base layer is outside the normative bit-exact path, which
// is what makes a sampler tap legal here at all.
//
// The whole file is compiled only on Android; on every other target it is an
// empty translation unit.
#if defined(__ANDROID__)

#include "internal.hpp"

#include <nxvc/vk/vk_common.hpp>

#include <android/hardware_buffer.h>

#include <cstring>

namespace nxvc::vk::external {

AhbInfo describeAHardwareBuffer(const Context& ctx, AHardwareBuffer* ahb) {
    if (!ahb) throw Error(NXVC_VK_ERR_ARG, "null AHardwareBuffer");
    if (!ctx.fns().getAhbProperties)
        throw Error(NXVC_VK_ERR_UNSUPPORTED,
                    "VK_ANDROID_external_memory_android_hardware_buffer not enabled");

    VkAndroidHardwareBufferFormatPropertiesANDROID fmt{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    props.pNext = &fmt;
    NXVC_VK_CHECK(ctx.fns().getAhbProperties(ctx.device(), ahb, &props));

    AhbInfo info;
    info.allocation_size = props.allocationSize;
    info.memory_type_bits = props.memoryTypeBits;
    info.external_format = fmt.externalFormat;
    info.format = fmt.format;
    info.format_features = fmt.formatFeatures;
    info.ycbcr_model = fmt.suggestedYcbcrModel;
    info.ycbcr_range = fmt.suggestedYcbcrRange;
    info.x_chroma_offset = fmt.suggestedXChromaOffset;
    info.y_chroma_offset = fmt.suggestedYChromaOffset;
    info.sampler_swizzle = fmt.samplerYcbcrConversionComponents;
    info.suggested_filter =
        (fmt.formatFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
            ? VK_FILTER_LINEAR
            : VK_FILTER_NEAREST;
    return info;
}

ExternalImage importAHardwareBuffer(const Context& ctx, AHardwareBuffer* ahb,
                                    VkImageUsageFlags usage) {
    const AhbInfo info = describeAHardwareBuffer(ctx, ahb);

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);

    ExternalImage out;
    out.ctx_ = &ctx;
    out.width_ = desc.width;
    out.height_ = desc.height;
    out.format_ = info.format;
    out.external_format_ = info.external_format != 0;

    // An external format forces SAMPLED-only usage and the YCbCr path.
    if (out.external_format_) usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    VkExternalFormatANDROID ext_fmt{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    ext_fmt.externalFormat = info.external_format;

    VkExternalMemoryImageCreateInfo emici{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    emici.pNext = out.external_format_ ? &ext_fmt : nullptr;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &emici;
    ici.imageType = VK_IMAGE_TYPE_2D;
    // With an external format the VkFormat *must* be UNDEFINED.
    ici.format = out.external_format_ ? VK_FORMAT_UNDEFINED : info.format;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    NXVC_VK_CHECK(vkCreateImage(ctx.device(), &ici, nullptr, &out.image_));

    // A dedicated allocation is required for AHB imports.
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = out.image_;
    VkImportAndroidHardwareBufferInfoANDROID imp{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    imp.buffer = ahb;
    imp.pNext = &ded;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = info.allocation_size;
    mai.memoryTypeIndex = ctx.findMemoryType(info.memory_type_bits, 0);
    NXVC_VK_CHECK(vkAllocateMemory(ctx.device(), &mai, nullptr, &out.memory_));
    NXVC_VK_CHECK(vkBindImageMemory(ctx.device(), out.image_, out.memory_, 0));

    // ------------------------------------------------- YCbCr conversion
    const bool needs_ycbcr =
        out.external_format_ ||
        info.ycbcr_model != VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    VkSamplerYcbcrConversionInfo conv_info{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};

    if (needs_ycbcr) {
        if (!ctx.fns().createYcbcrConversion)
            throw Error(NXVC_VK_ERR_UNSUPPORTED,
                        "samplerYcbcrConversion needed but not enabled");
        VkSamplerYcbcrConversionCreateInfo cci{
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
        cci.pNext = out.external_format_ ? &ext_fmt : nullptr;
        cci.format = out.external_format_ ? VK_FORMAT_UNDEFINED : info.format;
        cci.ycbcrModel = info.ycbcr_model;
        cci.ycbcrRange = info.ycbcr_range;
        cci.components = info.sampler_swizzle;
        cci.xChromaOffset = info.x_chroma_offset;
        cci.yChromaOffset = info.y_chroma_offset;
        cci.chromaFilter = info.suggested_filter;
        cci.forceExplicitReconstruction = VK_FALSE;
        NXVC_VK_CHECK(
            ctx.fns().createYcbcrConversion(ctx.device(), &cci, nullptr, &out.ycbcr_));
        conv_info.conversion = out.ycbcr_;
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.pNext = needs_ycbcr ? &conv_info : nullptr;
    vci.image = out.image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NXVC_VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &out.view_));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.pNext = needs_ycbcr ? &conv_info : nullptr;
    sci.magFilter = info.suggested_filter;
    sci.minFilter = info.suggested_filter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    NXVC_VK_CHECK(vkCreateSampler(ctx.device(), &sci, nullptr, &out.sampler_));
    return out;
}

}  // namespace nxvc::vk::external

#endif  // __ANDROID__
