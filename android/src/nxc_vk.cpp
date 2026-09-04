#include "nxc_vk.h"

#include <android/native_window.h>

#include <algorithm>
#include <cstring>

#include "hud.spv.h"
#include "present.spv.h"

namespace nxc {
namespace {

struct PresentPush { uint32_t img_w, img_h; };
struct HudPush {
    uint32_t img_w, img_h;
    uint32_t cols, rows, scale, origin;
};

const char* vkstr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        default: return "VkResult";
    }
}

#define VKCHK(expr)                                                    \
    do {                                                               \
        VkResult _r = (expr);                                          \
        if (_r != VK_SUCCESS) {                                        \
            log_err("%s failed: %s (%d)", #expr, vkstr(_r), int(_r));  \
            return false;                                              \
        }                                                              \
    } while (0)

void image_barrier(VkCommandBuffer cb, VkImage img,
                   VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

// ---------------------------------------------------------------- init

bool Renderer::init(ANativeWindow* window, const AppConfig& cfg) {
    cfg_ = cfg;
    window_ = window;
    if (!create_instance()) return false;
    if (!create_surface(window)) return false;
    if (!pick_device()) return false;
    if (!create_device()) return false;
    if (!create_swapchain()) return false;
    if (!create_images()) return false;
    if (!create_buffers()) return false;
    if (!create_descriptors()) return false;
    if (!create_pipelines()) return false;
    if (!create_frames()) return false;

    decoder_ = create_placeholder_decoder();
    DecoderCreateInfo dci;
    dci.device = device_;
    dci.physical_device = phys_;
    dci.stream = cfg_.stream;
    dci.subgroup_size = gpu_.subgroup_size;
    if (!decoder_->create(dci)) {
        log_err("decoder creation failed");
        return false;
    }
    update_descriptors();
    log_info("renderer ready: %ux%u on %s", extent_.width, extent_.height, gpu_.device_name);
    return true;
}

bool Renderer::create_instance() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "nxwarp-client";
    ai.applicationVersion = 1;
    ai.pEngineName = "nxwarp";
    ai.engineVersion = 1;
    // Vulkan 1.1: PAPER 3.2.6 needs VK_EXT_subgroup_size_control where offered
    // and subgroup properties, both reachable from a 1.1 core instance.
    ai.apiVersion = VK_API_VERSION_1_1;

    const char* exts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VKCHK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

bool Renderer::create_surface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = window;
    VKCHK(vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &surface_));
    return true;
}

