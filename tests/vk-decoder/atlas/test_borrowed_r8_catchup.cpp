// Exact pixel A/B for the decoder-owned and caller-owned R8 atlas targets.
// This intentionally uses the real decoder and a recorded ATLAS stream.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nxvc/nxvc_vk.h>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

struct V {
    VkInstance i{};
    VkPhysicalDevice p{};
    VkDevice d{};
    VkQueue q{};
    uint32_t qf{};
    VkPhysicalDeviceMemoryProperties mp{};
};
static uint32_t mt(const V& v, uint32_t bits, VkMemoryPropertyFlags f) {
    for (uint32_t i = 0; i < v.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (v.mp.memoryTypes[i].propertyFlags & f) == f)
            return i;
    return UINT32_MAX;
}
static bool ok(VkResult r, const char* n) {
    if (r) {
        std::fprintf(stderr, "%s: %d\n", n, (int)r);
        return false;
    }
    return true;
}
struct I {
    VkImage x{};
    VkImageView v{};
    VkDeviceMemory m{};
    uint32_t w{}, h{};
};
static bool image(V& c, I& o, VkFormat f, uint32_t w, uint32_t h) {
    o.w = w;
    o.h = h;
    VkImageCreateInfo a{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    a.imageType = VK_IMAGE_TYPE_2D;
    a.format = f;
    a.extent = {w, h, 1};
    a.mipLevels = a.arrayLayers = 1;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.tiling = VK_IMAGE_TILING_OPTIMAL;
    a.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!ok(vkCreateImage(c.d, &a, nullptr, &o.x), "vkCreateImage"))
        return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(c.d, o.x, &r);
    VkMemoryAllocateInfo al{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    al.allocationSize = r.size;
    al.memoryTypeIndex = mt(c, r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (al.memoryTypeIndex == UINT32_MAX ||
        !ok(vkAllocateMemory(c.d, &al, nullptr, &o.m), "vkAllocateMemory") ||
        !ok(vkBindImageMemory(c.d, o.x, o.m, 0), "vkBindImageMemory"))
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = o.x;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return ok(vkCreateImageView(c.d, &vi, nullptr, &o.v), "vkCreateImageView");
}
static bool context(V& c) {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    if (!ok(vkCreateInstance(&ii, nullptr, &c.i), "vkCreateInstance"))
        return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c.i, &n, nullptr);
    std::vector<VkPhysicalDevice> x(n);
    vkEnumeratePhysicalDevices(c.i, &n, x.data());
    for (auto p : x) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++)
            if (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                c.p = p;
                c.qf = q;
                break;
            }
        if (c.p)
            break;
    }
    if (!c.p)
        return false;
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(c.p, &supported);
    VkPhysicalDeviceFeatures2 supported2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDevice16BitStorageFeatures supported16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    supported2.pNext = &supported16;
    vkGetPhysicalDeviceFeatures2(c.p, &supported2);
    if (!supported.shaderInt16 || !supported.shaderStorageImageExtendedFormats ||
        !supported16.storageBuffer16BitAccess)
        return false;
    float pr = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = c.qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &pr;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt16 = VK_TRUE;
    enabled.shaderStorageImageExtendedFormats = VK_TRUE;
    VkPhysicalDevice16BitStorageFeatures enabled16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    enabled16.storageBuffer16BitAccess = VK_TRUE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &enabled16;
    di.pEnabledFeatures = &enabled;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    if (!ok(vkCreateDevice(c.p, &di, nullptr, &c.d), "vkCreateDevice"))
        return false;
    vkGetDeviceQueue(c.d, c.qf, 0, &c.q);
    vkGetPhysicalDeviceMemoryProperties(c.p, &c.mp);
    return true;
}
static bool readimg(V& c, I& i, uint32_t bpp, std::vector<uint8_t>& out,
                    bool leave_readonly = false) {
    VkDeviceSize n = (VkDeviceSize)i.w * i.h * bpp;
    VkBuffer b;
    VkDeviceMemory m;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!ok(vkCreateBuffer(c.d, &bi, nullptr, &b), "vkCreateBuffer"))
        return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(c.d, b, &r);
    VkMemoryAllocateInfo al{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    al.allocationSize = r.size;
    al.memoryTypeIndex =
        mt(c, r.memoryTypeBits,
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!ok(vkAllocateMemory(c.d, &al, nullptr, &m), "vkAllocateMemory") ||
        !ok(vkBindBufferMemory(c.d, b, m, 0), "vkBindBufferMemory"))
        return false;
    VkCommandPool cp;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = c.qf;
    if (!ok(vkCreateCommandPool(c.d, &pi, nullptr, &cp), "vkCreateCommandPool"))
        return false;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = cp;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    vkAllocateCommandBuffers(c.d, &ca, &cb);
    VkCommandBufferBeginInfo be{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    be.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &be);
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.image = i.x;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ib);
    VkBufferImageCopy cpv{};
    cpv.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cpv.imageExtent = {i.w, i.h, 1};
    vkCmdCopyImageToBuffer(cb, i.x, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &cpv);
    VkImageMemoryBarrier back{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = leave_readonly ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = leave_readonly ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    back.image = i.x;
    back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &back);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(c.q);
    void* p;
    vkMapMemory(c.d, m, 0, n, 0, &p);
    out.assign((uint8_t*)p, (uint8_t*)p + n);
    vkUnmapMemory(c.d, m);
    vkDestroyCommandPool(c.d, cp, nullptr);
    vkDestroyBuffer(c.d, b, nullptr);
    vkFreeMemory(c.d, m, nullptr);
    return true;
}
static bool file(const char* path, std::vector<uint8_t>& x) {
    std::ifstream f(path, std::ios::binary);
    return !!f && (x.assign(std::istreambuf_iterator<char>(f), {}), !x.empty());
}
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : std::getenv("NXVC_TEST_ATLAS_VIEW_INPUT");
    if (!path) {
        std::fprintf(stderr, "SKIP: fixture argument required\n");
        return 77;
    }
    std::vector<uint8_t> stream;
    if (!file(path, stream)) {
        std::fprintf(stderr, "SKIP: fixture unreadable\n");
        return 77;
    }
    V c;
    if (!context(c))
        return 77;
    nxvc_vkd_create_info ci;
    nxvc_vk_decoder_create_info_default(&ci);
    ci.instance = c.i;
    ci.physical_device = c.p;
    ci.device = c.d;
    ci.queue = c.q;
    ci.queue_family = c.qf;
    ci.output_format = NXVC_VKD_OUT_AUTO;
    nxvc_vk_decoder *owned = nullptr, *borrowed = nullptr;
    if (nxvc_vk_decoder_create(&ci, &owned) || nxvc_vk_decoder_create(&ci, &borrowed))
        return 1;
    size_t header = 0;
    if (nxvc_vk_decoder_parse_stream_header(owned, stream.data(), stream.size(), &header) ||
        nxvc_vk_decoder_parse_stream_header(borrowed, stream.data(), stream.size(), &header))
        return 1;
    if (nxvc_vk_decoder_set_atlas_borrowed_target(borrowed, nullptr) != NXVC_VKD_OK) {
        std::fprintf(stderr, "FAIL: NULL clear before view selection\n");
        return 1;
    }
    if (nxvc_vk_decoder_set_atlas_view(owned, NXVC_VKD_ATLAS_VIEW_R8) ||
        nxvc_vk_decoder_set_atlas_view(borrowed, NXVC_VKD_ATLAS_VIEW_R8))
        return 77;
    uint32_t yw, yh, cw, ch;
    nxvc_vk_decoder_plane_size(owned, 0, &yw, &yh);
    nxvc_vk_decoder_plane_size(owned, 1, &cw, &ch);
    I y{}, chroma{};
    if (!image(c, y, VK_FORMAT_R8_UNORM, yw, yh) || !image(c, chroma, VK_FORMAT_R8G8_UNORM, cw, ch))
        return 1;
    I y_extra[2]{}, c_extra[2]{};
    for (int i = 0; i < 2; ++i)
        if (!image(c, y_extra[i], VK_FORMAT_R8_UNORM, yw, yh) ||
            !image(c, c_extra[i], VK_FORMAT_R8G8_UNORM, cw, ch))
            return 1;
    nxvc_vkd_atlas_images target{};
    target.image[0] = y.x;
    target.view[0] = y.v;
    target.format[0] = VK_FORMAT_R8_UNORM;
    target.width[0] = yw;
    target.height[0] = yh;
    target.image[1] = chroma.x;
    target.view[1] = chroma.v;
    target.format[1] = VK_FORMAT_R8G8_UNORM;
    target.width[1] = cw;
    target.height[1] = ch;
    nxvc_vkd_atlas_images targets[3] = {target, target, target};
    for (int i = 0; i < 2; ++i) {
        targets[i + 1].image[0] = y_extra[i].x;
        targets[i + 1].view[0] = y_extra[i].v;
        targets[i + 1].image[1] = c_extra[i].x;
        targets[i + 1].view[1] = c_extra[i].v;
    }
    nxvc_vkd_atlas_images malformed = target;
    malformed.width[1]++;
    if (nxvc_vk_decoder_set_atlas_borrowed_target(borrowed, &malformed) != NXVC_VKD_ERR_ARG) {
        std::fprintf(stderr, "FAIL: malformed borrowed target was accepted\n");
        return 1;
    }
    if (nxvc_vk_decoder_set_atlas_borrowed_target_generation(borrowed, &target, 1, VK_IMAGE_LAYOUT_UNDEFINED) != NXVC_VKD_OK) {
        std::fprintf(stderr, "FAIL: supported borrowed target rejected: %s\n",
                     nxvc_vk_decoder_last_error(borrowed));
        return 1;
    }
    malformed = target;
    malformed.format[0] = VK_FORMAT_R16_UNORM;
    nxvc_vkd_atlas_images still_bound{};
    if (nxvc_vk_decoder_set_atlas_borrowed_target(borrowed, &malformed) != NXVC_VKD_ERR_ARG ||
        nxvc_vk_decoder_atlas_images(borrowed, &still_bound) != NXVC_VKD_OK ||
        still_bound.image[0] != target.image[0] || still_bound.image[1] != target.image[1]) {
        std::fprintf(stderr, "FAIL: rejected target disturbed active binding\n");
        return 1;
    }
    size_t off = header;
    std::vector<uint8_t> oy, oc, by, bc;
    uint32_t w, hh, bs;
    auto rd = [&](nxvc_vk_decoder* d, int p, std::vector<uint8_t>& z) {
        if (nxvc_vk_decoder_atlas_view_read(d, p, nullptr, 0, &w, &hh, &bs) != NXVC_VKD_OK)
            return false;
        z.resize((size_t)w * hh * bs);
        return nxvc_vk_decoder_atlas_view_read(d, p, z.data(), z.size(), &w, &hh, &bs) ==
               NXVC_VKD_OK;
    };
    VkImageLayout target_layouts[3] = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};
    int processed = 0;
    bool changing_pixels = false;
    std::vector<uint8_t> previous_owned_y;
    for (int frame = 0; frame < 8; ++frame) {
        const int ti = frame % 3;
        if (nxvc_vk_decoder_set_atlas_borrowed_target_generation(borrowed, &targets[ti], ti + 1,
                target_layouts[ti]) !=
                NXVC_VKD_OK) {
            std::fprintf(stderr, "FAIL: target switch %d\n", ti);
            return 1;
        }
        size_t used = 0;
        if (nxvc_vk_decode_frame(owned, stream.data() + off, stream.size() - off, &used) !=
            NXVC_VKD_OK)
            return 1;
        size_t used2 = 0;
        setenv("NXVC_VKD_ATLAS_VIEW_DIRTY", "1", 1);
        if (nxvc_vk_decode_frame(borrowed, stream.data() + off, stream.size() - off, &used2) !=
            NXVC_VKD_OK)
            return 1;
        unsetenv("NXVC_VKD_ATLAS_VIEW_DIRTY");
        if (used != used2) {
            std::fprintf(stderr, "FAIL: frame sizes differ\n");
            return 1;
        }
        off += used;
        if (!rd(owned, 0, oy) || !rd(owned, 1, oc) ||
            !readimg(c, ti == 0 ? y : y_extra[ti - 1], 1, by, (frame & 1) != 0) ||
            !readimg(c, ti == 0 ? chroma : c_extra[ti - 1], 2, bc, (frame & 1) != 0) || oy != by || oc != bc) {
            std::fprintf(stderr, "FAIL: borrowed pixel mismatch at frame %d\n", frame);
            for (size_t k = 0; k < oy.size() && k < by.size(); ++k)
                if (oy[k] != by[k]) { std::fprintf(stderr, "first Y mismatch byte=%zu tile=%zu (%ux%u) ref=%u got=%u\n", k, (k / (64u*64u)), (unsigned)(k % yw), (unsigned)(k / yw), oy[k], by[k]); break; }
            return 1;
        }
        target_layouts[ti] = (frame & 1) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        if (!previous_owned_y.empty() && previous_owned_y != oy)
            changing_pixels = true;
        previous_owned_y = oy;
        if (frame == 3) {
            nxvc_vkd_stats s{};
            if (nxvc_vk_decoder_stats(borrowed, &s) != NXVC_VKD_OK || s.frame_mode != 1u) {
                std::fprintf(stderr, "FAIL: frame 3 was not an ATLAS frame for partial reuse\n");
                return 1;
            }
        }
        ++processed;
    }
    if (nxvc_vk_decoder_set_atlas_borrowed_target(borrowed, nullptr) != NXVC_VKD_OK)
        return 1;
    nxvc_vkd_atlas_images restored{};
    if (nxvc_vk_decoder_atlas_images(borrowed, &restored) != NXVC_VKD_OK ||
        restored.image[0] == y.x) {
        std::fprintf(stderr, "FAIL: clear did not restore owned target\n");
        return 1;
    }
    nxvc_vk_decoder_destroy(owned);
    nxvc_vk_decoder_destroy(borrowed);
    vkDeviceWaitIdle(c.d);
    vkDestroyImageView(c.d, y.v, nullptr);
    vkDestroyImage(c.d, y.x, nullptr);
    vkFreeMemory(c.d, y.m, nullptr);
    vkDestroyImageView(c.d, chroma.v, nullptr);
    vkDestroyImage(c.d, chroma.x, nullptr);
    vkFreeMemory(c.d, chroma.m, nullptr);
    for (int i = 0; i < 2; ++i) {
        vkDestroyImageView(c.d, c_extra[i].v, nullptr);
        vkDestroyImage(c.d, c_extra[i].x, nullptr);
        vkFreeMemory(c.d, c_extra[i].m, nullptr);
        vkDestroyImageView(c.d, y_extra[i].v, nullptr);
        vkDestroyImage(c.d, y_extra[i].x, nullptr);
        vkFreeMemory(c.d, y_extra[i].m, nullptr);
    }
    vkDestroyDevice(c.d, nullptr);
    vkDestroyInstance(c.i, nullptr);
    if (processed < 8) return 1;
    if (!changing_pixels) {
        std::fprintf(stderr, "FAIL: temporal fixture produced identical owned pixels\n");
        return 1;
    }
    std::printf("PASS borrowed R8 exact pixels for %d frames across 3 targets Y=%zu C=%zu (%ux%u/%ux%u)\n", processed, by.size(),
                bc.size(), yw, yh, cw, ch);
    return 0;
}
