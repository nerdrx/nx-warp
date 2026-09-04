// stereo.disparity - the depth/disparity relation D = f*IPD/z is the whole
// basis of the STEREO seed, and the 1/16-pel sampler has to behave.
#include <cmath>
#include <cstdio>
#include <vector>

#include "analyze.h"
#include "predict.h"
#include "scene.h"

using namespace nxs;

static int g_fail = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++g_fail;                                        \
        }                                                    \
    } while (0)

static Scene plane_scene(double z) {
    Scene s;
    s.name = "plane";
    Box b;
    b.lo = {-40, -40, z};
    b.hi = {40, 40, z + 0.05};
    b.tex = Tex::kNoise;
    b.texel_per_m = 90.0;
    b.seed = 7;
    s.boxes.push_back(b);
    return s;
}

int main() {
    const int W = 256, H = 256;
    const double ipd = 0.064;
    Pose head;

    // --- filter table invariants
    {
        const i32(*f)[4] = filter_table();
        for (int p = 0; p < 16; ++p) {
            int sum = f[p][0] + f[p][1] + f[p][2] + f[p][3];
            CHECK(sum == 64, "phase %d sums to %d", p, sum);
        }
        CHECK(f[0][0] == 0 && f[0][1] == 64 && f[0][2] == 0 && f[0][3] == 0,
              "phase 0 is not the identity tap");
    }

    // --- sampler: integer positions copy, a flat field stays flat, and a
    //     half-pel sample of a linear ramp is the midpoint
    {
        Image img(32, 32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) img.at(x, y) = static_cast<u8>(4 * x);
        CHECK(sample_q4(img, 10 << 4, 5 << 4) == 40, "integer sample wrong: %d",
              sample_q4(img, 10 << 4, 5 << 4));
        CHECK(std::abs(sample_q4(img, (10 << 4) + 8, 5 << 4) - 42) <= 1, "half-pel ramp: %d",
              sample_q4(img, (10 << 4) + 8, 5 << 4));
        Image flat(16, 16);
        for (auto& p : flat.px) p = 173;
        for (int ph = 0; ph < 16; ++ph)
            CHECK(sample_q4(flat, (8 << 4) + ph, (8 << 4) + ph) == 173,
                  "flat field changed at phase %d: %d", ph, sample_q4(flat, (8 << 4) + ph, (8 << 4) + ph));
    }

    // --- D = f * IPD / z, recovered by search from the rendered pair
    for (double z : {0.5, 1.0, 2.0, 4.0, 8.0}) {
        Scene sc = plane_scene(z);
        Camera cl = make_camera(head, 0, ipd, W, H, 95.0);
        Camera cr = make_camera(head, 1, ipd, W, H, 95.0);
        View l = render(sc, cl), r = render(sc, cr);
        const double d_true = cl.f * ipd / z;

        // depth buffer agrees with the geometry
        CHECK(std::fabs(r.depth.at(W / 2, H / 2) - z) < 1e-3, "z=%g depth reads %f", z,
              r.depth.at(W / 2, H / 2));

        // quarter-pel search over the centre tile finds the predicted shift
        const int tx = W / kTile / 2, ty = H / kTile / 2;
        i64 best = -1;
        i32 best_q2 = 0;
        std::vector<i32> pred;
        for (i32 q2 = 0; q2 <= static_cast<i32>(d_true * 4) + 40; ++q2) {
            shift_tile(l.luma, tx, ty, q2, 0, &pred);
            i64 s = tile_sad(r.luma, tx, ty, pred);
            if (best < 0 || s < best) {
                best = s;
                best_q2 = q2;
            }
        }
        double d_found = best_q2 / 4.0;
        CHECK(std::fabs(d_found - d_true) <= 0.5, "z=%g: search found %.2f px, geometry says %.2f",
              z, d_found, d_true);

        // and the product D*z is the same constant f*IPD at every depth
        CHECK(std::fabs(d_found * z - cl.f * ipd) < 0.5 * z + 0.2, "z=%g: D*z = %.3f, f*IPD = %.3f",
              z, d_found * z, cl.f * ipd);
    }

    // --- disparity is horizontal only: a vertical offset always costs SAD
    {
        Scene sc = plane_scene(1.5);
        Camera cl = make_camera(head, 0, ipd, W, H, 95.0);
        Camera cr = make_camera(head, 1, ipd, W, H, 95.0);
        View l = render(sc, cl), r = render(sc, cr);
        const i32 d_q2 = static_cast<i32>(std::lround(cl.f * ipd / 1.5 * 4.0));
        std::vector<i32> p0, p1;
        shift_tile(l.luma, 2, 2, d_q2, 0, &p0);
        shift_tile(l.luma, 2, 2, d_q2, 4, &p1);
        CHECK(tile_sad(r.luma, 2, 2, p0) < tile_sad(r.luma, 2, 2, p1),
              "a one-pixel vertical offset did not cost SAD");
    }

    // --- the pose warp is the identity when the pose does not change
    {
        WarpQ w = quantize_warp(Mat3::identity(), Mat3::identity(), 400.0, 128.0, 128.0);
        for (int y = 0; y < 256; y += 31)
            for (int x = 0; x < 256; x += 29) {
                i32 sx = 0, sy = 0;
                warp_point_q6(w, x, y, &sx, &sy);
                CHECK(sx == x * 64 && sy == y * 64, "identity warp moved (%d,%d) to (%d,%d) q6", x,
                      y, sx, sy);
            }
    }

    // --- a pure yaw moves the image horizontally by about f*tan(yaw)
    {
        const double f = 400.0, yaw = 2.0 * M_PI / 180.0;
        WarpQ w = quantize_warp(Mat3::identity(), rot_y(yaw), f, 128.0, 128.0);
        i32 sx = 0, sy = 0;
        warp_point_q6(w, 128, 128, &sx, &sy);
        const double moved = sx / 64.0 - 128.0;
        const double expect = f * std::tan(yaw);
        CHECK(std::fabs(moved - expect) < 0.05, "yaw warp moved centre %.3f px, expected %.3f",
              moved, expect);
        CHECK(std::abs(sy - 128 * 64) <= 1, "yaw warp moved the centre vertically: %d", sy);
    }

    if (g_fail == 0) std::printf("stereo.disparity: OK\n");
    return g_fail ? 1 : 0;
}
