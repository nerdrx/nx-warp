// NX Warp decoder, ATLAS: [SYN] 13.12.2's composition in GLSL.
//
// The arithmetic only.  No buffers, no bindings, no entry point -- so this
// file is includable by the compose kernel and by anything else that later
// needs a composed matrix, and it is the one place the GPU's transcription of
// 13.12.2 lives.
//
// NORMATIVE SOURCE: docs/SYNTAX.md 13.12.2 and 3.1.1.
// CHECKED AGAINST: vk/decoder/atlas/atlas_model.cpp, itself checked against a
// 128-bit oracle (tests/vk-decoder/atlas) and pinned to ref/src/inter.h.
//
// ---------------------------------------------------------------------------
// WHY THE 64-BIT PRIMITIVES ARE COPIED AND NOT INCLUDED
//
// inter/warp_pred.glsl already carries umul_ext/imul_ext/add64/neg64/shl64/
// from_i32 and a restoring divide.  They are NOT included from there and
// warp_pred.glsl is NOT modified, for two reasons.  It is the one kernel in
// this decoder pinned byte-for-byte against the encoder (`vk.encoder.passw.
// same`) and ADR-0029 turns on it being untouched; and its `warp_div` is a
// 32-bit-quotient divide with the precondition `n.hi < d`, which the corner
// derivation guarantees and 13.12.2's renormalisation does not -- the
// numerator here is `|P| << 30` against a divisor of about 2^30, so the
// quotient does not fit 32 bits until after the envelope check has passed.
// So the primitives are transcribed and the divide is a different, wider one.
// The harness compares this kernel against the CPU model over random legal
// matrices, which is what keeps the two transcriptions honest.
// ---------------------------------------------------------------------------
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_ATLAS_ARITH_GLSL
#define NXVW_ATLAS_ARITH_GLSL

// ------------------------------------------------------- emulated 64-bit
// u64 is (x = lo, y = hi), the same convention warp_pred.glsl uses.
uvec2 atlas_umul_ext(uint a, uint b) {
    uint msb, lsb;
    umulExtended(a, b, msb, lsb);   // OpUMulExtended
    return uvec2(lsb, msb);
}

uvec2 atlas_imul_ext(int a, int b) {
    uvec2 r = atlas_umul_ext(uint(a), uint(b));
    // signed_hi = unsigned_hi - (a<0 ? b : 0) - (b<0 ? a : 0)
    if (a < 0) r.y -= uint(b);
    if (b < 0) r.y -= uint(a);
    return r;
}

uvec2 atlas_add64(uvec2 a, uvec2 b) {
    uint lo = a.x + b.x;
    uint hi = a.y + b.y + ((lo < a.x) ? 1u : 0u);
    return uvec2(lo, hi);
}

uvec2 atlas_sub64(uvec2 a, uvec2 b) {
    uint lo = a.x - b.x;
    uint hi = a.y - b.y - ((a.x < b.x) ? 1u : 0u);
    return uvec2(lo, hi);
}

uvec2 atlas_neg64(uvec2 a) {
    uint lo = ~a.x + 1u;
    uint hi = ~a.y + ((lo == 0u) ? 1u : 0u);
    return uvec2(lo, hi);
}

uvec2 atlas_from_i32(int v) { return uvec2(uint(v), (v < 0) ? 0xffffffffu : 0u); }

bool atlas_neg64_p(uvec2 a) { return (a.y & 0x80000000u) != 0u; }

uvec2 atlas_abs64(uvec2 a) { return atlas_neg64_p(a) ? atlas_neg64(a) : a; }

// Arithmetic right shift, 1 <= n <= 31.  The spec's `>>` on a signed 64-bit
// value is arithmetic, i.e. floor division, and getting that wrong on a
// negative intermediate is a one-LSB error in the composed matrix that only
// shows on half the inputs.  GLSL's `>>` on a signed int is arithmetic.
uvec2 atlas_sar64(uvec2 a, uint n) {
    uint lo = (a.x >> n) | (a.y << (32u - n));
    uint hi = uint(int(a.y) >> int(n));
    return uvec2(lo, hi);
}

// Logical left shift, 1 <= n <= 31.
uvec2 atlas_shl64(uvec2 a, uint n) {
    return uvec2(a.x << n, (a.y << n) | (a.x >> (32u - n)));
}

bool atlas_uge64(uvec2 a, uvec2 b) {
    return (a.y > b.y) || (a.y == b.y && a.x >= b.x);
}

