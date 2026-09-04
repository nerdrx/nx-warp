// Test-only: deterministic random tile generation for Pass A round trips.
// Header-only so the harness and the ctest binaries share exactly one
// generator and therefore exactly one corpus for a given seed.
#ifndef NXWARP_PASSA_TEST_GEN_H
#define NXWARP_PASSA_TEST_GEN_H

#include <cstdint>
#include <vector>

#include "passA_test_encoder.h"

namespace nxwarp_passA {
namespace test {

// splitmix64 - small, deterministic, no <random> implementation variance.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed + 0x9e3779b97f4a7c15ull) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return uint32_t(next() >> 32); }
    uint32_t below(uint32_t n) { return n ? u32() % n : 0; }
    // Probability in parts per 1024.
    bool chance(uint32_t per1024) { return below(1024) < per1024; }
};

// Deterministic, legal probability tables: a decaying distribution per
// context, normalised so every row sums to kProbScale with no zero entry.
inline void make_tables(Tables &t, uint64_t seed) {
    Rng rng(seed);
    for (int k = 0; k < kNumTableSets; ++k)
        for (int c = 0; c < kNumCtx; ++c) {
            uint32_t w[kNumSym];
            uint32_t sum = 0;
            // Heavier head for CBF/LAST, longer tail for LEVEL contexts.
            uint32_t decay = 40 + rng.below(40);
            uint32_t acc = 1000 + rng.below(500);
            for (int s = 0; s < kNumSym; ++s) {
                w[s] = acc + 1 + rng.below(16);
                acc = acc * decay / 100 + 1;
                sum += w[s];
            }
            uint32_t f[kNumSym];
            uint32_t total = 0;
            for (int s = 0; s < kNumSym; ++s) {
                uint32_t v = uint32_t(uint64_t(w[s]) * kProbScale / sum);
                if (v < 1) v = 1;
                if (v > kProbScale - (kNumSym - 1)) v = kProbScale - (kNumSym - 1);
                f[s] = v;
                total += v;
            }
            while (total < kProbScale) {
                int best = 0;
                for (int s = 1; s < kNumSym; ++s) if (f[s] > f[best]) best = s;
                ++f[best]; ++total;
            }
            while (total > kProbScale) {
                int best = -1;
                for (int s = 0; s < kNumSym; ++s)
                    if (f[s] > 1 && (best < 0 || f[s] > f[best])) best = s;
                if (best < 0) break;
                --f[best]; --total;
            }
            for (int s = 0; s < kNumSym; ++s) t.freq[k][c][s] = uint16_t(f[s]);
        }
    finalize(t);
}

// Generates coefficients for one tile.  `density` is the per-1024 chance
// that a coded position is nonzero; `cbf_prob` the chance a unit is coded.
// The result always satisfies the syntax: the coefficient at the LAST scan
// position of a coded unit is nonzero.  Returns the number of coded
// symbols (entropy operations are counted by the encoder, not here).
inline void make_tile_coefs(const TileShape &shape,
                            const std::vector<UnitInfo> &units, int ncoef_total,
                            Rng &rng, uint32_t cbf_prob, uint32_t density,
                            uint32_t mean_last, std::vector<int16_t> &coef) {
    coef.assign(size_t(ncoef_total), 0);
    for (const UnitInfo &u : units) {
        if (!rng.chance(cbf_prob)) continue;
        // Geometric-ish LAST position, clamped to the unit.
        uint32_t last = 0;
        while (last + 1 < uint32_t(u.ncoef) && rng.chance(mean_last)) ++last;
        for (uint32_t p = 0; p <= last; ++p) {
            bool nz = (p == last) || rng.chance(density);
            if (!nz) continue;
            // Mostly small magnitudes; occasionally past the escape boundary.
            int32_t mag;
            uint32_t r = rng.below(1024);
            if (r < 600) mag = 1;
            else if (r < 850) mag = 2 + int32_t(rng.below(3));
            else if (r < 990) mag = 5 + int32_t(rng.below(10));
            else mag = kLevelMaxDirect + 1 + int32_t(rng.below(600));
            if (rng.chance(512)) mag = -mag;
            coef[size_t(u.coef_base + scan_index(u.scan_id, int(p)))] =
                int16_t(mag);
        }
    }
}

}  // namespace test
}  // namespace nxwarp_passA

#endif  // NXWARP_PASSA_TEST_GEN_H
