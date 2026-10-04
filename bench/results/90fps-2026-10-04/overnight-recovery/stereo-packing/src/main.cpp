#include "nxastc_packet.h"
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <lz4.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <zstd.h>
using Clock = std::chrono::steady_clock;
constexpr int warmup = 20, iterations = 200;
struct Stage {
    double total{}, lz4{}, zstd{}, compact{};
    uint8_t encoding{};
    std::vector<uint8_t> packet;
};
struct Context {
    ZSTD_CCtx* z = ZSTD_createCCtx();
    std::vector<char> lzbuf, zbuf;
    std::vector<uint8_t> compact;
    Context() {
        if (!z)
            throw std::runtime_error("ZSTD context allocation failed");
    }
    ~Context() { ZSTD_freeCCtx(z); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
static std::vector<uint8_t> readBlocks(const std::string& path, uint32_t& w, uint32_t& h) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("open " + path);
    auto n = f.tellg();
    if (n < 16)
        throw std::runtime_error("short ASTC file");
    std::vector<uint8_t> b(static_cast<size_t>(n));
    f.seekg(0);
    f.read((char*)b.data(), n);
    if (!f || b[0] != 0x13 || b[1] != 0xab || b[2] != 0xa1 || b[3] != 0x5c || b[4] != 8 ||
        b[5] != 8 || b[6] != 1)
        throw std::runtime_error("not standard 8x8 ASTC");
    w = b[7] | uint32_t(b[8]) << 8 | uint32_t(b[9]) << 16;
    h = b[10] | uint32_t(b[11]) << 8 | uint32_t(b[12]) << 16;
    uint32_t d = b[13] | uint32_t(b[14]) << 8 | uint32_t(b[15]) << 16;
    uint64_t raw = ((uint64_t(w) + 7) / 8) * ((uint64_t(h) + 7) / 8) * 16;
    if (!w || !h || d != 1 || uint64_t(n) != 16 + raw)
        throw std::runtime_error("ASTC header/payload mismatch");
    return {b.begin() + 16, b.end()};
}
static Stage encode(std::span<const uint8_t> raw, Context& c, uint32_t w, uint32_t h,
                    bool compactOn) {
    auto begin = Clock::now();
    size_t rawSize = raw.size(), zsize = 0;
    int lsize = 0;
    uint32_t payloadSize = uint32_t(rawSize);
    const uint8_t* payload = raw.data();
    auto codec = wivrn::nxastc_packet::compression::none;
    Stage r;
    bool compactInput = false;
    if (compactOn) {
        auto t = Clock::now();
        size_t n = 0;
        if (wivrn::nxastc_packet::compact_bytes_for_raw(rawSize, n)) {
            c.compact.resize(n);
            compactInput = wivrn::nxastc_packet::compact_blocks(raw, c.compact);
        }
        r.compact = ms(t, Clock::now());
    }
    std::span<const uint8_t> zinput = compactInput ? std::span<const uint8_t>(c.compact) : raw;
    auto zstart = Clock::now();
    c.zbuf.resize(ZSTD_compressBound(zinput.size()));
    zsize = ZSTD_compressCCtx(c.z, c.zbuf.data(), c.zbuf.size(), zinput.data(), zinput.size(), 3);
    r.zstd = ms(zstart, Clock::now());
    const bool zstdOk = !ZSTD_isError(zsize) && zsize > 0;
    const bool preferZstd = true;
    if (!preferZstd || !zstdOk || zsize > rawSize / 2) {
        auto t = Clock::now();
        c.lzbuf.resize(LZ4_compressBound(int(rawSize)));
        lsize = LZ4_compress_default((const char*)raw.data(), c.lzbuf.data(), int(rawSize),
                                     int(c.lzbuf.size()));
        r.lz4 = ms(t, Clock::now());
    }
    if (lsize > 0 && size_t(lsize) < rawSize) {
        payloadSize = uint32_t(lsize);
        payload = (const uint8_t*)c.lzbuf.data();
        codec = wivrn::nxastc_packet::compression::lz4;
    }
    if (zstdOk && zsize * 100 <= uint64_t(payloadSize) * 90 &&
        (!compactInput || zsize <= c.compact.size())) {
        payloadSize = uint32_t(zsize);
        payload = (const uint8_t*)c.zbuf.data();
        codec = compactInput ? wivrn::nxastc_packet::compression::compact_zstd
                             : wivrn::nxastc_packet::compression::zstd;
    }
    auto hdr = wivrn::nxastc_packet::make_header(w, h, payloadSize, codec);
    r.packet.resize(hdr.size() + payloadSize);
    std::memcpy(r.packet.data(), hdr.data(), hdr.size());
    std::memcpy(r.packet.data() + hdr.size(), payload, payloadSize);
    r.encoding = uint8_t(codec);
    r.total = ms(begin, Clock::now());
    return r;
}

static bool verifyPacket(const std::vector<uint8_t>& p, std::span<const uint8_t> raw) {
    using namespace wivrn::nxastc_packet;
    if (p.size() < header_size || p[0] != 'N' || p[1] != 'A' || p[2] != 'S' || p[3] != 'T' ||
        read32(p.data() + 16) != raw.size() || read32(p.data() + 20) != p.size() - header_size)
        return false;
    auto enc = compression(p[5]);
    std::vector<uint8_t> decoded(raw.size());
    auto payload = std::span<const uint8_t>(p.data() + header_size, p.size() - header_size);
    if (enc == compression::none) {
        if (payload.size() != raw.size())
            return false;
        std::copy(payload.begin(), payload.end(), decoded.begin());
    } else if (enc == compression::lz4) {
        if (LZ4_decompress_safe((const char*)payload.data(), (char*)decoded.data(),
                                int(payload.size()), int(decoded.size())) != int(decoded.size()))
            return false;
    } else if (enc == compression::zstd) {
        if (ZSTD_decompress(decoded.data(), decoded.size(), payload.data(), payload.size()) !=
            decoded.size())
            return false;
    } else if (enc == compression::compact_zstd) {
        size_t n = 0;
        if (!compact_bytes_for_raw(raw.size(), n))
            return false;
        std::vector<uint8_t> compact(n);
        if (ZSTD_decompress(compact.data(), compact.size(), payload.data(), payload.size()) != n)
            return false;
        std::copy(compact.begin(), compact.end(), decoded.begin());
        if (!expand_compact_blocks(decoded, n))
            return false;
    } else
        return false;
    return std::equal(decoded.begin(), decoded.end(), raw.begin());
}
struct Row {
    double wall{}, launch{}, e0{}, e1{}, z0{}, z1{}, l0{}, l1{}, c0{}, c1{};
};
struct Stats {
    std::vector<Row> v;
};
static double pct(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[size_t((v.size() - 1) * p)];
}
static void report(const char* name, const Stats& s) {
    auto column = [&](auto f) {
        std::vector<double> v;
        v.reserve(s.v.size());
        for (auto& r : s.v)
            v.push_back(f(r));
        return v;
    };
    auto show = [&](const char* n, auto f) {
        auto v = column(f);
        std::cout << n << '=' << pct(v, .5) << '/' << pct(v, .95) << "ms ";
    };
    std::cout << name << " median/p95 ";
    show("batch", [](auto& r) { return r.wall; });
    show("eye0", [](auto& r) { return r.e0; });
    show("eye1", [](auto& r) { return r.e1; });
    show("eye0_zstd", [](auto& r) { return r.z0; });
    show("eye1_zstd", [](auto& r) { return r.z1; });
    show("eye0_lz4", [](auto& r) { return r.l0; });
    show("eye1_lz4", [](auto& r) { return r.l1; });
    show("eye0_compact", [](auto& r) { return r.c0; });
    show("eye1_compact", [](auto& r) { return r.c1; });
    if (std::string(name) == "async")
        show("launch", [](auto& r) { return r.launch; });
    std::cout << '\n';
}

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5)
            throw std::runtime_error(
                "usage: stereo-pack dark.astc forest.astc output-prefix [--compact]");
        bool compactOn = argc == 5 && std::string(argv[4]) == "--compact";
        if (argc == 5 && !compactOn)
            throw std::runtime_error("only explicit optional mode is --compact");
        uint32_t w0, h0, w1, h1;
        auto a = readBlocks(argv[1], w0, h0), b = readBlocks(argv[2], w1, h1);
        if (w0 != w1 || h0 != h1)
            throw std::runtime_error("eye dimensions differ");
        double loads[3]{};
        int loadOk = getloadavg(loads, 3);
        std::cout << "inputs=" << argv[1] << '+' << argv[2] << " dimensions=" << w0 << 'x' << h0
                  << " header_excluded_raw_eye_bytes=" << a.size() << " quality=6 mode="
                  << (compactOn ? "explicit-compact-one-zstd" : "legacy-default")
                  << " warmup=" << warmup << " iterations=" << iterations
                  << " threads=2 persistent std::jthread plus per-frame std::async";
        if (loadOk == 3)
            std::cout << " loadavg=" << loads[0] << '/' << loads[1] << '/' << loads[2];
        std::cout << '\n';
        Context s0, s1, async0, async1;
        std::array<Context, 2> wctx;
        std::array<Stage, 2> parallelOut;
        std::array<double, 2> workerMs{};
        std::barrier gate(3);
        std::jthread t0([&](std::stop_token) {
            for (int i = 0; i < warmup + iterations; i++) {
                gate.arrive_and_wait();
                auto t = Clock::now();
                parallelOut[0] = encode(a, wctx[0], w0, h0, compactOn);
                workerMs[0] = ms(t, Clock::now());
                gate.arrive_and_wait();
            }
        });
        std::jthread t1([&](std::stop_token) {
            for (int i = 0; i < warmup + iterations; i++) {
                gate.arrive_and_wait();
                auto t = Clock::now();
                parallelOut[1] = encode(b, wctx[1], w1, h1, compactOn);
                workerMs[1] = ms(t, Clock::now());
                gate.arrive_and_wait();
            }
        });
        Stats serial, parallel, async;
        std::array<std::vector<uint8_t>, 2> reference;
        bool equal = true;
        uint32_t compactWins[2]{};
        for (int i = 0; i < warmup + iterations; i++) {
            auto s = Clock::now();
            auto r0 = encode(a, s0, w0, h0, compactOn);
            auto r1 = encode(b, s1, w1, h1, compactOn);
            auto e = Clock::now();
            auto p = Clock::now();
            gate.arrive_and_wait();
            gate.arrive_and_wait();
            auto q = Clock::now();
            auto as = Clock::now();
            auto launch = Clock::now();
            auto rightFuture = std::async(std::launch::async, [&] {
                auto t = Clock::now();
                auto r = encode(b, async1, w1, h1, compactOn);
                return std::pair{std::move(r), ms(t, Clock::now())};
            });
            double launchMs = ms(launch, Clock::now());
            auto leftAsync = encode(a, async0, w0, h0, compactOn);
            auto rightAsync = rightFuture.get();
            auto ae = Clock::now();  // get() joins before the next input job can be released
            if (i == warmup) {
                reference[0] = r0.packet;
                reference[1] = r1.packet;
            }
            equal &= r0.packet == parallelOut[0].packet && r1.packet == parallelOut[1].packet &&
                     r0.packet == leftAsync.packet && r1.packet == rightAsync.first.packet;
            if (i >= warmup) {
                equal &= r0.packet == reference[0] && r1.packet == reference[1];
                compactWins[0] +=
                    r0.encoding == uint8_t(wivrn::nxastc_packet::compression::compact_zstd);
                compactWins[1] +=
                    r1.encoding == uint8_t(wivrn::nxastc_packet::compression::compact_zstd);
                serial.v.push_back({ms(s, e), 0, r0.total, r1.total, r0.zstd, r1.zstd, r0.lz4,
                                    r1.lz4, r0.compact, r1.compact});
                parallel.v.push_back({ms(p, q), 0, workerMs[0], workerMs[1], parallelOut[0].zstd,
                                      parallelOut[1].zstd, parallelOut[0].lz4, parallelOut[1].lz4,
                                      parallelOut[0].compact, parallelOut[1].compact});
                async.v.push_back({ms(as, ae), launchMs, leftAsync.total, rightAsync.second,
                                   rightAsync.first.zstd, leftAsync.zstd, rightAsync.first.lz4,
                                   leftAsync.lz4, rightAsync.first.compact, leftAsync.compact});
            }
        }
        if (!equal)
            throw std::runtime_error("serial/persistent/async packet mismatch or stale completion");
        if (!verifyPacket(reference[0], a) || !verifyPacket(reference[1], b))
            throw std::runtime_error("compressed packet did not round-trip to ASTC blocks");
        std::string prefix = argv[3];
        auto save = [&](const std::string& p, const std::vector<uint8_t>& v) {
            std::ofstream f(p, std::ios::binary);
            f.write((const char*)v.data(), v.size());
            if (!f)
                throw std::runtime_error("write " + p);
        };
        save(prefix + ".eye0.packet", reference[0]);
        save(prefix + ".eye1.packet", reference[1]);
        report("serial", serial);
        report("persistent", parallel);
        report("async", async);
        std::cout << "byte_identical_serial_persistent_async_and_repeats=true "
                     "roundtrip_verified=true packet_bytes="
                  << reference[0].size() << '/' << reference[1].size()
                  << " encoding=" << unsigned(reference[0][5]) << '/' << unsigned(reference[1][5])
                  << " compact_wins=" << compactWins[0] << '/' << compactWins[1] << '/'
                  << iterations << "\n";
        std::ofstream csv(prefix + ".csv");
        csv << "iteration,serial_batch_ms,serial_eye0_ms,serial_eye1_ms,serial_eye0_zstd_ms,serial_"
               "eye1_zstd_ms,persistent_batch_ms,persistent_eye0_ms,persistent_eye1_ms,async_batch_"
               "ms,async_launch_ms,async_eye0_ms,async_eye1_worker_ms,async_eye0_zstd_ms,async_"
               "eye1_zstd_ms,async_eye0_compact_ms,async_eye1_compact_ms\n";
        for (size_t i = 0; i < serial.v.size(); i++) {
            auto& s = serial.v[i];
            auto& p = parallel.v[i];
            auto& t = async.v[i];
            csv << i << ',' << s.wall << ',' << s.e0 << ',' << s.e1 << ',' << s.z0 << ',' << s.z1
                << ',' << p.wall << ',' << p.e0 << ',' << p.e1 << ',' << t.wall << ',' << t.launch
                << ',' << t.e0 << ',' << t.e1 << ',' << t.z1 << ',' << t.z0 << ',' << t.c1 << ','
                << t.c0 << '\n';
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
