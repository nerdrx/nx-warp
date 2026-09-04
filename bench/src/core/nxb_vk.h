// Minimal Vulkan 1.1 scaffolding shared by the Android app and the headless
// host CLI. No allocator library, no helper framework: the bench must build
// with nothing but the NDK on one side and a system Vulkan loader on the other.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nxb {

void logLine(const char* tag, const char* fmt, ...);
#define NXB_LOG(...)  ::nxb::logLine("nxwarp-bench", __VA_ARGS__)

void vkCheck(VkResult r, const char* expr, const char* file, int line);
#define NXB_VK(x) ::nxb::vkCheck((x), #x, __FILE__, __LINE__)

struct Buffer
{
    VkBuffer       buf  = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    VkDeviceSize   size = 0;
    void*          mapped = nullptr;
};

struct Image
{
    VkImage        img  = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    VkFormat       fmt  = VK_FORMAT_UNDEFINED;
    uint32_t       w = 0, h = 0;
    VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct DeviceInfo
{
    std::string name;
    std::string driver;
    uint32_t    apiVersion = 0;
    uint32_t    vendorID = 0, deviceID = 0;
    uint32_t    subgroupSize = 0;
    uint32_t    subgroupMin = 0, subgroupMax = 0;
    bool        subgroupBallot = false;
    bool        subgroupSizeControl = false;
    bool        pipelineExecutableProps = false;
    uint32_t    maxSharedMemory = 0;
    float       timestampPeriod = 1.0f;
    uint32_t    timestampValidBits = 0;
};

struct VkCtx
{
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         dev      = VK_NULL_HANDLE;
    VkQueue          queue    = VK_NULL_HANDLE;
    uint32_t         qfam     = 0;
    VkCommandPool    pool     = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties       props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    DeviceInfo info;

    bool create(const std::vector<const char*>& instExt,
                const std::vector<const char*>& devExt,
                bool validation,
                VkSurfaceKHR (*makeSurface)(VkInstance, void*) = nullptr,
                void* surfaceUser = nullptr,
                VkSurfaceKHR* outSurface = nullptr);
    void destroy();

    uint32_t findMem(uint32_t typeBits, VkMemoryPropertyFlags want) const;

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags props);
    Image  createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage);
    void   destroyBuffer(Buffer& b);
    void   destroyImage(Image& i);

    void upload(Buffer& dst, const void* data, VkDeviceSize bytes);
    void fillImage(Image& img, const void* data, VkDeviceSize bytes);
    void oneShot(const std::function<void(VkCommandBuffer)>& fn);

    VkShaderModule shader(const uint32_t* code, size_t bytes) const;
};

// A full shader-write to shader-read barrier plus image general->general.
// Between kernels so their timestamp pairs cannot overlap.
void fullBarrier(VkCommandBuffer cmd);

// Transitions an image to VK_IMAGE_LAYOUT_GENERAL from whatever it is.
void toGeneral(VkCommandBuffer cmd, Image& img);

} // namespace nxb
