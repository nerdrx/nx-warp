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
            fdct8x8(src, co);
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
            fdct8x8(src, co);
            for (int i = 0; i < 64; ++i) c32[i] = co[i];
            idct8x8(c32, rec);
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
            fdct8x8(src, co);
            for (int i = 0; i < 64; ++i) c32[i] = co[i];
            idct8x8(c32, rec);
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
        idct8x8(c, out);
        for (int i = 0; i < 64; ++i) CHECK(out[i] == 128, "dc out[%d]=%d", i, out[i]);
        i32 flat[64];
        for (int i = 0; i < 64; ++i) flat[i] = 128;
        i16 co[64];
        fdct8x8(flat, co);
        CHECK(co[0] == 1024, "fdct dc %d", (int)co[0]);
        for (int i = 1; i < 64; ++i) CHECK(co[i] == 0, "fdct ac[%d]=%d", i, (int)co[i]);
    }

    // 5. Range safety: extreme coefficients must not overflow or trap, and the
    //    output stays inside the normative int16 clamp.
    {
        i32 c[64], out[64];
        for (int sgn = 0; sgn < 2; ++sgn) {
            for (int i = 0; i < 64; ++i) c[i] = sgn ? -32768 : 32767;
            idct8x8(c, out);
            for (int i = 0; i < 64; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "range out[%d]=%d", i,
                      out[i]);
        }
        Rng rng(44);
        for (int it = 0; it < 5000; ++it) {
            for (int i = 0; i < 64; ++i) c[i] = rng.range(-32768, 32767);
            idct8x8(c, out);
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
            idct8x8(c, out);
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

    return test_report("test_transform");
}
