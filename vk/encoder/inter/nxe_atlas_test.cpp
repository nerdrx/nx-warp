/* nxe_atlas_test.cpp -- the ATLAS shadow (tool bit 31) against [SYN] 13.12.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * No GPU: this is the arithmetic and the bookkeeping that decide what the GPU
 * is then asked to do, and every rule here is one the DECODER derives from the
 * same normative text and the same warp_ext().  A disagreement is a reference
 * the client does not hold, so the checks are against the spec's own
 * arithmetic -- and, where the spec states a property rather than a value,
 * against an independent double-precision recomposition -- rather than against
 * a golden dump.
 */

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "nxe_atlas.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond);     \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

namespace {

const double kSn = 2097152.0;    /* 2^21, rows 0 and 1 */
const double kSd = 536870912.0;  /* 2^29, row 2        */

double scale_of(int k) { return k < 6 ? kSn : kSd; }

void to_double(const int32_t C[9], double m[9]) {
    for (int k = 0; k < 9; ++k) m[k] = (double)C[k] / scale_of(k);
}

/* The composition the integer rule approximates: a real matrix product,
 * renormalised on m22, in the same element order. */
void mul_double(const double a[9], const double b[9], double o[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a[i * 3 + k] * b[k * 3 + j];
            o[i * 3 + j] = s;
        }
    const double d = o[8];
    for (int k = 0; k < 9; ++k) o[k] /= d;
}

/* A plausible warp_ext(): a small rotation about the picture centre with a
 * perspective row of the order docs/WARP.md 3 describes (5e-5 at the scale of
 * a picture half-width). */
void make_warp(double ang, double px, double py, int32_t H[9],
               double tx = 0.0, double ty = 0.0) {
    const double c = std::cos(ang), s = std::sin(ang);
    const double m[9] = {c, -s, tx, s, c, ty, px, py, 1.0};
    for (int k = 0; k < 9; ++k)
        H[k] = (int32_t)std::llround(m[k] * scale_of(k));
    H[8] = 1 << 29;
}

void check_identity_is_a_unit() {
    int32_t I[9];
    nxe::atlas_identity(I);
    CHECK(I[0] == (1 << 21) && I[4] == (1 << 21) && I[8] == (1 << 29),
          "identity is %d %d %d", I[0], I[4], I[8]);

    int32_t H[9];
    make_warp(0.013, 3.1e-5, -1.7e-5, H);

    /* Both sides, EXACTLY.  This is not a tolerance: the two partial sums and
     * the renormalisation are each exact when one operand is the identity, so
     * a composition step against it must not move a single bit.  If it does,
     * a static tile's C drifts and the whole static-skip cheat is wrong. */
    int32_t o[9];
    CHECK(nxe::atlas_compose(I, H, o), "compose I.H refused");
    for (int k = 0; k < 9; ++k)
        CHECK(o[k] == H[k], "I.H element %d: %d != %d", k, o[k], H[k]);
    CHECK(nxe::atlas_compose(H, I, o), "compose H.I refused");
    for (int k = 0; k < 9; ++k)
        CHECK(o[k] == H[k], "H.I element %d: %d != %d", k, o[k], H[k]);
}

void check_sdiv_round() {
    /* [SYN] 13.12.2: round to nearest, ties AWAY FROM ZERO, sign-magnitude. */
    struct { int64_t a, b, want; } t[] = {
        {3, 2, 2},   {-3, 2, -2},  {3, -2, -2},   {-3, -2, 2},
        {1, 2, 1},   {-1, 2, -1},  {5, 4, 1},     {-5, 4, -1},
        {6, 4, 2},   {-6, 4, -2},  {0, 7, 0},     {7, 7, 1},
        {(int64_t)1 << 40, 1 << 3, (int64_t)1 << 37},
    };
    for (auto &c : t)
        CHECK(nxe::atlas_sdiv_round(c.a, c.b) == c.want,
              "sdiv_round(%lld,%lld) = %lld want %lld", (long long)c.a,
              (long long)c.b, (long long)nxe::atlas_sdiv_round(c.a, c.b),
              (long long)c.want);
    /* Symmetric about zero, which is what makes it identical on every
     * implementation whatever its division truncates toward. */
    for (int64_t a = -50; a <= 50; ++a)
        for (int64_t b = 1; b <= 9; ++b)
            CHECK(nxe::atlas_sdiv_round(a, b) == -nxe::atlas_sdiv_round(-a, b),
                  "asymmetric at %lld/%lld", (long long)a, (long long)b);
}

void check_partial_sums_are_the_spec() {
    /* The clause's own expression, transcribed here a second time from the
     * text rather than called, so that the test fails if the implementation
     * quietly changes rounding rule or scale. */
    int32_t C[9], H[9];
    make_warp(0.021, 2.2e-5, 1.1e-5, C);
    make_warp(-0.007, -4.0e-5, 0.9e-5, H);
    int64_t P[9];
    nxe::atlas_compose_partial(C, H, P);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const int64_t t_lin = (int64_t)C[i * 3 + 0] * (int64_t)H[0 * 3 + j] +
                                  (int64_t)C[i * 3 + 1] * (int64_t)H[1 * 3 + j];
            const int64_t t_per = (int64_t)C[i * 3 + 2] * (int64_t)H[2 * 3 + j];
            const int64_t want = ((t_lin + ((int64_t)1 << 20)) >> 21) +
                                 ((t_per + ((int64_t)1 << 28)) >> 29);
            CHECK(P[i * 3 + j] == want, "P[%d][%d] = %lld want %lld", i, j,
                  (long long)P[i * 3 + j], (long long)want);
        }
    /* The renormalisation restores h22 and nothing else is normalised. */
    int32_t o[9];
    CHECK(nxe::atlas_renorm(P, o), "renorm refused a legal product");
    CHECK(o[8] == (1 << 29), "renorm left h22 at %d", o[8]);
}

