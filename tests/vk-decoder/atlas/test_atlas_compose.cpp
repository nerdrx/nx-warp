// [SYN] 13.12.2 and 13.12.3 step 1, checked against an INDEPENDENT oracle.
//
// The oracle is written in __int128 from the spec text rather than by calling
// the model, so an int64 overflow or a misplaced rounding constant shows up as
// a disagreement instead of being reproduced identically on both sides.  That
// is the whole point: a model tested against itself tests nothing.
//
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <cmath>
#include "atlas_model.h"

using namespace nxvw;

namespace {

int g_fail = 0;
void fail(const char *what, long long a = 0, long long b = 0) {
    std::printf("FAIL %s (%lld vs %lld)\n", what, a, b);
    ++g_fail;
}

// ---- the oracle: 13.12.2 in 128-bit, transcribed from the spec ------------
using i128 = __int128;

i128 orc_sdiv_round(i128 a, i128 b) {
    const bool neg = (a < 0) != (b < 0);
    const i128 ua = a < 0 ? -a : a;
    const i128 ub = b < 0 ? -b : b;
    const i128 q = (ua * 2 + ub) / (ub * 2);
    return neg ? -q : q;
}

void orc_compose(const int32_t C[9], const int32_t H[9], i128 P[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const i128 t_lin = (i128)C[i * 3 + 0] * H[0 * 3 + j] +
                               (i128)C[i * 3 + 1] * H[1 * 3 + j];
            const i128 t_per = (i128)C[i * 3 + 2] * H[2 * 3 + j];
            // Arithmetic shift on a negative i128 is floor division, which is
            // what >> means in the spec's pseudocode.
            P[i * 3 + j] = ((t_lin + ((i128)1 << 20)) >> 21) +
                           ((t_per + ((i128)1 << 28)) >> 29);
        }
}

// ---- legal matrices -------------------------------------------------------
// A homography in the wire's scales that satisfies 3.1.1 for a w x h eye:
// near-identity linear part, a bounded translation, and a perspective row
// small enough that `den` stays inside [2^28, 2^30) at every corner.
void make_legal(std::mt19937_64 &rng, int32_t w, int32_t h, double scale,
                int32_t H[9]) {
    const double one = (double)(1 << kWarpQNum);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (;;) {
        const double a = 1.0 + u(rng) * 0.05 * scale;
        const double b = u(rng) * 0.05 * scale;
        const double c = u(rng) * 0.05 * scale;
        const double d = 1.0 + u(rng) * 0.05 * scale;
        const double tx = u(rng) * 64.0 * scale;
        const double ty = u(rng) * 64.0 * scale;
        // `den` swings by |p|*(w/2) + |q|*(h/2) around h22; keep that inside
        // a quarter of the [2^28, 2^30) band so a few compositions still fit.
        const double lim = 0.25 * (double)kWarpH22 / (double)(w / 2 + h / 2);
        const double p = u(rng) * lim * scale;
        const double q = u(rng) * lim * scale;
        H[0] = (int32_t)llround(a * one);
        H[1] = (int32_t)llround(b * one);
        H[2] = (int32_t)llround(tx * one);
        H[3] = (int32_t)llround(c * one);
        H[4] = (int32_t)llround(d * one);
        H[5] = (int32_t)llround(ty * one);
        H[6] = (int32_t)llround(p);
        H[7] = (int32_t)llround(q);
        H[8] = kWarpH22;
        if (atlas_in_envelope(H, w, h)) return;
    }
}

}  // namespace