bool Renderer::pick_device() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (n == 0) { log_err("no Vulkan physical devices"); return false; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());

    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t i = 0; i < qn; ++i) {
            // One queue that can do compute, transfer and present. Every mobile
            // part has such a family; splitting them would buy nothing here.
            if (!(qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if (!present) continue;
            phys_ = d;
            queue_family_ = i;
            break;
        }
        if (phys_) break;
    }
    if (!phys_) { log_err("no device with a compute+present queue family"); return false; }

    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sub;
    vkGetPhysicalDeviceProperties2(phys_, &p2);

    std::snprintf(gpu_.device_name, sizeof(gpu_.device_name), "%s", p2.properties.deviceName);
    gpu_.api_version = p2.properties.apiVersion;
    gpu_.driver_version = p2.properties.driverVersion;
    gpu_.vendor_id = p2.properties.vendorID;
    gpu_.subgroup_size = sub.subgroupSize;
    gpu_.timestamp_period_ns = p2.properties.limits.timestampPeriod;
    gpu_.timestamps_supported = p2.properties.limits.timestampComputeAndGraphics;

    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(phys_, &feats);
    gpu_.storage_extended_formats = feats.shaderStorageImageExtendedFormats == VK_TRUE;

    log_info("GPU %s, api %u.%u.%u, subgroupSize %u, timestampPeriod %.2f ns",
             gpu_.device_name, VK_VERSION_MAJOR(gpu_.api_version),
             VK_VERSION_MINOR(gpu_.api_version), VK_VERSION_PATCH(gpu_.api_version),
             gpu_.subgroup_size, gpu_.timestamp_period_ns);
    // PAPER 3.2.6 / 3.7: a subgroup below 8 is the hybrid path, not pure compute.
    if (gpu_.subgroup_size && gpu_.subgroup_size < kMinSubgroupSize) {
        log_warn("subgroup size %u < %u: PAPER 3.7 puts this part (Mali Bifrost class) "
                 "on hybrid decode, not the pure-compute path", gpu_.subgroup_size,
                 kMinSubgroupSize);
    }

    // The decoder writes R8_UNORM and R8G8_UNORM storage images, which is only
    // guaranteed with shaderStorageImageExtendedFormats. Check the formats too:
    // the feature bit and the per-format bit are separate promises.
    VkFormatProperties fp_r8{}, fp_rg8{};
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R8_UNORM, &fp_r8);
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R8G8_UNORM, &fp_rg8);
    const bool r8_store  = (fp_r8.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    const bool rg8_store = (fp_rg8.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    if (!gpu_.storage_extended_formats || !r8_store || !rg8_store) {
        log_err("device cannot write R8/R8G8 storage images "
                "(extendedFormats=%d r8=%d rg8=%d); the 2-plane 4:2:0 decoder "
                "output needs all three", int(gpu_.storage_extended_formats),
                int(r8_store), int(rg8_store));
        return false;
    }
    return true;
}

bool Renderer::create_device() {
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* exts[] = {"VK_KHR_swapchain"};
    VkPhysicalDeviceFeatures feats{};
    feats.shaderStorageImageExtendedFormats = VK_TRUE;   // R8 / R8G8 stores

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures = &feats;
    VKCHK(vkCreateDevice(phys_, &ci, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    VKCHK(vkCreateCommandPool(device_, &pci, nullptr, &pool_));
    return true;
}

bool Renderer::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VKCHK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));

    extent_ = caps.currentExtent;
    if (extent_.width == 0xffffffffu) {
        extent_.width = uint32_t(ANativeWindow_getWidth(window_));
        extent_.height = uint32_t(ANativeWindow_getHeight(window_));
    }
    extent_.width  = std::clamp(extent_.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    extent_.height = std::clamp(extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent_.width == 0 || extent_.height == 0) return false;

    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        log_err("swapchain images do not support TRANSFER_DST; the compute-then-blit "
                "path needs it");
        return false;
    }

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (const auto& f : fmts) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    sc_format_ = chosen.format;

    uint32_t want_images = caps.minImageCount + 1;
    if (caps.maxImageCount && want_images > caps.maxImageCount) want_images = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = want_images;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is always supported and is what a headset runtime would do anyway;
    // this shell is measuring the receive path, not the present path.
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    VKCHK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    sc_images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, sc_images_.data());

    sc_render_done_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHK(vkCreateSemaphore(device_, &si, nullptr, &sc_render_done_[i]));
    }
    log_info("swapchain %ux%u, %u images, format %d", extent_.width, extent_.height, n,
             int(sc_format_));
    return true;
}

void Renderer::destroy_swapchain() {
    if (!device_) return;
    for (VkSemaphore s : sc_render_done_)
        if (s) vkDestroySemaphore(device_, s, nullptr);
    sc_render_done_.clear();
    sc_images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------- memory

int Renderer::find_memory_type(uint32_t bits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return int(i);
    }
    return -1;
}

bool Renderer::alloc_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags props, VkBuffer* buf,
                            VkDeviceMemory* mem, void** mapped) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHK(vkCreateBuffer(device_, &bi, nullptr, buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, *buf, &req);
    const int type = find_memory_type(req.memoryTypeBits, props);
    if (type < 0) { log_err("no memory type for buffer"); return false; }

    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = uint32_t(type);
    VKCHK(vkAllocateMemory(device_, &mi, nullptr, mem));
    VKCHK(vkBindBufferMemory(device_, *buf, *mem, 0));
    if (mapped) VKCHK(vkMapMemory(device_, *mem, 0, size, 0, mapped));
    return true;
}

bool Renderer::alloc_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                           VkImage* img, VkDeviceMemory* mem, VkImageView* view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHK(vkCreateImage(device_, &ii, nullptr, img));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, *img, &req);
    const int type = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { log_err("no device-local memory type for image"); return false; }

    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = uint32_t(type);
    VKCHK(vkAllocateMemory(device_, &mi, nullptr, mem));
    VKCHK(vkBindImageMemory(device_, *img, *mem, 0));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKCHK(vkCreateImageView(device_, &vi, nullptr, view));
    return true;
}

