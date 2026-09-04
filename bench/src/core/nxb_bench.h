// The Phase 0 kernels K1..K6 (PAPER 3.4), their resources, and the timestamp
// plumbing. Shared verbatim by the Android app and the headless host CLI: the
// only thing the two frontends do differently is own a swapchain.
#pragma once

#include "nxb_rans.h"
#include "nxb_vk.h"

#include <array>
#include <string>
#include <vector>

namespace nxb {

// Dense int16 coefficients, 64 blocks of 64 per tile: 8 KB per tile, 16.8 MB
// per frame at 2048 tiles (PAPER 3.2.5).
constexpr uint32_t NXB_COEFS_PER_TILE_BYTES = 4096 * 2;

enum Kid { K1_COPY = 0, K2_GATHER4, K2B_SAMPLER, K3_IDCT, K4_RANS, K5_FULL, K6_HYBRID, KID_COUNT };

const char* kidName(int k);
const char* kidDesc(int k);

struct Config
{
    // 2 views x 2048^2, laid out view-stacked as one 2048 x 4096 image, which
    // makes the tile grid exactly 32 x 64 = 2048 tiles (PAPER 3.1).
    int width  = 2048;
    int height = 4096;

    // The dummy reprojection co-tenant: 2 x 2160^2 (PAPER 3.4).
    int reproW = 2160;
    int reproH = 4320;

    int  warmup = 120;
    int  frames = 600;
    bool cotenant = true;
    bool validation = false;

    int qp = 26;
    double symbolsPerPixel = 0.5;      // PAPER 3.4 K4

    double thermalSeconds = 0.0;       // > 0 selects the 10-minute thermal mode
    uint32_t kernelMask = 0x3f;        // K1..K5 by default; K6 is opt-in
    uint64_t seed = 0x5741525000000001ull;

    // Force a subgroup width on the rANS kernel via
    // VK_EXT_subgroup_size_control. 0 means "whatever the driver picks".
    // This is how the 8-lane cluster rule of PAPER 3.2.6 gets tested across
    // widths on one GPU, without needing four different GPUs.
    uint32_t forceSubgroupSize = 0;

    std::string outPath;
    std::string label;                 // free-form, echoed into the JSON
};

struct KernelResult
{
    std::string name;
    bool   ran = false;
    std::string skipReason;
    int    frames = 0;
    double p50 = 0, p95 = 0, p99 = 0, minMs = 0, maxMs = 0, mean = 0;
    double gbPerSec = 0;               // K1 only
    double firstMinuteP50 = 0;         // thermal mode
    double lastMinuteP50 = 0;
    std::vector<double> minuteP50;
    // Threshold as stated in PAPER 3.4. thresholdP50 <= 0 means informational.
    double thresholdP50 = 0;
    double thresholdP99 = 0;
    double thresholdGbps = 0;
};

double percentile(std::vector<double> v, double p);

class Bench
{
public:
    bool init(VkCtx& ctx, const Config& cfg);
    void destroy();

    // Records the co-tenant reprojection dispatch. Not timed: it is the load,
    // not the subject.
    void recordCotenant(VkCommandBuffer cmd, float jx, float jy);

    // Records one kernel with a VK_QUERY_TYPE_TIMESTAMP pair around it.
    void recordKernel(VkCommandBuffer cmd, int kid, uint32_t slot);

    // Must be called at the top of every command buffer that uses `slot`.
    void resetQueries(VkCommandBuffer cmd, uint32_t slot);

    // Reads back one timestamp pair, in milliseconds. Returns < 0 if the
    // result was not available.
    double readMs(uint32_t slot);

    // Bit-exactness checks against the CPU reference (PAPER 3.9: the CPU
    // reference is the specification, the SPIR-V is validated against it).
    bool verifyPassA(std::string* msg);
    bool verifyPassB(std::string* msg);

