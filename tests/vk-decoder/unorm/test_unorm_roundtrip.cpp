// vk.decoder.unorm_roundtrip -- is an 8-bit UNORM storage image exact?
//
// Pass B can write its 8-bit output through UNORM images instead of integer
// ones (specialization constant 5, `kUnormStore`), because an integer storage
// image costs about 3x a normalised one on Adreno.  That substitution is only
// legal if it changes no pixel, and ADR 0023 does not let "8-bit UNORM
// probably round-trips" stand in for a measurement.  This is the measurement,
// and it is deliberately independent of the decoder: a driver that fails here
// must not be given the UNORM path whatever the conformance sweep says.
//
// Two claims are checked, over all 256 values in every channel of
// R8G8B8A8_UNORM, R8_UNORM and R8G8_UNORM:
//
//   store side -- the kernel stores v/255.0 and the byte in device memory,
//                 copied out with vkCmdCopyImageToBuffer, is exactly v.  This
//                 is what the decoder's readback and any sampler downstream
//                 of the image actually see.
//   load  side -- imageLoad of that texel, re-encoded, is exactly v.  This is
//                 what the Phase 2 reference-ring reads will see.
//
// Exit 0 exact, 1 not exact, 77 no usable Vulkan ICD (a ctest skip).

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "unorm_roundtrip.spv.h"

namespace {

const char *kFmtName[3] = {"R8G8B8A8_UNORM", "R8_UNORM", "R8G8_UNORM"};

struct Ctx {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkPhysicalDeviceMemoryProperties mem{};
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]{};
};

#define VKOK(expr)                                                      \
    do {                                                                \
        VkResult r_ = (expr);                                           \
        if (r_ != VK_SUCCESS) {                                         \
            std::fprintf(stderr, "%s failed: VkResult %d\n", #expr,     \
                         (int)r_);                                      \
            return false;                                               \
        }                                                               \
    } while (0)

uint32_t mem_type(const Ctx &c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (c.mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

bool lower_contains(const char *hay, const char *needle) {
    std::string a(hay), b(needle);
    for (auto &ch : a) ch = (char)tolower((unsigned char)ch);
    for (auto &ch : b) ch = (char)tolower((unsigned char)ch);
    return a.find(b) != std::string::npos;
}

// Returns false with no message when there is simply no device to run on;
// `skip` distinguishes that from a real failure.
bool make_ctx(Ctx &c, const char *want, bool &skip) {
    skip = true;
    uint32_t api = VK_API_VERSION_1_0;
    if (auto fp = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))
        fp(&api);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "nxvc unorm roundtrip";
    app.apiVersion = api;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    if (vkCreateInstance(&ii, nullptr, &c.inst) != VK_SUCCESS) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> ds(n);
    if (n) vkEnumeratePhysicalDevices(c.inst, &n, ds.data());
    for (VkPhysicalDevice pd : ds) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        if (want && *want && !lower_contains(p.deviceName, want)) continue;
        // Every format this test writes has to be a storage image, or the
        // question does not arise on this device.
        bool ok = true;
        for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM,
                           VK_FORMAT_R8G8_UNORM}) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
                ok = false;
        }
        if (!ok) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; ++q)
            if (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                c.phys = pd;
                c.qfam = q;
                std::memcpy(c.name, p.deviceName, sizeof c.name);
                break;
            }
        if (c.phys) break;
    }
    if (!c.phys) return false;
    skip = false;
    float pr = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = c.qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &pr;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VKOK(vkCreateDevice(c.phys, &di, nullptr, &c.dev));
    vkGetDeviceQueue(c.dev, c.qfam, 0, &c.queue);
    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem);
    return true;
}

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, texel = 0;
};