int main() {
    const int32_t W = 1088, H_ = 1088;
    std::mt19937_64 rng(20260906u);

    // 1. sdiv_round against the oracle, including both signs and exact ties.
    {
        const int64_t vals[] = {0, 1, -1, 2, -2, 3, -3, 5, -5, 7, -7,
                                (int64_t)1 << 40, -((int64_t)1 << 40),
                                ((int64_t)1 << 40) + 1};
        for (int64_t a : vals)
            for (int64_t b : vals) {
                if (b == 0) continue;
                const int64_t got = atlas_sdiv_round(a, b);
                const i128 want = orc_sdiv_round(a, b);
                if ((i128)got != want) fail("sdiv_round", (long long)got,
                                            (long long)want);
            }
        // Ties go AWAY from zero, both signs: 3/2 -> 2, -3/2 -> -2.
        if (atlas_sdiv_round(3, 2) != 2) fail("tie +");
        if (atlas_sdiv_round(-3, 2) != -2) fail("tie -");
    }

    // 2. The identity property, which pins the whole step without trusting
    //    either transcription: renorm(I . H) must be H exactly, for any legal
    //    H, because I is exact in both scales and renorm divides by h22.
    for (int t = 0; t < 4000; ++t) {
        int32_t Hm[9], C[9], out[9];
        make_legal(rng, W, H_, 1.0, Hm);
        atlas_identity(C);
        int64_t P[9];
        atlas_compose_step(C, Hm, P);
        if (!atlas_renorm(P, out)) { fail("renorm(I.H) failed"); continue; }
        for (int k = 0; k < 9; ++k)
            if (out[k] != Hm[k]) fail("identity.H != H", out[k], Hm[k]);
    }

    // 3. compose_step against the 128-bit oracle: the int64 form must produce
    //    the same P, which is where an overflow would show.
    for (int t = 0; t < 20000; ++t) {
        int32_t A[9], B[9];
        make_legal(rng, W, H_, 1.0, A);
        make_legal(rng, W, H_, 1.0, B);
        int64_t P[9];
        i128 Q[9];
        atlas_compose_step(A, B, P);
        orc_compose(A, B, Q);
        for (int k = 0; k < 9; ++k)
            if ((i128)P[k] != Q[k]) fail("compose_step != oracle");
        // and renorm against the oracle's
        int32_t out[9];
        if (atlas_renorm(P, out)) {
            for (int k = 0; k < 9; ++k) {
                const i128 want = orc_sdiv_round(Q[k] << 29, Q[8]);
                if ((i128)out[k] != want) fail("renorm != oracle", out[k],
                                               (long long)want);
            }
        }
    }

    // 4. renorm restores h22, which is what lets 13.3's predictor consume a
    //    composed matrix unchanged.
    for (int t = 0; t < 4000; ++t) {
        int32_t A[9], B[9], out[9];
        make_legal(rng, W, H_, 1.0, A);
        make_legal(rng, W, H_, 1.0, B);
        int64_t P[9];
        atlas_compose_step(A, B, P);
        if (atlas_renorm(P, out) && out[8] != kWarpH22)
            fail("renorm did not restore h22", out[8], kWarpH22);
    }

    // 5. The entry rules of 13.12.3 step 1.
    {
        int32_t Hm[9];
        make_legal(rng, W, H_, 1.0, Hm);
        // invalid: untouched, and stays invalid.
        AtlasEntry e{};
        atlas_identity(e.C);
        e.gen = 7;
        if (atlas_advance_entry(e, Hm, W, H_, 0)) fail("invalid advanced");
        if (e.gen != 7) fail("invalid entry's gen moved", e.gen, 7);
        // static: gen moves, C does not.
        AtlasEntry s{};
        atlas_identity(s.C);
        s.flags = NXVW_ATLAS_FLAG_VALID | NXVW_ATLAS_FLAG_STATIC;
        int32_t before[9];
        for (int k = 0; k < 9; ++k) before[k] = s.C[k];
        if (!atlas_advance_entry(s, Hm, W, H_, 0)) fail("static invalidated");
        if (s.gen != 1) fail("static gen", s.gen, 1);
        for (int k = 0; k < 9; ++k)
            if (s.C[k] != before[k]) fail("static C moved");
        // gen_max caps.
        AtlasEntry g{};
        atlas_identity(g.C);
        g.flags = NXVW_ATLAS_FLAG_VALID;
        // "exceeds" is >, so a gen that lands exactly ON the cap survives and
        // the next one does not.  Both directions are pinned.
        g.gen = 4;
        if (!atlas_advance_entry(g, Hm, W, H_, 5)) fail("gen == cap invalidated");
        if (g.gen != 5) fail("gen after advance", g.gen, 5);
        if (atlas_advance_entry(g, Hm, W, H_, 5)) fail("gen_max not hit");
        if (g.flags & NXVW_ATLAS_FLAG_VALID) fail("gen_max left it valid");
    }

    // 6. How deep the H ring has to be.  Two numbers, because they answer
    //    different questions.
    //
    //    The first is the worst case 3.1.1 permits at all: the most aggressive
    //    matrix `make_legal` will produce.  The second is the one that matters
    //    -- a real head at the fastest rotation the paper measures, 123 deg/s,
    //    which at 90 Hz is 1.37 deg a frame.  The ring only has to outlast the
    //    envelope, so the ring depth has to clear the SECOND number.
    {
        int worst = 1 << 30;
        for (int t = 0; t < 200; ++t) {
            int32_t Hm[9], C[9];
            make_legal(rng, W, H_, 1.0, Hm);
            atlas_identity(C);
            int n = 0;
            while (n < 100000 && atlas_advance(C, Hm, W, H_)) ++n;
            if (n < worst) worst = n;
        }
        std::printf("-- extreme legal matrix: %d compositions before the "
                    "envelope trips\n", worst);
        if (worst < 1) fail("a single composition already leaves the envelope");

        // A yaw of `deg` per frame as a homography: K R K^-1 normalised so
        // h22 is 2^29.  f is taken as w/2, i.e. a 90-degree horizontal field,
        // which is the widest a headset eye plausibly has and so the harshest
        // f for a given rotation.
        auto make_yaw = [&](double deg, int32_t Hm[9]) {
            const double one = (double)(1 << kWarpQNum);
            const double th = deg * 3.14159265358979323846 / 180.0;
            const double f = (double)W / 2.0;
            const double t = std::tan(th);
            Hm[0] = (int32_t)llround(one);
            Hm[1] = 0;
            Hm[2] = (int32_t)llround(f * t * one);
            Hm[3] = 0;
            Hm[4] = (int32_t)llround((1.0 / std::cos(th)) * one);
            Hm[5] = 0;
            Hm[6] = (int32_t)llround((-t / f) * (double)kWarpH22);
            Hm[7] = 0;
            Hm[8] = kWarpH22;
        };
        for (double deg : {1.37, 2.74, 5.0}) {
            int32_t Hm[9], C[9];
            make_yaw(deg, Hm);
            if (!atlas_in_envelope(Hm, W, H_)) {
                std::printf("-- yaw %.2f deg/frame: not a legal matrix\n", deg);
                continue;
            }
            atlas_identity(C);
            int n = 0;
            while (n < 100000 && atlas_advance(C, Hm, W, H_)) ++n;
            std::printf("-- yaw %.2f deg/frame (%.0f deg/s at 90 Hz): %d "
                        "compositions before the envelope trips\n",
                        deg, deg * 90.0, n);
        }
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
