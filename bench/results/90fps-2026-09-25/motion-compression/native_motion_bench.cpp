// Offline native-centre motion compression experiment. Not a live wire protocol.
// Requires a matching reference supplied explicitly; no ACK, eviction or loss model.
#include "nxwarp_direct.h"
#include "nxwarp_direct_lz4.h"
#include "nxwarp_direct_zstd.h"
#include <algorithm>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
using namespace wivrn::nxwarp_direct;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
static constexpr uint32_t W = 2176, H = 2176, E = 2, TILES = (W / 32) * (H / 32) * E;
static constexpr size_t PIXELS_PER_TILE = 32 * 32 * 4, META = 24;
static constexpr uint32_t META_MAGIC = 0x564d584e;  // NXMV
struct NativeInfo {
    std::vector<int32_t> pixel;
    size_t count = 0;
};
struct Motion {
    int dx = 0, dy = 0;
    double sad = 255;
    size_t hits = 0;
};
static Bytes read(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static void write(const std::filesystem::path& p, std::span<const uint8_t> b) {
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)b.data(), b.size());
}
static double pct(std::vector<double> x, int p) {
    std::sort(x.begin(), x.end());
    return x[std::max<size_t>(0, (x.size() * p + 99) / 100 - 1)];
}
static bool build_native(std::span<const uint8_t> raw, NativeInfo& out) {
    layout l{W, H, E, true};
    if (!parse_frame(l, raw))
        return false;
    out.pixel.assign(TILES, -1);
    out.count = 0;
    auto ds = raw.subspan(16, TILES * 4);
    for (uint32_t i = 0; i < TILES; ++i) {
        uint32_t d = read32(ds, i * 4);
        if (!(d & 0x20000000u))
            continue;
        if (d & 0x10000000u)
            return false;
        size_t pos = 16 + TILES * 4 + size_t(d & 0x0fffffffu) * 4;
        if (pos + PIXELS_PER_TILE > raw.size())
            return false;
        out.pixel[i] = int32_t(pos);
        ++out.count;
    }
    return out.count > 0;
}
static size_t tile_index(unsigned eye, int x, int y) {
    return size_t(y / 32) * (W / 32 * E) + size_t(eye) * (W / 32) + size_t(x / 32);
}
static bool get_pixel(std::span<const uint8_t> raw, const NativeInfo& n, unsigned eye, int x, int y,
                      const uint8_t*& p) {
    if (x < 0 || y < 0 || x >= int(W) || y >= int(H))
        return false;
    int32_t pos = n.pixel[tile_index(eye, x, y)];
    if (pos < 0)
        return false;
    pos += int32_t(((y & 31) * 32 + (x & 31)) * 4);
    p = raw.data() + pos;
    return true;
}
static Motion estimate(std::span<const uint8_t> old, const NativeInfo& oi,
                       std::span<const uint8_t> now, const NativeInfo& ni) {
    struct Sample {
        uint8_t eye;
        int x, y;
    };
    std::array<Sample, 512> s{};
    size_t sn = 0;
    for (unsigned e = 0; e < E; ++e)
        for (int y = int(H / 2) - 64; y < int(H / 2) + 64; y += 8)
            for (int x = int(W / 2) - 64; x < int(W / 2) + 64; x += 8) {
                const uint8_t *a, *b;
                if (get_pixel(now, ni, e, x, y, a) && get_pixel(old, oi, e, x, y, b))
                    s[sn++] = {uint8_t(e), x, y};
            }
    Motion best{};
    best.sad = 1e9;
    auto test = [&](int dx, int dy) {
        uint64_t cost = 0;
        size_t hits = 0;
        for (size_t i = 0; i < sn; ++i) {
            const auto& q = s[i];
            const uint8_t *a, *b;
            if (!get_pixel(now, ni, q.eye, q.x, q.y, a) ||
                !get_pixel(old, oi, q.eye, q.x + dx, q.y + dy, b))
                continue;
            cost += std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1])) +
                    std::abs(int(a[2]) - int(b[2]));
            ++hits;
        }
        if (hits < sn / 2)
            return;
        double score = double(cost) / (hits * 3);
        if (score < best.sad - 1e-9 ||
            (std::abs(score - best.sad) < 1e-9 &&
             std::abs(dx) + std::abs(dy) < std::abs(best.dx) + std::abs(best.dy)))
            best = {dx, dy, score, hits};
    };
    for (int dy = -16; dy <= 16; dy += 4)
        for (int dx = -16; dx <= 16; dx += 4)
            test(dx, dy);
    int cx = best.dx, cy = best.dy;
    for (int dy = std::max(-16, cy - 3); dy <= std::min(16, cy + 3); ++dy)
        for (int dx = std::max(-16, cx - 3); dx <= std::min(16, cx + 3); ++dx)
            test(dx, dy);
    return best;
}
static Bytes pack_current(std::span<const uint8_t> raw) {
    Bytes fastbuf, zbuf, pbuf, scratch;
    auto fast = compress_lz4(raw, fastbuf);
    auto dense = compress_zstd(raw, zbuf);
    auto predicted = compress_zstd_predicted(raw, pbuf, scratch);
    if (predicted.size() * 100 <= dense.size() * 95)
        dense = predicted;
    auto picked = dense.size() * 100 <= fast.size() * 90 ? dense : fast;
    return Bytes(picked.begin(), picked.end());
}
static bool unpack_current(std::span<const uint8_t> wire, Bytes& raw) {
    layout l{W, H, E, true, 256, false, true, true, false};
    if (is_zstd(wire))
        return decompress_zstd(l, wire, raw);
    if (is_lz4(wire))
        return decompress_lz4(l, wire, raw);
    raw.assign(wire.begin(), wire.end());
    return true;
}
static Bytes residual_frame(std::span<const uint8_t> old, const NativeInfo& oi,
                            std::span<const uint8_t> now, const NativeInfo& ni, int dx, int dy) {
    Bytes r(now.begin(), now.end());
    for (uint32_t tile = 0; tile < TILES; ++tile) {
        int32_t dst = ni.pixel[tile];
        if (dst < 0)
            continue;
        int ty = tile / (W / 32 * E), rem = tile % (W / 32 * E);
        unsigned eye = rem / (W / 32);
        int tx = rem % (W / 32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const uint8_t* src = nullptr;
                get_pixel(old, oi, eye, tx * 32 + x + dx, ty * 32 + y + dy, src);
                size_t dp = size_t(dst) + (size_t(y) * 32 + x) * 4;
                for (int c = 0; c < 4; ++c)
                    r[dp + c] = uint8_t(now[dp + c] - (src ? src[c] : 0));
            }
    }
    return r;
}
static void add_bytes(uint8_t* dst, const uint8_t* src, size_t n) {
    size_t i = 0;
#ifdef __aarch64__
    for (; i + 16 <= n; i += 16)
        vst1q_u8(dst + i, vaddq_u8(vld1q_u8(dst + i), vld1q_u8(src + i)));
#endif
    for (; i < n; ++i)
        dst[i] = uint8_t(dst[i] + src[i]);
}
static void inverse_residual(const NativeInfo& oi, std::span<const uint8_t> old,
                             const NativeInfo& ni, Bytes& r, int dx, int dy) {
    for (uint32_t tile = 0; tile < TILES; ++tile) {
        int32_t dst = ni.pixel[tile];
        if (dst < 0)
            continue;
        int ty = tile / (W / 32 * E), rem = tile % (W / 32 * E);
        unsigned eye = rem / (W / 32);
        int tx = rem % (W / 32);
        for (int y = 0; y < 32; ++y) {
            int sy = ty * 32 + y + dy;
            if (sy < 0 || sy >= int(H))
                continue;
            int x = 0;
            while (x < 32) {
                int sx = tx * 32 + x + dx;
                if (sx < 0 || sx >= int(W)) {
                    ++x;
                    continue;
                }
                int stx = sx / 32, sty = sy / 32;
                size_t st = tile_index(eye, stx * 32, sty * 32);
                int32_t srcbase = oi.pixel[st];
                int end = std::min(32, x + (32 - (sx & 31)));
                if (srcbase >= 0) {
                    srcbase += int32_t(((sy & 31) * 32 + (sx & 31)) * 4);
                    size_t dp = size_t(dst) + (size_t(y) * 32 + x) * 4;
                    add_bytes(r.data() + dp, old.data() + srcbase, size_t(end - x) * 4);
                }
                x = end;
            }
        }
    }
}
static Bytes make_wire(int dx, int dy, uint32_t ref, std::span<const uint8_t> body) {
    Bytes w(META + body.size());
    uint32_t u[] = {META_MAGIC,           1, ref, uint32_t(dx) & 0xffffu, uint32_t(dy) & 0xffffu,
                    uint32_t(body.size())};
    for (int i = 0; i < 6; ++i)
        for (int k = 0; k < 4; ++k)
            w[i * 4 + k] = uint8_t(u[i] >> (8 * k));
    std::copy(body.begin(), body.end(), w.begin() + META);
    return w;
}
static bool parse_meta(std::span<const uint8_t> w, int& dx, int& dy, uint32_t& ref) {
    if (w.size() < META || read32(w, 0) != META_MAGIC || read32(w, 4) != 1 ||
        read32(w, 20) != w.size() - META)
        return false;
    ref = read32(w, 8);
    dx = int16_t(read32(w, 12) & 0xffff);
    dy = int16_t(read32(w, 16) & 0xffff);
    return true;
}
static bool decode_candidate(std::span<const uint8_t> old, const NativeInfo& oi,
                             std::span<const uint8_t> wire, Bytes& reconstructed,
                             uint32_t expected_ref = 1) {
    int dx, dy;
    uint32_t ref;
    if (!parse_meta(wire, dx, dy, ref) || ref != expected_ref)
        return false;
    Bytes residual;
    if (!unpack_current(wire.subspan(META), residual))
        return false;
    NativeInfo ni;
    if (!build_native(residual, ni))
        return false;
    inverse_residual(oi, old, ni, residual, dx, dy);
    reconstructed = std::move(residual);
    return true;
}
struct Results {
    std::vector<double> baseEnc, candEnc, baseDec, cachedDec, uncachedDec;
};
static bool run_pair(std::string name, const std::filesystem::path& oldpath,
                     const std::filesystem::path& nowpath, const std::filesystem::path& exportdir) {
    Bytes old = read(oldpath), now = read(nowpath);
    NativeInfo oi;
    if (!build_native(old, oi)) {
        std::cerr << "invalid old native frame " << oldpath << "\n";
        return false;
    }
    Bytes baseWire = pack_current(now);
    NativeInfo ni;
    if (!build_native(now, ni)) {
        std::cerr << "invalid current native frame " << nowpath << "\n";
        return false;
    }
    Motion m = estimate(old, oi, now, ni);
    Results a;
    Bytes exportwire;
    for (int i = 0; i < 40; ++i) {
        auto t = Clock::now();
        Bytes bw = pack_current(now);
        double bem = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        t = Clock::now();
        Bytes bd;
        bool bok = unpack_current(bw, bd);
        double bdm = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        if (!bok || bd != now)
            return false;
        t = Clock::now();
        NativeInfo curInfo;
        if (!build_native(now, curInfo))
            return false;
        Motion em = estimate(old, oi, now, curInfo);
        Bytes residual = residual_frame(old, oi, now, curInfo, em.dx, em.dy);
        Bytes body = pack_current(residual);
        Bytes wire = make_wire(em.dx, em.dy, 1, body);
        double cem = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        t = Clock::now();
        Bytes round;
        bool cok = decode_candidate(old, oi, wire, round);
        double cdm = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        if (!cok || round != now) {
            std::cerr << "cached candidate mismatch " << name << " ok=" << cok << "\n";
            return false;
        }
        t = Clock::now();
        NativeInfo uncached;
        if (!build_native(old, uncached)) {
            std::cerr << "uncached reference parse failed " << name << "\n";
            return false;
        }
        Bytes round2;
        bool uok = decode_candidate(old, uncached, wire, round2);
        double udm = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        if (!uok || round2 != now) {
            std::cerr << "uncached candidate mismatch " << name << " ok=" << uok << "\n";
            return false;
        }
        if (i >= 8) {
            a.baseEnc.push_back(bem);
            a.baseDec.push_back(bdm);
            a.candEnc.push_back(cem);
            a.cachedDec.push_back(cdm);
            a.uncachedDec.push_back(udm);
        }
        if (i == 39)
            exportwire = std::move(wire);
    }
    size_t candBytes = exportwire.size();
    bool useCandidate = candBytes * 100 <= baseWire.size() * 90;
    size_t selectedBytes = useCandidate ? candBytes : baseWire.size();
    if (!exportdir.empty()) {
        std::filesystem::create_directories(exportdir);
        auto out = exportdir / (name + ".native-motion-proposal.bin");
        write(out, exportwire);
        auto bout = exportdir / (name + ".baseline.bin");
        write(bout, baseWire);
    }
    std::cout << "pair=" << name << ",old=" << oldpath.filename().string()
              << ",current=" << nowpath.filename().string() << ",estimated_dx=" << m.dx
              << ",estimated_dy=" << m.dy << ",sad=" << m.sad << ",sample_hits=" << m.hits
              << ",native_tiles=" << ni.count << ",baseline_bytes=" << baseWire.size()
              << ",proposal_bytes=" << candBytes
              << ",saving_pct=" << 100.0 * (double(baseWire.size()) - candBytes) / baseWire.size()
              << ",selected=" << (useCandidate ? "candidate" : "baseline")
              << ",selected_bytes=" << selectedBytes
              << ",baseline_encode_p50_ms=" << pct(a.baseEnc, 50)
              << ",baseline_encode_p95_ms=" << pct(a.baseEnc, 95)
              << ",candidate_encode_p50_ms=" << pct(a.candEnc, 50)
              << ",candidate_encode_p95_ms=" << pct(a.candEnc, 95)
              << ",baseline_decode_p50_ms=" << pct(a.baseDec, 50)
              << ",candidate_decode_cached_p50_ms=" << pct(a.cachedDec, 50)
              << ",candidate_decode_uncached_p50_ms=" << pct(a.uncachedDec, 50)
              << ",candidate_decode_cached_p95_ms=" << pct(a.cachedDec, 95)
              << ",candidate_decode_uncached_p95_ms=" << pct(a.uncachedDec, 95) << ",exact=1\n";
    return true;
}
static bool decode_only(const std::filesystem::path& oldpath, const std::filesystem::path& nowpath,
                        const std::filesystem::path& basepath,
                        const std::filesystem::path& wirepath,
                        const std::filesystem::path& csvpath) {
    Bytes old = read(oldpath), now = read(nowpath), base = read(basepath), wire = read(wirepath);
    NativeInfo oi;
    if (!build_native(old, oi))
        return false;
    Bytes initial;
    if (!unpack_current(base, initial) || initial != now)
        return false;
    Results a;
    std::ofstream csv;
    if (!csvpath.empty())
        csv.open(csvpath);
    if (csv)
        csv << "round,position,mode,elapsed_ms\n";
    std::mt19937 rng(0x4e58564d);
    for (int i = 0; i < 176; ++i) {
        std::array<int, 3> order{0, 1, 2};
        std::shuffle(order.begin(), order.end(), rng);
        for (int pos = 0; pos < 3; ++pos) {
            int mode = order[pos];
            auto t = Clock::now();
            Bytes rec;
            bool ok = false;
            if (mode == 0)
                ok = unpack_current(base, rec);
            else if (mode == 1)
                ok = decode_candidate(old, oi, wire, rec);
            else {
                NativeInfo uncached;
                ok = build_native(old, uncached) && decode_candidate(old, uncached, wire, rec);
            }
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
            if (!ok || rec != now)
                return false;
            if (i >= 16) {
                if (mode == 0)
                    a.baseDec.push_back(ms);
                else if (mode == 1)
                    a.cachedDec.push_back(ms);
                else
                    a.uncachedDec.push_back(ms);
                if (csv)
                    csv << i << ',' << pos << ','
                        << (mode == 0   ? "baseline"
                            : mode == 1 ? "cached"
                                        : "uncached")
                        << ',' << ms << '\n';
            }
        }
    }
    std::cout << "decode_only,baseline_bytes=" << base.size() << ",candidate_bytes=" << wire.size()
              << ",samples=160,baseline_decode_p50_ms=" << pct(a.baseDec, 50)
              << ",baseline_decode_p95_ms=" << pct(a.baseDec, 95)
              << ",baseline_decode_p99_ms=" << pct(a.baseDec, 99)
              << ",candidate_decode_cached_p50_ms=" << pct(a.cachedDec, 50)
              << ",candidate_decode_cached_p95_ms=" << pct(a.cachedDec, 95)
              << ",candidate_decode_cached_p99_ms=" << pct(a.cachedDec, 99)
              << ",candidate_decode_uncached_p50_ms=" << pct(a.uncachedDec, 50)
              << ",candidate_decode_uncached_p95_ms=" << pct(a.uncachedDec, 95)
              << ",candidate_decode_uncached_p99_ms=" << pct(a.uncachedDec, 99) << ",exact=1\n";
    return true;
}
int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) == "--decode-only")
        return decode_only(argv[2], argv[3], argv[4], argv[5], {}) ? 0 : 2;
    if (argc == 7 && std::string(argv[1]) == "--decode-only")
        return decode_only(argv[2], argv[3], argv[4], argv[5], argv[6]) ? 0 : 2;
    std::filesystem::path out;
    int arg = 1;
    if (argc >= 3 && std::string(argv[1]) == "--export") {
        out = argv[2];
        arg = 3;
    }
    if (arg == argc) {
        std::cerr << "usage: bench [--export DIR] old.nxdf current.nxdf [old.nxdf current.nxdf "
                     "...]\n       bench --decode-only old.nxdf current.nxdf baseline.bin "
                     "candidate.native-motion.bin [samples.csv]\n";
        return 2;
    }
    if ((argc - arg) % 2)
        return 2;
    for (; arg < argc; arg += 2) {
        auto old = std::filesystem::path(argv[arg]), now = std::filesystem::path(argv[arg + 1]);
        if (!run_pair(old.stem().string() + "->" + now.stem().string(), old, now, out))
            return 1;
    }
    return 0;
}