    bool available(int kid, std::string* why) const;
    double bytesMoved(int kid) const;   // for the K1 GB/s figure

    // K6 hooks, implemented by the Android frontend. When no external base
    // image has been supplied, Pass C samples a synthetic stand-in so the
    // kernel still times correctly (best-effort mode).
    void setHybridBase(VkImageView view, VkSampler sampler);

    Image& displayImage() { return outImg_; }
    const Config& config() const { return cfg_; }
    const RansStream& stream() const { return stream_; }

    static const uint32_t kSlots = 4;

private:
    struct Kern
    {
        VkPipeline            pipe = VK_NULL_HANDLE;
        VkPipelineLayout      layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkDescriptorSet       set = VK_NULL_HANDLE;
        uint32_t gx = 1, gy = 1, gz = 1;
    };

    Kern makeKern(const uint32_t* spv, size_t bytes,
                  const std::vector<VkDescriptorSetLayoutBinding>& binds,
                  uint32_t pushBytes, bool requireFullSubgroups);
    void writeSets();

    VkCtx*  ctx_ = nullptr;
    Config  cfg_{};

    Image refUint_{}, refUnorm_{}, outImg_{};
    Image reproSrc_{}, reproDst_{};
    Image prevResid_{}, newResid_{};
    Buffer coef_{}, deq_{}, tileRec_{}, bits_{}, tileOff_{}, tab_{}, delta_{};
    VkSampler linearSampler_ = VK_NULL_HANDLE;

    VkImageView hybridView_ = VK_NULL_HANDLE;
    VkSampler   hybridSampler_ = VK_NULL_HANDLE;

    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkQueryPool      queryPool_ = VK_NULL_HANDLE;

    Kern kCopy_, kWarp_, kSampler_, kIdct_, kRans_, kPassB_, kPassC_, kRepro_;
    Kern* kernOf_[KID_COUNT] = {};

    RansTables tables_{};
    RansStream stream_{};

    int tilesX_ = 0, tilesY_ = 0, tileCount_ = 0;
    int symsPerLane_ = 0;
    bool k6Ready_ = false;
};

// ------------------------------------------------------------- run driver
// Both frontends share this loop. The Android app supplies hooks that acquire
// a swapchain image, blit the output into it and present; the host CLI
// supplies none.
struct RunHooks
{
    std::function<bool(int frameIdx)>    preFrame;     // false aborts the run
    std::function<void(VkCommandBuffer)> extraRecord;  // e.g. the swapchain blit
    std::function<void()>                postFrame;    // e.g. present

    // Lets the frontend attach swapchain synchronisation to the bench submit.
    // The Android app uses it to wait on the acquire semaphore before the
    // command buffer copies into the swapchain image.
    std::function<void(std::vector<VkSemaphore>& waits,
                       std::vector<VkPipelineStageFlags>& stages,
                       std::vector<VkSemaphore>& signals)> submitSync;
};

class Runner
{
public:
    bool init(VkCtx& ctx, Bench& bench);
    void destroy();
    KernelResult runPass(int kid, const Config& cfg, const RunHooks& hooks);

private:
    VkCtx* ctx_ = nullptr;
    Bench* bench_ = nullptr;
    VkCommandBuffer cmd_[Bench::kSlots]{};
    VkFence fence_[Bench::kSlots]{};
};

// ------------------------------------------------------------ JSON output
struct RunInfo
{
    DeviceInfo device;
    Config     cfg;
    std::string mode;      // "bench" or "thermal"
    std::string platform;  // "android" or "host"
    std::string verdict;   // decision rule of PAPER 3.4
    double      hybridDecodeLatencyP50 = -1.0;   // K6, milliseconds
};

std::string buildJson(const RunInfo& info, const std::vector<KernelResult>& results);
std::string buildTable(const RunInfo& info, const std::vector<KernelResult>& results);
std::string verdictFor(const std::vector<KernelResult>& results);

} // namespace nxb
