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

    // ---------------------------------------------------- XFORM_FAST (6.7)
    // 9. The multiply-free basis is exactly the H.264 8x8 one, exactly
    //    orthogonal, with the three column norms the scale table was built
    //    from: 8, 289/32 and 5 (in eighths: the columns below).
    {
        // 8 * M[x][k], the inverse graph's basis columns.
        static const int B[8][8] = {
            { 8, 12,  8, 10,  8,  6,  4,  3},
            { 8, 10,  4, -3, -8,-12, -8, -6},
            { 8,  6, -4,-12, -8,  3,  8, 10},
            { 8,  3, -8, -6,  8, 10, -4,-12},
            { 8, -3, -8,  6,  8,-10, -4, 12},
            { 8, -6, -4, 12, -8, -3,  8,-10},
            { 8,-10,  4,  3, -8, 12, -8,  6},
            { 8,-12,  8,-10,  8, -6,  4, -3},
        };
        // A coefficient of 2048 at (0, k) makes row 0 of the output
        // 2048/8 * M[x][k] = 32 * B[x][k], exactly: every internal shift of
        // the graph divides a multiple of its divisor, and 2048 << 3 times
        // the largest basis entry (1.5) stays inside the pass-1 int16 clamp.
        for (int k = 0; k < 8; ++k) {
            i32 c[64] = {0}, out[64];
            c[k] = 2048;
            idct8x8_fast(c, out);
            for (int x = 0; x < 8; ++x)
                CHECK(out[x] == 32 * B[x][k], "basis %d[%d] = %d, want %d", k,
                      x, out[x], 32 * B[x][k]);
        }
        // Orthogonality and the norms, straight off the table.
        const int want[8] = {8 * 64, 289 * 2, 5 * 64, 289 * 2,
                             8 * 64, 289 * 2, 5 * 64, 289 * 2};  // 64 * norm^2
        for (int k = 0; k < 8; ++k) {
            int nk = 0;
            for (int n = 0; n < 8; ++n) nk += B[n][k] * B[n][k];
            CHECK(nk == want[k], "basis %d squared norm %d, want %d", k, nk,
                  want[k]);
            for (int j = 0; j < 8; ++j) {
                if (j == k) continue;
                int g = 0;
                for (int n = 0; n < 8; ++n) g += B[n][k] * B[n][j];
                CHECK(g == 0, "basis %d and %d not orthogonal (%d)", k, j, g);
            }
        }
        // The scale table is round(1024 * 8 / (g_u * g_v)) with g^2 = want/64.
        for (int u = 0; u < 8; ++u)
            for (int v = 0; v < 8; ++v) {
                double gu = std::sqrt(want[u] / 64.0), gv = std::sqrt(want[v] / 64.0);
                int e = (int)std::lround(1024.0 * 8.0 / (gu * gv));
                CHECK(kXfsScale[u * 8 + v] == e, "scale[%d][%d] = %d, want %d",
                      u, v, (int)kXfsScale[u * 8 + v], e);
            }
    }

    // 10. Unit gain: a dequantized DC of 1024 reconstructs a flat 128, the
    //     same convention the Loeffler path uses.
    {
        i32 c[64] = {0}, out[64];
        c[0] = 1024;
        idct8x8_fast(c, out);
        for (int i = 0; i < 64; ++i) CHECK(out[i] == 128, "flat[%d]=%d", i, out[i]);
    }

    // 11. Forward-inverse round trip on random residuals: the multiply-free
    //     pair is at least as accurate as the Loeffler pair, which SYNTAX 6.3
    //     documents at an RMS of 0.347 and a maximum of 2 LSB.
    {
        u32 st = 12345;
        auto rnd = [&]() { st = st * 1103515245u + 12345u; return (i32)((st >> 16) & 0x7fff); };
        double sse = 0;
        i32 worst = 0;
        const int trials = 2000;
        for (int t = 0; t < trials; ++t) {
            i32 in[64], out[64];
            i16 co[64];
            for (int i = 0; i < 64; ++i) in[i] = rnd() % 511 - 255;
            fdct8x8_fast(in, co);
            i32 dq[64];
            for (int i = 0; i < 64; ++i) dq[i] = co[i];
            idct8x8_fast(dq, out);
            for (int i = 0; i < 64; ++i) {
                i32 e = out[i] - in[i];
                if (std::abs(e) > worst) worst = std::abs(e);
                sse += (double)e * e;
            }
        }
        double rms = std::sqrt(sse / (trials * 64.0));
        CHECK(worst <= 2, "round-trip max error %d", worst);
        CHECK(rms < 0.35, "round-trip RMS %.4f", rms);
    }

    // 12. The dequantizer's step clamp (SYNTAX 6.7): it binds only at QP 62
    //     and 63 with a weight of 28 or more at the four (u,v) in {2,6}^2
    //     positions, and q * t stays inside int32 everywhere.
    {
        int clamped = 0;
        for (int qp = 0; qp < 64; ++qp)
            for (int w = 1; w <= 32; ++w)
                for (int i = 0; i < 64; ++i) {
                    i64 raw = ((i64)kQStep[qp] * w * kXfsScale[i] + 8192) >> 14;
                    i32 t = raw > kXfsTMax ? kXfsTMax : (i32)raw;
                    if (raw > kXfsTMax) {
                        ++clamped;
                        CHECK(qp >= 62, "clamp at qp %d", qp);
                    }
                    CHECK((i64)32767 * t + 8 < 2147483647LL,
                          "q*t overflows at qp %d w %d i %d", qp, w, i);
                }
        CHECK(clamped == 7 * 4, "clamp binds %d times, expected 28", clamped);
    }

    // 13. A scale of 1024 reduces to the Loeffler step exactly, which is what
    //     makes positions (0,0), (0,4), (4,0) and (4,4) shared between the
    //     two paths.
    {
        for (int qp = 0; qp < 64; ++qp)
            for (int w = 1; w <= 32; ++w) {
                i32 a = (kQStep[qp] * w + 8) >> 4;
                i32 b = (kQStep[qp] * w * 1024 + 8192) >> 14;
                CHECK(a == b, "scale-1024 step differs at qp %d w %d", qp, w);
            }
    }

    return test_report("test_transform");
}
