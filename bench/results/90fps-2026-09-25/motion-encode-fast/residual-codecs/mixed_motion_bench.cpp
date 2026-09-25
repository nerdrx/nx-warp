// Scratch-only, controlled regional-motion probe based on native_motion_bench.cpp.
#define main archived_native_motion_main
#include "native_motion_bench.cpp"
#undef main

using Vec = std::pair<int, int>;

static unsigned region_count(unsigned mode) { return mode == 3 ? 4 : 2; }

static unsigned region_id(unsigned mode, int tx, int ty) {
    constexpr int origin = (int(W) - 256) / 2;
    unsigned qx = unsigned(tx * 32 + 16 >= origin + 128);
    unsigned qy = unsigned(ty * 32 + 16 >= origin + 128);
    return mode == 1 ? qx : mode == 2 ? qy : 2 * qy + qx;
}

static Bytes make_regional_frame(std::span<const uint8_t> base, const NativeInfo& info,
                                 unsigned mode, const std::array<Vec, 4>& vectors) {
    Bytes now(base.begin(), base.end());
    constexpr int origin = (int(W) - 256) / 2;
    for (uint32_t tile = 0; tile < TILES; ++tile) {
        int32_t dst = info.pixel[tile];
        if (dst < 0) continue;
        int ty = tile / (W / 32 * E), rem = tile % (W / 32 * E);
        int tx = rem % (W / 32);
        const auto [dx, dy] = vectors[region_id(mode, tx, ty)];
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const uint8_t* src = nullptr;
            get_pixel(base, info, rem / (W / 32), tx * 32 + x + dx,
                      ty * 32 + y + dy, src);
            size_t at = size_t(dst) + (size_t(y) * 32 + x) * 4;
            for (int c = 0; c < 4; ++c) now[at + c] = src ? src[c] : 0;
        }
    }
    (void)origin;
    return now;
}

static std::array<Motion, 4> estimate_regions(std::span<const uint8_t> old, const NativeInfo& oi,
                                              std::span<const uint8_t> now, const NativeInfo& ni,
                                              unsigned mode) {
    constexpr int origin = (int(W) - 256) / 2;
    struct Sample { uint8_t eye; int x, y; };
    std::array<Motion, 4> result{};
    for (unsigned region = 0; region < region_count(mode); ++region) {
        int x0 = origin, y0 = origin, x1 = origin + 256, y1 = origin + 256;
        if (mode == 1) { x0 += int(region) * 128; x1 = x0 + 128; }
        else if (mode == 2) { y0 += int(region) * 128; y1 = y0 + 128; }
        else { x0 += int(region % 2) * 128; x1 = x0 + 128;
               y0 += int(region / 2) * 128; y1 = y0 + 128; }
        std::vector<Sample> samples;
        for (unsigned eye = 0; eye < E; ++eye)
            for (int y = y0 + 16; y < y1 - 16; y += 8)
                for (int x = x0 + 16; x < x1 - 16; x += 8) {
                    const uint8_t *a, *b;
                    if (get_pixel(now, ni, eye, x, y, a) && get_pixel(old, oi, eye, x, y, b))
                        samples.push_back({uint8_t(eye), x, y});
                }
        Motion best{}; best.sad = 1e9;
        auto test = [&](int dx, int dy) {
            uint64_t cost = 0; size_t hits = 0;
            for (const auto& q : samples) {
                const uint8_t *a, *b;
                if (!get_pixel(now, ni, q.eye, q.x, q.y, a) ||
                    !get_pixel(old, oi, q.eye, q.x + dx, q.y + dy, b)) continue;
                cost += std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1])) +
                        std::abs(int(a[2]) - int(b[2]));
                ++hits;
            }
            if (hits < samples.size() / 2) return;
            double score = double(cost) / (hits * 3);
            if (score < best.sad - 1e-9 ||
                (std::abs(score - best.sad) < 1e-9 &&
                 std::abs(dx) + std::abs(dy) < std::abs(best.dx) + std::abs(best.dy)))
                best = {dx, dy, score, hits};
        };
        for (int dy = -16; dy <= 16; dy += 4)
            for (int dx = -16; dx <= 16; dx += 4) test(dx, dy);
        int cx = best.dx, cy = best.dy;
        for (int dy = std::max(-16, cy - 3); dy <= std::min(16, cy + 3); ++dy)
            for (int dx = std::max(-16, cx - 3); dx <= std::min(16, cx + 3); ++dx) test(dx, dy);
        result[region] = best;
    }
    return result;
}