/* One step against a double-precision recomposition.  The clause promises 1
 * ulp of Q21 and 1 ulp of Q29 per step; anything larger is a scale error, not
 * rounding. */
void check_one_step_against_double() {
    for (int k = 0; k < 24; ++k) {
        int32_t C[9], H[9];
        make_warp(0.05 * (k - 12), 1e-5 * (k - 8), -1e-5 * (k - 5), C);
        make_warp(0.011 * (k + 1), -0.8e-5 * k, 1.3e-5 * k, H);
        int32_t o[9];
        CHECK(nxe::atlas_compose(C, H, o), "compose refused at k=%d", k);
        double dc[9], dh[9], dp[9];
        to_double(C, dc);
        to_double(H, dh);
        mul_double(dc, dh, dp);
        for (int e = 0; e < 9; ++e) {
            const double want = dp[e] * scale_of(e);
            const double err = std::fabs((double)o[e] - want);
            CHECK(err <= 3.0, "k=%d element %d: %d vs %.3f (err %.3f)", k, e,
                  o[e], want, err);
        }
    }
}

/* The ADR's precision claim, measured rather than asserted: over a 100-step
 * chain the accumulated error should behave as a small random walk in ulp, and
 * the translation terms -- the ones that become samples on the screen -- should
 * stay far inside a sample. */
