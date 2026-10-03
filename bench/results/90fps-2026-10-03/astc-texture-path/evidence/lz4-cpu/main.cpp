#include "lz4_payload.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned kWarmups = 12;
constexpr unsigned kSamples = 30;
using Bytes = std::vector<char>;

bool read_file(const char* path, Bytes& data) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > INT32_MAX) return false;
    data.resize(static_cast<size_t>(size));
    f.seekg(0);
    return data.empty() || static_cast<bool>(f.read(data.data(), size));
}

double elapsed_ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return (v[v.size() / 2 - (v.size() % 2 == 0)] + v[v.size() / 2]) * 0.5;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s input.lz4 reference.astc\n", argv[0]);
        return 2;
    }
    Bytes compressed, astc;
    if (!read_file(argv[1], compressed) || !read_file(argv[2], astc) || astc.size() < 16 || compressed.empty()) {
        std::fprintf(stderr, "cannot read input payload or ASTC reference\n");
        return 1;
    }
    const size_t decoded_size = astc.size() - 16;
    if (compressed.size() > INT32_MAX || decoded_size > INT32_MAX) {
        std::fprintf(stderr, "payload exceeds LZ4 safe API size limit\n");
        return 1;
    }
    // Persistent output buffer: no per-sample allocation or file I/O.
    Bytes decoded(decoded_size);
    auto decode = [&]() {
        return nx_lz4_decompress_payload(compressed.data(), compressed.size(),
            decoded.data(), decoded.size());
    };
    for (unsigned i = 0; i < kWarmups; ++i) {
        if (decode() != static_cast<int>(decoded_size)) {
            std::fprintf(stderr, "warmup decompressed size mismatch\n"); return 1;
        }
    }

    std::vector<double> times;
    times.reserve(kSamples);
    bool exact = true;
    for (unsigned i = 0; i < kSamples; ++i) {
        const auto t0 = Clock::now();
        const int produced = decode();
        times.push_back(elapsed_ms(Clock::now() - t0));
        exact &= produced == static_cast<int>(decoded_size);
        exact &= std::equal(decoded.begin(), decoded.end(), astc.begin() + 16);
    }
    std::printf("input=%s reference=%s compressed=%zu decoded=%zu samples=%u warmups=%u\n",
        argv[1], argv[2], compressed.size(), decoded_size, kSamples, kWarmups);
    std::printf("LZ4 payload decode median=%.3f ms p95=%.3f ms\n", median(times), percentile(times, 0.95));
    std::printf("all-sample-payload-byte-exact=%s\n", exact ? "yes" : "no");
    return exact ? 0 : 3;
}
