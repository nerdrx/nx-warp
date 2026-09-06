/* nxe_rate.h -- an exact integer bit cost for a tile, in Q10 bits.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The piece ADR 0028 deferred, and the one thing standing between the
 * reference encoder's per-tile QP search (-6.8 % BD-rate,
 * vk/encoder/README.md "Why there is no level 2") and the GPU.
 *
 * WHY THE REFERENCE'S RATE MODEL COULD NOT CROSS
 * ----------------------------------------------
 * `ref/src/codec.cpp`'s `build_rate_cost` fills a Q10 table with
 * `-std::log2(freq / 1024.0) * 1024 + 0.5`, and `table_set_cost` sums
 * `std::log2` terms in `double`.  Neither survives the trip: `log2` is not the
 * same function on a host libm and on a device, and a rate model that
 * disagrees by one unit in the last place is a mode decision that picks a
 * different tile, which is a different stream.
 *
 * WHAT THIS IS INSTEAD
 * --------------------
 * Two observations, and no new syntax:
 *
 *   1. The cost of a symbol is `-log2(freq / kProbTotal)`, `freq` is 10 bits,
 *      so there are exactly 1024 possible costs.  `nxe_neglog2_q10` is those
 *      1024 numbers, generated once by `scripts/gen-neglog2.py` and checked
 *      in.  A table lookup is the same function everywhere; `log2` is not.
 *
 *   2. `nxe_unit_ops` ALREADY produces the exact symbol stream -- it is what
 *      E4 encodes -- as a list of (context, symbol) and (bypass, bitcount)
 *      operations.  So the rate of a unit is a sum over that list, and the
 *      model cannot drift from the coder because it IS the coder's own
 *      operation list.  Nothing here re-implements the syntax.
 *
 * The result is exact in the sense that matters: it is the entropy of the
 * symbols that will actually be coded, under the table that will actually
 * code them, in integers, identically on every device.  What it is NOT is a
 * byte count -- see the tolerance note on `nxe_tile_bits_q10`.
 *
 * The GLSL mirror is `nxe_rate.glsl`; `vk.encoder.mirror` pins the constants.
 */

#ifndef NXE_RATE_H
#define NXE_RATE_H

#include <stdint.h>

#include "nxe_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Q10 bits, indexed by a 10-bit rANS frequency.  See the file comment. */
#define NXE_PROB_TOTAL (1 << NXE_PROB_BITS)

static const uint16_t nxe_neglog2_q10[NXE_PROB_TOTAL + 1] = {
#include "nxe_neglog2.inc"
};

/* The cost of one operation list, in Q10 bits.
 *
 * `freq` is `&tabs->freq[set][0][0]` -- the frequency plane for this tile's
 * table set, the same `TAB_FREQ(set, ctx, sym)` layout E4 reads, so the caller
 * passes the table set it will encode with and not the one it would like to
 * have used.  The row stride is NXE_NUM_SYM.
 *
 * A bypass operation costs exactly its bit count -- rANS codes `k` bypass bits
 * with `f = 1 << (10 - k)`, which is `k` bits with no rounding at all
 * (`ref/src/entropy.cpp`, the `bypass` path), so this term is not an estimate.
 */
static inline uint32_t nxe_ops_bits_q10(const uint32_t *ops, int n,
                                        const uint32_t *freq) {
    uint32_t bits = 0;
    int i;
    for (i = 0; i < n; ++i) {
        const uint32_t w = ops[i];
        if (NXE_OP_KIND(w) == NXE_OP_BYPASS) {
            bits += (uint32_t)NXE_OP_ARG(w) << 10;
        } else {
            const uint32_t f =
                freq[NXE_OP_ARG(w) * NXE_NUM_SYM + NXE_OP_VALUE(w)];
            bits += nxe_neglog2_q10[f];
        }
    }
    return bits;
}