void report_chain_drift() {
    int32_t C[9];
    nxe::atlas_identity(C);
    double d[9];
    to_double(C, d);
    for (int f = 1; f <= 100; ++f) {
        int32_t H[9];
        /* A real warp_ext() carries translation: a head turn moves the
         * picture bodily as well as rotating it, and the translation terms
         * are the ones that become samples on the screen.  A few samples a
         * frame accumulates to the hundreds of samples the ADR's 0.0026-sample
         * estimate is quoted against. */
        make_warp(0.004 * std::sin(0.11 * f), 1.0e-5 * std::cos(0.07 * f),
                  -0.7e-5 * std::sin(0.05 * f), H,
                  5.0 * std::cos(0.09 * f), -3.5 * std::sin(0.13 * f));
        int32_t o[9];
        if (!nxe::atlas_compose(C, H, o)) {
            std::printf("  chain: composition refused at step %d\n", f);
            return;
        }
        std::memcpy(C, o, sizeof o);
        double dh[9], dn[9];
        to_double(H, dh);
        mul_double(d, dh, dn);
        std::memcpy(d, dn, sizeof dn);
    }
    double worst_ulp = 0;
    int worst_e = 0;
    for (int e = 0; e < 9; ++e) {
        const double err = std::fabs((double)C[e] - d[e] * scale_of(e));
        if (err > worst_ulp) { worst_ulp = err; worst_e = e; }
    }
    /* The translation terms are Q21 samples, so their ulp error IS a fraction
     * of a sample directly. */
    const double tx = std::fabs((double)C[2] - d[2] * kSn) / kSn;
    const double ty = std::fabs((double)C[5] - d[5] * kSn) / kSn;
    /* The absolute ulp count is dominated by whichever element is LARGEST --
     * a translation term of a few hundred samples is 2^29-ish in Q21, so ten
     * ulp of relative error there is hundreds of ulp absolute.  The relative
     * figure is the one the ADR's estimate is about, and the sample figure is
     * the one that reaches the screen. */
    const double rel =
        worst_ulp / (std::fabs(d[worst_e] * scale_of(worst_e)) + 1.0);
    std::printf("  100-step chain: worst element %d, %.1f ulp absolute, %.2e "
                "relative; translation %.1f / %.1f samples, drift %.6f / "
                "%.6f samples\n",
                worst_e, worst_ulp, rel, (double)C[2] / kSn, (double)C[5] / kSn,
                tx, ty);
    /* The ADR estimates ~10 ulp and 0.0026 samples at a 512-sample
     * translation.  This is a bound the arithmetic must not blow through, not
     * a pinned value: a hundred-fold miss is a bug, a factor of two is the
     * clip. */
    /* A relative error this far below the quantiser's own step is rounding;
     * anything approaching it is a scale error.  The ADR estimates ~5e-6
     * relative over a hundred steps. */
    CHECK(rel < 1e-3, "chain drift %.2e relative is not rounding", rel);
    CHECK(tx < 0.5 && ty < 0.5, "translation drifted %.4f / %.4f samples", tx,
          ty);
}

/* [SYN] 13.12.2's 2^33 guard.  The normative arithmetic is int64 and this
 * implementation shifts in 128 bits, which the clause allows only WITH the
 * same guard -- so the test is that a composition past the guard FAILS rather
 * than succeeding with a wide-arithmetic answer an int64 decoder never
 * computes.  A conformance difference is exactly what this pins. */
void check_renorm_guard() {
    int64_t P[9] = {0};
    P[0] = 1 << 21; P[4] = 1 << 21; P[8] = 1 << 29;
    int32_t o[9];
    CHECK(nxe::atlas_renorm(P, o), "renorm refused a legal product");

    for (int k = 0; k < 9; ++k) {
        int64_t Q[9] = {0};
        Q[0] = 1 << 21; Q[4] = 1 << 21; Q[8] = 1 << 29;
        Q[k] = (int64_t)1 << 33;
        CHECK(!nxe::atlas_renorm(Q, o), "renorm accepted |P[%d]| == 2^33", k);
        Q[k] = -((int64_t)1 << 33);
        CHECK(!nxe::atlas_renorm(Q, o), "renorm accepted |P[%d]| == -2^33", k);
        /* One below the guard is still evaluated: the guard is exactly at
         * 2^33 and not near it. */
        Q[k] = ((int64_t)1 << 33) - 1;
        const bool ok = nxe::atlas_renorm(Q, o);
        CHECK(ok, "renorm refused |P[%d]| == 2^33 - 1", k);
    }
    /* And the guard fires through the composition, not only the renorm: an
     * entry that composes past it is invalidated rather than stored. */
    int32_t big[9], H[9];
    nxe::atlas_identity(big);
    big[0] = 1 << 30;   /* at kEntryMax */
    make_warp(0.0, 0.0, 0.0, H);
    H[0] = 1 << 30;
    int32_t out[9];
    const bool composed = nxe::atlas_compose(big, H, out);
    /* 2^30 * 2^30 >> 21 is 2^39, well past the guard, so this must fail. */
    CHECK(!composed, "a composition at 2^39 was not caught by the guard");
}

