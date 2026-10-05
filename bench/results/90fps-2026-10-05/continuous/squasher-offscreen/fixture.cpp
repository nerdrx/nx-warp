#include "render/render_interface.h"
#include "compositor/layer_squasher.h"
#include "driver/wivrn_hmd.h"
#include "driver/configuration.h"
#include "utils/wivrn_vk_bundle.h"
#include "wivrn_packets.h"
#include "wivrn_ipc.h"
#include "main/comp_compositor.h"
#include "util/comp_layer_accum.h"
#include "util/comp_swapchain.h"
#include "vk/allocation.h"

#include <array>
#include <memory>
#include <vector>
#include <iostream>
#include "runtime-check.h"

using namespace wivrn;


extern "C" { int listen_socket = -1; }
std::optional<wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets>> wivrn_ipc_socket_monado;

int squasher_fixture_entry(const std::filesystem::path & config_path, const std::filesystem::path & shader_path)
{
    // Actual cached production compositor, isolated fixtures and no server entrypoint.
    configuration::set_config_file(config_path);
    wivrn::vk_bundle vkb;
    std::cout << "device," << vkb.physical_device.getProperties().deviceName << "\n";
    from_headset::headset_info_packet headset{};
    headset.render_eye_width = headset.render_eye_height = 2176;
    headset.fov[0] = XrFovf{-0.8f, 0.8f, 0.8f, -0.8f};
    headset.fov[1] = headset.fov[0];
    wivrn_hmd hmd(nullptr, headset);

    constexpr uint32_t side = 2176;
    layer_squasher squasher(vkb, vk::Extent3D{side, side, 1});
    std::array<image_allocation, 2> source_images;
    std::array<vk::raii::ImageView, 2> source_views{nullptr, nullptr};
    std::array<VkImageView, 2> raw_views{};
    std::array<struct comp_swapchain, 2> swapchains{};
    for (size_t eye = 0; eye < 2; ++eye) {
        source_images[eye] = image_allocation{
            vkb.device,
            vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR8G8B8A8Srgb,
                .extent = vk::Extent3D{side, side, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            },
            VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO}};
        source_views[eye] = vk::raii::ImageView{
            vkb.device,
            vk::ImageViewCreateInfo{
                .image = source_images[eye],
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Srgb,
                .subresourceRange = vk::ImageSubresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            }};
        raw_views[eye] = *source_views[eye];
        swapchains[eye].images[0].views.no_alpha = &raw_views[eye];
        swapchains[eye].images[0].array_size = 1;
    }

    comp_layer_accum layers{};
    layers.layer_count = 1;
    auto & layer = layers.layers[0];
    layer.data.type = XRT_LAYER_PROJECTION;
    constexpr uint64_t display_ns = 1'000'000'000;
    layer.data.timestamp = display_ns;
    layer.sc_array[0] = reinterpret_cast<xrt_swapchain *>(&swapchains[0]);
    layer.sc_array[1] = reinterpret_cast<xrt_swapchain *>(&swapchains[1]);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto & view = layer.data.proj.v[eye];
        view.sub.image_index = 0;
        view.sub.array_index = 0;
        view.sub.norm_rect = xrt_normalized_rect{0.f, 0.f, 1.f, 1.f};
        view.fov = xrt_fov{-0.8f, 0.8f, 0.8f, -0.8f};
        view.pose.orientation.w = 1.f;
    }
    comp_frame frame{};
    frame.predicted_display_time_ns = display_ns;

    const auto pixels = record_squasher_color_check(vkb, squasher, source_images, hmd, frame, layers, side, shader_path);
    for (size_t eye=0;eye<2;++eye) for(size_t i=0;i<5;++i) {
        std::cout << "sample," << eye << ',' << i;
        for(size_t c=0;c<4;++c) std::cout << ',' << pixels[eye*20+i*4+c];
        std::cout << '\n';
    }
    std::cout << "Native2176x2176/eye production do_layers: 40 finite RGBA component checks PASS\n";
    return 0;
}

int main(int argc, char ** argv) {
    if (argc!=4 || std::string_view(argv[1])!="--run") {
        std::cerr << "Usage: squasher-fixture --run empty-config.json sample-output.spv\n"; return 2;
    }
    try { return squasher_fixture_entry(argv[2], argv[3]); }
    catch (const std::exception & e) { std::cerr << e.what() << '\n'; return 1; }
}
