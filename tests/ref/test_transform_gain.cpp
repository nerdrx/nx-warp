// ref.transform_gain -- the 2D-gain invariant of the transform family.
//
// docs/MERGE-PLAN.md 4.4.  The tournament merged one transform family over
// four edges from two packages that each built part of it: the detail package
// contributed 4x4 (tool bit 19, the per-block split of SYNTAX.md 6.8) and the
// transform package 16x16 and 32x32 (tool bit 27, the per-tile size of 6.7).
// Two families would have been two dequantiser scales, and this test exists
// because the second one would have been wrong SILENTLY:
//
//     A scale that is off by a factor of two does not fail loudly.  It shifts
//     the effective QP by 6.  Every rate and every PSNR number stays
//     plausible, every round trip still round-trips, every conformance vector
//     still matches itself -- and every comparison in the tournament is
//     wrong.
//
// So the invariant is checked against something outside the integer pipeline
// entirely: a floating-point orthonormal DCT-II.
//
// **The invariant is that the quantiser sees ORTHONORMAL coefficients at
// every size**: a coefficient of the integer forward transform is the
// coefficient of the orthonormal DCT-II, at unit scale, whatever the block
// edge.  That is what lets ONE qstep table serve all four sizes, and it is
// what makes coefficient-domain squared error equal sample-domain squared
// error, which the RD trellis relies on.
//
// It is NOT the statement that every size has the same 2D gain, and the two
// are easy to confuse.  The 2^20 of docs/MERGE-PLAN.md 4.4 is the gain of the
// INTERNAL fixed-point graph -- the 1D matrices are the orthonormal ones
// scaled by 2^10 -- and the shift chain is what removes it again.  Because
// the unnormalized graph's output grows by sqrt(2) per doubling, those
// internal 2D gains are 2^20, 2^20, 2^21, 2^22 along the family and the shift
// chains differ accordingly.  The 2^20 figure applies at 8x8 and is not a
// constant the other sizes have to hit; what every size must hit is the unit
// scale below.
//
// Tolerance is 0.1 % of the coefficient scale.  That is tight enough to catch
// a half-bit shift error (which is 41 %) by a factor of four hundred, and
// loose enough for the rounding of a fixed-point graph.
#include <cmath>
#include <cstdlib>

#include "common.h"
#include "test_util.h"
#include "transform.h"

using namespace nxvc;

namespace {

// The orthonormal DCT-II of an n x n block, in floating point.  This is the
// definition the integer transform is an approximation OF; nothing in it
// comes from ref/src/transform.cpp, which is the whole point.
void float_dct2(const double *in, double *out, int n) {
    const double pi = std::acos(-1.0);
    for (int u = 0; u < n; ++u)
        for (int v = 0; v < n; ++v) {
            double s = 0;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    s += in[y * n + x] *
                         std::cos((2 * x + 1) * v * pi / (2 * n)) *
                         std::cos((2 * y + 1) * u * pi / (2 * n));
            const double cu = (u == 0) ? std::sqrt(1.0 / n) : std::sqrt(2.0 / n);
            const double cv = (v == 0) ? std::sqrt(1.0 / n) : std::sqrt(2.0 / n);
            out[u * n + v] = s * cu * cv;
        }
}

// The reference scale the quantiser is calibrated for.  A coefficient of the
// integer forward transform is kRefScale times the orthonormal one, and
// kRefScale is 1 at every size: the internal 2^10-per-dimension gain of the
// fixed-point matrices is removed by the shift chain, not passed on to the
// quantiser.  A flat block of value v has DC exactly n*v at every size, which
// is the orthonormal DCT's own DC, and section 3 checks that directly.
constexpr double kRefScale = 1.0;

}  // namespace

