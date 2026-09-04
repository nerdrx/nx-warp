// K6 hybrid base layer (PAPER 3.5).
//
// STATUS: the AHardwareBuffer -> Vulkan import path below is complete and is
// the part the codec will reuse. The MediaCodec feed is a straight Annex-B
// push of a pre-encoded elementary stream shipped as an asset; it measures
// decode latency but does not yet do the things a real client does.
//
// TODO before this is production:
//   * Qualcomm vendor key "vendor.qti-ext-dec-low-latency.enable" alongside
//     KEY_LOW_LATENCY, and verify it actually took (PAPER 3.4 says K6 failing
//     on latency means low-latency mode is not working on this firmware).
//   * Release the image with AImage_deleteAsync and a sync fd exported from
//     the Pass C submit, instead of the vkQueueWaitIdle used here.
//   * Import the acquire sync fd into a binary VkSemaphore
//     (VK_KHR_external_semaphore_fd) and wait on it in the Pass C submit,
//     rather than relying on the decoder having finished.
//   * Cache imports by AHardwareBuffer identity across the whole pool; this
//     re-imports whenever the buffer pointer changes.
#include "k6_hybrid.h"

#include <android/hardware_buffer.h>
#include <android/imagedecoder.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace nxb {

namespace {
double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

bool HybridBase::start(VkCtx& ctx, int width, int height, int fps,
                       const std::string& assetPath)
{
    ctx_ = &ctx;
    w_ = width; h_ = height; fps_ = fps;

    FILE* f = fopen(assetPath.c_str(), "rb");
    if (!f)
    {
        status_ = "no HEVC asset at " + assetPath;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bitstream_.resize(size_t(n));
    if (fread(bitstream_.data(), 1, size_t(n), f) != size_t(n))
    {
        fclose(f);
        status_ = "short read on HEVC asset";
        return false;
    }
    fclose(f);

    media_status_t ms = AImageReader_newWithUsage(
        width, height, AIMAGE_FORMAT_PRIVATE,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &reader_);
    if (ms != AMEDIA_OK || !reader_)
    {
        status_ = "AImageReader_newWithUsage failed";
        return false;
    }

    ANativeWindow* window = nullptr;
    if (AImageReader_getWindow(reader_, &window) != AMEDIA_OK || !window)
    {
        status_ = "AImageReader_getWindow failed";
        return false;
    }

    codec_ = AMediaCodec_createDecoderByType("video/hevc");
    if (!codec_) { status_ = "no HEVC decoder"; return false; }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    // KEY_LOW_LATENCY. The NDK constant only exists from API 30; the string is
    // stable and is what the framework looks for.
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);

    if (AMediaCodec_configure(codec_, fmt, window, nullptr, 0) != AMEDIA_OK)
    {
        AMediaFormat_delete(fmt);
        status_ = "AMediaCodec_configure failed";
        return false;
    }
    AMediaFormat_delete(fmt);

    if (AMediaCodec_start(codec_) != AMEDIA_OK)
    {
        status_ = "AMediaCodec_start failed";
        return false;
    }

    status_ = "started";
    // Prime the pipeline so the first poll() has something to acquire.
    for (int i = 0; i < 8; ++i) poll();
    return true;
}

void HybridBase::stop()
{
    if (codec_) { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr; }
    if (reader_) { AImageReader_delete(reader_); reader_ = nullptr; }
    if (!ctx_) return;
    if (view_)    { vkDestroyImageView(ctx_->dev, view_, nullptr); view_ = VK_NULL_HANDLE; }
    if (sampler_) { vkDestroySampler(ctx_->dev, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (conv_)    { vkDestroySamplerYcbcrConversion(ctx_->dev, conv_, nullptr); conv_ = VK_NULL_HANDLE; }
    if (image_)   { vkDestroyImage(ctx_->dev, image_, nullptr); image_ = VK_NULL_HANDLE; }
    if (mem_)     { vkFreeMemory(ctx_->dev, mem_, nullptr); mem_ = VK_NULL_HANDLE; }
}

bool HybridBase::poll()
{
    if (!codec_) return false;

    // ---- feed: one Annex-B access unit per input buffer. The asset is a
    // plain elementary stream, so access units are split at 0x00000001 with a
    // VCL NAL leading.
    ssize_t inIdx = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (inIdx >= 0)
    {
        if (readPos_ >= bitstream_.size()) readPos_ = 0;   // loop the clip

        size_t start = readPos_;
        size_t end = bitstream_.size();
        for (size_t i = start + 4; i + 4 < bitstream_.size(); ++i)
            if (bitstream_[i] == 0 && bitstream_[i + 1] == 0 &&
                bitstream_[i + 2] == 0 && bitstream_[i + 3] == 1)
            {
                // Next start code: cut here if the following NAL begins a
                // picture (nal_unit_type < 32 is VCL in HEVC).
                uint8_t nal = uint8_t((bitstream_[i + 4] >> 1) & 0x3f);
                if (nal < 32) { end = i; break; }
            }

        size_t bytes = end - start;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, size_t(inIdx), &cap);
        if (buf && cap >= bytes)
        {
            memcpy(buf, bitstream_.data() + start, bytes);
            int64_t pts = frameIndex_ * (1000000ll / (fps_ ? fps_ : 90));
            AMediaCodec_queueInputBuffer(codec_, size_t(inIdx), 0, bytes, uint64_t(pts), 0);
            submitTime_.push_back(nowMs());
            ++frameIndex_;
            readPos_ = end;
        }
        else if (buf)
        {
            AMediaCodec_queueInputBuffer(codec_, size_t(inIdx), 0, 0, 0, 0);
        }
    }

    // ---- drain: render to the reader's surface, one output per input, never
    // queued (PAPER 3.5 step 1).
    AMediaCodecBufferInfo bi;
    ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec_, &bi, 0);
    if (outIdx >= 0)
    {
        AMediaCodec_releaseOutputBuffer(codec_, size_t(outIdx), true);
        if (!submitTime_.empty())
        {
            latency_.push_back(nowMs() - submitTime_.front());
            submitTime_.erase(submitTime_.begin());
        }
        return importCurrent();
    }
    return false;
}

bool HybridBase::importCurrent()
{
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader_, &img) != AMEDIA_OK || !img)
        return false;

    AHardwareBuffer* ahb = nullptr;
    if (AImage_getHardwareBuffer(img, &ahb) != AMEDIA_OK || !ahb)
    {
        AImage_delete(img);
        return false;
    }

    if (ahb == importedBuffer_ && view_ != VK_NULL_HANDLE)
    {
        // Already imported. The pool is small and recycled, so this is the
        // common case after the first few frames.
        AImage_delete(img);
        return true;
    }

    auto getProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(ctx_->dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!getProps)
    {
        status_ = "VK_ANDROID_external_memory_android_hardware_buffer not available";
        AImage_delete(img);
        return false;
    }

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    props.pNext = &fmtProps;
    if (getProps(ctx_->dev, ahb, &props) != VK_SUCCESS)
    {
        status_ = "vkGetAndroidHardwareBufferPropertiesANDROID failed";
        AImage_delete(img);
        return false;
    }

    // Tear down any previous import.
    if (view_)    { vkDestroyImageView(ctx_->dev, view_, nullptr); view_ = VK_NULL_HANDLE; }
    if (sampler_) { vkDestroySampler(ctx_->dev, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (conv_)    { vkDestroySamplerYcbcrConversion(ctx_->dev, conv_, nullptr); conv_ = VK_NULL_HANDLE; }
    if (image_)   { vkDestroyImage(ctx_->dev, image_, nullptr); image_ = VK_NULL_HANDLE; }
    if (mem_)     { vkFreeMemory(ctx_->dev, mem_, nullptr); mem_ = VK_NULL_HANDLE; }

    // The decoder emits a vendor-tiled YCbCr format (typically UBWC NV12),
    // which has no VkFormat: it comes through as an external format.
    VkExternalFormatANDROID extFmt{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    extFmt.externalFormat = fmtProps.externalFormat;

    VkExternalMemoryImageCreateInfo extImg{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    extImg.pNext = &extFmt;
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &extImg;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (fmtProps.externalFormat != 0) ? VK_FORMAT_UNDEFINED : fmtProps.format;
    ici.extent = {uint32_t(w_), uint32_t(h_), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_->dev, &ici, nullptr, &image_) != VK_SUCCESS)
    {
        status_ = "vkCreateImage on the external format failed";
        AImage_delete(img);
        return false;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    importInfo.buffer = ahb;
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image_;
    dedicated.pNext = &importInfo;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &dedicated;
    mai.allocationSize = props.allocationSize;
    mai.memoryTypeIndex = ctx_->findMem(props.memoryTypeBits, 0);
    if (vkAllocateMemory(ctx_->dev, &mai, nullptr, &mem_) != VK_SUCCESS)
    {
        status_ = "import allocation failed";
        AImage_delete(img);
        return false;
    }

    VkBindImageMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    bind.image = image_;
    bind.memory = mem_;
    bind.memoryOffset = 0;
    auto bind2 = (PFN_vkBindImageMemory2)
        vkGetDeviceProcAddr(ctx_->dev, "vkBindImageMemory2");
    if (!bind2) bind2 = (PFN_vkBindImageMemory2)
        vkGetDeviceProcAddr(ctx_->dev, "vkBindImageMemory2KHR");
    if (!bind2 || bind2(ctx_->dev, 1, &bind) != VK_SUCCESS)
    {
        status_ = "vkBindImageMemory2 failed";
        AImage_delete(img);
        return false;
    }

    // A YCbCr conversion over the external format, and an immutable sampler
    // built on it: Pass C samples the base through this (allowed, because the
    // base is not in the bit-exact path).
    VkSamplerYcbcrConversionCreateInfo cci{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    cci.pNext = &extFmt;
    cci.format = VK_FORMAT_UNDEFINED;
    cci.ycbcrModel = fmtProps.suggestedYcbcrModel;
    cci.ycbcrRange = fmtProps.suggestedYcbcrRange;
    cci.components = fmtProps.samplerYcbcrConversionComponents;
    cci.xChromaOffset = fmtProps.suggestedXChromaOffset;
    cci.yChromaOffset = fmtProps.suggestedYChromaOffset;
    cci.chromaFilter = VK_FILTER_NEAREST;
    cci.forceExplicitReconstruction = VK_FALSE;
    auto mkConv = (PFN_vkCreateSamplerYcbcrConversion)
        vkGetDeviceProcAddr(ctx_->dev, "vkCreateSamplerYcbcrConversion");
    if (!mkConv) mkConv = (PFN_vkCreateSamplerYcbcrConversion)
        vkGetDeviceProcAddr(ctx_->dev, "vkCreateSamplerYcbcrConversionKHR");
    if (!mkConv || mkConv(ctx_->dev, &cci, nullptr, &conv_) != VK_SUCCESS)
    {
        status_ = "vkCreateSamplerYcbcrConversion failed";
        AImage_delete(img);
        return false;
    }

    VkSamplerYcbcrConversionInfo convInfo{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    convInfo.conversion = conv_;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.pNext = &convInfo;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(ctx_->dev, &sci, nullptr, &sampler_) != VK_SUCCESS)
    {
        status_ = "ycbcr sampler creation failed";
        AImage_delete(img);
        return false;
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.pNext = &convInfo;
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_UNDEFINED;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx_->dev, &vci, nullptr, &view_) != VK_SUCCESS)
    {
        status_ = "external image view creation failed";
        AImage_delete(img);
        return false;
    }

    importedBuffer_ = ahb;
    status_ = "imported external format " +
              std::to_string((unsigned long long)fmtProps.externalFormat);
    AImage_delete(img);
    return true;
}

} // namespace nxb
