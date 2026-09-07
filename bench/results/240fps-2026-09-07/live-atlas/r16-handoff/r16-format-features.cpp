#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main()
{
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS)
        return 2;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());
    for (VkPhysicalDevice dev : devices)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        std::printf("device %s api %u.%u.%u\n", props.deviceName,
                    VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                    VK_VERSION_PATCH(props.apiVersion));
        for (VkFormat f : {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UINT})
        {
            VkFormatProperties p{};
            vkGetPhysicalDeviceFormatProperties(dev, f, &p);
            std::printf("format %d optimal=0x%x sampled=%d linear=%d transferSrc=%d transferDst=%d\n",
                        int(f), p.optimalTilingFeatures, bool(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
                        bool(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
                        bool(p.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),
                        bool(p.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT));
        }
    }
    vkDestroyInstance(instance, nullptr);
}