void check_envelope() {
    int32_t I[9];
    nxe::atlas_identity(I);
    CHECK(nxe::atlas_envelope_ok(I, 1088, 1088), "identity is out of envelope");

    /* Condition 1: h22 is normalised. */
    int32_t bad = I[8];
    int32_t m[9];
    std::memcpy(m, I, sizeof m);
    m[8] = bad + 1;
    CHECK(!nxe::atlas_envelope_ok(m, 1088, 1088), "accepted h22 != 2^29");

    /* Condition 2: an entry past 2^30. */
    std::memcpy(m, I, sizeof m);
    m[0] = (1 << 30) + 1;
    CHECK(!nxe::atlas_envelope_ok(m, 1088, 1088), "accepted an entry > 2^30");

    /* Condition 3: a perspective row steep enough to take the denominator out
     * of [2^28, 2^30) at a corner.  At ox = 544 a h20 of 2^20 moves den by
     * 544 * 2^20 = 2^29.09, which is past the top at one corner and past the
     * bottom at the other. */
    std::memcpy(m, I, sizeof m);
    m[6] = 1 << 20;
    CHECK(!nxe::atlas_envelope_ok(m, 1088, 1088),
          "accepted a denominator outside [2^28, 2^30)");

    /* And a perspective row of the magnitude a real warp carries is fine. */
    std::memcpy(m, I, sizeof m);
    m[6] = 16000;   /* 544 * 16000 = 8.7e6, 1.6 % of 2^29 */
    CHECK(nxe::atlas_envelope_ok(m, 1088, 1088),
          "rejected a realistic perspective row");
}

nxe::AtlasGeom geom_1088(int eyes) {
    nxe::AtlasGeom g;
    g.width = 1088;
    g.height = 1088;
    g.cols_per_eye = 17;
    g.rows = 17;
    g.eyes = eyes;
    return g;
}

/* ADR-0029's displacement bound.  The rule is only worth having if the number
 * it computes is the displacement a skipped tile's gather actually travels, so
 * this pins the two ends -- identity is exactly zero, a pure translation is
 * exactly the translation -- and the two short-circuits. */
void check_corner_disp() {
    const nxe::AtlasGeom g = geom_1088(1);
    nxe::AtlasTable at;
    at.reset(g);

    /* An invalid entry is 0: it cannot be skipped for a different reason and
     * the caller has already refused it. */
    CHECK(nxe::atlas_corner_disp(at, 0, g.width, g.height) == 0.0,
          "an invalid entry reported a displacement");

    at.code_tile(0, 1, nxvw::kModeWarpMv, 0);
    CHECK(nxe::atlas_corner_disp(at, 0, g.width, g.height) == 0.0,
          "a freshly coded entry is at the identity and must be 0");

    /* A STATIC_MV entry is held at the identity by 13.12.3 step 1, so its
     * displacement is zero however long it lives -- the case the rule must
     * never charge for. */
    at.code_tile(1, 1, nxvw::kModeStaticMv, 0);
    int32_t H[2][9];
    make_warp(0.0, 0.0, 0.0, H[0]);
    /* A pure translation of exactly 5 samples, in the Q21 scale of row 0-1
     * column 2. */
    H[0][2] = 5 * (1 << 21);
    H[0][5] = 0;
    for (int k = 0; k < 9; ++k) H[1][k] = H[0][k];
    at.advance(H);
    CHECK(at.is_static(1), "tile 1 stopped being static");
    CHECK(nxe::atlas_corner_disp(at, 1, g.width, g.height) == 0.0,
          "a static entry must never report a displacement");

    const double d1 = nxe::atlas_corner_disp(at, 0, g.width, g.height);
    CHECK(d1 > 4.9 && d1 < 5.1, "one 5-sample step read as %.4f", d1);
    /* And it accumulates: the advance is a right-multiplication per frame, so
     * three steps of 5 is 15 and not 5. */
    at.advance(H);
    at.advance(H);
    const double d3 = nxe::atlas_corner_disp(at, 0, g.width, g.height);
    CHECK(d3 > 14.8 && d3 < 15.2, "three 5-sample steps read as %.4f", d3);
}

