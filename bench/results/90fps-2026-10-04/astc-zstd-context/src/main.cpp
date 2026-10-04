#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p);
    return {std::istreambuf_iterator<char>(f), {}};
}
static void writeFile(const std::string &p, const std::vector<uint8_t> &v) {
    std::ofstream f(p, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char *>(v.data()), v.size()))
        throw std::runtime_error("cannot write " + p);
}
static bool decode(const std::vector<uint8_t> &payload, uint8_t *scratch,
                   size_t rawSize, ZSTD_DCtx *ctx, bool reuse) {
    const size_t frame = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
    if (ZSTD_isError(frame) || frame != payload.size()) return false;
    const unsigned long long content = ZSTD_getFrameContentSize(payload.data(), payload.size());
    if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN || content != rawSize) return false;
    const size_t got = reuse ? ZSTD_decompressDCtx(ctx, scratch, rawSize, payload.data(), payload.size())
                             : ZSTD_decompress(scratch, rawSize, payload.data(), payload.size());
    return !ZSTD_isError(got) && got == rawSize;
}
static double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>((v.size() - 1) * p)];
}
static void run(const std::string &root, const std::string &scene, bool reuse, int warm, int count) {
    auto payload = readFile(root + "/" + scene + "-q6.blocks.zst");
    auto expected = readFile(root + "/" + scene + "-q6.blocks");
    std::vector<uint8_t> scratch(expected.size()), upload(expected.size());
    ZSTD_DCtx *ctx = ZSTD_createDCtx();
    if (!ctx) throw std::runtime_error("ZSTD_createDCtx failed");
    std::vector<double> us;
    for (int i = 0; i < warm + count; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        if (!decode(payload, scratch.data(), expected.size(), ctx, reuse))
            throw std::runtime_error("decode failed");
        std::memcpy(upload.data(), scratch.data(), scratch.size());
        auto t1 = std::chrono::steady_clock::now();
        if (std::memcmp(upload.data(), expected.data(), expected.size()))
            throw std::runtime_error("output differs from fixed block payload");
        if (i >= warm) us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::string tag = reuse ? "dctx" : "oneshot";
    writeFile(root + "/" + scene + "-" + tag + ".out", upload);
    std::cout << scene << ',' << tag << ',' << warm << ',' << count << ','
              << percentile(us, .5) << ',' << percentile(us, .95) << ','
              << payload.size() << ',' << expected.size() << ",byte_exact\n";
    ZSTD_freeDCtx(ctx);
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: zstd-dctx-probe <fixture-dir>");
        std::cout << "scene,method,warmups,samples,median_us,p95_us,payload_bytes,raw_bytes,check\n";
        for (const char *scene : {"dark", "forest"}) {
            run(argv[1], scene, false, 12, 30);
            run(argv[1], scene, true, 12, 30);
            run(argv[1], scene, false, 12, 30);
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
}