bool Renderer::create_images() {
    // Luma at display resolution, chroma at half (4:2:0). Both are written as
    // storage images by the decoder and read as sampled images by the present
    // pass, so they carry both usages.
    chroma_extent_ = {std::max(1u, extent_.width / 2), std::max(1u, extent_.height / 2)};

    const VkImageUsageFlags plane_usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!alloc_image(extent_.width, extent_.height, VK_FORMAT_R8_UNORM, plane_usage,
                     &luma_img_, &luma_mem_, &luma_view_)) return false;
    if (!alloc_image(chroma_extent_.width, chroma_extent_.height, VK_FORMAT_R8G8_UNORM,
                     plane_usage, &chroma_img_, &chroma_mem_, &chroma_view_)) return false;
    if (!alloc_image(extent_.width, extent_.height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     &rgba_img_, &rgba_mem_, &rgba_view_)) return false;

    if (plane_sampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        // Linear on the chroma plane is what gives the 4:2:0 upsample for free.
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        VKCHK(vkCreateSampler(device_, &si, nullptr, &plane_sampler_));
    }
    images_initialised_ = false;
    return true;
}

void Renderer::destroy_images() {
    if (!device_) return;
    auto drop = [&](VkImage& i, VkDeviceMemory& m, VkImageView& v) {
        if (v) vkDestroyImageView(device_, v, nullptr);
        if (i) vkDestroyImage(device_, i, nullptr);
        if (m) vkFreeMemory(device_, m, nullptr);
        i = VK_NULL_HANDLE; m = VK_NULL_HANDLE; v = VK_NULL_HANDLE;
    };
    drop(luma_img_, luma_mem_, luma_view_);
    drop(chroma_img_, chroma_mem_, chroma_view_);
    drop(rgba_img_, rgba_mem_, rgba_view_);
    images_initialised_ = false;
}

bool Renderer::create_buffers() {
    const VkMemoryPropertyFlags host =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    meta_bytes_ = VkDeviceSize(cfg_.stream.tiles_per_frame()) * sizeof(uint32_t);
    if (!alloc_buffer(meta_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host,
                      &meta_buf_, &meta_mem_, &meta_map_)) return false;
    std::memset(meta_map_, 0, size_t(meta_bytes_));

    // Sized for the largest HUD the app builds; see nxc_app.cpp.
    hud_bytes_ = 128 * 48 * sizeof(uint32_t);
    if (!alloc_buffer(hud_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host,
                      &hud_buf_, &hud_mem_, &hud_map_)) return false;
    std::memset(hud_map_, 0xff, size_t(hud_bytes_));   // 0xffffffff = untouched

    const auto& rows = font_rows();
    font_bytes_ = VkDeviceSize(rows.size() * sizeof(uint32_t));
    if (!alloc_buffer(font_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host,
                      &font_buf_, &font_mem_, &font_map_)) return false;
    std::memcpy(font_map_, rows.data(), size_t(font_bytes_));
    return true;
}

