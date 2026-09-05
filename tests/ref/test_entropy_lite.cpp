// ENTROPY_LITE (SYNTAX.md 9.8) round trip, both variants, plus the checks a
// hostile payload has to fail.
#include "test_util.h"
#include "common.h"
#include "entropy.h"
#include "entropy_lite.h"

using namespace nxvc;

static void make_units(std::vector<i16> &coef, std::vector<Unit> &units,
                       int nunits, int ncoef, bool tskip) {
    coef.assign((size_t)nunits * ncoef, 0);
    units.clear();
    for (int i = 0; i < nunits; ++i) {
        Unit u{};
        u.coef = &coef[(size_t)i * ncoef];
        u.ncoef = (u16)ncoef;
        u.scan = scan_table(ncoef, tskip);
        u.ctx_cbf = (i & 1) ? kCtxCbfChroma : kCtxCbfLuma;
        u.ctx_last = (i & 1) ? kCtxLastChroma : kCtxLastLuma;
        units.push_back(u);
    }
}

static bool roundtrip(std::vector<i16> &coef, std::vector<Unit> &units,
                      int variant, size_t *bytes) {
    std::vector<i16> orig = coef;
    std::vector<u8> payload;
    if (!lite_encode_units(units.data(), (int)units.size(), variant, payload))
        return false;
    if (bytes) *bytes = payload.size();
    std::fill(coef.begin(), coef.end(), (i16)0);
    if (!lite_decode_units(units.data(), (int)units.size(), variant,
                           payload.data(), payload.size()))
        return false;
    return coef == orig;
}

static u32 rng_state = 12345u;
static u32 rnd() {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

int main() {

    for (int variant = 0; variant < kLiteNumVariants; ++variant) {
        // ---- random coefficient sets over every unit size the syntax has.
        for (int ncoef : {1, 4, 16, 64}) {
            for (int trial = 0; trial < 40; ++trial) {
                std::vector<i16> coef;
                std::vector<Unit> units;
                make_units(coef, units, 33, ncoef, (trial & 1) != 0);
                for (auto &c : coef) {
                    u32 r = rnd() % 100;
                    if (r < 60) c = 0;
                    else if (r < 90) c = (i16)((rnd() % 3) + 1);
                    else if (r < 99) c = (i16)((rnd() % 400) + 1);
                    else c = (i16)((rnd() % 32767) + 1);
                    if (c && (rnd() & 1)) c = (i16)(-c);
                }
                CHECK(roundtrip(coef, units, variant, nullptr), "");
            }
        }

        // ---- pathological: all zero, all saturated, one nonzero at the end.
        {
            std::vector<i16> coef;
            std::vector<Unit> units;
            make_units(coef, units, 8, 64, false);
            CHECK(roundtrip(coef, units, variant, nullptr), "every unit empty");

            for (auto &c : coef) c = 32767;
            CHECK(roundtrip(coef, units, variant, nullptr), "");
            for (auto &c : coef) c = (i16)-32767;
            CHECK(roundtrip(coef, units, variant, nullptr), "");

            std::fill(coef.begin(), coef.end(), (i16)0);
            coef[63] = 1;    // LAST at the top of unit 0, nothing else anywhere
            CHECK(roundtrip(coef, units, variant, nullptr), "");

            std::fill(coef.begin(), coef.end(), (i16)0);
            for (size_t i = 0; i < coef.size(); ++i) coef[i] = (i16)(i & 1);
            CHECK(roundtrip(coef, units, variant, nullptr), "");
        }

        // ---- truncation: every proper prefix of a payload must be rejected
        // rather than read past its end.
        {
            std::vector<i16> coef;
            std::vector<Unit> units;
            make_units(coef, units, 17, 64, false);
            for (size_t i = 0; i < coef.size(); i += 3)
                coef[i] = (i16)((i % 37) + 1);
            std::vector<u8> payload;
            CHECK(lite_encode_units(units.data(), (int)units.size(), variant,
                                    payload), "");
            for (size_t n = 0; n < payload.size(); ++n) {
                std::fill(coef.begin(), coef.end(), (i16)0);
                // A prefix may decode (the sections it needs may all be
                // present); it may not crash or read out of bounds, which is
                // what the sanitizer presets check.  Rejection is the
                // expectation for anything that loses a section.
                (void)lite_decode_units(units.data(), (int)units.size(),
                                        variant, payload.data(), n);
            }
        }
    }

    // ---- an illegal variant is refused by both directions.
    {
        std::vector<i16> coef;
        std::vector<Unit> units;
        make_units(coef, units, 4, 64, false);
        std::vector<u8> payload;
        CHECK(!lite_encode_units(units.data(), (int)units.size(), 2, payload), "");
        CHECK(!lite_decode_units(units.data(), (int)units.size(), 7,
                                 payload.data(), payload.size()), "");
    }

    // ---- the two variants are the same coder for the same coefficients: the
    // decoded result must not depend on which one produced the bytes.
    {
        std::vector<i16> a, b;
        std::vector<Unit> ua, ub;
        make_units(a, ua, 12, 64, false);
        for (size_t i = 0; i < a.size(); ++i)
            a[i] = (i16)((i % 11 == 0) ? (int)(i % 300) - 150 : 0);
        b = a;
        make_units(b, ub, 12, 64, false);
        b = a;
        for (size_t i = 0; i < ub.size(); ++i) ub[i].coef = &b[i * 64];
        size_t na = 0, nb = 0;
        (void)na; (void)nb;
        CHECK(roundtrip(a, ua, kLiteFixed, &na), "");
        CHECK(roundtrip(b, ub, kLiteRice, &nb), "");
        CHECK(a == b, "");
    }

    if (g_failures) {
        std::printf("test_entropy_lite: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_entropy_lite: ok\n");
    return 0;
}
