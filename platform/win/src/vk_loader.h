// NX Warp Windows interop probe: minimal runtime Vulkan loader.
//
// vulkan-1.lib is not shipped with llvm-mingw, and a probe must survive a box
// with no Vulkan ICD at all (it should print a JSON verdict, not fail to
// start). So vulkan-1.dll is loaded with LoadLibrary and every entry point is
// resolved through vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#pragma once

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace nxwarp::win {

// ---- global-level entry points (no instance) --------------------------------
#define NXW_VK_GLOBAL_FUNCS(X)                                                 \
    X(vkEnumerateInstanceVersion)                                              \
    X(vkEnumerateInstanceExtensionProperties)                                  \
    X(vkCreateInstance)

// ---- instance-level entry points --------------------------------------------
#define NXW_VK_INSTANCE_FUNCS(X)                                               \
    X(vkDestroyInstance)                                                       \
    X(vkEnumeratePhysicalDevices)                                              \
    X(vkGetPhysicalDeviceProperties)                                           \
    X(vkGetPhysicalDeviceProperties2)                                          \
    X(vkGetPhysicalDeviceFeatures2)                                            \
    X(vkGetPhysicalDeviceMemoryProperties)                                     \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                \
    X(vkGetPhysicalDeviceImageFormatProperties2)                               \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)                          \
    X(vkEnumerateDeviceExtensionProperties)                                    \
    X(vkCreateDevice)                                                          \
    X(vkGetDeviceProcAddr)

// ---- device-level entry points ----------------------------------------------
#define NXW_VK_DEVICE_FUNCS(X)                                                 \
    X(vkDestroyDevice)                                                         \
    X(vkGetDeviceQueue)                                                        \
    X(vkDeviceWaitIdle)                                                        \
    X(vkQueueSubmit)                                                           \
    X(vkQueueWaitIdle)                                                         \
    X(vkCreateImage)                                                           \
    X(vkDestroyImage)                                                          \
    X(vkGetImageMemoryRequirements2)                                           \
    X(vkAllocateMemory)                                                        \
    X(vkFreeMemory)                                                            \
    X(vkBindImageMemory2)                                                      \
    X(vkCreateImageView)                                                       \
    X(vkDestroyImageView)                                                      \
    X(vkCreateSemaphore)                                                       \
    X(vkDestroySemaphore)                                                      \
    X(vkGetSemaphoreCounterValue)                                              \
    X(vkWaitSemaphores)                                                        \
    X(vkCreateShaderModule)                                                    \
    X(vkDestroyShaderModule)                                                   \
    X(vkCreateDescriptorSetLayout)                                             \
    X(vkDestroyDescriptorSetLayout)                                            \
    X(vkCreatePipelineLayout)                                                  \
    X(vkDestroyPipelineLayout)                                                 \
    X(vkCreateComputePipelines)                                                \
    X(vkDestroyPipeline)                                                       \
    X(vkCreateDescriptorPool)                                                  \
    X(vkDestroyDescriptorPool)                                                 \
    X(vkAllocateDescriptorSets)                                                \
    X(vkUpdateDescriptorSets)                                                  \
    X(vkCreateCommandPool)                                                     \
    X(vkDestroyCommandPool)                                                    \
    X(vkAllocateCommandBuffers)                                                \
    X(vkBeginCommandBuffer)                                                    \
    X(vkEndCommandBuffer)                                                      \
    X(vkCmdBindPipeline)                                                       \
    X(vkCmdBindDescriptorSets)                                                 \
    X(vkCmdPushConstants)                                                      \
    X(vkCmdDispatch)                                                           \
    X(vkCmdPipelineBarrier)                                                    \
    X(vkImportSemaphoreWin32HandleKHR)                                         \
    X(vkGetMemoryWin32HandlePropertiesKHR)

struct VkApi {
#define NXW_DECL(name) PFN_##name name = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    NXW_VK_GLOBAL_FUNCS(NXW_DECL)
    NXW_VK_INSTANCE_FUNCS(NXW_DECL)
    NXW_VK_DEVICE_FUNCS(NXW_DECL)
#undef NXW_DECL
};

} // namespace nxwarp::win
