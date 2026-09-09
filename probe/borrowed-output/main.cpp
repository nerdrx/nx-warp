#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <nxvc/nxvc_vk.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

static void vkcheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " (" + std::to_string(r) + ")");
}
static std::vector<uint8_t> file_bytes(const char* p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("open input");
    auto n = f.tellg();
    std::vector<uint8_t> b((size_t)n);
    f.seekg(0);
    f.read((char*)b.data(), n);
    return b;
}
struct Image {
    VkImage image{};
    VkDeviceMemory mem{};
    VkImageView y{}, uv{};
};
struct Ctx {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("memory type");
    }
    void init() {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "borrowed-output";
        ai.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &ai;
        vkcheck(vkCreateInstance(&ii, nullptr, &instance), "vkCreateInstance");
        uint32_t n = 0;
        vkcheck(vkEnumeratePhysicalDevices(instance, &n, nullptr), "enumerate");
        std::vector<VkPhysicalDevice> ps(n);
        vkcheck(vkEnumeratePhysicalDevices(instance, &n, ps.data()), "enumerate");
        for (auto p : ps) {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, q.data());
            for (uint32_t i = 0; i < qn; i++)
                if (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    physical = p;
                    family = i;
                    break;
                }
            if (physical)
                break;
        }
        if (!physical)
            throw std::runtime_error("no compute device");
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        bool need_format_list = VK_VERSION_MINOR(props.apiVersion) < 2;
        float pr = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &pr;
        VkPhysicalDeviceFeatures features{};
        features.shaderStorageImageExtendedFormats = VK_TRUE;
        features.shaderInt16 = VK_TRUE;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        timeline.timelineSemaphore = VK_TRUE;
        VkPhysicalDevice16BitStorageFeatures storage{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        storage.storageBuffer16BitAccess = VK_TRUE;
        storage.pNext = &timeline;
        const char* extensions[] = {VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
                                    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.pEnabledFeatures = &features;
        di.pNext = &storage;
        if (need_format_list) {
            di.enabledExtensionCount = 2;
            di.ppEnabledExtensionNames = extensions;
        }
        vkcheck(vkCreateDevice(physical, &di, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkcheck(vkCreateCommandPool(device, &pi, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        vkcheck(vkAllocateCommandBuffers(device, &ca, &cmd), "command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkcheck(vkCreateFence(device, &fi, nullptr, &fence), "fence");
    }
    Image make(uint32_t w, uint32_t h) {
        Image x{};
        VkFormat formats[3] = {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_R8_UINT,
                               VK_FORMAT_R8G8_UINT};
        VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        fl.viewFormatCount = 3;
        fl.pViewFormats = formats;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext = &fl;
        ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkcheck(vkCreateImage(device, &ci, nullptr, &x.image), "image");
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device, x.image, &mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkcheck(vkAllocateMemory(device, &ma, nullptr, &x.mem), "image memory");
        vkcheck(vkBindImageMemory(device, x.image, x.mem, 0), "bind image");
        VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        vu.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.pNext = &vu;
        vi.image = x.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 1, 0, 1};
        vi.format = VK_FORMAT_R8_UINT;
        vkcheck(vkCreateImageView(device, &vi, nullptr, &x.y), "Y view");
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        vi.format = VK_FORMAT_R8G8_UINT;
        vkcheck(vkCreateImageView(device, &vi, nullptr, &x.uv), "UV view");
        return x;
    }
    void read(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& y,
              std::vector<uint8_t>& uv) {
        // Read the two distinct NV12 planes into contiguous Y and interleaved UV.
        VkDeviceSize ys = VkDeviceSize(w) * h,
                     us = VkDeviceSize(w / 2) * (h / 2) * 2, total = ys + us;
        VkBuffer b{};
        VkDeviceMemory m{};
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = total;
        bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkcheck(vkCreateBuffer(device, &bc, nullptr, &b), "readback buffer");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device, b, &mr);
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkcheck(vkAllocateMemory(device, &ma, nullptr, &m), "readback memory");
        vkBindBufferMemory(device, b, m, 0);
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.image = im.image;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
        c.bufferOffset = ys;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        c.imageExtent = {w / 2, h / 2, 1};
        vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &bar);
        vkcheck(vkEndCommandBuffer(cmd), "end readback");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkResetFences(device, 1, &fence);
        vkcheck(vkQueueSubmit(queue, 1, &si, fence), "submit readback");
        vkcheck(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait readback");
        void* p = nullptr;
        vkMapMemory(device, m, 0, total, 0, &p);
        y.assign((uint8_t*)p, (uint8_t*)p + ys);
        uv.assign((uint8_t*)p + ys, (uint8_t*)p + total);
        vkUnmapMemory(device, m);
        vkDestroyBuffer(device, b, nullptr);
        vkFreeMemory(device, m, nullptr);
    }
    void destroy(Image& x) {
        vkDestroyImageView(device, x.y, nullptr);
        vkDestroyImageView(device, x.uv, nullptr);
        vkDestroyImage(device, x.image, nullptr);
        vkFreeMemory(device, x.mem, nullptr);
    }
    ~Ctx() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s input.nxv output.nv12 [--compact]\n", argv[0]);
        return 2;
    }
    try {
        auto bytes = file_bytes(argv[1]);
        Ctx c;
        c.init();
        nxvc_vkd_create_info ci;
        nxvc_vk_decoder_create_info_default(&ci);
        ci.instance = c.instance;
        ci.physical_device = c.physical;
        ci.device = c.device;
        ci.queue = c.queue;
        ci.queue_family = c.family;
        ci.output_format = NXVC_VKD_OUT_YCBCR420;
        ci.flags = NXVC_VKD_FLAG_INDEPENDENT_TILES;
        if (argc > 3 && std::string(argv[3]) == "--compact")
            ci.flags |= NXVC_VKD_FLAG_COMPACT_CENTRE;
        nxvc_vk_decoder* d = nullptr;
        if (nxvc_vk_decoder_create(&ci, &d) != NXVC_VKD_OK)
            throw std::runtime_error(nxvc_vk_decoder_last_create_error());
        size_t head = 0;
        if (nxvc_vk_decoder_parse_stream_header(d, bytes.data(), bytes.size(), &head) !=
            NXVC_VKD_OK)
            throw std::runtime_error(nxvc_vk_decoder_last_error(d));
        nxvc_vkd_stream_info si{};
        if (nxvc_vk_decoder_stream_info(d, &si) != NXVC_VKD_OK || si.chroma != 0 ||
            si.color_transform != 0)
            throw std::runtime_error("input is not independent YCbCr420");
        nxvc_vkd_images output{};
        if (nxvc_vk_decoder_images(d, &output) != NXVC_VKD_OK || output.count != 2)
            throw std::runtime_error("missing output geometry");
        uint32_t ow = output.width[0], oh = output.height[0];
        Image imgs[2] = {c.make(ow, oh), c.make(ow, oh)};
        for (auto& im : imgs) {
            nxvc_vkd_output_images oi{{im.image, im.image},
                                      {im.y, im.uv},
                                      {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT},
                                      {ow, ow / 2},
                                      {oh, oh / 2},
                                      VK_IMAGE_LAYOUT_UNDEFINED};
            if (nxvc_vk_decoder_set_borrowed_output(d, &oi) != NXVC_VKD_OK)
                throw std::runtime_error(nxvc_vk_decoder_last_error(d));
            break;
        }
        nxvc_vkd_output_images bad{{imgs[0].image, imgs[0].image},
                                   {imgs[0].y, imgs[0].uv},
                                   {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT},
                                   {ow + 1, ow / 2},
                                   {oh, oh / 2},
                                   VK_IMAGE_LAYOUT_UNDEFINED};
        if (nxvc_vk_decoder_set_borrowed_output(d, &bad) == NXVC_VKD_OK)
            throw std::runtime_error("invalid dimensions accepted");
        if (nxvc_vk_decoder_set_borrowed_output(d, nullptr) != NXVC_VKD_OK)
            throw std::runtime_error("NULL restore failed");
        std::ofstream out(argv[2], std::ios::binary);
        size_t off = head;
        for (int f = 0; f < 3; f++) {
            if (off >= bytes.size())
                throw std::runtime_error("input has fewer than three frames");
            auto& im = imgs[f & 1];
            nxvc_vkd_output_images oi{{im.image, im.image},
                                      {im.y, im.uv},
                                      {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT},
                                      {ow, ow / 2},
                                      {oh, oh / 2},
                                      f < 2 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL};
            if (nxvc_vk_decoder_set_borrowed_output(d, &oi) != NXVC_VKD_OK)
                throw std::runtime_error(nxvc_vk_decoder_last_error(d));
            size_t used = 0;
            if (nxvc_vk_decode_frame_ex(d, bytes.data() + off, bytes.size() - off,
                                        f == 2 ? NXVC_VKD_SUBMIT_ASYNC : 0, &used) != NXVC_VKD_OK)
                throw std::runtime_error(nxvc_vk_decoder_last_error(d));
            if (!used)
                throw std::runtime_error("decoder consumed zero bytes");
            if (f == 2) {
                if (nxvc_vk_decoder_set_borrowed_output(d, nullptr) != NXVC_VKD_OK)
                    throw std::runtime_error("async NULL restore failed");
                nxvc_vkd_images restored{};
                if (nxvc_vk_decoder_images(d, &restored) != NXVC_VKD_OK ||
                    restored.image[0] == imgs[0].image || restored.image[0] == imgs[1].image)
                    throw std::runtime_error("NULL did not restore owned images");
            }
            off += used;
            std::vector<uint8_t> y, uv;
            c.read(im, ow, oh, y, uv);
            out.write((char*)y.data(), y.size());
            out.write((char*)uv.data(), uv.size());
            if (!out)
                throw std::runtime_error("output write failed");
        }
        nxvc_vk_decoder_set_borrowed_output(d, nullptr);
        nxvc_vk_decoder_destroy(d);
        c.destroy(imgs[0]);
        c.destroy(imgs[1]);
        std::fprintf(stderr,
                     "borrowed-output: PASS: two targets, 3 frames, async NULL restore, invalid "
                     "geometry; NV12 %ux%u\n",
                     ow, oh);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "borrowed-output: %s\n", e.what());
        return 1;
    }
}