int main() {
    constexpr int kMaxCoef = kMaxBlock * kMaxBlock;
    Rng rng(20260905);

    // ------------------------------------------------------------------ 1
    // The forward transform against the float DCT-II, at every size.  This is
    // the check that pins the scale: if a size's shift chain is off by a bit,
    // its coefficients come out 2x or 0.5x the reference and the ratio moves
    // by 100 % or 50 % where the tolerance is 0.1 %.
    //
    // Measured as a ratio of energies rather than per coefficient, because a
    // single coefficient can be small enough that fixed-point rounding is a
    // large relative error while the transform is perfectly scaled.  Energy
    // over 400 random blocks is what the quantiser actually sees.
    for (int n : {4, 8, 16, 32}) {
        double e_int = 0, e_ref = 0;
        for (int it = 0; it < 400; ++it) {
            i32 src[kMaxCoef];
            double din[kMaxCoef], dref[kMaxCoef];
            for (int i = 0; i < n * n; ++i) {
                src[i] = rng.range(-255, 255);
                din[i] = src[i];
            }
            i16 co[kMaxCoef];
            fdct_block(src, co, n);
            float_dct2(din, dref, n);
            for (int i = 0; i < n * n; ++i) {
                e_int += (double)co[i] * co[i];
                e_ref += (kRefScale * dref[i]) * (kRefScale * dref[i]);
            }
        }
        const double ratio = std::sqrt(e_int / e_ref);
        CHECK(std::fabs(ratio - 1.0) < 0.001,
              "n=%d forward gain vs float DCT-II: %.6f, want 1.0 +- 0.001 "
              "(a factor of 2 here is an effective QP shift of 6)",
              n, ratio);
    }

    // ------------------------------------------------------------------ 2
    // Orthonormality, which is the property the shared qstep table depends
    // on: coefficient-domain squared error IS sample-domain squared error, so
    // the RD trellis may compare the two.  Perturb one coefficient by a known
    // amount, invert, and check the energy that appears in the samples.
    //
    // The dequantized coefficient domain is the sample domain scaled by
    // kRefScale, so a coefficient perturbation of d becomes a sample-domain
    // energy of (d / kRefScale)^2 -- at unit scale, d^2.  This is the
    // property in the form the trellis uses it.
    for (int n : {4, 8, 16, 32}) {
        for (int k : {0, 1, 3, 7}) {
            const int pos = k % (n * n);
            i32 a[kMaxCoef] = {}, b[kMaxCoef] = {};
            i32 oa[kMaxCoef], ob[kMaxCoef];
            const i32 d = 4096;
            a[pos] = 0;
            b[pos] = d;
            idct_block(a, oa, n);
            idct_block(b, ob, n);
            double se = 0;
            for (int i = 0; i < n * n; ++i) {
                const double e = (double)ob[i] - (double)oa[i];
                se += e * e;
            }
            const double want = ((double)d / kRefScale) * ((double)d / kRefScale);
            CHECK(std::fabs(se - want) / want < 0.002,
                  "n=%d coef %d: perturbing by %d moved the samples by %.4f, "
                  "orthonormal says %.4f",
                  n, pos, d, se, want);
        }
    }

    // ------------------------------------------------------------------ 3
    // A flat block is the DC basis and nothing else, at every size.  This is
    // the cheap, near-exact half of the same statement: the orthonormal DC of
    // a flat block of value v is n*v, so a DC of n*128 inverts to a flat 128
    // at every size and a flat 128 transforms to a DC of n*128 with every
    // other coefficient zero.  The forward direction is allowed one LSB: at
    // 32x32 the two-pass rounding lands on 4095 rather than 4096, which is
    // 0.024 % and well inside the 0.1 % the scale is pinned to.
    for (int n : {4, 8, 16, 32}) {
        i32 src[kMaxCoef] = {}, dst[kMaxCoef];
        src[0] = n * 128;
        idct_block(src, dst, n);
        for (int i = 0; i < n * n; ++i)
            CHECK(dst[i] == 128, "n=%d flat DC dst[%d] = %d, want 128", n, i,
                  dst[i]);

        i32 flat[kMaxCoef];
        i16 co[kMaxCoef];
        for (int i = 0; i < n * n; ++i) flat[i] = 128;
        fdct_block(flat, co, n);
        CHECK(std::abs((int)co[0] - n * 128) <= 1,
              "n=%d flat forward DC = %d, want %d +- 1", n, (int)co[0],
              n * 128);
        for (int i = 1; i < n * n; ++i)
            CHECK(co[i] == 0, "n=%d flat forward co[%d] = %d, want 0", n, i,
                  (int)co[i]);
    }

    // ------------------------------------------------------------------ 4
    // The family is closed: every edge the syntax can name is in it, and
    // nothing else is.  A size that block_size_ok() accepts but the shift
    // tables do not cover would read past them.
    for (int n = 1; n <= 64; ++n) {
        const bool ok = block_size_ok(n);
        const bool want = (n == 4 || n == 8 || n == 16 || n == 32);
        CHECK(ok == want, "block_size_ok(%d) = %d, want %d", n, (int)ok,
              (int)want);
    }

    return test_report("test_transform_gain");
}