bool make_image(Ctx &c, Image &o, VkFormat f, uint32_t w, uint32_t h,
                uint32_t texel) {
    o.w = w;
    o.h = h;
    o.texel = texel;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(c.dev, &ci, nullptr, &o.img));
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(c.dev, o.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex =
        mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKOK(vkAllocateMemory(c.dev, &ai, nullptr, &o.mem));
    VKOK(vkBindImageMemory(c.dev, o.img, o.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = o.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKOK(vkCreateImageView(c.dev, &vi, nullptr, &o.view));
    return true;
}

int run(const char *want, bool verbose) {
    Ctx c;
    bool skip = false;
    if (!make_ctx(c, want, skip)) {
        if (skip) {
            std::printf("SKIP: no Vulkan device with 8-bit UNORM storage "
                        "images%s%s\n",
                        (want && *want) ? " matching " : "",
                        (want && *want) ? want : "");
            return 77;
        }
        return 1;
    }
    std::printf("-- device: %s\n", c.name);

    // Column x holds value x; the rows separate the channels.
    Image rgba, r8, rg8;
    if (!make_image(c, rgba, VK_FORMAT_R8G8B8A8_UNORM, 256, 4, 4) ||
        !make_image(c, r8, VK_FORMAT_R8_UNORM, 256, 1, 1) ||
        !make_image(c, rg8, VK_FORMAT_R8G8_UNORM, 256, 2, 2))
        return 1;

    // One host-visible buffer: 5 result uints, then the three images' bytes.
    const VkDeviceSize kResultBytes = 64;
    const VkDeviceSize offRgba = kResultBytes;
    const VkDeviceSize offR8 = offRgba + 256 * 4 * 4;
    const VkDeviceSize offRg8 = offR8 + 256 * 1 * 1;
    const VkDeviceSize total = offRg8 + 256 * 2 * 2;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = total;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VKOK(vkCreateBuffer(c.dev, &bi, nullptr, &buf));
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(c.dev, buf, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex =
            mem_type(c, mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKOK(vkAllocateMemory(c.dev, &ai, nullptr, &bmem));
        VKOK(vkBindBufferMemory(c.dev, buf, bmem, 0));
    }
    void *mapped = nullptr;
    VKOK(vkMapMemory(c.dev, bmem, 0, total, 0, &mapped));
    std::memset(mapped, 0, (size_t)total);

    // ---- descriptors, layouts, both pipelines
    VkDescriptorSetLayoutBinding lb[4]{};
    for (int i = 0; i < 4; ++i) {
        lb[i].binding = (uint32_t)i;
        lb[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                     : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo dli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 4;
    dli.pBindings = lb;
    VKOK(vkCreateDescriptorSetLayout(c.dev, &dli, nullptr, &dsl));
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo pli{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    VKOK(vkCreatePipelineLayout(c.dev, &pli, nullptr, &pl));

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorPoolCreateInfo dpi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    VKOK(vkCreateDescriptorPool(c.dev, &dpi, nullptr, &dpool));
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VKOK(vkAllocateDescriptorSets(c.dev, &dai, &dset));
    {
        VkDescriptorImageInfo ii[3] = {
            {VK_NULL_HANDLE, rgba.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, r8.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, rg8.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkDescriptorBufferInfo bi{buf, 0, kResultBytes};
        VkWriteDescriptorSet w[4]{};
        for (int i = 0; i < 4; ++i) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = dset;
            w[i].dstBinding = (uint32_t)i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i < 3)
                w[i].pImageInfo = &ii[i];
            else
                w[i].pBufferInfo = &bi;
        }
        vkUpdateDescriptorSets(c.dev, 4, w, 0, nullptr);
    }

    VkShaderModule sm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(unorm_roundtrip_spv);
    smi.pCode = unorm_roundtrip_spv;
    VKOK(vkCreateShaderModule(c.dev, &smi, nullptr, &sm));
    VkPipeline pipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (int p = 0; p < 2; ++p) {
        int32_t v = p;
        VkSpecializationMapEntry me{0, 0, 4};
        VkSpecializationInfo si{1, &me, sizeof v, &v};
        VkComputePipelineCreateInfo ci{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = sm;
        ci.stage.pName = "main";
        ci.stage.pSpecializationInfo = &si;
        ci.layout = pl;
        VKOK(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &pipe[p]));
    }

    // ---- one command buffer: transition, write, read back, copy out
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = c.qfam;
    VKOK(vkCreateCommandPool(c.dev, &cpi, nullptr, &cpool));
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cpool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKOK(vkAllocateCommandBuffers(c.dev, &cai, &cmd));
    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKOK(vkBeginCommandBuffer(cmd, &bbi));
    for (const Image *im : {&rgba, &r8, &rg8}) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        b.image = im->img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
                            &dset, 0, nullptr);
    auto barrier = [&](VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = src;
        mb.dstAccessMask = dst;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe[0]);
    vkCmdDispatch(cmd, 1, 1, 1);
    barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe[1]);
    vkCmdDispatch(cmd, 1, 1, 1);
    barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    auto grab = [&](const Image &im, VkDeviceSize off) {
        VkBufferImageCopy r{};
        r.bufferOffset = off;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {im.w, im.h, 1};
        vkCmdCopyImageToBuffer(cmd, im.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1,
                               &r);
    };
    grab(rgba, offRgba);
    grab(r8, offR8);
    grab(rg8, offRg8);
    barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
    VKOK(vkEndCommandBuffer(cmd));

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKOK(vkCreateFence(c.dev, &fi, nullptr, &fence));
    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    su.commandBufferCount = 1;
    su.pCommandBuffers = &cmd;
    VKOK(vkQueueSubmit(c.queue, 1, &su, fence));
    VKOK(vkWaitForFences(c.dev, 1, &fence, VK_TRUE, UINT64_MAX));

    // ---- the two verdicts
    const uint32_t *res = (const uint32_t *)mapped;
    const uint8_t *bytes = (const uint8_t *)mapped;
    int bad = 0;

    // store side, on the host, over the raw image bytes
    auto check_bytes = [&](const char *what, VkDeviceSize off, uint32_t rows,
                           uint32_t chans) {
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t v = 0; v < 256; ++v)
                for (uint32_t ch = 0; ch < chans; ++ch) {
                    const uint32_t got =
                        bytes[off + (row * 256 + v) * chans + ch];
                    const uint32_t want_v = (ch == row) ? v : 255 - v;
                    if (got != want_v) {
                        if (bad < 8)
                            std::printf(
                                "   %s store: value %u channel %u came back "
                                "as %u\n",
                                what, want_v, ch, got);
                        ++bad;
                    }
                }
    };
    check_bytes(kFmtName[0], offRgba, 4, 4);
    // R8_UNORM has one channel and one row, so row 0 is channel 0.
    check_bytes(kFmtName[1], offR8, 1, 1);
    check_bytes(kFmtName[2], offRg8, 2, 2);
    const int store_bad = bad;

    // load side, counted by the kernel itself
    const uint32_t load_bad = res[0];
    if (load_bad)
        std::printf("   %s load: wrote %u channel %u, read back %u\n",
                    kFmtName[res[1] < 3 ? res[1] : 0], res[3], res[2], res[4]);

    if (verbose || store_bad || load_bad)
        std::printf("-- 768 store checks per value, 256 values: %d store "
                    "mismatch(es), %u load mismatch(es)\n",
                    store_bad, load_bad);
    std::printf("%s: 8-bit UNORM storage images are %s on %s\n",
                (store_bad || load_bad) ? "FAIL" : "ok",
                (store_bad || load_bad) ? "NOT exact" : "exact", c.name);

    vkDestroyFence(c.dev, fence, nullptr);
    vkDestroyCommandPool(c.dev, cpool, nullptr);
    for (VkPipeline p : pipe) vkDestroyPipeline(c.dev, p, nullptr);
    vkDestroyShaderModule(c.dev, sm, nullptr);
    vkDestroyDescriptorPool(c.dev, dpool, nullptr);
    vkDestroyPipelineLayout(c.dev, pl, nullptr);
    vkDestroyDescriptorSetLayout(c.dev, dsl, nullptr);
    vkUnmapMemory(c.dev, bmem);
    vkDestroyBuffer(c.dev, buf, nullptr);
    vkFreeMemory(c.dev, bmem, nullptr);
    for (Image *im : {&rgba, &r8, &rg8}) {
        vkDestroyImageView(c.dev, im->view, nullptr);
        vkDestroyImage(c.dev, im->img, nullptr);
        vkFreeMemory(c.dev, im->mem, nullptr);
    }
    vkDestroyDevice(c.dev, nullptr);
    vkDestroyInstance(c.inst, nullptr);
    return (store_bad || load_bad) ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    const char *want = std::getenv("NXVC_VKD_DEVICE");
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--device") && i + 1 < argc)
            want = argv[++i];
        else if (!std::strcmp(argv[i], "--verbose"))
            verbose = true;
        else {
            std::fprintf(stderr, "usage: %s [--device SUBSTR] [--verbose]\n",
                         argv[0]);
            return 2;
        }
    }
    return run(want, verbose);
}
