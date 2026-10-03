#include "transcoder/basisu_transcoder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned kWarmups = 12;
constexpr unsigned kSamples = 30;
using Bytes = std::vector<uint8_t>;

struct Sample {
    double init_start_ms;
    double transcode_ms;
    double full_ms;
};

double ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return (v[v.size() / 2 - (v.size() % 2 == 0)] + v[v.size() / 2]) * 0.5;
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}

bool read_file(const char* path, Bytes& data) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > UINT32_MAX) return false;
    data.resize(static_cast<size_t>(size));
    f.seekg(0);
    return data.empty() || static_cast<bool>(f.read(reinterpret_cast<char*>(data.data()), size));
}

bool write_astc(const char* path, unsigned w, unsigned h, unsigned bw, unsigned bh, const Bytes& blocks) {
    uint8_t header[16] = {0x13, 0xAB, 0xA1, 0x5C,
        static_cast<uint8_t>(bw), static_cast<uint8_t>(bh), 1,
        static_cast<uint8_t>(w), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w >> 16),
        static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h >> 16),
        1, 0, 0};
    std::ofstream f(path, std::ios::binary);
    return f && f.write(reinterpret_cast<const char*>(header), sizeof(header)) &&
        f.write(reinterpret_cast<const char*>(blocks.data()), blocks.size());
}

bool run_one(const Bytes& input, Sample& sample, Bytes& output,
             unsigned& width, unsigned& height, unsigned& bw, unsigned& bh,
             std::string& error) {
    const auto full0 = Clock::now();
    basist::ktx2_transcoder transcoder;
    const auto init0 = Clock::now();
    if (!transcoder.init(input.data(), static_cast<uint32_t>(input.size()))) {
        error = "KTX2 init failed"; return false;
    }
    if (!transcoder.is_xuastc_ldr()) {
        error = "input is not XUASTC LDR"; return false;
    }
    if (!transcoder.start_transcoding()) {
        error = "start_transcoding failed"; return false;
    }
    sample.init_start_ms = ms(Clock::now() - init0);

    basist::ktx2_image_level_info info;
    if (!transcoder.get_image_level_info(info, 0, 0, 0)) {
        error = "mip 0 info failed"; return false;
    }
    width = info.m_orig_width;
    height = info.m_orig_height;
    bw = transcoder.get_block_width();
    bh = transcoder.get_block_height();
    if (!width || !height || !bw || !bh || transcoder.get_faces() != 1 || transcoder.get_layers() > 1) {
        error = "expected non-array 2D mip 0"; return false;
    }
    const uint64_t blocks = uint64_t((width + bw - 1) / bw) * ((height + bh - 1) / bh);
    if (blocks > SIZE_MAX / 16) { error = "output too large"; return false; }
    output.resize(static_cast<size_t>(blocks * 16));
    const auto transcode0 = Clock::now();
    const auto fmt = basist::basis_get_transcoder_texture_format_from_basis_tex_format(transcoder.get_basis_tex_format());
    if (!basist::basis_is_format_supported(fmt, transcoder.get_basis_tex_format()) ||
        basist::basis_get_bytes_per_block_or_pixel(fmt) != 16 ||
        !transcoder.transcode_image_level(0, 0, 0, output.data(), static_cast<uint32_t>(blocks), fmt)) {
        error = "XUASTC to matching ASTC transcode failed"; return false;
    }
    sample.transcode_ms = ms(Clock::now() - transcode0);
    sample.full_ms = ms(Clock::now() - full0);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s input.ktx2 output.astc\n", argv[0]);
        return 2;
    }
    Bytes input;
    if (!read_file(argv[1], input) || input.size() < 80) {
        std::fprintf(stderr, "cannot read KTX2 input (or file too large)\n");
        return 1;
    }
    basist::basisu_transcoder_init();
    Bytes warm_output;
    unsigned width = 0, height = 0, bw = 0, bh = 0;
    std::string error;
    for (unsigned i = 0; i < kWarmups; ++i) {
        Sample ignored{};
        if (!run_one(input, ignored, warm_output, width, height, bw, bh, error)) {
            std::fprintf(stderr, "%s\n", error.c_str()); return 1;
        }
    }

    std::vector<Sample> samples;
    samples.reserve(kSamples);
    Bytes reference, result;
    bool stable = true;
    for (unsigned i = 0; i < kSamples; ++i) {
        Sample s{};
        if (!run_one(input, s, result, width, height, bw, bh, error)) {
            std::fprintf(stderr, "%s\n", error.c_str()); return 1;
        }
        if (i == 0) reference = result;
        else stable &= (reference == result);
        samples.push_back(s);
    }
    if (!write_astc(argv[2], width, height, bw, bh, result)) {
        std::fprintf(stderr, "cannot write ASTC output\n"); return 1;
    }

    std::vector<double> prep, decode, full, prep_pct, decode_pct;
    for (const auto& s : samples) {
        prep.push_back(s.init_start_ms); decode.push_back(s.transcode_ms); full.push_back(s.full_ms);
        prep_pct.push_back(100.0 * s.init_start_ms / s.full_ms);
        decode_pct.push_back(100.0 * s.transcode_ms / s.full_ms);
    }
    auto mean = [](const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
    std::printf("input=%s output=%s size=%ux%u block=%ux%u samples=%u warmups=%u\n",
        argv[1], argv[2], width, height, bw, bh, kSamples, kWarmups);
    std::printf("init+start median=%.3f ms share(mean)=%.1f%%\n", median(prep), mean(prep_pct));
    std::printf("transcode median=%.3f ms share(mean)=%.1f%%\n", median(decode), mean(decode_pct));
    std::printf("full-call median=%.3f ms p95=%.3f ms\n", median(full), percentile(full, 0.95));
    std::printf("repeat-output-byte-exact=%s bytes=%zu\n", stable ? "yes" : "no", result.size());
    return stable ? 0 : 3;
}