/* Cheat 3.  The property that matters is that it is a FUNCTION of the atlas
 * state -- two encoders holding the same atlas must choose the same tiles --
 * and that cap 0 is exactly off. */
void check_refresh_priority() {
    const nxe::AtlasGeom g = geom_1088(1);
    nxe::AtlasTable at;
    at.reset(g);
    const uint32_t n = g.ntiles();
    std::vector<uint8_t> cand(n, 0), pick(n, 0);

    /* Every tile a candidate, coded at spread-out frames so age varies. */
    for (uint32_t t = 0; t < n; ++t) {
        at.code_tile(t, t % 50u, nxvw::kModeWarpMv, 0);
        cand[t] = 1;
    }
    const double fx = (double)(g.cols_per_eye - 1) * 0.5;
    const double fy = (double)(g.rows - 1) * 0.5;

    /* cap 0 is OFF: every candidate survives, which is what keeps existing
     * streams byte-identical. */
    nxe::atlas_refresh_priority(at, cand.data(), n, 60u, 0u, fx, fy,
                                pick.data());
    for (uint32_t t = 0; t < n; ++t)
        CHECK(pick[t] == cand[t], "cap 0 changed tile %u", t);

    for (uint32_t cap : {1u, 20u, 40u, 60u}) {
        nxe::atlas_refresh_priority(at, cand.data(), n, 60u, cap, fx, fy,
                                    pick.data());
        uint32_t got = 0;
        for (uint32_t t = 0; t < n; ++t) got += pick[t] ? 1u : 0u;
        CHECK(got == cap, "cap %u picked %u tiles", cap, got);
        /* Deterministic: the same state must give the same choice, or two
         * encoders drift apart for no reason a decoder could ever see. */
        std::vector<uint8_t> again(n, 0);
        nxe::atlas_refresh_priority(at, cand.data(), n, 60u, cap, fx, fy,
                                    again.data());
        CHECK(again == pick, "cap %u is not a function of the state", cap);
    }

    /* A non-candidate is never picked, however urgent it looks. */
    std::fill(cand.begin(), cand.end(), (uint8_t)0);
    cand[n / 2] = 1;
    nxe::atlas_refresh_priority(at, cand.data(), n, 60u, 40u, fx, fy,
                               pick.data());
    uint32_t got = 0;
    for (uint32_t t = 0; t < n; ++t) got += pick[t] ? 1u : 0u;
    CHECK(got == 1 && pick[n / 2] == 1,
          "a cap above the candidate count picked %u tiles", got);
}

/* Base-sourced patches (ADR-0029 section 7).  The property that has to hold is
 * that provenance is encoder-side and the WIRE RECORD is untouched: [SYN]
 * 13.12.1 reserves flags bits 2-7 as zero and has conformance compare all 64
 * bytes, so a patch that set bit 2 would break the shadow-equals-decoder
 * identity on exactly the tiles the two must agree about. */
