// Small device proof for the reusable nxvc::PlanarDirect API.
#define main planar_probe_main
#include "main.cpp"
#undef main
#include "nxvc/planar_direct.h"

static void readback(Probe& p, const std::string& path) {
    Buffer out = p.buffer(VkDeviceSize(p.w) * p.h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    p.begin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = p.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(p.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {p.w, p.h, 1};
    vkCmdCopyImageToBuffer(p.cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.b, 1, &c);
    p.submit_wait(true);
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(out.ptr), out.bytes);
    if (!f) throw std::runtime_error("readback write failed");
    p.free_buffer(out);
}

int main(int argc, char** argv) try {
    if (argc != 4) {
        std::fprintf(stderr, "usage: direct-harness STREAM SHADER_DIR OUT_PREFIX\n");
        return 2;
    }
    auto data = read_file(argv[1]);
    nxvcvk::StreamInfo si{};
    size_t off = 0;
    if (nxvcvk::parse_stream_header(data.data(), data.size(), si, &off) != NXVC_VKD_OK)
        throw std::runtime_error("invalid PLANAR header");
    Probe p;
    p.init(si.width * si.eyes, si.height, si.tile_count, argv[2]);
    nxvc::PlanarDirect direct(p.physical, p.device, p.queue, p.family);
    direct.configure(data.data(), off);
    if (direct.width() != p.w || direct.height() != p.h)
        throw std::runtime_error("geometry mismatch");
    nxvcvk::InterCtx inter;
    inter.resize(si.tile_count);
    std::vector<const uint8_t*> frames;
    std::vector<size_t> sizes;
    while (off < data.size()) {
        nxvcvk::FrameParse fp{};
        if (nxvcvk::parse_frame(si, data.data() + off, data.size() - off, false, fp, &inter) != NXVC_VKD_OK)
            throw std::runtime_error("invalid frame");
        frames.push_back(data.data() + off);
        sizes.push_back(fp.frame_bytes);
        off += fp.frame_bytes;
    }
    if (frames.size() < 20) throw std::runtime_error("fixture has fewer than 20 frames");
    auto s19 = direct.render(frames[19], sizes[19], p.image, p.view, p.w, p.h);
    readback(p, std::string(argv[3]) + ".frame19.rgba");
    std::printf("frame19 parse_ms=%.6f upload_ms=%.6f gpu_ms=%.6f total_ms=%.6f tiles=%u\n",
                s19.parse_ms, s19.upload_ms, s19.gpu_ms, s19.total_ms, s19.tiles);
    std::ofstream csv(std::string(argv[3]) + ".motion.csv");
    csv << "frame,parse_ms,upload_ms,gpu_ms,total_ms,deadline_miss\n";
    double total = 0.0, gpu = 0.0;
    unsigned misses = 0;
    for (unsigned i = 0; i < 720; ++i) {
        auto start = Clock::now();
        auto s = direct.render(frames[i % frames.size()], sizes[i % frames.size()], p.image, p.view, p.w, p.h);
        double elapsed = ms(start, Clock::now());
        total += elapsed;
        if (s.gpu_ms >= 0) gpu += s.gpu_ms;
        bool miss = elapsed > 11.111;
        misses += miss;
        csv << i << ',' << s.parse_ms << ',' << s.upload_ms << ',' << s.gpu_ms << ',' << elapsed
            << ',' << (miss ? 1 : 0) << '\n';
        std::this_thread::sleep_until(start + std::chrono::microseconds(11111));
    }
    if (!csv) throw std::runtime_error("timing CSV write failed");
    std::printf("motion720 frames=720 cadence_hz=90.000 wall_ms=%.6f avg_ms=%.6f avg_gpu_ms=%.6f\n",
                total, total / 720.0, gpu / 720.0);
    std::printf("motion720 deadline_misses=%u\n", misses);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