bool Renderer::create_descriptors() {
    // Present set: sampler(luma), sampler(chroma), storage image(rgba).
    {
        VkDescriptorSetLayoutBinding b[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 3;
        ci.pBindings = b;
        VKCHK(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &present_layout_));
    }
    // HUD set: storage image(rgba), SSBO(text), SSBO(font).
    {
        VkDescriptorSetLayoutBinding b[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = (i == 0) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 3;
        ci.pBindings = b;
        VKCHK(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &hud_layout_));
    }

    VkDescriptorPoolSize ps[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = ps;
    VKCHK(vkCreateDescriptorPool(device_, &pci, nullptr, &desc_pool_));

    VkDescriptorSetLayout layouts[2] = {present_layout_, hud_layout_};
    VkDescriptorSet sets[2] = {};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = desc_pool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    VKCHK(vkAllocateDescriptorSets(device_, &ai, sets));
    present_set_ = sets[0];
    hud_set_ = sets[1];
    return true;
}

void Renderer::update_descriptors() {
    VkDescriptorImageInfo luma{plane_sampler_, luma_view_,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo chroma{plane_sampler_, chroma_view_,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo rgba{VK_NULL_HANDLE, rgba_view_, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo hud{hud_buf_, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo font{font_buf_, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet w[6] = {};
    for (auto& x : w) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = present_set_; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &luma;
    w[1].dstSet = present_set_; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &chroma;
    w[2].dstSet = present_set_; w[2].dstBinding = 2; w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo = &rgba;
    w[3].dstSet = hud_set_; w[3].dstBinding = 0; w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[3].pImageInfo = &rgba;
    w[4].dstSet = hud_set_; w[4].dstBinding = 1; w[4].descriptorCount = 1;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &hud;
    w[5].dstSet = hud_set_; w[5].dstBinding = 2; w[5].descriptorCount = 1;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[5].pBufferInfo = &font;
    vkUpdateDescriptorSets(device_, 6, w, 0, nullptr);
}

VkShaderModule Renderer::make_module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

bool Renderer::create_pipelines() {
    auto build = [&](VkDescriptorSetLayout set_layout, uint32_t push_bytes,
                     const uint32_t* spv, size_t spv_bytes,
                     VkPipelineLayout* out_layout, VkPipeline* out_pipe) -> bool {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set_layout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VKCHK(vkCreatePipelineLayout(device_, &plci, nullptr, out_layout));

        VkShaderModule sm = make_module(spv, spv_bytes);
        if (!sm) { log_err("shader module creation failed"); return false; }
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = sm;
        cpci.stage.pName = "main";
        cpci.layout = *out_layout;
        VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, out_pipe);
        vkDestroyShaderModule(device_, sm, nullptr);
        if (r != VK_SUCCESS) { log_err("compute pipeline: %s", vkstr(r)); return false; }
        return true;
    };

    if (!build(present_layout_, sizeof(PresentPush), present_spv, sizeof(present_spv),
               &present_pipe_layout_, &present_pipe_)) return false;
    if (!build(hud_layout_, sizeof(HudPush), hud_spv, sizeof(hud_spv),
               &hud_pipe_layout_, &hud_pipe_)) return false;
    return true;
}

bool Renderer::create_frames() {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKCHK(vkAllocateCommandBuffers(device_, &ai, &frames_[i].cb));

        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHK(vkCreateFence(device_, &fi, nullptr, &frames_[i].fence));

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHK(vkCreateSemaphore(device_, &si, nullptr, &frames_[i].image_available));

        if (gpu_.timestamps_supported) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            VKCHK(vkCreateQueryPool(device_, &qi, nullptr, &frames_[i].queries));
        }
    }
    return true;
}

// ---------------------------------------------------------------- frame

bool Renderer::render(const std::vector<uint32_t>& tile_meta, const TextCanvas& hud,
                      uint16_t frame_id) {
    if (!ready()) return false;
    Frame& f = frames_[frame_index_];

    vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);

    // Decode time of the frame that just finished (PAPER 4.9, "decode ms against
    // the governor target").
    if (f.queries_valid && f.queries) {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(device_, f.queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
            last_decode_us_ = uint32_t(double(ts[1] - ts[0]) * gpu_.timestamp_period_ns / 1000.0);
        }
    }

    uint32_t image_index = 0;
    VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                        f.image_available, VK_NULL_HANDLE, &image_index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        destroy_images();
        if (!create_swapchain() || !create_images()) return false;
        update_descriptors();
        return false;
    }
    if (ar != VK_SUCCESS) { log_err("acquire: %s", vkstr(ar)); return false; }

    vkResetFences(device_, 1, &f.fence);

    // Upload this frame's inputs. Both buffers are host coherent, so the write
    // is visible to the device without a flush.
    if (meta_map_ && !tile_meta.empty()) {
        const size_t n = std::min(size_t(meta_bytes_), tile_meta.size() * sizeof(uint32_t));
        std::memcpy(meta_map_, tile_meta.data(), n);
    }
    if (hud_map_) {
        const size_t n = std::min(size_t(hud_bytes_), hud.bytes());
        std::memcpy(hud_map_, hud.cells().data(), n);
    }

    VkCommandBuffer cb = f.cb;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    if (f.queries) {
        vkCmdResetQueryPool(cb, f.queries, 0, 2);
        f.queries_valid = false;
    }

    // Planes and the RGBA target into GENERAL for the compute writes. The very
    // first use comes from UNDEFINED; after that the planes are coming back from
    // SHADER_READ_ONLY (the previous frame's present pass sampled them).
    const VkImageLayout plane_from = images_initialised_
                                         ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                         : VK_IMAGE_LAYOUT_UNDEFINED;
    const VkImageLayout rgba_from = images_initialised_
                                        ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier(cb, luma_img_, plane_from, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    image_barrier(cb, chroma_img_, plane_from, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    image_barrier(cb, rgba_img_, rgba_from, VK_IMAGE_LAYOUT_GENERAL,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    images_initialised_ = true;

    // ---- decode (PAPER 3.2.1: two dispatches, not one)
    if (f.queries) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.queries, 0);

    DecodeSubmit ds{};
    ds.tile_meta = meta_buf_;
    ds.tile_meta_count = cfg_.stream.tiles_per_frame();
    ds.output_luma = luma_view_;
    ds.output_chroma = chroma_view_;
    ds.output_width = extent_.width;
    ds.output_height = extent_.height;
    ds.current_slot = frame_id % cfg_.stream.ring_slots;
    ds.frame_id = frame_id;

    decoder_->record_pass_a(cb, ds);
    decoder_->record_pass_b(cb, ds);

    if (f.queries) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.queries, 1);
        f.queries_valid = true;
    }

    // ---- present pass: planes become sampled, RGBA stays storage
    image_barrier(cb, luma_img_, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    image_barrier(cb, chroma_img_, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    PresentPush pp{extent_.width, extent_.height};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, present_pipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, present_pipe_layout_, 0, 1,
                            &present_set_, 0, nullptr);
    vkCmdPushConstants(cb, present_pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
    vkCmdDispatch(cb, (extent_.width + 15) / 16, (extent_.height + 15) / 16, 1);

    // ---- HUD on top of the RGBA image
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    if (cfg_.hud) {
        HudPush hp{};
        hp.img_w = extent_.width;
        hp.img_h = extent_.height;
        hp.cols = hud.cols();
        hp.rows = hud.rows();
        hp.scale = std::max(1u, extent_.height / 480u);
        hp.origin = (8u << 16) | 8u;
        const uint32_t gw = 6 * hp.scale, gh = 8 * hp.scale;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hud_pipe_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hud_pipe_layout_, 0, 1,
                                &hud_set_, 0, nullptr);
        vkCmdPushConstants(cb, hud_pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(hp), &hp);
        vkCmdDispatch(cb, (hp.cols * gw + 15) / 16, (hp.rows * gh + 15) / 16, 1);
    }

    // ---- blit into the swapchain image
    image_barrier(cb, rgba_img_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cb, sc_images_[image_index], VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
    blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
    vkCmdBlitImage(cb, rgba_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sc_images_[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);

    image_barrier(cb, sc_images_[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(cb);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.image_available;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sc_render_done_[image_index];
    if (vkQueueSubmit(queue_, 1, &si, f.fence) != VK_SUCCESS) {
        log_err("queue submit failed");
        return false;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sc_render_done_[image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        destroy_images();
        if (create_swapchain() && create_images()) update_descriptors();
    } else if (pr != VK_SUCCESS) {
        log_err("present: %s", vkstr(pr));
    }

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    return true;
}

// ---------------------------------------------------------------- teardown

void Renderer::surface_lost() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    destroy_swapchain();
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    window_ = nullptr;
}

bool Renderer::surface_regained(ANativeWindow* window) {
    if (!device_) return false;
    window_ = window;
    if (!create_surface(window)) return false;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, surface_, &ok);
    if (!ok) { log_err("regained surface not presentable on the chosen queue"); return false; }
    destroy_images();
    if (!create_swapchain()) return false;
    if (!create_images()) return false;
    update_descriptors();
    return true;
}

void Renderer::shutdown() {
    if (device_) vkDeviceWaitIdle(device_);
    if (decoder_) { decoder_->destroy(); decoder_.reset(); }

    if (device_) {
        for (auto& f : frames_) {
            if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
            if (f.image_available) vkDestroySemaphore(device_, f.image_available, nullptr);
            if (f.queries) vkDestroyQueryPool(device_, f.queries, nullptr);
            f = Frame{};
        }
        if (present_pipe_) vkDestroyPipeline(device_, present_pipe_, nullptr);
        if (hud_pipe_) vkDestroyPipeline(device_, hud_pipe_, nullptr);
        if (present_pipe_layout_) vkDestroyPipelineLayout(device_, present_pipe_layout_, nullptr);
        if (hud_pipe_layout_) vkDestroyPipelineLayout(device_, hud_pipe_layout_, nullptr);
        if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (present_layout_) vkDestroyDescriptorSetLayout(device_, present_layout_, nullptr);
        if (hud_layout_) vkDestroyDescriptorSetLayout(device_, hud_layout_, nullptr);

        auto drop_buf = [&](VkBuffer& b, VkDeviceMemory& m, void*& map) {
            if (map) { vkUnmapMemory(device_, m); map = nullptr; }
            if (b) vkDestroyBuffer(device_, b, nullptr);
            if (m) vkFreeMemory(device_, m, nullptr);
            b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
        };
        drop_buf(meta_buf_, meta_mem_, meta_map_);
        drop_buf(hud_buf_, hud_mem_, hud_map_);
        drop_buf(font_buf_, font_mem_, font_map_);

        destroy_images();
        if (plane_sampler_) vkDestroySampler(device_, plane_sampler_, nullptr);
        plane_sampler_ = VK_NULL_HANDLE;
        destroy_swapchain();
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

}  // namespace nxc