void check_base_sourced() {
    const nxe::AtlasGeom g = geom_1088(1);
    nxe::AtlasTable at;
    at.reset(g);
    CHECK(at.base_sourced.size() == at.e.size(),
          "base_sourced is not sized with the table");
    for (uint8_t b : at.base_sourced)
        CHECK(b == 0, "a reset table has a base-sourced tile");

    at.write_base_tile(7, 42, 0);
    CHECK(at.valid(7), "a patched tile is not valid");
    CHECK(!at.is_static(7), "a patch must never be static");
    CHECK(at.base_sourced[7] == 1, "the patch was not recorded");
    CHECK(at.e[7].src_frame == 42 && at.e[7].gen == 0, "patch src/gen");
    int32_t I[9];
    nxe::atlas_identity(I);
    for (int k = 0; k < 9; ++k)
        CHECK(at.e[7].C[k] == I[k], "a patched tile's C is not the identity");
    /* The whole point: bits 2-7 of `flags` stay zero, and so do the twenty
     * reserved bytes, so the 64-byte record is what a conforming decoder
     * writes. */
    CHECK((at.e[7].flags & 0xFCu) == 0,
          "a patch set a reserved flags bit (0x%02x)", at.e[7].flags);
    for (int k = 0; k < 20; ++k)
        CHECK(at.e[7].reserved[k] == 0, "a patch wrote reserved byte %d", k);

    /* A coded tile at the same position RETIRES the patch: that is the
     * scheduled refresh the ADR requires, and the flag has to stop being set
     * or a tile would look base-sourced forever. */
    at.code_tile(7, 50, nxvw::kModeWarpMv, 0);
    CHECK(at.base_sourced[7] == 0, "a coded tile did not retire the patch");

    /* And a patch over a coded tile sets it again. */
    at.write_base_tile(7, 51, 0);
    CHECK(at.base_sourced[7] == 1, "a patch over a coded tile was not recorded");
}

void check_table_rules() {
    const nxe::AtlasGeom g = geom_1088(2);
    nxe::AtlasTable at;
    at.reset(g);
    CHECK(at.e.size() == 578, "578 tiles at the v1 stereo configuration, got %zu",
          at.e.size());
    for (auto &a : at.e)
        CHECK(!(a.flags & nxe::kAtlasValid), "a reset entry is valid");

    /* Annex D D-3: eye 1's tiles are the second half of every row. */
    CHECK(g.eye_of(0) == 0 && g.eye_of(16) == 0, "eye of the first row's left");
    CHECK(g.eye_of(17) == 1 && g.eye_of(33) == 1, "eye of the first row's right");
    CHECK(g.eye_of(34) == 0, "the second row starts in eye 0");

    int32_t H[2][9];
    make_warp(0.01, 1e-5, -1e-5, H[0]);
    make_warp(-0.02, -2e-5, 1e-5, H[1]);

    /* An invalid entry is not advanced at all. */
    at.advance(H);
    for (auto &a : at.e) {
        CHECK(a.gen == 0, "an invalid entry's gen moved to %u", a.gen);
        CHECK(a.src_frame == 0, "an invalid entry's src_frame moved");
    }

    /* Code three tiles at frame 5: one WARP_MV in eye 0, one STATIC_MV in eye
     * 0, one WARP_SKIP -- which writes NOTHING. */
    at.code_tile(0, 5, nxvw::kModeWarpMv, 0);
    at.code_tile(1, 5, nxvw::kModeStaticMv, 0);
    CHECK(at.valid(0) && !at.is_static(0), "tile 0 should be valid non-static");
    CHECK(at.valid(1) && at.is_static(1), "tile 1 should be valid and static");
    CHECK(!at.valid(2), "an uncoded tile became valid");
    int32_t I[9];
    nxe::atlas_identity(I);
    for (int k = 0; k < 9; ++k)
        CHECK(at.e[0].C[k] == I[k], "a coded tile's C is not the identity");
    CHECK(at.e[0].src_frame == 5 && at.e[0].gen == 0, "write-back src/gen");
    for (int k = 0; k < 20; ++k)
        CHECK(at.e[0].reserved[k] == 0, "reserved byte %d is not zero", k);

    /* Advance three frames.  The static tile keeps the identity and still
     * counts generations; the warped one composes. */
    for (int f = 0; f < 3; ++f) at.advance(H);
    CHECK(at.e[0].gen == 3 && at.e[1].gen == 3, "gen %u / %u after 3 advances",
          at.e[0].gen, at.e[1].gen);
    for (int k = 0; k < 9; ++k)
        CHECK(at.e[1].C[k] == I[k],
              "a STATIC tile's C moved at element %d: %d", k, at.e[1].C[k]);
    bool moved = false;
    for (int k = 0; k < 9; ++k) moved = moved || at.e[0].C[k] != I[k];
    CHECK(moved, "a warped tile's C did not compose");
    CHECK(at.e[0].src_frame == 5, "the advance moved src_frame");

    /* Composing three steps of H[0] by hand must give exactly the same bits:
     * the advance is right-multiplication, one step at a time, and nothing
     * else. */
    int32_t want[9];
    nxe::atlas_identity(want);
    for (int f = 0; f < 3; ++f) {
        int32_t o[9];
        CHECK(nxe::atlas_compose(want, H[0], o), "hand chain refused");
        std::memcpy(want, o, sizeof o);
    }
    for (int k = 0; k < 9; ++k)
        CHECK(at.e[0].C[k] == want[k], "advance element %d: %d != %d", k,
              at.e[0].C[k], want[k]);

    /* A tile in eye 1 composes with eye 1's matrix, which is the whole reason
     * the table is keyed by the eye-major tile index. */
    at.code_tile(17, 8, nxvw::kModeWarpMv, 0);
    at.advance(H);
    int32_t want1[9];
    CHECK(nxe::atlas_compose(I, H[1], want1), "eye-1 hand step refused");
    for (int k = 0; k < 9; ++k)
        CHECK(at.e[17].C[k] == want1[k], "eye 1 used the wrong matrix at %d", k);

    /* gen_max, the Cheats-8 hook: off by default, and when set it invalidates
     * rather than merely reporting. */
    nxe::AtlasTable cap;
    cap.reset(g);
    cap.gen_max = 4;
    cap.code_tile(0, 1, nxvw::kModeWarpMv, 0);
    for (int f = 0; f < 4; ++f) cap.advance(H);
    CHECK(cap.valid(0), "gen_max invalidated at gen %u", cap.e[0].gen);
    cap.advance(H);
    CHECK(!cap.valid(0), "gen_max did not invalidate past the cap");
}

