// stereo.raster - the synthetic renderer has to be geometrically right before
// any disparity number from it means anything.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "analyze.h"
#include "predict.h"
#include "scene.h"

using namespace nxs;

static int g_fail = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++g_fail;                                          \
        }                                                      \
    } while (0)

static Scene plane_scene(double z, Tex t = Tex::kNoise) {
    Scene s;
    s.name = "plane";
    Box b;
    b.lo = {-20, -20, z};
    b.hi = {20, 20, z + 0.05};
    b.tex = t;
    b.texel_per_m = 80.0;
    b.seed = 5;
    s.boxes.push_back(b);
    return s;
}

int main() {
    const int W = 256, H = 256;
    Pose head;
    head.rot = Mat3::identity();
    head.pos = Vec3{0, 0, 0};

    // --- depth of a fronto-parallel plane is the plane distance everywhere
    {
        Scene sc = plane_scene(2.0);
        Camera cam = make_camera(head, 0, 0.064, W, H, 95.0);
        View v = render(sc, cam);
        for (int y = 0; y < H; y += 37)
            for (int x = 0; x < W; x += 41)
                CHECK(std::fabs(v.depth.at(x, y) - 2.0) < 1e-3, "depth at %d,%d = %f", x, y,
                      v.depth.at(x, y));
    }

    // --- a bounded box leaves background (no hit) around it
    {
        Scene sc;
        sc.name = "small";
        Box b;
        b.lo = {-0.2, -0.2, 3.0};
        b.hi = {0.2, 0.2, 3.1};
        b.tex = Tex::kChecker;
        b.texel_per_m = 80.0;
        b.seed = 1;
        sc.boxes.push_back(b);
        Camera cam = make_camera(head, 0, 0.064, W, H, 95.0);
        View v = render(sc, cam);
        CHECK(v.depth.at(W / 2, H / 2) < 3.2, "centre should hit the box, got %f",
              v.depth.at(W / 2, H / 2));
        CHECK(v.depth.at(2, 2) >= kFarZ * 0.5, "corner should miss, got %f", v.depth.at(2, 2));
        CHECK(v.luma.at(2, 2) == static_cast<u8>(sc.bg_luma + 0.5) ||
                  v.luma.at(2, 2) <= 9,
              "corner luma should be background, got %d", v.luma.at(2, 2));
    }

    // --- projection: a box edge lands where the pinhole model says it does
    {
        const double z = 3.0, xw = 0.5;
        Scene sc;
        sc.name = "edge";
        Box b;
        b.lo = {-xw, -20, z};
        b.hi = {xw, 20, z + 0.05};
        b.tex = Tex::kFlat;
        b.texel_per_m = 40.0;
        b.seed = 2;
        sc.boxes.push_back(b);
        Camera cam = make_camera(head, 0, 0.0, W, H, 95.0);  // ipd 0: eye at origin
        View v = render(sc, cam);
        const int expected = static_cast<int>(std::lround(cam.cx + cam.f * xw / z));
        int found = -1;
        for (int x = W / 2; x < W; ++x)
            if (v.depth.at(x, H / 2) >= kFarZ * 0.5) {
                found = x;
                break;
            }
        CHECK(found >= 0 && std::abs(found - expected) <= 1, "right edge at %d, expected %d", found,
              expected);
    }

    // --- both eyes see the same surface, shifted, and never identically
    {
        Scene sc = plane_scene(1.5);
        Camera cl = make_camera(head, 0, 0.064, W, H, 95.0);
        Camera cr = make_camera(head, 1, 0.064, W, H, 95.0);
        CHECK(std::fabs((cr.pos.x - cl.pos.x) - 0.064) < 1e-12, "eye separation is %f",
              cr.pos.x - cl.pos.x);
        View l = render(sc, cl), r = render(sc, cr);
        CHECK(l.luma.px != r.luma.px, "left and right images are identical");
        long diff = 0;
        for (size_t i = 0; i < l.luma.px.size(); ++i)
            diff += std::abs(static_cast<int>(l.luma.px[i]) - r.luma.px[i]);
        CHECK(diff > 0, "no luma difference between eyes");
    }

    // --- the text texture is bimodal (ink and paper), which is what makes the
    //     panel scene a text-quality probe rather than another noise field
    {
        int dark = 0, light = 0, mid = 0;
        for (int i = 0; i < 400; ++i)
            for (int j = 0; j < 60; ++j) {
                double s = sample_texture(Tex::kText, 0, i / 156.0, j / 156.0, 156.0);
                if (s < 60) ++dark;
                else if (s > 200) ++light;
                else ++mid;
            }
        CHECK(dark > 200, "text texture has too little ink: %d", dark);
        CHECK(light > 1000, "text texture has too little paper: %d", light);
        CHECK(mid == 0, "text texture should be two-valued, %d mid samples", mid);
    }

    // --- world-locked texture: the same surface point has the same luma in
    //     both eyes (the premise of inter-view prediction)
    {
        // Choose the plane depth so the disparity is exactly 4 px: the right
        // image is then a pure integer shift of the left one and the two must
        // agree to within rounding.
        Camera probe = make_camera(head, 0, 0.064, W, H, 95.0);
        const int di = 4;
        const double z = probe.f * 0.064 / di;
        // Smooth texture on purpose: a hard-edged checker at this pitch puts a
        // texel boundary every ~6 px, where a 1e-13 difference in the hit point
        // flips a supersample and shows up as luma noise.  That is a property
        // of the scene, not of the projection, and it is measured properly by
        // the sim's residual numbers rather than by an equality assert.
        Scene sc = plane_scene(z, Tex::kNoise);
        Camera cl = make_camera(head, 0, 0.064, W, H, 95.0);
        Camera cr = make_camera(head, 1, 0.064, W, H, 95.0);
        View l = render(sc, cl), r = render(sc, cr);
        const double d = cl.f * 0.064 / z;
        CHECK(std::fabs(d - di) < 1e-9, "disparity is %f, expected %d", d, di);
        long err = 0;
        int n = 0;
        for (int y = 64; y < H - 64; y += 3)
            for (int x = 64; x < W - 64 - di; x += 3) {
                err += std::abs(static_cast<int>(r.luma.at(x, y)) - l.luma.at(x + di, y));
                ++n;
            }
        double mae = static_cast<double>(err) / n;
        CHECK(mae < 1.0, "inter-view MAE at the true disparity is %.2f, expected < 1", mae);
    }

    if (g_fail == 0) std::printf("stereo.raster: OK\n");
    return g_fail ? 1 : 0;
}
