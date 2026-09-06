// NX Warp decoder, ATLAS: the CPU model of [SYN] 13.12.2's composition.
//
// The normative arithmetic, in one place, in ordinary C++.  The GPU kernel
// (atlas_compose.comp) is checked against this the way passA_model.cpp checks
// rans_decode.comp, and this is checked against an independent oracle in
// tests/vk-decoder/atlas.
//
// NORMATIVE SOURCE: docs/SYNTAX.md 13.12.2 and 3.1.1.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_ATLAS_MODEL_H
#define NXVW_ATLAS_MODEL_H

#include <cstdint>
#include "atlas_layout.h"

namespace nxvw {

// The identity in the wire's scales: rows 0-1 Q10.21, row 2 Q2.29.
void atlas_identity(int32_t C[9]);

// [SYN] 13.12.2, one step: P = C . H, with two independently rounded partial
// sums per element so every intermediate fits int64.  P is returned unshifted
// and unrenormalised; rows 0-1 are Q21 and row 2 is Q29, as the spec's
// pseudocode leaves them.
void atlas_compose_step(const int32_t C[9], const int32_t H[9], int64_t P[9]);

// [SYN] 13.12.2, renormalise so C[2][2] is 0x20000000 again:
//   C'[i][j] = sdiv_round(P[i][j] << 29, P[2][2])
// Returns false when P[2][2] is zero, which no matrix satisfying 3.1.1
// condition 3 can produce, or when the shift would overflow int64 -- which is
// likewise unreachable for legal inputs and is reported rather than wrapped.
bool atlas_renorm(const int64_t P[9], int32_t out[9]);

// sign(a/b) * ((|a|*2 + |b|) / (|b|*2)), truncating toward zero: round to
// nearest, ties away from zero.  Exposed because the test pins it directly.
int64_t atlas_sdiv_round(int64_t a, int64_t b);

// [SYN] 3.1.1 conditions 2 and 3, which 13.12.3 step 1 applies to a composed
// matrix.  `width` and `height` are the eye's luma dimensions; the origin is
// derived as 3.1.1 derives it.
bool atlas_in_envelope(const int32_t C[9], int32_t width, int32_t height);

// [SYN] 13.12.3 step 1 for one entry: advance C by one frame's H and say
// whether the entry survives.  On false the caller clears `valid`; C is then
// undefined and must not be used.
bool atlas_advance(int32_t C[9], const int32_t H[9], int32_t width,
                   int32_t height);

// One table entry, as the model sees it.  The wire layout is
// atlas_layout.h's; this is the unpacked form the rules are written against.
struct AtlasEntry {
    int32_t C[9];
    uint32_t src_frame;
    uint32_t gen;
    uint32_t flags;      // NXVW_ATLAS_FLAG_VALID | _STATIC | base_sourced
    uint32_t res_level;
};

// [SYN] 13.12.3 step 1 for one entry, whole:
//   * an invalid entry is not advanced at all;
//   * `gen` increments for every valid entry, static or not;
//   * `static` entries keep C at the identity and are not composed;
//   * a composition that leaves 3.1.1's envelope, or a `gen` past `gen_max`,
//     clears `valid`.
// `gen_max` of 0 means no cap.  Returns the entry's validity afterwards.
bool atlas_advance_entry(AtlasEntry &e, const int32_t H[9], int32_t width,
                         int32_t height, uint32_t gen_max);

}  // namespace nxvw

#endif  // NXVW_ATLAS_MODEL_H