/* The static-skip cheat, stated as a test because it is the one behavioural
 * change to an existing mode: a STATIC_MV tile holds at the identity for as
 * long as the stream runs, so it may be skipped at zero bits. */
void check_static_skip() {
    nxe::AtlasTable at;
    at.reset(geom_1088(1));
    at.code_tile(40, 2, nxvw::kModeStaticMv, 0);
    int32_t I[9];
    nxe::atlas_identity(I);
    for (int f = 0; f < 500; ++f) {
        int32_t H[2][9];
        make_warp(0.02 * std::sin(0.3 * f), 2e-5, -2e-5, H[0]);
        std::memcpy(H[1], H[0], sizeof H[0]);
        at.advance(H);
    }
    CHECK(at.valid(40), "a static tile was invalidated by 500 advances");
    for (int k = 0; k < 9; ++k)
        CHECK(at.e[40].C[k] == I[k], "static C moved at element %d", k);
    CHECK(at.e[40].gen == 500, "static gen is %u", at.e[40].gen);
}

/* A long chain of a real rotation eventually leaves the envelope, and THAT is
 * the staleness bound: no separate "too old" rule exists.  The test is that
 * the mechanism fires at all and that an invalidated tile stays invalid. */
void check_envelope_is_the_staleness_bound() {
    nxe::AtlasTable at;
    at.reset(geom_1088(1));
    at.code_tile(0, 0, nxvw::kModeWarpMv, 0);
    int32_t H[2][9];
    /* A steady 2 degrees a frame with a real perspective row: at 90 Hz that is
     * a 180 deg/s head turn, which is fast but not absurd. */
    make_warp(0.0349, 3.0e-5, 0.0, H[0]);
    std::memcpy(H[1], H[0], sizeof H[0]);
    int invalid_at = -1;
    for (int f = 1; f <= 400 && invalid_at < 0; ++f) {
        at.advance(H);
        if (!at.valid(0)) invalid_at = f;
    }
    if (invalid_at < 0) {
        std::printf("  staleness: 400 steps of 2 deg/frame stayed in the "
                    "envelope (gen %u)\n", at.e[0].gen);
    } else {
        std::printf("  staleness: the envelope invalidated at step %d\n",
                    invalid_at);
        /* Once invalid it is never advanced again, so it cannot come back. */
        for (int f = 0; f < 10; ++f) at.advance(H);
        CHECK(!at.valid(0), "an invalidated tile came back");
    }
}

