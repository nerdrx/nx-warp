// SPDX-License-Identifier: Apache-2.0
#include "atlas_model.h"

namespace nxvw {

void atlas_identity(int32_t C[9]) {
    C[0] = 1 << kWarpQNum;  C[1] = 0;               C[2] = 0;
    C[3] = 0;               C[4] = 1 << kWarpQNum;  C[5] = 0;
    C[6] = 0;               C[7] = 0;               C[8] = kWarpH22;
}

void atlas_compose_step(const int32_t C[9], const int32_t H[9], int64_t P[9]) {
    // [SYN] 13.12.2.  The two sums are rounded INDEPENDENTLY and at different
    // scales -- Q21 for the linear pair, Q29 for the perspective term -- which
    // is what keeps every intermediate inside int64 without 128-bit
    // arithmetic.  Summing first and rounding once would be a different
    // number, so the two shifts stay separate.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int64_t t_lin = (int64_t)C[i * 3 + 0] * (int64_t)H[0 * 3 + j] +
                                  (int64_t)C[i * 3 + 1] * (int64_t)H[1 * 3 + j];
            const int64_t t_per = (int64_t)C[i * 3 + 2] * (int64_t)H[2 * 3 + j];
            P[i * 3 + j] = ((t_lin + ((int64_t)1 << 20)) >> 21) +
                           ((t_per + ((int64_t)1 << 28)) >> 29);
        }
    }
}

int64_t atlas_sdiv_round(int64_t a, int64_t b) {
    // sign(a/b) * ((|a|*2 + |b|) / (|b|*2)), the division truncating toward
    // zero: round to nearest, ties away from zero.  Written on magnitudes so
    // the rounding does not depend on the sign of either operand, which a
    // shift-based form would get wrong for negatives.
    const bool neg = (a < 0) != (b < 0);
    const uint64_t ua = a < 0 ? (uint64_t)(-(a + 1)) + 1u : (uint64_t)a;
    const uint64_t ub = b < 0 ? (uint64_t)(-(b + 1)) + 1u : (uint64_t)b;
    const uint64_t q = (ua * 2u + ub) / (ub * 2u);
    return neg ? -(int64_t)q : (int64_t)q;
}

bool atlas_renorm(const int64_t P[9], int32_t out[9]) {
    const int64_t den = P[8];
    if (den == 0) return false;
    for (int k = 0; k < 9; ++k) {
        // `P << 29` must not overflow.  For any pair of matrices satisfying
        // 3.1.1 it cannot -- rows 0-1 of P are about 2^31 and row 2 about
        // 2^29 -- so this is a guard against being handed something illegal,
        // not a case the arithmetic is expected to reach.
        if (P[k] > (INT64_MAX >> 29) || P[k] < (INT64_MIN >> 29)) return false;
        const int64_t v = atlas_sdiv_round(P[k] << 29, den);
        if (v > INT32_MAX || v < INT32_MIN) return false;
        out[k] = (int32_t)v;
    }
    return true;
}

bool atlas_in_envelope(const int32_t C[9], int32_t width, int32_t height) {
    // [SYN] 3.1.1 condition 1 is structural and 13.12.2's renorm restores it,
    // so it is checked rather than assumed.
    if (C[8] != kWarpH22) return false;
    // Condition 2: every entry within +-kEntryMax.
    for (int k = 0; k < 9; ++k)
        if (C[k] < -kWarpEntryMax || C[k] > kWarpEntryMax) return false;
    // Condition 3: at each of the four picture corners the denominator is
    // accumulated in 64 bits, must fit int32, and must lie in
    // [kWarpDenMin, kWarpDenMax).
    const int32_t ox = width >> 1, oy = height >> 1;
    const int32_t cxs[2] = {-ox, width - ox};
    const int32_t cys[2] = {-oy, height - oy};
    for (int a = 0; a < 2; ++a) {
        for (int b = 0; b < 2; ++b) {
            const int64_t den = (int64_t)C[6] * (int64_t)cxs[a] +
                                (int64_t)C[7] * (int64_t)cys[b] +
                                (int64_t)C[8];
            if (den > INT32_MAX || den < INT32_MIN) return false;
            if (den < kWarpDenMin || den >= kWarpDenMax) return false;
        }
    }
    return true;
}

bool atlas_advance(int32_t C[9], const int32_t H[9], int32_t width,
                   int32_t height) {
    int64_t P[9];
    atlas_compose_step(C, H, P);
    int32_t out[9];
    if (!atlas_renorm(P, out)) return false;
    if (!atlas_in_envelope(out, width, height)) return false;
    for (int k = 0; k < 9; ++k) C[k] = out[k];
    return true;
}

bool atlas_advance_entry(AtlasEntry &e, const int32_t H[9], int32_t width,
                         int32_t height, uint32_t gen_max) {
    if ((e.flags & NXVW_ATLAS_FLAG_VALID) == 0u) return false;
    // `gen` counts composition steps since src_frame and increments for a
    // static entry too -- it is the cadence clock, not a count of matrix
    // multiplications ([SYN] 13.12.3 step 1).
    e.gen += 1u;
    if ((e.flags & NXVW_ATLAS_FLAG_STATIC) == 0u) {
        if (!atlas_advance(e.C, H, width, height)) {
            e.flags &= ~NXVW_ATLAS_FLAG_VALID;
            return false;
        }
    }
    if (gen_max != 0u && e.gen > gen_max) {
        e.flags &= ~NXVW_ATLAS_FLAG_VALID;
        return false;
    }
    return true;
}

}  // namespace nxvw