static Bytes regional_residual(std::span<const uint8_t> base, const NativeInfo& bi,
                               std::span<const uint8_t> now, const NativeInfo& ni,
                               unsigned mode, const std::array<Vec, 4>& vectors) {
    Bytes residual(now.begin(), now.end());
    for (uint32_t tile = 0; tile < TILES; ++tile) {
        int32_t dst = ni.pixel[tile];
        if (dst < 0) continue;
        int ty = tile / (W / 32 * E), rem = tile % (W / 32 * E);
        int tx = rem % (W / 32);
        const auto [dx, dy] = vectors[region_id(mode, tx, ty)];
        unsigned eye = rem / (W / 32);
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const uint8_t* src = nullptr;
            get_pixel(base, bi, eye, tx * 32 + x + dx, ty * 32 + y + dy, src);
            size_t at = size_t(dst) + (size_t(y) * 32 + x) * 4;
            for (int c = 0; c < 4; ++c)
                residual[at + c] = uint8_t(now[at + c] - (src ? src[c] : 0));
        }
    }
    return residual;
}

static void regional_inverse(std::span<const uint8_t> base, const NativeInfo& bi,
                             const NativeInfo& ni, Bytes& residual,
                             unsigned mode, const std::array<Vec, 4>& vectors) {
    for (uint32_t tile = 0; tile < TILES; ++tile) {
        int32_t dst = ni.pixel[tile];
        if (dst < 0) continue;
        int ty = tile / (W / 32 * E), rem = tile % (W / 32 * E);
        int tx = rem % (W / 32);
        const auto [dx, dy] = vectors[region_id(mode, tx, ty)];
        unsigned eye = rem / (W / 32);
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const uint8_t* src = nullptr;
            get_pixel(base, bi, eye, tx * 32 + x + dx, ty * 32 + y + dy, src);
            if (!src) continue;
            size_t at = size_t(dst) + (size_t(y) * 32 + x) * 4;
            for (int c = 0; c < 4; ++c) residual[at + c] = uint8_t(residual[at + c] + src[c]);
        }
    }
}

