/* lite_cpu.h -- bit-exact CPU model of E4-lite (`lite_encode.comp`).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ENTROPY_LITE, tool bit 30, FIXED variant.  This is the same relationship to
 * `lite_encode.comp` that `rans_cpu.h` has to `rans_encode.comp`: the model is
 * the specification, the shader is validated against it, and both are required
 * to be byte-identical to `ref/src/entropy_lite.cpp`'s `lite_encode_units()`
 * for the same unit list -- which is what `nxv-enc --entropy lite` emits.
 *
 * ---------------------------------------------------------------------------
 * Why this is a second entropy stage and not a switch inside E4
 * ---------------------------------------------------------------------------
 * E4's whole structure -- rounds, lanes, a backward sweep, emissions anchored
 * at the end of the slot -- exists to serialise an arithmetic coder across
 * eight lanes.  Lite has no coder and no state: a unit's bits depend on that
 * unit alone, and its bit OFFSET is a prefix sum.  Sharing a kernel would mean
 * carrying E4's machinery through a path that uses none of it, so the two are
 * separate kernels behind one host switch.
 *
 * ---------------------------------------------------------------------------
 * The layout
 * ---------------------------------------------------------------------------
 * Five sections, each padded to a byte boundary, bits MSB-first inside a byte:
 *
 *   H0  one bit per group of NXE_LITE_CBF_GROUP units: is any unit in the
 *       group coded?
 *   H1  one bit per unit, but only for the groups H0 flagged.
 *   P   for every coded COEFFICIENT unit, in unit order: LAST in
 *       ceil(log2(ncoef)) bits, then the 3-bit magnitude class.
 *   S   for every coded unit, in unit order: a mode unit contributes one
 *       MPM-hit bit per block; a coefficient unit contributes one significance
 *       bit for scan positions 0 .. LAST-1 (position LAST is nonzero by
 *       construction and is not coded).
 *   B   the bodies: a mode unit's non-MPM indices, three bits each; a
 *       coefficient unit's |q|-1 in the class's field width, each followed by
 *       its sign bit.
 *
 * Every section start follows from the one before it by a quantity the decoder
 * can compute in parallel, which is the property the tool exists for.
 */

#ifndef NXE_LITE_CPU_H
#define NXE_LITE_CPU_H

#include <stdint.h>

#include "nxe_enc.h"
#include "rans_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FIXED: 3-bit per-unit magnitude class -> field width.  The width covers
 * |q| in 1 .. 2^bits, coded as |q| - 1, so class 7 spans the whole int16
 * range and the variant needs no escape at all. */
extern const uint8_t nxe_lite_mag_bits[8];

/* Bits the per-unit LAST field takes, given the unit's coefficient count. */
int nxe_lite_last_bits(int ncoef);

/* E4-lite over one tile.  Writes the 8-byte tile header followed by the
 * payload into `out` (at least NXE_TILE_BYTES_MAX_LITE) when `out` is
 * non-null; when it is null only the length is computed.  Returns the payload
 * length in bytes, or -1 if it exceeds 65535 (the tile header's field width)
 * or the unit list is malformed.
 *
 * `variant` is NXE_LITE_FIXED; NXE_LITE_RICE is refused, here as in the
 * decoder, because Pass A implements only FIXED. */
int nxe_lite_tile(const nxe_frame_params *fp, const nxe_tile_job *job,
                  const nxe_tile_units *tu, const int16_t *coef,
                  const uint8_t *modes, int variant, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NXE_LITE_CPU_H */
