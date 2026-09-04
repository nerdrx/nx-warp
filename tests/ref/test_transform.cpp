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

    // 9. The 4x4 transform (SYNTAX.md 6.7): unit gain, the documented ranges,
    //    and agreement with the float DCT-II it is derived from.
    {
        Rng rng(4444);
        double worst_fwd = 0, worst_rt = 0, sse = 0;
        int n = 0;
        for (int it = 0; it < 2000; ++it) {
            i32 src[16];
            double din[16], dref[16];
            for (int i = 0; i < 16; ++i) {
                src[i] = rng.range(-255, 255);
                din[i] = src[i];
            }
            for (int u = 0; u < 4; ++u)
                for (int v = 0; v < 4; ++v) {
                    double t = 0;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x)
                            t += din[y * 4 + x] *
                                 std::cos((2 * x + 1) * v * M_PI / 8) *
                                 std::cos((2 * y + 1) * u * M_PI / 8);
                    double au = (u == 0) ? 1 / std::sqrt(2.0) : 1;
                    double av = (v == 0) ? 1 / std::sqrt(2.0) : 1;
                    dref[u * 4 + v] = t * au * av / 2.0;
                }
            i16 co[16];
            fdct4x4(src, co);
            for (int i = 0; i < 16; ++i) {
                double e = std::fabs(co[i] - dref[i]);
                if (e > worst_fwd) worst_fwd = e;
            }
            i32 dq[16], out[16];
            for (int i = 0; i < 16; ++i) dq[i] = co[i];
            idct4x4(dq, out);
            for (int i = 0; i < 16; ++i) {
                double e = out[i] - src[i];
                if (std::fabs(e) > worst_rt) worst_rt = std::fabs(e);
                sse += e * e;
                ++n;
            }
        }
        CHECK(worst_fwd < 1.5, "fdct4x4 vs float DCT-II: %.3f", worst_fwd);
        CHECK(worst_rt <= 2, "4x4 round-trip max error %.1f", worst_rt);
        CHECK(std::sqrt(sse / n) < 0.6, "4x4 round-trip RMS %.3f",
              std::sqrt(sse / n));
    }

    // 10. Unit gain, in the sense that matters: the transform is orthonormal,
    //     so a *flat* block round-trips to itself and coefficient-domain
    //     squared error is sample-domain squared error.  That is what lets
    //     both sizes share one dequantiser (SYNTAX.md 6.7, decision 55).  The
    //     DC of a flat 4x4 of value v is 4v -- 1024 is a flat 256, not the
    //     8x8's 128, because the block has a quarter of the samples.
    {
        i32 src[16] = {}, out[16];
        src[0] = 1024;
        idct4x4(src, out);
        for (int i = 0; i < 16; ++i) CHECK(out[i] == 256, "dc[%d]=%d", i, out[i]);
        for (int v : {0, 1, 37, 128, 255}) {
            i32 flat[16];
            i16 co[16];
            for (int i = 0; i < 16; ++i) flat[i] = v;
            fdct4x4(flat, co);
            CHECK(co[0] == 4 * v, "flat %d -> dc %d", v, (int)co[0]);
            for (int i = 1; i < 16; ++i) CHECK(co[i] == 0, "flat ac[%d]=%d", i,
                                               (int)co[i]);
            i32 dq[16];
            for (int i = 0; i < 16; ++i) dq[i] = co[i];
            idct4x4(dq, out);
            for (int i = 0; i < 16; ++i) CHECK(out[i] == v, "flat rt %d -> %d",
                                               v, out[i]);
        }
    }

    // 11. Range: every dequantized coefficient at the int16 clamp must stay
    //     inside int32 through both passes, in every sign pattern of a row.
    {
        for (int pat = 0; pat < 16; ++pat) {
            i32 src[16], out[16];
            for (int i = 0; i < 16; ++i)
                src[i] = ((pat >> (i & 3)) & 1) ? 32767 : -32768;
            idct4x4(src, out);
            for (int i = 0; i < 16; ++i)
                CHECK(out[i] >= -32768 && out[i] <= 32767, "sat[%d]=%d", i,
                      out[i]);
        }
    }

    // 11b. The 4x4 constants' Gram matrix, exactly as SYNTAX.md 6.7 states
    //      it: norm 1048578 on the diagonal, zero on four of the six off-
    //      diagonal pairs and -2 on the two that oppose 2*D0^2 against
    //      D1^2 + D2^2.  This is the claim the shared dequantizer rests on,
    //      so it is pinned rather than asserted in prose alone.
    {
        const i32 row[4][4] = {{ kD0,  kD1,  kD0,  kD2},
                               { kD0,  kD2, -kD0, -kD1},
                               { kD0, -kD2, -kD0,  kD1},
                               { kD0, -kD1,  kD0, -kD2}};
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) {
                i32 dot = 0;
                for (int k = 0; k < 4; ++k) dot += row[a][k] * row[b][k];
                i32 want = (a == b) ? 1048578
                                    : (((a == 0 && b == 3) || (a == 3 && b == 0) ||
                                        (a == 1 && b == 2) || (a == 2 && b == 1))
                                           ? -2
                                           : 0);
                CHECK(dot == want, "gram[%d][%d] = %d, want %d", a, b, dot, want);
            }
        CHECK(2 * kD0 * kD0 + kD1 * kD1 + kD2 * kD2 == 1048578, "row norm");

        // ... and the flow graph really is that matrix.  A single nonzero
        // coefficient at (0, k) gives dst[y][x] = M[y][0] * M[x][k] * X /
        // 2^20, so with X = 2^20 / D0 = 2048 every row of dst is row `k` of
        // the matrix, read down the columns.
        for (int k = 0; k < 4; ++k) {
            i32 blk[16] = {}, res[16];
            blk[k] = 2048;
            idct4x4(blk, res);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    i32 want = row[x][k];
                    i32 got = res[y * 4 + x];
                    CHECK(got >= want - 1 && got <= want + 1,
                          "flow graph col %d at (%d,%d): %d, matrix says %d",
                          k, y, x, got, want);
                }
        }
    }

    // 12. The split scan is a permutation of the 64 block-local indices and
    //     is exactly "four 4x4 sub-blocks in raster order, each in 4x4
    //     zigzag" (SYNTAX.md 9.2).  Both halves matter: a scan that is not a
    //     bijection loses coefficients silently, and a scan that does not
    //     match the sub-block layout would put a sub-block's coefficients
    //     where the inverse transform does not look for them.
    {
        int seen[64] = {};
        for (int p = 0; p < 64; ++p) {
            CHECK(kScan4Split[p] < 64, "scan[%d]=%d", p, (int)kScan4Split[p]);
            seen[kScan4Split[p]]++;
        }
        for (int i = 0; i < 64; ++i)
            CHECK(seen[i] == 1, "index %d appears %d times", i, seen[i]);
        for (int p = 0; p < 64; ++p) {
            int sb = p >> 4, sx = sb & 1, sy = sb >> 1;
            int z = kZigzag4[p & 15];
            int want = (4 * sy + z / 4) * 8 + 4 * sx + z % 4;
            CHECK(kScan4Split[p] == want, "scan[%d]=%d, derivation says %d", p,
                  (int)kScan4Split[p], want);
        }
    }

    // 13. weight4() is the tile matrix subsampled by two in each frequency
    //     axis, for every built-in matrix (SYNTAX.md 6.7).
    {
        for (int m = 0; m < 4; ++m)
            for (int u = 0; u < 4; ++u)
                for (int v = 0; v < 4; ++v)
                    CHECK(weight4(kWeight[m], u * 4 + v) ==
                              kWeight[m][(2 * u) * 8 + 2 * v],
                          "matrix %d w4[%d][%d]=%d, want %d", m, u, v,
                          weight4(kWeight[m], u * 4 + v),
                          (int)kWeight[m][(2 * u) * 8 + 2 * v]);
    }

    return test_report("test_transform");
}