static bool run(std::string name, const std::filesystem::path& path, unsigned mode,
                std::array<Vec, 4> vectors) {
    Bytes base = read(path);
    NativeInfo bi;
    if (!build_native(base, bi)) return false;
    Bytes now = make_regional_frame(base, bi, mode, vectors);
    NativeInfo ni;
    if (!build_native(now, ni)) return false;
    Bytes baseWire = pack_current(now), decodedBase;
    if (!unpack_current(baseWire, decodedBase) || decodedBase != now) return false;

    Motion global{};
    std::array<Motion, 4> regional{};
    std::vector<double> globalEst, localEst, globalEnc, localEnc;
    for (int i = 0; i < 32; ++i) {
        auto start = Clock::now();
        global = estimate(base, bi, now, ni);
        double gt = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        start = Clock::now();
        regional = estimate_regions(base, bi, now, ni, mode);
        double lt = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (i >= 8) { globalEst.push_back(gt); localEst.push_back(lt); }
    }
    Bytes globalResidual = residual_frame(base, bi, now, ni, global.dx, global.dy);
    Bytes globalWire = make_wire(global.dx, global.dy, 1, pack_current(globalResidual));
    Bytes globalRound;
    bool globalExact = decode_candidate(base, bi, globalWire, globalRound) && globalRound == now;

    std::array<Vec, 4> estimated{};
    for (unsigned i = 0; i < region_count(mode); ++i)
        estimated[i] = {regional[i].dx, regional[i].dy};
    Bytes localResidual = regional_residual(base, bi, now, ni, mode, estimated);
    Bytes localBody = pack_current(localResidual);
    unsigned groups = region_count(mode);
    Bytes metadata(4 + groups * 4);
    metadata[0] = uint8_t(mode); // fixed split: left/right, top/bottom, or four quadrants
    metadata[1] = uint8_t(groups);
    for (unsigned i = 0; i < groups; ++i) {
        uint16_t dx = uint16_t(int16_t(estimated[i].first));
        uint16_t dy = uint16_t(int16_t(estimated[i].second));
        metadata[4 + i * 4 + 0] = uint8_t(dx);
        metadata[4 + i * 4 + 1] = uint8_t(dx >> 8);
        metadata[4 + i * 4 + 2] = uint8_t(dy);
        metadata[4 + i * 4 + 3] = uint8_t(dy >> 8);
    }
    metadata.insert(metadata.end(), localBody.begin(), localBody.end());
    Bytes localWire = make_wire(0, 0, 1, metadata);
    Bytes unpackedLocal, localRound;
    bool localExact = unpack_current(std::span<const uint8_t>(localWire).subspan(24 + 4 + groups * 4),
                                     unpackedLocal);
    NativeInfo ri;
    localExact = localExact && build_native(unpackedLocal, ri);
    if (localExact) {
        regional_inverse(base, bi, ri, unpackedLocal, mode, estimated);
        localRound = std::move(unpackedLocal);
        localExact = localRound == now;
    }
    const double globalSave = 100.0 * (double(baseWire.size()) - globalWire.size()) / baseWire.size();
    const double localSave = 100.0 * (double(baseWire.size()) - localWire.size()) / baseWire.size();
    for (int i = 0; i < 32; ++i) {
        auto start = Clock::now();
        auto r = residual_frame(base, bi, now, ni, global.dx, global.dy);
        auto b = pack_current(r);
        auto gw = make_wire(global.dx, global.dy, 1, b);
        double gt = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        start = Clock::now();
        auto lr = regional_residual(base, bi, now, ni, mode, estimated);
        auto lb = pack_current(lr);
        auto lm = metadata;
        lm.insert(lm.end(), lb.begin(), lb.end());
        auto lw = make_wire(0, 0, 1, lm);
        double lt = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (i >= 8) { globalEnc.push_back(gt); localEnc.push_back(lt); }
        (void)gw; (void)lw;
    }
    std::cout << name << ',' << path.filename().string() << ',' << ni.count << ',' << groups
              << ',' << global.dx << ',' << global.dy << ',' << baseWire.size() << ','
              << globalWire.size() << ',' << globalSave << ',' << globalExact << ','
              << localWire.size() << ',' << localSave << ',' << localExact
              << ',' << regional[0].dx << ':' << regional[0].dy << ':' << regional[0].sad << ':' << regional[0].hits
              << ',' << regional[1].dx << ':' << regional[1].dy << ':' << regional[1].sad << ':' << regional[1].hits
              << ',' << regional[2].dx << ':' << regional[2].dy << ':' << regional[2].sad << ':' << regional[2].hits
              << ',' << regional[3].dx << ':' << regional[3].dy << ':' << regional[3].sad << ':' << regional[3].hits
              << ',' << pct(globalEst, 50) << ',' << pct(localEst, 50)
              << ',' << pct(globalEnc, 50) << ',' << pct(localEnc, 50) << '\n';
    return globalExact && localExact;
}

int archived_mixed_main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::cout << "sequence,source,native_tiles,local_groups,global_dx,global_dy,baseline_bytes,global_bytes,global_saved_pct,global_exact,local_bytes,local_saved_pct,local_exact,r0_dx:dy:sad:hits,r1_dx:dy:sad:hits,r2_dx:dy:sad:hits,r3_dx:dy:sad:hits,global_est_p50_ms,local_est_p50_ms,global_encode_p50_ms,local_encode_p50_ms\n";
    std::array<Vec, 4> splitX{{{8, 0}, {-8, 0}, {0, 0}, {0, 0}}};
    std::array<Vec, 4> splitY{{{0, 8}, {0, -8}, {0, 0}, {0, 0}}};
    std::array<Vec, 4> quad{{{8, 0}, {-8, 0}, {0, 8}, {0, -8}}};
    return run("forest-x-halves", argv[1], 1, splitX) &&
           run("dark-y-halves", argv[2], 2, splitY) &&
           run("forest-four-quadrants", argv[1], 3, quad) ? 0 : 1;
}
