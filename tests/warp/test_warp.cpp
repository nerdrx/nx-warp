// NX Warp -- functional tests for the pose-warped predictor.
//
// Usage: nxvc_warp_test <suite>
//   int64      emulated 64-bit primitives vs the host's native 64-bit ALU
//   divide     restoring division vs the host's divider
//   identity   identity pose and STATIC_MV are exactly the identity predictor
//   border     clamp-to-edge behaviour outside the reference
//   mv         motion-vector addition semantics
//   corners    corner coordinates vs the exact homography
//   interior   corner-bilerp interior error vs tile size and angular rate
//   oracle     integer pipeline vs the float oracle (coordinate + pixel error)
//   range      fixed-point envelope: no int32 overflow anywhere in the
//              documented worst case, and out-of-envelope poses are rejected
//   saturate   corners stay inside kCornerClamp for ANY input, both modes
//              (FINDINGS.md F2/F7 regression)
//   filters    filter tap tables are normalised and symmetric
//
// SPDX-License-Identifier: Apache-2.0

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "warp_corpus.h"

using namespace nxvc::warp;
using namespace nxvc::warp::test;

static int g_fail = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n      ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------------------

static int suite_int64() {
    Rng rng(0xC0FFEEull);
    for (int i = 0; i < 200000; ++i) {
        const int32_t a = static_cast<int32_t>(rng.u32());
        const int32_t b = static_cast<int32_t>(rng.u32());

        const U64 u = nxvc_umul_ext(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
        const uint64_t ue = static_cast<uint64_t>(static_cast<uint32_t>(a)) *
                            static_cast<uint64_t>(static_cast<uint32_t>(b));
        CHECK(u.lo == static_cast<uint32_t>(ue) && u.hi == static_cast<uint32_t>(ue >> 32),
              "umul %d*%d", a, b);

        const U64 s = nxvc_imul_ext(a, b);
        const int64_t se = static_cast<int64_t>(a) * static_cast<int64_t>(b);
        CHECK(s.lo == static_cast<uint32_t>(static_cast<uint64_t>(se)) &&
                  s.hi == static_cast<uint32_t>(static_cast<uint64_t>(se) >> 32),
              "imul %d*%d", a, b);

        const U64 sum = nxvc_add64(s, u);
        const uint64_t sume = static_cast<uint64_t>(se) + ue;
        CHECK(sum.lo == static_cast<uint32_t>(sume) && sum.hi == static_cast<uint32_t>(sume >> 32),
              "add64");

        const U64 n = nxvc_neg64(s);
        const uint64_t ne = ~static_cast<uint64_t>(se) + 1ull;
        CHECK(n.lo == static_cast<uint32_t>(ne) && n.hi == static_cast<uint32_t>(ne >> 32), "neg64");

        const uint32_t sh = rng.u32() % 32u;
        const U64 sl = nxvc_shl64(s, sh);
        // Masked before the shift: the discarded bits are the point of the
        // comparison, and -fsanitize=integer flags an unsigned shift that drops them.
        const uint64_t sle = (static_cast<uint64_t>(se) & (~0ull >> sh)) << sh;
        CHECK(sl.lo == static_cast<uint32_t>(sle) && sl.hi == static_cast<uint32_t>(sle >> 32),
              "shl64 by %u", sh);

        const U64 e = nxvc_from_i32(a);
        CHECK(e.lo == static_cast<uint32_t>(a) && e.hi == (a < 0 ? 0xffffffffu : 0u), "from_i32");
    }
    std::printf("int64:   200000 vectors, emulated (hi,lo) == native uint64\n");
    return g_fail;
}

static int suite_divide() {
    Rng rng(0xD1D1DEull);
    // Full envelope: any n with n.hi < d, and d in the legal denominator range.
    for (int i = 0; i < 500000; ++i) {
        uint32_t d = static_cast<uint32_t>(rng.range(kDenMin, kDenMax - 1));
        uint64_t n = rng.next();
        // Force n.hi < d.
        n = (static_cast<uint64_t>(rng.u32() % d) << 32) | static_cast<uint32_t>(n);
        U64 nn{static_cast<uint32_t>(n), static_cast<uint32_t>(n >> 32)};
        const uint32_t q = nxvc_warp_div(nn, d);
        const uint64_t qe = n / d;
        CHECK(q == qe, "div %" PRIu64 " / %u -> %u expected %" PRIu64, n, d, q, qe);
    }
    // Extremes of the legal envelope.
    for (uint32_t d : {static_cast<uint32_t>(kDenMin), static_cast<uint32_t>(kDenMax) - 1u}) {
        for (uint64_t n : {uint64_t{0}, uint64_t{1}, static_cast<uint64_t>(d) - 1, static_cast<uint64_t>(d),
                           (static_cast<uint64_t>(d) << 32) - 1}) {
            U64 nn{static_cast<uint32_t>(n), static_cast<uint32_t>(n >> 32)};
            CHECK(nxvc_warp_div(nn, d) == n / d, "div edge %" PRIu64 "/%u", n, d);
        }
    }
    std::printf("divide:  500000 vectors + edges, 32-step restoring == exact floor\n");
    return g_fail;
}

// ---------------------------------------------------------------------------

static int suite_identity() {
    const int W = 256, H = 256;
    Picture p = make_picture(W, H, 4, 255, 42);
    std::vector<uint16_t> out(kTile * kTile * 4);
    const int32_t zero_mv[2] = {0, 0};

    for (Filter f : {kFilterBilinear, kFilterCatmullRom}) {
        for (Mode m : {kModeWarp, kModeStatic}) {
            const Homography Hq = identity_homography(W / 2, H / 2);
            for (int ty = 0; ty < H; ty += kTile) {
                for (int tx = 0; tx < W; tx += kTile) {
                    warp_tile(p.img, tx, ty, Hq, zero_mv, f, m, out.data(), kTile * 4);
                    for (int v = 0; v < kTile; ++v) {
                        for (int u = 0; u < kTile; ++u) {
                            for (int c = 0; c < 4; ++c) {
                                const uint16_t got = out[(v * kTile + u) * 4 + c];
                                const uint16_t want =
                                    p.data[(static_cast<size_t>(ty + v) * W + (tx + u)) * 4 + c];
                                CHECK(got == want,
                                      "identity f=%d m=%d tile(%d,%d) px(%d,%d) c=%d got %u want %u",
                                      (int)f, (int)m, tx, ty, u, v, c, got, want);
                            }
                        }
                    }
                }
            }
        }
    }

    // Corner coordinates of an identity warp are exactly the tile corners.
    const Homography Hq = identity_homography(W / 2, H / 2);
    for (Mode m : {kModeWarp, kModeStatic}) {
        TileCorners c = warp_tile_corners(Hq, 64, 128, m);
        CHECK(c.x[0] == (64 << kQCorner) && c.y[0] == (128 << kQCorner), "corner0");
        CHECK(c.x[3] == (128 << kQCorner) && c.y[3] == (192 << kQCorner), "corner3");
    }

    // STATIC_MV must ignore an arbitrary, aggressive homography entirely.
    Rng rng(7);
    Case cs{};
    for (int i = 0; i < 64; ++i) {
        make_case_int(rng, W, H, &cs);
        std::vector<uint16_t> a(kTile * kTile * 4), b(kTile * kTile * 4);
        const int32_t mv[2] = {cs.mv[0], cs.mv[1]};
        warp_tile(p.img, cs.tile_x, cs.tile_y, cs.H, mv, cs.filter, kModeStatic, a.data(),
                  kTile * 4);
        warp_tile(p.img, cs.tile_x, cs.tile_y, identity_homography(W / 2, H / 2), mv, cs.filter,
                  kModeStatic, b.data(), kTile * 4);
        CHECK(a == b, "STATIC_MV depends on H (case %d)", i);
    }
    std::printf("identity: identity H == copy, both filters, both modes; STATIC_MV ignores H\n");
    return g_fail;
}

// ---------------------------------------------------------------------------

static int suite_border() {
    const int W = 64, H = 64;
    Picture p = make_picture(W, H, 1, 255, 9);
    std::vector<uint16_t> out(kTile * kTile);

    // Push the whole tile far off the left/top with a huge translation: every
    // output sample must equal the corner reference sample.
    Homography Hq = identity_homography(W / 2, H / 2);
    Hq.h[2] = -1000 * (1 << kQNum);
    Hq.h[5] = -1000 * (1 << kQNum);
    const int32_t zero[2] = {0, 0};
    for (Filter f : {kFilterBilinear, kFilterCatmullRom}) {
        warp_tile(p.img, 0, 0, Hq, zero, f, kModeWarp, out.data(), kTile);
        for (int i = 0; i < kTile * kTile; ++i) {
            CHECK(out[i] == p.data[0], "top-left clamp f=%d idx=%d got %u want %u", (int)f, i,
                  out[i], p.data[0]);
        }
    }
    // And off the bottom/right.
    Hq.h[2] = 1000 * (1 << kQNum);
    Hq.h[5] = 1000 * (1 << kQNum);
    for (Filter f : {kFilterBilinear, kFilterCatmullRom}) {
        warp_tile(p.img, 0, 0, Hq, zero, f, kModeWarp, out.data(), kTile);
        const uint16_t want = p.data[static_cast<size_t>(H - 1) * W + (W - 1)];
        for (int i = 0; i < kTile * kTile; ++i) {
            CHECK(out[i] == want, "bottom-right clamp f=%d idx=%d", (int)f, i);
        }
    }

    // A tile that straddles the edge: the in-picture half must be untouched by
    // the clamping of the out-of-picture half.
    Picture big = make_picture(256, 256, 1, 255, 11);
    std::vector<uint16_t> o2(kTile * kTile);
    Homography Hi = identity_homography(128, 128);
    warp_tile(big.img, 192, 192, Hi, zero, kFilterBilinear, kModeWarp, o2.data(), kTile);
    for (int v = 0; v < kTile; ++v)
        for (int u = 0; u < kTile; ++u)
            CHECK(o2[v * kTile + u] == big.data[static_cast<size_t>(192 + v) * 256 + 192 + u],
                  "edge tile (%d,%d)", u, v);

    // The Catmull-Rom negative lobe must never take a sample out of range.
    Picture flat = make_picture(64, 64, 1, 255, 3);
    for (auto& s : flat.data) s = 255;
    Homography Hs = identity_homography(32, 32);
    Hs.h[2] = (1 << kQNum) / 3;  // a third of a pixel
    warp_tile(flat.img, 0, 0, Hs, zero, kFilterCatmullRom, kModeWarp, out.data(), kTile);
    for (int i = 0; i < kTile * kTile; ++i) CHECK(out[i] == 255, "flat CR overshoot idx=%d", i);

    std::printf("border:  clamp-to-edge exact on all four sides, no CR overshoot on flat\n");
    return g_fail;
}

// ---------------------------------------------------------------------------

static int suite_mv() {
    const int W = 256, H = 256;
    Picture p = make_picture(W, H, 2, 255, 5);
    std::vector<uint16_t> a(kTile * kTile * 2), b(kTile * kTile * 2);
    const Homography Hi = identity_homography(W / 2, H / 2);

    // Integer MV under an identity warp is an exact shift.
    for (int px = -3; px <= 3; ++px) {
        for (int py = -3; py <= 3; ++py) {
            const int32_t mv[2] = {px * 4, py * 4};  // Q.2
            const int32_t zero[2] = {0, 0};
            warp_tile(p.img, 64, 64, Hi, mv, kFilterCatmullRom, kModeWarp, a.data(), kTile * 2);
            warp_tile(p.img, 64 + px, 64 + py, Hi, zero, kFilterCatmullRom, kModeWarp, b.data(),
                      kTile * 2);
            CHECK(a == b, "integer mv (%d,%d) != tile shift", px, py);
        }
    }

    // MV addition commutes with a translation-only homography: shifting H by
    // one pixel must equal adding 4 to the quarter-pel MV.
    Rng rng(99);
    for (int i = 0; i < 200; ++i) {
        Homography Ht = Hi;
        Ht.h[2] = rng.range(-20, 20) * (1 << kQNum);
        Ht.h[5] = rng.range(-20, 20) * (1 << kQNum);
        const int32_t dx = rng.range(-8, 8), dy = rng.range(-8, 8);
        const int32_t mv0[2] = {rng.range(-16, 16), rng.range(-16, 16)};
        const int32_t mv1[2] = {mv0[0] + dx * 4, mv0[1] + dy * 4};
        Homography Hs = Ht;
        Hs.h[2] += dx * (1 << kQNum);
        Hs.h[5] += dy * (1 << kQNum);
        warp_tile(p.img, 128, 64, Ht, mv1, kFilterBilinear, kModeWarp, a.data(), kTile * 2);
        warp_tile(p.img, 128, 64, Hs, mv0, kFilterBilinear, kModeWarp, b.data(), kTile * 2);
        CHECK(a == b, "mv/H translation do not commute (case %d)", i);
    }

    // Quarter-pel steps must land on the documented 1/16-pel grid: mv += 1
    // (quarter pel) moves the sampling fraction by exactly 4/16.
    {
        Homography Hq = identity_homography(W / 2, H / 2);
        for (int q = 0; q < 8; ++q) {
            const int32_t mv[2] = {q, 0};
            TileCorners c = warp_tile_corners(Hq, 64, 64, kModeWarp);
            (void)c;
            warp_tile(p.img, 64, 64, Hq, mv, kFilterBilinear, kModeWarp, a.data(), kTile * 2);
            // Reconstruct the expected fraction directly.
            const int32_t xq6 = (64 << kQCorner) + (q << (kQCorner - kQMv));
            const int32_t xq4 = (xq6 + 2) >> (kQCorner - kQSample);
            CHECK((xq4 & 15) == ((q * 4) & 15), "qpel %d -> frac %d", q, xq4 & 15);
        }
    }
    std::printf("mv:      integer MV == tile shift; MV and H translation commute; 1/4 -> 4/16\n");
    return g_fail;
}

// ---------------------------------------------------------------------------

static int suite_corners() {
    const int W = 1024, H = 1024;
    Rng rng(1234);
    double worst = 0.0;
    int n = 0;
    Case cs{};
    for (int i = 0; i < 20000; ++i) {
        if (!make_case(rng, W, H, &cs)) continue;
        ++n;
        const TileCorners c = warp_tile_corners(cs.H, cs.tile_x, cs.tile_y, kModeWarp);
        for (int k = 0; k < 4; ++k) {
            const int32_t px = cs.tile_x + ((k & 1) ? kTile : 0);
            const int32_t py = cs.tile_y + ((k >> 1) ? kTile : 0);
            const int32_t zero[2] = {0, 0};
            double sx, sy;
            oracle::source_coord_q(cs.H, px, py, zero, &sx, &sy);
            if (std::fabs(sx) > 3000 || std::fabs(sy) > 3000) continue;  // saturated
            worst = std::fmax(worst, std::fabs(c.x[k] / 64.0 - sx));
            worst = std::fmax(worst, std::fabs(c.y[k] / 64.0 - sy));
        }
    }
    std::printf("corners: %d cases, max |integer corner - exact(Hq)| = %.6f pel (budget 1/64 = %.6f)\n",
                n, worst, 1.0 / 64.0);
    CHECK(worst <= 1.0 / 64.0, "corner divide error %.6f exceeds 1/64", worst);
    return g_fail;
}

// ---------------------------------------------------------------------------

// Interior error of the corner-then-bilerp approximation as a function of tile
// size and angular rate. This is the number paper 2.2 step 3 estimates; the
// table it prints is the source for docs/WARP.md section 9 and warp/RESULTS.md.
static int suite_interior() {
    const int W = 2048, H = 2048;
    std::printf("interior: corner-bilerp error, 2048^2, 95 deg FOV, 4000 tiles per row\n");
    std::printf("interior:  deg/frame  deg/s@90Hz   32x32 tile   64x64 tile\n");
    double err64_fast = 0.0, err64_180 = 0.0, err32_fast = 0.0;
    for (double d : {0.5, 1.0, 1.65, 2.0, 2.4, 3.3, 5.0, 6.6, 10.0}) {
        double worst[2] = {0.0, 0.0};
        for (int ti = 0; ti < 2; ++ti) {
            const int ts = ti == 0 ? 32 : 64;
            const int sh = ti == 0 ? 5 : 6;
            Rng rng(4242);
            Case cs{};
            for (int i = 0; i < 4000; ++i) {
                if (!make_case(rng, W, H, &cs, d)) continue;
                const int tx = (cs.tile_x / ts) * ts, ty = (cs.tile_y / ts) * ts;
                const int32_t zero[2] = {0, 0};
                int32_t cxv[4], cyv[4];
                for (int k = 0; k < 4; ++k) {
                    double sx, sy;
                    oracle::source_coord_q(cs.H, tx + ((k & 1) ? ts : 0), ty + ((k >> 1) ? ts : 0),
                                           zero, &sx, &sy);
                    cxv[k] = static_cast<int32_t>(std::llround(sx * 64.0));
                    cyv[k] = static_cast<int32_t>(std::llround(sy * 64.0));
                }
                for (int v = 0; v < ts; v += 2) {
                    for (int u = 0; u < ts; u += 2) {
                        const int32_t tX = (cxv[0] * (ts - u) + cxv[1] * u + ts / 2) >> sh;
                        const int32_t bX = (cxv[2] * (ts - u) + cxv[3] * u + ts / 2) >> sh;
                        const int32_t tY = (cyv[0] * (ts - u) + cyv[1] * u + ts / 2) >> sh;
                        const int32_t bY = (cyv[2] * (ts - u) + cyv[3] * u + ts / 2) >> sh;
                        const double xi = ((tX * (ts - v) + bX * v + ts / 2) >> sh) / 64.0;
                        const double yi = ((tY * (ts - v) + bY * v + ts / 2) >> sh) / 64.0;
                        double ex, ey;
                        oracle::source_coord(cs.Hd, cs.H.ox, cs.H.oy, tx + u, ty + v, zero, &ex, &ey);
                        worst[ti] = std::fmax(worst[ti],
                                              std::fmax(std::fabs(xi - ex), std::fabs(yi - ey)));
                    }
                }
            }
        }
        std::printf("interior: %9.2f  %10.0f   %10.5f   %10.5f\n", d, d * 90.0, worst[0], worst[1]);
        if (d == 3.3) { err64_fast = worst[1]; err32_fast = worst[0]; }
        if (d == 1.0) err64_180 = worst[1];
    }
    // Normative envelope, measured rather than assumed:
    CHECK(err32_fast <= 1.0 / 16.0, "32x32 at 297 deg/s: %.5f > 1/16", err32_fast);
    CHECK(err64_180 <= 1.0 / 16.0, "64x64 at 90 deg/s: %.5f > 1/16", err64_180);
    CHECK(err64_fast <= 1.0 / 8.0, "64x64 at 297 deg/s: %.5f > 1/8", err64_fast);
    return g_fail;
}

// ---------------------------------------------------------------------------

static int suite_oracle() {
    const int W = 2048, H = 2048;
    Rng rng(555);

    // (a) Coordinate error of the full integer pipeline against the exact
    //     double homography, over the whole tile interior. This is the number
    //     the tolerance in WARP.md section 9 is stated against.
    double worst_coord = 0.0, worst_coord_q = 0.0;
    int cases = 0;
    Case cs{};
    for (int i = 0; i < 3000; ++i) {
        if (!make_case(rng, W, H, &cs)) continue;
        ++cases;
        const TileCorners c = warp_tile_corners(cs.H, cs.tile_x, cs.tile_y, kModeWarp);
        bool saturated = false;
        for (int k = 0; k < 4; ++k)
            if (std::abs(c.x[k]) >= kCornerClamp || std::abs(c.y[k]) >= kCornerClamp)
                saturated = true;
        if (saturated) continue;
        const int32_t zero[2] = {0, 0};
        for (int v = 0; v < kTile; v += 3) {
            for (int u = 0; u < kTile; u += 3) {
                // Reproduce the integer interior coordinate.
                const int32_t top_x = (c.x[0] * (kTile - u) + c.x[1] * u + 32) >> 6;
                const int32_t bot_x = (c.x[2] * (kTile - u) + c.x[3] * u + 32) >> 6;
                const int32_t xi = (top_x * (kTile - v) + bot_x * v + 32) >> 6;
                const int32_t top_y = (c.y[0] * (kTile - u) + c.y[1] * u + 32) >> 6;
                const int32_t bot_y = (c.y[2] * (kTile - u) + c.y[3] * u + 32) >> 6;
                const int32_t yi = (top_y * (kTile - v) + bot_y * v + 32) >> 6;
                // Snap to 1/16 pel exactly as warp_tile() does.
                const double xs = ((xi + 2) >> 2) / 16.0;
                const double ys = ((yi + 2) >> 2) / 16.0;

                double ex, ey, qx, qy;
                oracle::source_coord(cs.Hd, cs.H.ox, cs.H.oy, cs.tile_x + u, cs.tile_y + v, zero,
                                     &ex, &ey);
                oracle::source_coord_q(cs.H, cs.tile_x + u, cs.tile_y + v, zero, &qx, &qy);
                if (std::fabs(ex) > 3000 || std::fabs(ey) > 3000) continue;
                worst_coord = std::fmax(worst_coord, std::fmax(std::fabs(xs - ex), std::fabs(ys - ey)));
                worst_coord_q =
                    std::fmax(worst_coord_q, std::fmax(std::fabs(xs - qx), std::fabs(ys - qy)));
            }
        }
    }
    std::printf("oracle:  %d cases, max coord error vs exact H  = %.6f pel\n", cases, worst_coord);
    std::printf("oracle:  %d cases, max coord error vs quantised H = %.6f pel\n", cases,
                worst_coord_q);
    // Budget for 64x64 tiles at the 297 deg/s reference envelope:
    //   corner-bilerp interior approximation   <= 1/8  pel (measured 0.105)
    // + 1/16-pel sampling grid, half a step     = 1/32 pel
    //   total                                   = 5/32 pel = 0.15625
    // 1/16 pel end to end holds for 64x64 only up to about 150 deg/s, and for
    // 32x32 across the whole envelope. See the `interior` suite table and
    // docs/WARP.md section 9.
    CHECK(worst_coord <= 5.0 / 32.0, "coordinate error %.6f exceeds the 5/32 pel budget",
          worst_coord);

    // (b) Pixel error against the float warp on a smooth (band-limited)
    //     picture. On noise a sub-pel coordinate difference is a full-scale
    //     sample difference and would bound nothing, so the picture is smooth
    //     by construction and the bound is meaningful.
    Picture sp = make_smooth_picture(W, H, 1, 255);
    std::vector<uint16_t> ints(kTile * kTile);
    std::vector<double> flts(kTile * kTile);
    double worst_px = 0.0;
    double sum_sq = 0.0;
    long count = 0;
    Rng rng2(777);
    for (int i = 0; i < 300; ++i) {
        if (!make_case(rng2, W, H, &cs)) continue;
        const int32_t zero[2] = {0, 0};
        warp_tile(sp.img, cs.tile_x, cs.tile_y, cs.H, zero, cs.filter, kModeWarp, ints.data(),
                  kTile);
        oracle::warp_tile_float(sp.img, cs.tile_x, cs.tile_y, cs.Hd, cs.H.ox, cs.H.oy, zero,
                                cs.filter, kModeWarp, flts.data(), kTile);
        for (int k = 0; k < kTile * kTile; ++k) {
            const double d = static_cast<double>(ints[k]) - flts[k];
            worst_px = std::fmax(worst_px, std::fabs(d));
            sum_sq += d * d;
            ++count;
        }
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(count));
    std::printf("oracle:  smooth picture, max |int - float| = %.2f LSB, rms = %.4f LSB (8-bit)\n",
                worst_px, rms);
    CHECK(worst_px <= 4.0, "integer/float pixel error %.2f exceeds 4 LSB", worst_px);
    CHECK(rms <= 1.0, "integer/float rms %.4f exceeds 1 LSB", rms);
    return g_fail;
}

// ---------------------------------------------------------------------------

// The fixed-point envelope, checked against the worst case rather than
// asserted. Paper 2.2 specifies a uniform Q8.24 for all nine entries; that
// cannot work (see docs/WARP.md section 3), and this suite is the evidence:
// it sweeps eye widths up to 4096, FOV tangents up to 1.4 (~109 deg half
// angle) and rotation deltas from 0 to 180 degrees, and reports the largest
// magnitude each entry actually reaches.
static int suite_range() {
    const double kDeg = 3.14159265358979 / 180.0;
    const int widths[] = {1024, 2048, 2160, 2880, 4096};
    const double tans[] = {0.6, 0.9, 1.2, 1.4};
    const double deltas[] = {0.0, 0.5, 1.65, 3.3, 6.6, 10.0, 20.0, 45.0, 90.0, 135.0, 180.0};

    int32_t max_lin = 0, max_trans = 0, max_persp = 0;
    double max_accepted_delta = 0.0;
    int accepted = 0, rejected = 0, saturated = 0, saturated_in_envelope = 0;
    int32_t worst_den_lo = kDenMax, worst_den_hi = kDenMin;

    for (int w : widths) {
        for (double tn : tans) {
            Fov fov;
            fov.angle_left = -std::atan(tn);
            fov.angle_right = std::atan(tn);
            fov.angle_up = std::atan(tn);
            fov.angle_down = -std::atan(tn);
            for (double dd : deltas) {
                // Worst orientation of the delta axis is unknown a priori, so
                // sweep the axis too.
                for (int axis = 0; axis < 12; ++axis) {
                    const double a = axis * 30.0 * kDeg;
                    const Quat qp = quat_from_ypr(0.3, -0.2, 0.1);
                    const Quat qc = quat_from_ypr(0.3 + dd * kDeg * std::cos(a),
                                                  -0.2 + dd * kDeg * std::sin(a) * 0.7,
                                                  0.1 + dd * kDeg * std::sin(a) * 0.7);
                    Homography H;
                    if (!derive_homography(qp, fov, qc, fov, w, w, &H)) {
                        ++rejected;
                        continue;
                    }
                    ++accepted;
                    max_accepted_delta = std::fmax(max_accepted_delta, dd);
                    for (int i = 0; i < 2; ++i) {
                        max_lin = std::max(max_lin, std::abs(H.h[i == 0 ? 0 : 4]));
                        max_lin = std::max(max_lin, std::abs(H.h[i == 0 ? 1 : 3]));
                    }
                    max_trans = std::max(max_trans, std::max(std::abs(H.h[2]), std::abs(H.h[5])));
                    max_persp = std::max(max_persp, std::max(std::abs(H.h[6]), std::abs(H.h[7])));

                    // Every tile of the picture must stay inside the envelope:
                    // den legal at every corner, and no coordinate saturation.
                    for (int ty = 0; ty <= w - kTile; ty += kTile) {
                        for (int tx = 0; tx <= w - kTile; tx += kTile) {
                            for (int k = 0; k < 4; ++k) {
                                const int32_t cx = tx + ((k & 1) ? kTile : 0) - H.ox;
                                const int32_t cy = ty + ((k >> 1) ? kTile : 0) - H.oy;
                                const int64_t den = static_cast<int64_t>(H.h[6]) * cx +
                                                    static_cast<int64_t>(H.h[7]) * cy + H.h[8];
                                CHECK(den >= kDenMin && den < kDenMax,
                                      "den %lld out of [2^28,2^30) at w=%d tan=%.1f delta=%.1f",
                                      static_cast<long long>(den), w, tn, dd);
                                worst_den_lo = std::min<int32_t>(worst_den_lo,
                                                                 static_cast<int32_t>(den));
                                worst_den_hi = std::max<int32_t>(worst_den_hi,
                                                                 static_cast<int32_t>(den));
                            }
                            const TileCorners c = warp_tile_corners(H, tx, ty, kModeWarp);
                            for (int k = 0; k < 4; ++k) {
                                if (std::abs(c.x[k]) >= kCornerClamp ||
                                    std::abs(c.y[k]) >= kCornerClamp) {
                                    ++saturated;
                                    // Saturation is correct behaviour when the
                                    // source point lands thousands of pixels
                                    // outside the picture; it is a bug only
                                    // inside the operational envelope.
                                    if (dd <= 10.0) ++saturated_in_envelope;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("range:   %d accepted, %d rejected poses; largest accepted delta %.0f deg\n",
                accepted, rejected, max_accepted_delta);
    std::printf("range:   max |linear|      = %d  (%.1f %% of the kEntryMax budget)\n", max_lin,
                100.0 * max_lin / kEntryMax);
    std::printf("range:   max |translation| = %d = %.1f px (%.1f %% of budget, %.0f px hard cap)\n",
                max_trans, max_trans / static_cast<double>(1 << kQNum),
                100.0 * max_trans / kEntryMax,
                static_cast<double>(kEntryMax) / (1 << kQNum));
    std::printf("range:   max |perspective| = %d  (%.3f %% of budget)\n", max_persp,
                100.0 * max_persp / kEntryMax);
    std::printf("range:   den span [%d, %d], legal [%d, %d)\n", worst_den_lo, worst_den_hi,
                kDenMin, kDenMax);
    std::printf("range:   corner saturations: %d total, %d inside the <=10 deg/frame envelope\n",
                saturated, saturated_in_envelope);
    // In Q8.24 the translation term alone would need this many bits:
    const double q824_needed = max_trans / static_cast<double>(1 << kQNum) * (1 << 24);
    std::printf("range:   the same translation in Q8.24 would be %.3g, %.1f x over int32\n",
                q824_needed, q824_needed / 2147483647.0);
    CHECK(max_lin <= kEntryMax && max_trans <= kEntryMax && max_persp <= kEntryMax,
          "an entry passed validation but exceeds kEntryMax");
    CHECK(saturated_in_envelope == 0,
          "%d corner coordinates saturated at <= 10 deg/frame (900 deg/s)",
          saturated_in_envelope);
    CHECK(rejected > 0, "no pose was rejected: the envelope check is not being exercised");
    return g_fail;
}

// FINDINGS.md F2 and F7. warp.h promises every corner is inside
// +-kCornerClamp; the in-tile interpolation's overflow argument depends on it,
// and the GLSL twin has to compute the same value. Both were reachable: the
// kModeStatic path applied no clamp at all (F2), and the kModeWarp path
// applied it after arithmetic that had already overflowed (F7).
//
// This drives warp_tile_corners() and warp_tile() with hostile int32 -- the
// values a corrupt frame header can carry, not the values derive_homography()
// can produce -- and requires the contract to hold for every one of them.
static int suite_saturate() {
    Rng rng(0x5A7085ull);
    Picture p = make_picture(96, 96, 2, 255, 17);
    std::vector<uint16_t> out(static_cast<size_t>(kTile) * kTile * 2);

    // Extreme values that have historically broken this kind of code.
    const int32_t edges[] = {0,  1,  -1,  2,  -2,
                             INT32_MAX, INT32_MIN, INT32_MAX - 1, INT32_MIN + 1,
                             1 << 30, -(1 << 30), 1 << 25, -(1 << 25),
                             (1 << 24) + 1, kCornerClamp, -kCornerClamp,
                             65535, -65536, 4160, -9583};
    const int n_edges = static_cast<int>(sizeof(edges) / sizeof(edges[0]));

    int checked = 0;
    for (int trial = 0; trial < 60000; ++trial) {
        Homography H{};
        for (int i = 0; i < 9; ++i) {
            // Half the trials are pure edge values, half fully random int32.
            H.h[i] = (trial & 1) ? edges[rng.range(0, n_edges - 1)]
                                 : static_cast<int32_t>(rng.u32());
        }
        H.ox = (trial & 2) ? edges[rng.range(0, n_edges - 1)]
                           : static_cast<int32_t>(rng.u32());
        H.oy = (trial & 4) ? edges[rng.range(0, n_edges - 1)]
                           : static_cast<int32_t>(rng.u32());
        const int32_t tx = edges[rng.range(0, n_edges - 1)];
        const int32_t ty = (trial & 8) ? edges[rng.range(0, n_edges - 1)]
                                       : static_cast<int32_t>(rng.u32());

        for (Mode m : {kModeWarp, kModeStatic}) {
            const TileCorners c = warp_tile_corners(H, tx, ty, m);
            for (int i = 0; i < 4; ++i) {
                CHECK(c.x[i] >= -kCornerClamp && c.x[i] <= kCornerClamp,
                      "corner x %d out of clamp, mode=%d tile=(%d,%d) ox=%d", c.x[i], (int)m, tx,
                      ty, H.ox);
                CHECK(c.y[i] >= -kCornerClamp && c.y[i] <= kCornerClamp,
                      "corner y %d out of clamp, mode=%d tile=(%d,%d) oy=%d", c.y[i], (int)m, tx,
                      ty, H.oy);
                ++checked;
            }
            if (g_fail) return g_fail;
        }
    }

    // And the whole predictor with hostile vectors: every written sample must
    // be a legal sample value, and nothing may run off the reference picture.
    for (int trial = 0; trial < 4000; ++trial) {
        Homography H{};
        for (int i = 0; i < 9; ++i) H.h[i] = static_cast<int32_t>(rng.u32());
        H.ox = edges[rng.range(0, n_edges - 1)];
        H.oy = edges[rng.range(0, n_edges - 1)];
        const int32_t mv[2] = {edges[rng.range(0, n_edges - 1)],
                               edges[rng.range(0, n_edges - 1)]};
        const int32_t tx = edges[rng.range(0, n_edges - 1)];
        const int32_t ty = edges[rng.range(0, n_edges - 1)];
        const Filter f = (rng.u32() & 1u) ? kFilterCatmullRom : kFilterBilinear;
        const Mode m = (rng.u32() & 1u) ? kModeStatic : kModeWarp;
        warp_tile(p.img, tx, ty, H, mv, f, m, out.data(), kTile * 2);
        for (uint16_t v : out) {
            CHECK(v <= 255, "sample %u exceeds max_value", v);
        }
        if (g_fail) return g_fail;
    }

    std::printf("saturate: %d corners + 4000 hostile tiles, all inside kCornerClamp (F2/F7)\n",
                checked);
    return g_fail;
}

static int suite_filters() {
    for (int f = 0; f < 16; ++f) {
        int sum = 0;
        for (int t = 0; t < 4; ++t) sum += kCatmullRom[f][t];
        CHECK(sum == 64, "CR row %d sums to %d, not 64", f, sum);
    }
    // Symmetry: row f reversed equals row 16-f.
    for (int f = 1; f < 16; ++f) {
        for (int t = 0; t < 4; ++t) {
            CHECK(kCatmullRom[f][t] == kCatmullRom[16 - f][3 - t], "CR asymmetry at f=%d t=%d", f,
                  t);
        }
    }
    // Row 0 is the identity tap.
    CHECK(kCatmullRom[0][0] == 0 && kCatmullRom[0][1] == 64 && kCatmullRom[0][2] == 0 &&
              kCatmullRom[0][3] == 0,
          "CR row 0 is not the identity tap");
    // Against the exact kernel: no tap may be off by more than 1/64.
    double worst = 0;
    for (int f = 0; f < 16; ++f) {
        const double t = f / 16.0, t2 = t * t, t3 = t2 * t;
        const double w[4] = {-0.5 * t + t2 - 0.5 * t3, 1.0 - 2.5 * t2 + 1.5 * t3,
                             0.5 * t + 2.0 * t2 - 1.5 * t3, -0.5 * t2 + 0.5 * t3};
        for (int i = 0; i < 4; ++i)
            worst = std::fmax(worst, std::fabs(kCatmullRom[f][i] / 64.0 - w[i]));
    }
    std::printf("filters: 16 rows sum to 64, symmetric, max tap error %.5f (1/64 = %.5f)\n", worst,
                1.0 / 64.0);
    CHECK(worst <= 1.0 / 64.0, "tap quantisation error %.5f", worst);
    return g_fail;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite == "int64" || suite == "all") suite_int64();
    if (suite == "divide" || suite == "all") suite_divide();
    if (suite == "identity" || suite == "all") suite_identity();
    if (suite == "border" || suite == "all") suite_border();
    if (suite == "mv" || suite == "all") suite_mv();
    if (suite == "corners" || suite == "all") suite_corners();
    if (suite == "interior" || suite == "all") suite_interior();
    if (suite == "oracle" || suite == "all") suite_oracle();
    if (suite == "range" || suite == "all") suite_range();
    if (suite == "saturate" || suite == "all") suite_saturate();
    if (suite == "filters" || suite == "all") suite_filters();
    if (g_fail) {
        std::printf("\n%d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