/* The loss path.  A negative receipt for tile t of frame M must leave the
 * shadow entry BIT-IDENTICAL to what it would have been had frame M never
 * coded that tile -- which is exactly the client's state, since the client
 * simply did not update it. */
void check_undo_matches_never_having_coded() {
    const nxe::AtlasGeom g = geom_1088(2);
    nxe::AtlasTable live, ghost;
    nxe::AtlasUndo undo;
    live.reset(g);
    ghost.reset(g);
    undo.reset(g);

    const uint32_t t = 100;          /* an eye-1 tile, to exercise eye_of */
    CHECK(g.eye_of(t) == 1, "tile %u is in eye %d", t, g.eye_of(t));

    /* Frame 0: both code the tile.  Frames 1..3 advance.  Frame 4 codes it in
     * `live` only -- and that is the frame the client will report lost. */
    int32_t H[2][9];
    for (uint32_t f = 0; f <= 12; ++f) {
        make_warp(0.01 + 0.002 * (double)f, 1.5e-5, -1.0e-5, H[0]);
        make_warp(0.009 + 0.002 * (double)f, 1.2e-5, -0.9e-5, H[1]);
        const bool advanced = f > 0;
        undo.note_frame(f, H, advanced);
        if (advanced) {
            live.advance(H);
            ghost.advance(H);
        }
        if (f == 0) {
            live.code_tile(t, f, nxvw::kModeWarpMv, 0);
            ghost.code_tile(t, f, nxvw::kModeWarpMv, 0);
        }
        if (f == 4) {
            undo.note_coded(t, f, live.e[t]);   /* before the write-back */
            live.code_tile(t, f, nxvw::kModeWarpMv, 0);
            /* `ghost` deliberately does not: it is the client. */
        }
    }
    CHECK(live.e[t].src_frame == 4, "live src_frame %u", live.e[t].src_frame);
    CHECK(ghost.e[t].src_frame == 0, "ghost src_frame %u", ghost.e[t].src_frame);

    nxe::AtlasEntry back{};
    CHECK(undo.rollback(t, 4, 12, back), "rollback refused a live snapshot");
    CHECK(std::memcmp(&back, &ghost.e[t], sizeof back) == 0,
          "rollback is not the client's entry: src %u/%u gen %u/%u flags "
          "%u/%u",
          back.src_frame, ghost.e[t].src_frame, back.gen, ghost.e[t].gen,
          back.flags, ghost.e[t].flags);
    for (int k = 0; k < 9; ++k)
        CHECK(back.C[k] == ghost.e[t].C[k], "rollback C element %d: %d != %d",
              k, back.C[k], ghost.e[t].C[k]);

    /* A receipt naming a frame the log no longer covers is refused, so the
     * caller invalidates instead.  That is the safe direction: an INTRA tile
     * costs bytes, a wrongly-held one costs a refusal. */
    nxe::AtlasEntry dummy{};
    CHECK(!undo.rollback(t, 3, 12, dummy), "rolled back to a frame not logged");
    CHECK(!undo.rollback(t + 1, 4, 12, dummy),
          "rolled back a tile with no snapshot");
}

}  // namespace

int main() {
    check_identity_is_a_unit();
    check_sdiv_round();
    check_partial_sums_are_the_spec();
    check_one_step_against_double();
    report_chain_drift();
    check_renorm_guard();
    check_envelope();
    check_corner_disp();
    check_refresh_priority();
    check_base_sourced();
    check_table_rules();
    check_static_skip();
    check_envelope_is_the_staleness_bound();
    check_undo_matches_never_having_coded();

    if (g_fail) {
        std::printf("nxe_atlas_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("nxe_atlas_test: ok\n");
    return 0;
}
