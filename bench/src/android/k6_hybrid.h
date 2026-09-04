// K6: the hybrid base layer. MediaCodec decodes HEVC into an AImageReader
// whose buffers are AHardwareBuffers; each distinct buffer is imported into
// Vulkan once, through VkExternalFormatANDROID plus a VkSamplerYcbcrConversion
// (PAPER 3.5). Pass C then samples it.
#pragma once

#include "nxb_vk.h"

#include <string>
#include <vector>

struct AImageReader;
struct AMediaCodec;
struct AMediaFormat;

namespace nxb {

class HybridBase
{
public:
    // width/height of the base layer, and the target frame rate.
    bool start(VkCtx& ctx, int width, int height, int fps, const std::string& assetPath);
    void stop();

    // Pumps the decoder. Returns true when a new frame has been imported and
    // view()/sampler() are valid. Non-blocking.
    bool poll();

    VkImageView view() const { return view_; }
    VkSampler   sampler() const { return sampler_; }
    bool ready() const { return view_ != VK_NULL_HANDLE; }

    // Decode latency samples, milliseconds from queueInputBuffer to the image
    // becoming available (PAPER 3.4: threshold p50 < 15 ms).
    const std::vector<double>& latencies() const { return latency_; }
    const std::string& status() const { return status_; }

private:
    bool importCurrent();

    VkCtx* ctx_ = nullptr;
    AImageReader* reader_ = nullptr;
    AMediaCodec*  codec_  = nullptr;

    VkImage        image_ = VK_NULL_HANDLE;
    VkDeviceMemory mem_   = VK_NULL_HANDLE;
    VkImageView    view_  = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion conv_ = VK_NULL_HANDLE;
    void*          importedBuffer_ = nullptr;   // AHardwareBuffer*

    std::vector<uint8_t> bitstream_;
    size_t readPos_ = 0;
    std::vector<double> latency_;
    std::vector<double> submitTime_;
    std::string status_ = "not started";
    int w_ = 0, h_ = 0, fps_ = 90;
    long long frameIndex_ = 0;
};

} // namespace nxb
