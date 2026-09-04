// rANS + lane-machine round trip on random and pathological coefficient sets.
#include "test_util.h"
#include "common.h"
#include "entropy.h"

using namespace nxvc;

// Builds a unit list over a coefficient buffer laid out as `nunits` blocks of
// `ncoef` levels each, mirroring the tile layout.
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
                      int nlanes, const TableSet &tabs, size_t *bytes) {
    std::vector<i16> orig = coef;
    std::vector<u8> payload;
    if (!encode_units(units.data(), (int)units.size(), nlanes, tabs, payload))
        return false;
    if (bytes) *bytes = payload.size();
    std::fill(coef.begin(), coef.end(), (i16)0);
    if (!decode_units(units.data(), (int)units.size(), nlanes, tabs,
                      payload.data(), payload.size()))
        return false;
    return coef == orig;
}

int main() {
    TableSet tabs[8];
    for (int i = 0; i < 8; ++i) build_default_set(tabs[i], i);

    // 1. Default tables are well formed.
    for (int k = 0; k < 8; ++k)
        for (int c = 0; c < kNumCtx; ++c) {
            u32 sum = 0;
            for (int s = 0; s < kNumSym; ++s) {
                CHECK(tabs[k].ctx[c].freq[s] >= 1, "set %d ctx %d sym %d freq 0",
                      k, c, s);
                sum += tabs[k].ctx[c].freq[s];
            }
            CHECK(sum == (u32)kProbTotal, "set %d ctx %d sums to %u", k, c, sum);
        }

    // 2. Random coefficient fields, every lane count, both scans.
    {
        Rng rng(1234);
        for (int nl_log2 = 0; nl_log2 <= 5; ++nl_log2) {
            int nlanes = 1 << nl_log2;
            for (int trial = 0; trial < 24; ++trial) {
                int nunits = rng.range(1, 40);
                std::vector<i16> coef;
                std::vector<Unit> units;
                bool tskip = (trial & 1) != 0;
                make_units(coef, units, nunits, 64, tskip);
                int density = rng.range(0, 100);
                int maxmag = 1 << rng.range(0, 12);
                for (auto &c : coef)
                    if ((int)(rng.next() % 100) < density)
                        c = (i16)clamp_i32(rng.range(-maxmag, maxmag), -32767, 32767);
                CHECK(roundtrip(coef, units, nlanes, tabs[trial % 8], nullptr),
                      "random rt failed nlanes=%d trial=%d", nlanes, trial);
            }
        }
    }

    // 3. Pathological: all zero, single coefficient, all maximum magnitude,
    //    DC only, LAST at the very end, alternating signs.
    {
        std::vector<i16> coef;
        std::vector<Unit> units;
        size_t bytes = 0;

        make_units(coef, units, 9, 64, false);
        CHECK(roundtrip(coef, units, 8, tabs[0], &bytes), "all-zero rt");
        CHECK(bytes == 32, "all-zero payload is %zu bytes (expected 8x4)", bytes);

        make_units(coef, units, 9, 64, false);
        coef[63] = 1;
        CHECK(roundtrip(coef, units, 8, tabs[0], nullptr), "single coef rt");

        make_units(coef, units, 8, 64, false);
        for (auto &c : coef) c = 32767;
        CHECK(roundtrip(coef, units, 8, tabs[0], nullptr), "max magnitude rt");

        make_units(coef, units, 8, 64, false);
        for (auto &c : coef) c = -32767;
        CHECK(roundtrip(coef, units, 1, tabs[7], nullptr), "min magnitude rt");

        make_units(coef, units, 16, 64, false);
        for (int i = 0; i < 16; ++i) coef[(size_t)i * 64] = (i16)(i - 8);
        CHECK(roundtrip(coef, units, 8, tabs[3], nullptr), "dc-only rt");

        make_units(coef, units, 5, 64, false);
        for (int i = 0; i < 5; ++i) {
            coef[(size_t)i * 64 + kZigzag8[63]] = 3;
            coef[(size_t)i * 64 + kZigzag8[0]] = -14;
            coef[(size_t)i * 64 + kZigzag8[1]] = 15;
            coef[(size_t)i * 64 + kZigzag8[2]] = 16;
        }
        CHECK(roundtrip(coef, units, 8, tabs[0], nullptr), "escape boundary rt");

        // every escape prefix length
        make_units(coef, units, 8, 64, false);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 17 && j < 64; ++j) {
                int v = 15 + (1 << j) - 8;
                coef[(size_t)i * 64 + kZigzag8[j]] =
                    (i16)clamp_i32(v, -32767, 32767);
            }
        CHECK(roundtrip(coef, units, 8, tabs[0], nullptr), "escape ladder rt");
    }

    // 4. Small units (the DC planes of low-resolution tiles): 16, 4 and 1.
    {
        Rng rng(99);
        for (int nc : {16, 4, 1})
            for (int trial = 0; trial < 20; ++trial) {
                std::vector<i16> coef;
                std::vector<Unit> units;
                make_units(coef, units, rng.range(1, 12), nc, false);
                for (auto &c : coef)
                    if (rng.next() % 3) c = (i16)rng.range(-300, 300);
                CHECK(roundtrip(coef, units, 8, tabs[trial % 8], nullptr),
                      "small unit rt ncoef=%d trial=%d", nc, trial);
            }
    }

    // 5. Skewed tables still round trip (every symbol must stay decodable).
    {
        TableSet skew;
        for (int c = 0; c < kNumCtx; ++c) {
            for (int s = 0; s < kNumSym; ++s) skew.ctx[c].freq[s] = 1;
            skew.ctx[c].freq[c % kNumSym] = (u16)(kProbTotal - 15);
            CHECK(finalize_ctx(skew.ctx[c]), "skew ctx %d finalize", c);
        }
        Rng rng(555);
        for (int trial = 0; trial < 20; ++trial) {
            std::vector<i16> coef;
            std::vector<Unit> units;
            make_units(coef, units, 10, 64, false);
            for (auto &c : coef)
                if (rng.next() % 4 == 0) c = (i16)rng.range(-2000, 2000);
            CHECK(roundtrip(coef, units, 8, skew, nullptr), "skew rt %d", trial);
        }
    }

    // 6. Truncated payloads must be rejected, never crash.
    {
        std::vector<i16> coef;
        std::vector<Unit> units;
        make_units(coef, units, 12, 64, false);
        Rng rng(777);
        for (auto &c : coef)
            if (rng.next() % 2) c = (i16)rng.range(-500, 500);
        std::vector<u8> payload;
        CHECK(encode_units(units.data(), (int)units.size(), 8, tabs[0], payload),
              "encode for truncation test");
        for (size_t cut = 0; cut < payload.size(); cut += 7) {
            std::fill(coef.begin(), coef.end(), (i16)0);
            decode_units(units.data(), (int)units.size(), 8, tabs[0],
                         payload.data(), cut);  // must not crash
        }
    }

    // 7. Random bytes fed to the unit decoder must not crash.
    {
        Rng rng(31337);
        std::vector<u8> junk(4096);
        for (int trial = 0; trial < 400; ++trial) {
            for (auto &b : junk) b = (u8)rng.next();
            std::vector<i16> coef;
            std::vector<Unit> units;
            make_units(coef, units, 9, 64, (trial & 1) != 0);
            size_t len = rng.next() % junk.size();
            decode_units(units.data(), (int)units.size(), 8, tabs[trial % 8],
                         junk.data(), len);
        }
    }

    return test_report("test_rans");
}
