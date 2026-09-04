#include "nxb_vk.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace nxb {

void logLine(const char* tag, const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, tag, "%s", buf);
#else
    (void)tag;
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
#endif
}

void vkCheck(VkResult r, const char* expr, const char* file, int line)
{
    if (r == VK_SUCCESS) return;
    NXB_LOG("VULKAN ERROR %d at %s:%d -- %s", int(r), file, line, expr);
    abort();
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCb(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        NXB_LOG("[validation] %s", d->pMessage);
    return VK_FALSE;
}

bool VkCtx::create(const std::vector<const char*>& instExtIn,
                   const std::vector<const char*>& devExtIn,
                   bool validation,
                   VkSurfaceKHR (*makeSurface)(VkInstance, void*),
                   void* surfaceUser,
                   VkSurfaceKHR* outSurface)
{
    std::vector<const char*> instExt = instExtIn;
    std::vector<const char*> layers;

    uint32_t nLayerProps = 0;
    vkEnumerateInstanceLayerProperties(&nLayerProps, nullptr);
    std::vector<VkLayerProperties> layerProps(nLayerProps);
    if (nLayerProps) vkEnumerateInstanceLayerProperties(&nLayerProps, layerProps.data());

    uint32_t nInstExt = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &nInstExt, nullptr);
    std::vector<VkExtensionProperties> instExtProps(nInstExt);
    if (nInstExt) vkEnumerateInstanceExtensionProperties(nullptr, &nInstExt, instExtProps.data());
    auto haveInstExt = [&](const char* n) {
        for (auto& e : instExtProps) if (!strcmp(e.extensionName, n)) return true;
        return false;
    };

    bool debugUtils = false;
    if (validation)
    {
        for (auto& l : layerProps)
            if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"))
                layers.push_back("VK_LAYER_KHRONOS_validation");
        if (!layers.empty() && haveInstExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            instExt.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debugUtils = true;
        }
    }
    if (haveInstExt(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        instExt.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxwarp-bench";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(instExt.size());
    ici.ppEnabledExtensionNames = instExt.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    NXB_VK(vkCreateInstance(&ici, nullptr, &instance));

    if (debugUtils)
    {
        auto f = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (f)
        {
            VkDebugUtilsMessengerCreateInfoEXT ci{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            ci.pfnUserCallback = debugCb;
            VkDebugUtilsMessengerEXT m;
            f(instance, &ci, nullptr, &m);
        }
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (makeSurface)
    {
        surface = makeSurface(instance, surfaceUser);
        if (outSurface) *outSurface = surface;
    }

    uint32_t nDev = 0;
    vkEnumeratePhysicalDevices(instance, &nDev, nullptr);
    if (!nDev) { NXB_LOG("no Vulkan physical device"); return false; }
    std::vector<VkPhysicalDevice> devs(nDev);
    vkEnumeratePhysicalDevices(instance, &nDev, devs.data());

    // Prefer a discrete GPU, then integrated, then whatever is left. On a
    // headset there is exactly one; on this desktop it picks the dGPU, and
    // lavapipe (CPU type) is only chosen when it is the only ICD.
    int bestScore = -1;
    for (auto d : devs)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        int score = 0;
        switch (p.deviceType)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 4; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 3; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 2; break;
        default:                                     score = 1; break;
        }
        if (score > bestScore) { bestScore = score; phys = d; props = p; }
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    // ---- queue: one universal family (graphics + compute + timestamps)
    uint32_t nQ = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nQ, nullptr);
    std::vector<VkQueueFamilyProperties> qp(nQ);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nQ, qp.data());
    bool found = false;
    for (uint32_t i = 0; i < nQ; ++i)
    {
        bool gfx = (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        bool cmp = (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        if (!cmp) continue;
        if (surface != VK_NULL_HANDLE)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
            if (!present || !gfx) continue;
        }
        qfam = i;
        info.timestampValidBits = qp[i].timestampValidBits;
        found = true;
        break;
    }
    if (!found) { NXB_LOG("no suitable queue family"); return false; }

    // ---- device extensions, taking only what the driver actually offers
    uint32_t nExt = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &nExt, nullptr);
    std::vector<VkExtensionProperties> extProps(nExt);
    if (nExt) vkEnumerateDeviceExtensionProperties(phys, nullptr, &nExt, extProps.data());
    auto haveExt = [&](const char* n) {
        for (auto& e : extProps) if (!strcmp(e.extensionName, n)) return true;
        return false;
    };

    std::vector<const char*> devExt;
    for (auto* e : devExtIn)
        if (haveExt(e)) devExt.push_back(e);
        else NXB_LOG("device extension %s not present, continuing without it", e);

    if (haveExt(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
    {
        devExt.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        info.subgroupSizeControl = true;
    }
    if (haveExt(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME))
        info.pipelineExecutableProps = true;   // enabled by the caller if wanted

    // ---- subgroup properties (PAPER 3.2.6 gate: refuse below 8)
    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sub;
    VkPhysicalDeviceSubgroupSizeControlPropertiesEXT sizeCtl{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT};
    if (info.subgroupSizeControl) { sub.pNext = &sizeCtl; }
    vkGetPhysicalDeviceProperties2(phys, &p2);

    info.name = props.deviceName;
    info.apiVersion = props.apiVersion;
    info.vendorID = props.vendorID;
    info.deviceID = props.deviceID;
    info.subgroupSize = sub.subgroupSize;
    info.subgroupBallot = (sub.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
    info.maxSharedMemory = props.limits.maxComputeSharedMemorySize;
    info.timestampPeriod = props.limits.timestampPeriod;
    if (info.subgroupSizeControl)
    {
        info.subgroupMin = sizeCtl.minSubgroupSize;
        info.subgroupMax = sizeCtl.maxSubgroupSize;
    }
    else
    {
        info.subgroupMin = info.subgroupMax = sub.subgroupSize;
    }

    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2b.pNext = &drv;
    if (props.apiVersion >= VK_API_VERSION_1_2)
    {
        vkGetPhysicalDeviceProperties2(phys, &p2b);
        info.driver = std::string(drv.driverName) + " " + drv.driverInfo;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures feat{};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feat;
    dci.enabledExtensionCount = uint32_t(devExt.size());
    dci.ppEnabledExtensionNames = devExt.data();
    dci.enabledLayerCount = uint32_t(layers.size());
    dci.ppEnabledLayerNames = layers.data();

    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sizeFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};
    if (info.subgroupSizeControl)
    {
        sizeFeat.subgroupSizeControl = VK_TRUE;
        sizeFeat.computeFullSubgroups = VK_TRUE;
        dci.pNext = &sizeFeat;
    }

    NXB_VK(vkCreateDevice(phys, &dci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam;
    NXB_VK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    return true;
}

void VkCtx::destroy()
{
    if (dev)
    {
        vkDeviceWaitIdle(dev);
        if (pool) vkDestroyCommandPool(dev, pool, nullptr);
        vkDestroyDevice(dev, nullptr);
        dev = VK_NULL_HANDLE;
    }
    if (instance) { vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
}

uint32_t VkCtx::findMem(uint32_t typeBits, VkMemoryPropertyFlags want) const
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want)
            return i;
    NXB_LOG("no memory type for bits %u flags %u", typeBits, unsigned(want));
    abort();
}

Buffer VkCtx::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags mp)
{
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    NXB_VK(vkCreateBuffer(dev, &ci, nullptr, &b.buf));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMem(req.memoryTypeBits, mp);
    NXB_VK(vkAllocateMemory(dev, &ai, nullptr, &b.mem));
    NXB_VK(vkBindBufferMemory(dev, b.buf, b.mem, 0));

    if (mp & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        NXB_VK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

Image VkCtx::createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage)
{
    Image im;
    im.w = w; im.h = h; im.fmt = fmt;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    NXB_VK(vkCreateImage(dev, &ci, nullptr, &im.img));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im.img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    NXB_VK(vkAllocateMemory(dev, &ai, nullptr, &im.mem));
    NXB_VK(vkBindImageMemory(dev, im.img, im.mem, 0));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NXB_VK(vkCreateImageView(dev, &vi, nullptr, &im.view));
    return im;
}

void VkCtx::destroyBuffer(Buffer& b)
{
    if (b.mapped) { vkUnmapMemory(dev, b.mem); b.mapped = nullptr; }
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
    b = {};
}

void VkCtx::destroyImage(Image& i)
{
    if (i.view) vkDestroyImageView(dev, i.view, nullptr);
    if (i.img)  vkDestroyImage(dev, i.img, nullptr);
    if (i.mem)  vkFreeMemory(dev, i.mem, nullptr);
    i = {};
}

void VkCtx::oneShot(const std::function<void(VkCommandBuffer)>& fn)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    NXB_VK(vkAllocateCommandBuffers(dev, &ai, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    NXB_VK(vkBeginCommandBuffer(cmd, &bi));
    fn(cmd);
    NXB_VK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    NXB_VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    NXB_VK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

void VkCtx::upload(Buffer& dst, const void* data, VkDeviceSize bytes)
{
    Buffer stage = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(stage.mapped, data, size_t(bytes));
    oneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy c{0, 0, bytes};
        vkCmdCopyBuffer(cmd, stage.buf, dst.buf, 1, &c);
    });
    destroyBuffer(stage);
}

void VkCtx::fillImage(Image& img, const void* data, VkDeviceSize bytes)
{
    Buffer stage = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(stage.mapped, data, size_t(bytes));
    oneShot([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {img.w, img.h, 1};
        vkCmdCopyBufferToImage(cmd, stage.buf, img.img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    destroyBuffer(stage);
    img.layout = VK_IMAGE_LAYOUT_GENERAL;
}

VkShaderModule VkCtx::shader(const uint32_t* code, size_t bytes) const
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    NXB_VK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

void fullBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void toGeneral(VkCommandBuffer cmd, Image& img)
{
    if (img.layout == VK_IMAGE_LAYOUT_GENERAL) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = img.layout;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = VK_IMAGE_LAYOUT_GENERAL;
}

} // namespace nxb
