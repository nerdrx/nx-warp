#include <turbojpeg.h>
#include <jconfig.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using bytes = std::vector<uint8_t>;
using clock_type = std::chrono::steady_clock;
#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

static bytes read_file(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open JPEG: " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

static void hash_bytes(uint64_t &hash, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t hash = 14695981039346656037ull;
    hash_bytes(hash, p, n);
    return hash;
}

static double percentile(std::vector<double> v, double q)
{
    std::sort(v.begin(), v.end());
    return v[std::max<size_t>(1, size_t(q * v.size() + 0.999999)) - 1];
}

struct image
{
    std::string path;
    bytes jpeg, rgba;
    tjhandle decoder = nullptr;
    int width = 0, height = 0;
    uint64_t expected_rgba_hash = 0, jpeg_hash = 0;
    ~image() { tj3Destroy(decoder); }
};

int main(int argc, char **argv)
try
{
    // Backward compatible single-image form; multiple paths make one timed frame.
    if (argc < 3) {
        std::cerr << "usage: jpeg_decode_bench input.jpg [input2.jpg ...] samples.csv\n";
        return 2;
    }
    std::vector<std::unique_ptr<image>> images;
    size_t total_jpeg_bytes = 0;
    for (int i = 1; i < argc - 1; ++i) {
        auto im = std::make_unique<image>();
        im->path = argv[i];
        im->jpeg = read_file(im->path);
        if (im->jpeg.empty()) throw std::runtime_error("empty JPEG: " + im->path);
        im->jpeg_hash = fnv1a(im->jpeg.data(), im->jpeg.size());
        im->decoder = tj3Init(TJINIT_DECOMPRESS);
        if (!im->decoder) throw std::runtime_error("tj3Init failed");
        if (tj3DecompressHeader(im->decoder, im->jpeg.data(), im->jpeg.size()) < 0)
            throw std::runtime_error(std::string("JPEG header failed: ") + tj3GetErrorStr(im->decoder));
        im->width = tj3Get(im->decoder, TJPARAM_JPEGWIDTH);
        im->height = tj3Get(im->decoder, TJPARAM_JPEGHEIGHT);
        if (im->width <= 0 || im->height <= 0 || size_t(im->width) * size_t(im->height) > SIZE_MAX / 4)
            throw std::runtime_error("invalid JPEG dimensions: " + im->path);
        im->rgba.resize(size_t(im->width) * size_t(im->height) * 4);
        total_jpeg_bytes += im->jpeg.size();
        images.push_back(std::move(im));
    }

    // Fixed protocol: 12 warmups, then 24 measured whole-frame repetitions.
    constexpr int warmups = 12, measured = 24;
    std::ofstream samples(argv[argc - 1]);
    if (!samples) throw std::runtime_error("cannot create samples CSV");
    samples << "phase,sample,decode_us,image_count,encoded_bytes,rgba_fnv1a,exact\n";
    uint64_t expected_frame_hash = 0;
    std::vector<double> times;
    for (int repetition = 0; repetition < warmups + measured; ++repetition) {
        const auto start = clock_type::now();
        for (auto &im : images) {
            if (tj3Decompress8(im->decoder, im->jpeg.data(), im->jpeg.size(),
                               im->rgba.data(), 0, TJPF_RGBA) < 0)
                throw std::runtime_error(std::string("JPEG decode failed: ") + tj3GetErrorStr(im->decoder));
        }
        const double us = std::chrono::duration<double, std::micro>(clock_type::now() - start).count();

        // Consume and check every decoded image outside the timed interval.
        uint64_t frame_hash = 14695981039346656037ull;
        for (auto &im : images) {
            const uint64_t hash = fnv1a(im->rgba.data(), im->rgba.size());
            if (repetition == 0) im->expected_rgba_hash = hash;
            if (hash != im->expected_rgba_hash) throw std::runtime_error("decoded RGB changed: " + im->path);
            hash_bytes(frame_hash, im->rgba.data(), im->rgba.size());
        }
        if (repetition == 0) expected_frame_hash = frame_hash;
        if (frame_hash != expected_frame_hash) throw std::runtime_error("decoded frame changed between samples");
        const char *phase = repetition < warmups ? "warm" : "measured";
        samples << phase << ',' << (repetition < warmups ? repetition : repetition - warmups) << ','
                << std::setprecision(9) << us << ',' << images.size() << ',' << total_jpeg_bytes << ','
                << std::hex << frame_hash << std::dec << ",1\n";
        if (repetition >= warmups) times.push_back(us);
    }
    samples.close();

    std::cout << "scope=CPU libjpeg-turbo tj3Decompress8 to RGBA8888; GPU=none; hardware_decoder=none\n"
              << "libjpeg_turbo=" << STRINGIFY(LIBJPEG_TURBO_VERSION)
              << ",frame_mode=" << (images.size() == 1 ? "single-jpeg" : "multi-jpeg")
              << ",image_count=" << images.size() << ",total_jpeg_bytes=" << total_jpeg_bytes
              << ",frame_rgba_fnv1a=" << std::hex << expected_frame_hash << std::dec
              << ",warmups=" << warmups << ",measured=" << measured
              << ",total_decode_p50_us=" << percentile(times, .50)
              << ",total_decode_p95_us=" << percentile(times, .95) << '\n';
    for (const auto &im : images)
        std::cout << "image=" << im->path << ",width=" << im->width << ",height=" << im->height
                  << ",jpeg_bytes=" << im->jpeg.size() << ",jpeg_fnv1a=" << std::hex << im->jpeg_hash
                  << ",rgba_fnv1a=" << im->expected_rgba_hash << std::dec << '\n';
    return 0;
}
catch (const std::exception &e)
{
    std::cerr << "jpeg_decode_bench: " << e.what() << '\n';
    return 1;
}
