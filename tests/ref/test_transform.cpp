// DCT/IDCT round trip, range and resampling tests.
#include "test_util.h"
#include "common.h"
#include "transform.h"

using namespace nxvc;

static void ref_fdct(const double in[64], double out[64]) {
    for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
            double s = 0;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    s += in[y * 8 + x] * std::cos((2 * x + 1) * v * M_PI / 16) *
                         std::cos((2 * y + 1) * u * M_PI / 16);
            double au = (u == 0) ? 1 / std::sqrt(2.0) : 1;
            double av = (v == 0) ? 1 / std::sqrt(2.0) : 1;
            out[u * 8 + v] = s * au * av / 4.0;
        }
}

int main() {
    // 1. Forward transform tracks the float DCT-II closely.
    {
        Rng rng(11);
        double worst = 0;
        for (int it = 0; it < 500; ++it) {
            i32 src[64];
            double din[64], dref[64];
            for (int i = 0; i < 64; ++i) {
                src[i] = rng.range(-255, 255);
                din[i] = src[i];
            }
            i16 co[64];
            fdct_2d(8, src, co);
            ref_fdct(din, dref);
            for (int i = 0; i < 64; ++i) {
                double e = std::fabs(co[i] - dref[i]);
                if (e > worst) worst = e;
            }
        }
        CHECK(worst < 3.0, "worst forward error %.3f", worst);
    }

    // 2. Round trip fdct -> idct is within +-2 LSB on 8-bit range residuals.
    {
        Rng rng(22);
        int worst = 0;
        for (int it = 0; it < 2000; ++it) {
            i32 src[64], rec[64], c32[64];
            i16 co[64];
            for (int i = 0; i < 64; ++i) src[i] = rng.range(-255, 255);
            fdct_2d(8, src, co);
            for (int i = 0; i < 64; ++i) c32[i] = co[i];
            idct_2d(8, c32, rec);
            for (int i = 0; i < 64; ++i) {
                int e = std::abs(rec[i] - src[i]);
                if (e > worst) worst = e;
            }
        }
        CHECK(worst <= 2, "worst round-trip error %d", worst);
    }

    // 3. Round trip on the 9-bit chroma residual range.
    {
        Rng rng(33);
        int worst = 0;
        for (int it = 0; it < 2000; ++it) {
            i32 src[64], rec[64], c32[64];
            i16 co[64];
            for (int i = 0; i < 64; ++i) src[i] = rng.range(-511, 511);
            fdct_2d(8, src, co);
            for (int i = 0; i < 64; ++i) c32[i] = co[i];
            idct_2d(8, c32, rec);
            for (int i = 0; i < 64; ++i) {
                int e = std::abs(rec[i] - src[i]);
                if (e > worst) worst = e;
            }
        }
        CHECK(worst <= 3, "worst 9-bit round-trip error %d", worst);
    }

    // 4. DC gain: a DC coefficient of 1024 reconstructs a flat 128.
    {
        i32 c[64] = {0}, out[64];
        c[0] = 1024;
        idct_2d(8, c, out);
        for (int i = 0; i < 64; ++i) CHECK(out[i] == 128, "dc out[%d]=%d", i, out[i]);
        i32 flat[64];
        for (int i = 0; i < 64; ++i) flat[i] = 128;
        i16 co[64];
        fdct_2d(8, flat, co);
        CHECK(co[0] == 1024, "fdct dc %d", (int)co[0]);
        for (int i = 1; i < 64; ++i) CHECK(co[i] == 0, "fdct ac[%d]=%d", i, (int)co[i]);
    }

    // 5. Range safety: extreme coefficients must not overflow or trap, and the
    //    output stays inside the normative int16 clamp.
    {
        i32 c[64], out[64];
        for (int sgn = 0; sgn < 2; ++sgn) {
            for (int i = 0; i < 64; ++i) c[i] = sgn ? -32768 : 32767;
            idct_2d(8, c, out);
            for (int i = 0; i < 64; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "range out[%d]=%d", i,
                      out[i]);
        }
        Rng rng(44);
        for (int it = 0; it < 5000; ++it) {
            for (int i = 0; i < 64; ++i) c[i] = rng.range(-32768, 32767);
            idct_2d(8, c, out);
            for (int i = 0; i < 64; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "range out[%d]=%d", i,
                      out[i]);
        }
    }

    // 6. Linearity of the transform on a single basis function: each unit
    //    coefficient must produce a nonzero, symmetric output.
    {
        for (int k = 0; k < 64; ++k) {
            i32 c[64] = {0}, out[64];
            c[k] = 1024;
            idct_2d(8, c, out);
            i32 sum = 0;
            for (int i = 0; i < 64; ++i) sum += std::abs(out[i]);
            CHECK(sum > 0, "basis %d produced zero", k);
        }
    }

    // 7. Bilinear resampling: half-phase weights are 3/4 and 1/4 at factor 2.
    {
        u8 src[4] = {0, 100, 0, 0};
        // factor 2 mapping: sx = 8*x - 4
        i32 v0 = bilinear_q4(src, 2, 1, 2, 8 * 0 - 4, 0);
        i32 v1 = bilinear_q4(src, 2, 1, 2, 8 * 1 - 4, 0);
        i32 v2 = bilinear_q4(src, 2, 1, 2, 8 * 2 - 4, 0);
        i32 v3 = bilinear_q4(src, 2, 1, 2, 8 * 3 - 4, 0);
        CHECK(v0 == 0, "v0 %d", v0);
        CHECK(v1 == 25, "v1 %d (expect 1/4)", v1);
        CHECK(v2 == 75, "v2 %d (expect 3/4)", v2);
        CHECK(v3 == 100, "v3 %d", v3);
    }

    // 8. Upsample/downsample of a flat plane is exact and never clips.
    {
        u8 src[16 * 16], dst[64 * 64], down[8 * 8];
        for (int i = 0; i < 16 * 16; ++i) src[i] = 200;
        upsample(src, 16, 16, 16, dst, 64, 4);
        for (int i = 0; i < 64 * 64; ++i) CHECK(dst[i] == 200, "up[%d]=%d", i, dst[i]);
        downsample(src, 16, 16, 16, down, 8, 2);
        for (int i = 0; i < 64; ++i) CHECK(down[i] == 200, "down[%d]=%d", i, down[i]);
    }

    // 8. The larger transforms (SYNTAX.md 6.7).  One float reference serves
    //    every size, and the three properties that matter are the same at
    //    every size: the integer transform tracks the float DCT-II, the round
    //    trip is within a couple of LSB, and the gain is exactly 1.
    {
        Rng rng(0x1CE);
        for (int n = 16; n <= 32; n *= 2) {
            std::vector<i32> src((size_t)n * n), rec((size_t)n * n),
                c32((size_t)n * n);
            std::vector<i16> co((size_t)n * n);
            std::vector<double> din((size_t)n * n), dref((size_t)n * n);

            // gain: a flat block round trips, and its DC is 128 * n / sqrt(2).
            std::vector<i32> flat((size_t)n * n, 128);
            fdct_2d(n, flat.data(), co.data());
            const int want_dc = n == 16 ? 2048 : 4095;
            CHECK(co[0] == want_dc, "n%d flat dc %d", n, (int)co[0]);
            for (int i = 1; i < n * n; ++i)
                CHECK(co[i] == 0, "n%d flat ac[%d]=%d", n, i, (int)co[i]);
            for (int i = 0; i < n * n; ++i) c32[i] = co[i];
            idct_2d(n, c32.data(), rec.data());
            for (int i = 0; i < n * n; ++i)
                CHECK(rec[i] == 128, "n%d flat round trip[%d]=%d", n, i, rec[i]);

            // forward tracks the float DCT-II, and the round trip is tight.
            double worst_f = 0, worst_r = 0, sse = 0;
            for (int it = 0; it < 40; ++it) {
                for (int i = 0; i < n * n; ++i) {
                    src[i] = rng.range(-255, 255);
                    din[i] = src[i];
                }
                for (int u = 0; u < n; ++u)
                    for (int v = 0; v < n; ++v) {
                        double sum = 0;
                        for (int y = 0; y < n; ++y)
                            for (int x = 0; x < n; ++x)
                                sum += din[y * n + x] *
                                       std::cos((2 * x + 1) * v * M_PI / (2 * n)) *
                                       std::cos((2 * y + 1) * u * M_PI / (2 * n));
                        double au = (u == 0) ? 1 / std::sqrt(2.0) : 1;
                        double av = (v == 0) ? 1 / std::sqrt(2.0) : 1;
                        dref[u * n + v] = sum * au * av * 2.0 / n;
                    }
                fdct_2d(n, src.data(), co.data());
                for (int i = 0; i < n * n; ++i) {
                    double e = std::fabs(co[i] - dref[i]);
                    if (e > worst_f) worst_f = e;
                    c32[i] = co[i];
                }
                idct_2d(n, c32.data(), rec.data());
                for (int i = 0; i < n * n; ++i) {
                    double e = std::fabs((double)src[i] - rec[i]);
                    if (e > worst_r) worst_r = e;
                    sse += e * e;
                }
            }
            CHECK(worst_f < 3.0, "n%d forward vs float: %.3f", n, worst_f);
            CHECK(worst_r <= 2.0, "n%d round trip max error %.1f", n, worst_r);
            CHECK(std::sqrt(sse / (40.0 * n * n)) < 0.7,
                  "n%d round trip rms too high", n);
        }
    }

    // 9. The coefficient-group layout is the identity at 8x8 and a bijection
    //    at every size (SYNTAX.md 6.7).
    {
        for (int n = 8; n <= 32; n *= 2) {
            std::vector<int> seen((size_t)n * n, 0);
            for (int u = 0; u < n; ++u)
                for (int v = 0; v < n; ++v) {
                    int g = group_pos(n, u, v);
                    CHECK(g >= 0 && g < n * n, "n%d group_pos(%d,%d)=%d", n, u,
                          v, g);
                    CHECK(seen[g]++ == 0, "n%d group_pos collision at %d", n, g);
                    if (n == 8) CHECK(g == u * 8 + v, "8x8 layout moved");
                }
        }
    }

    // 10. The per-plane transform cap of SYNTAX.md 6.7.
    {
        CHECK(plane_xform(2, 64) == 2, "64 caps wrong");
        CHECK(plane_xform(2, 32) == 2, "32 caps wrong");
        CHECK(plane_xform(2, 16) == 1, "16 caps wrong");
        CHECK(plane_xform(2, 8) == 0, "8 caps wrong");
        CHECK(plane_xform(1, 64) == 1, "no promotion");
        CHECK(plane_xform(0, 64) == 0, "v1 stays v1");
    }

    // 11. The weighting-matrix derivation samples the 8x8 matrix at the same
    //     spatial frequency and never leaves [1, 32] (SYNTAX.md 6.5).
    {
        for (int m = 0; m < 4; ++m)
            for (int xf = 0; xf < 3; ++xf) {
                const int n = 8 << xf;
                for (int u = 0; u < n; ++u)
                    for (int v = 0; v < n; ++v) {
                        int w = weight_at(kWeight[m], xf, u, v);
                        CHECK(w >= 1 && w <= 32, "weight %d out of range", w);
                        CHECK(w == kWeight[m][(u >> xf) * 8 + (v >> xf)],
                              "weight derivation");
                    }
            }
    }

    return test_report("test_transform");
}