/* What a tile costs beyond its symbols, in Q10 bits.
 *
 * Two terms outside the entropy coder: the 8-byte tile header, exactly, and
 * the rANS state flush, which is where the one modelling choice in this file
 * is.
 *
 * THE FLUSH IS 32 BITS ON THE WIRE AND COSTS 24.
 *
 * Each active lane writes its final 32-bit state.  But the state is not 32
 * bits of new information: it lives in [L, 2^16 * L) with L = 2^16, so the
 * symbols coded last are still inside it when it is flushed -- and this model
 * has already charged them as entropy.  The duplicated part is
 * `log2(x / L)`, uniform over [0, 16), so 8 bits in expectation, and the net
 * cost of the flush is 32 - 8 = 24 bits per lane.
 *
 * The reference charges the full 32 (`codec_impl.inc`'s
 * `bits += 8 * (kTileHeaderBytes + 4 * active)`).  It can afford to: every
 * candidate in its QP search has the same lane count, so a constant term
 * cancels out of the comparison and only the ranking matters there.  This
 * model is also read as an absolute size -- it is what a rate allocator would
 * ask -- so it is worth being unbiased rather than merely monotone.  Measured
 * on pan8 at 1, 2, 8 and 32 lanes, the full-32 charge is +0.80 %, +1.69 %,
 * +4.63 % biased; at 24 it is -0.04 %, +0.06 %, -1.02 % and -6.88 %.
 *
 * The last of those is the expectation argument failing where it should: with
 * 32 lanes a lane codes few enough symbols that its state never mixes, so less
 * of the flush duplicates and the true charge is nearer 28 bits.  24 is chosen
 * for 1..8 lanes, 8 being the shipped default (`--nsub 3`, and what Lite
 * forces).  A model that tracked the lane count would be fitting a curve to
 * four points; this is one number with a derivation and a documented edge.
 * See `--rate-check` and vk/encoder/README.md.
 *
 * `active` is `min(1 << nsub_log2, nunits)`, the same clamp E4 applies: a tile
 * with fewer units than lanes does not flush the lanes it never used.
 */
#define NXE_RATE_LANE_FLUSH_BITS 24

static inline uint32_t nxe_tile_overhead_bits_q10(int active) {
    return (uint32_t)(8 * NXE_TILE_HEADER_BYTES +
                      NXE_RATE_LANE_FLUSH_BITS * active)
           << 10;
}

/* Lite (ENTROPY_LITE, tool 30) is not estimated at all -- it is COUNTED.
 *
 * There is no arithmetic coder to be within a fraction of a bit of: every field
 * Lite writes is a fixed width, so a tile's payload is a sum of widths.  The
 * only term that is not a plain sum is the align-to-byte at the end of each of
 * the five sections (H0, H1, P, S, B; see lite_cpu.h) -- and that is a sum one
 * level up, because a section's bit total is additive over units and its pad is
 * a function of that total.
 *
 * So this takes five section totals and rounds each, which is five workgroup
 * reductions rather than one and no serial bit-writer.  `nxe_lite_tile_bits_q10`
 * in lite_cpu.c fills them; the result is EXACT, and `--rate-check` requires it
 * to equal what `nxe_lite_tile` actually wrote, to the bit.
 *
 * The tile header is added here for the same reason it is in the rANS path: a
 * decision comparing candidates should be comparing whole tiles.
 */
static inline uint32_t nxe_align8(uint32_t bits) {
    return (bits + 7u) & ~7u;
}

static inline uint32_t nxe_lite_bits_q10(int h0, int h1, int p, int s, int b) {
    const uint32_t payload =
        nxe_align8((uint32_t)h0) + nxe_align8((uint32_t)h1) +
        nxe_align8((uint32_t)p) + nxe_align8((uint32_t)s) +
        nxe_align8((uint32_t)b);
    /* ref appends a zero byte rather than emit a zero-length payload; it is
     * unreachable for a nonempty unit list, and the encoders agree about
     * unreachable cases too (lite_cpu.c says the same). */
    const uint32_t pay = payload ? payload : 8u;
    return (uint32_t)((8 * NXE_TILE_HEADER_BYTES + pay) << 10);
}

/* The integer RD cost of a tile: distortion plus lambda times rate.
 *
 * `lam_q8` is `nxe_rdoq_lambda_q8(nxe_qstep[qp])` -- the SAME lambda family
 * effort 1 uses, deliberately.  There is one fitted constant in this encoder
 * (`NXE_RDOQ_LAM_Q12`) and a second one here would be a second thing to sweep
 * and a second thing to disagree about; the requantiser and the quantiser
 * choice are the same trade at the same operating point.
 *
 * `sse` is a tile's summed squared error, which for 64x64 three-plane 8-bit
 * tops out near 2^30, and `bits_q10` near 2^20 for a dense tile.  The product
 * `lam_q8 * bits_q10` reaches 2^48, so it is formed in 64 bits and shifted
 * back: `(lam_q8 * bits_q10) >> 18` puts lambda*rate in the same units as SSE
 * (Q8 lambda times Q10 bits, both removed).  GLSL forms the same product with
 * `umulExtended`, exactly as `nxe_rdoq_lambda_q8` already does.
 */
static inline uint64_t nxe_rd_cost(uint32_t sse, uint32_t bits_q10,
                                   uint32_t lam_q8) {
    return (uint64_t)sse +
           (((uint64_t)lam_q8 * (uint64_t)bits_q10) >> 18);
}

#ifdef __cplusplus
}
#endif

#endif /* NXE_RATE_H */