// Fixed 64-iteration restoring division, branchless, full 64-bit quotient.
//
// Precondition: d != 0.  Unlike warp_pred.glsl's warp_div there is no
// `n.hi < d` precondition and no 32-bit quotient assumption, because
// 13.12.2's numerator is `|P| << 30` (up to 2^63 under the 2^33 guard) and
// nothing bounds the divisor `|P[2][2]| * 2` from below until the envelope
// check that comes AFTER this.  An out-of-range quotient has to come back as
// a number the caller can reject, not as a wrap.
uvec2 atlas_udiv64(uvec2 n, uvec2 d) {
    uvec2 rem = uvec2(0u, 0u);
    uvec2 q = uvec2(0u, 0u);
    for (int k = 63; k >= 0; --k) {
        uint bit = (k >= 32) ? ((n.y >> uint(k - 32)) & 1u)
                             : ((n.x >> uint(k)) & 1u);
        rem = uvec2((rem.x << 1) | bit, (rem.y << 1) | (rem.x >> 31));
        // A select, not a masked subtract: masking the divisor word by word
        // and subtracting unconditionally gets the borrow wrong, because the
        // borrow out of the low word depends on the masked value.
        bool ge = atlas_uge64(rem, d);
        rem = ge ? atlas_sub64(rem, d) : rem;
        uint gb = ge ? 1u : 0u;
        if (k >= 32) q.y |= gb << uint(k - 32);
        else         q.x |= gb << uint(k);
    }
    return q;
}

// [SYN] 13.12.2: sign(a/b) * ((|a|*2 + |b|) / (|b|*2)), the division
// truncating toward zero -- round to nearest, ties away from zero.  Written
// on magnitudes so the rounding does not depend on the sign of either
// operand, which a shift-based form gets wrong for negatives.
//
// `neg` is returned separately rather than folded in, because the quotient is
// unrestricted here: negating an out-of-range magnitude and then range
// checking loses the case at exactly 2^31.
uvec2 atlas_sdiv_round_mag(uvec2 a, uvec2 b, out bool neg) {
    neg = atlas_neg64_p(a) != atlas_neg64_p(b);
    uvec2 ua = atlas_abs64(a);
    uvec2 ub = atlas_abs64(b);
    uvec2 num = atlas_add64(atlas_shl64(ua, 1u), ub);
    return atlas_udiv64(num, atlas_shl64(ub, 1u));
}

// ------------------------------------------------- 13.12.2, one step
// P = C . H, with TWO INDEPENDENTLY ROUNDED partial sums at different scales
// -- Q21 for the linear pair, Q29 for the perspective term.  That is not
// laziness about the rounding: it is what keeps every intermediate inside 64
// bits.  Summing first and rounding once is a different number.
//
// One element, so the nine can be written out with literal indices and this
// kernel keeps no function-scope array at all (docs/ADRENO-RULES.md rule 1:
// an array the compiler cannot prove is constant-indexed lands in private
// memory on the Adreno 650, and in one measured case was read back wrong).
uvec2 atlas_pelem(int c0, int c1, int c2, int h0, int h1, int h2) {
    uvec2 t_lin = atlas_add64(atlas_imul_ext(c0, h0), atlas_imul_ext(c1, h1));
    uvec2 t_per = atlas_imul_ext(c2, h2);
    return atlas_add64(
        atlas_sar64(atlas_add64(t_lin, atlas_from_i32(1 << 20)), 21u),
        atlas_sar64(atlas_add64(t_per, atlas_from_i32(1 << 28)), 29u));
}

// [SYN] 13.12.2's normative guard, over one element: |P[k]| >= 2^33 fails the
// composition, and it is tested over ALL NINE before any shift is evaluated.
// 2^33 is (hi = 2, lo = 0), so the predicate is `abs64(P).hi >= 2`.
bool atlas_pguard_fails(uvec2 p) {
    return atlas_abs64(p).y >= NXVW_ATLAS_PGUARD_HI;
}

// One renormalised element: sdiv_round(P[k] << 29, P[8]), rejected when it
// leaves +-kWarpEntryMax.  The reference rejects here rather than deferring
// to 3.1.1 condition 2, and the two must fail at the same step or the table
// they leave behind differs.
bool atlas_renorm_elem(uvec2 p, uvec2 den, out int v) {
    bool neg;
    uvec2 q = atlas_sdiv_round_mag(atlas_shl64(p, 29u), den, neg);
    if (q.y != 0u || q.x > uint(kWarpEntryMax)) { v = 0; return false; }
    v = neg ? -int(q.x) : int(q.x);
    return true;
}

#endif  // NXVW_ATLAS_ARITH_GLSL
