// NX Warp decoder, ATLAS (tool bit 31): the one description of the per-tile
// table and of the composition arithmetic.
//
// Shared verbatim by the CPU model (atlas_model.cpp), the GPU kernel
// (atlas_compose.comp) and the host, so there is exactly one layout and one
// definition of the arithmetic and they cannot drift.
//
// NORMATIVE SOURCE: docs/SYNTAX.md 13.12 and
// docs/adr/0029-atlas-reference.md.  Where this file and those disagree, they
// win.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_ATLAS_LAYOUT_H
#define NXVW_ATLAS_LAYOUT_H

#include "../inter/inter_layout.h"   // kWarpQNum, kWarpQDen, kWarpH22, ...

#ifdef __cplusplus
#include <cstdint>
namespace nxvw {
#define NXVW_AFN inline int
#define NXVW_AFNU inline uint32_t
#else
#define NXVW_AFN int
#define NXVW_AFNU uint
#endif

// ----------------------------------------------------------- the table
// [SYN] 13.12.1.  64 bytes per tile position, one entry for every tile of
// every eye, in the linear tile order of [SYN] 3.3.
//
//   off  size  field
//     0    36  C[9]      composed homography, this frame -> source frame;
//                        nine little-endian i32, rows 0-1 Q10.21, row 2 Q2.29
//    36     4  src_frame u32, the frame that last coded this position
//    40     2  gen       u16, composition steps since src_frame
//    42     1  flags     bit 0 valid, bit 1 static, bits 2-7 reserved zero
//    43     1  res_level of the tile that last coded it; advisory
//    44    20  reserved, zero
//
// Sixteen uints.  `gen`, `flags` and `res_level` share uint 10, little-endian,
// so a GLSL kernel reads one word and unpacks rather than doing byte access.
#define NXVW_ATLAS_ENTRY_BYTES 64
#define NXVW_ATLAS_ENTRY_UINTS 16
#define NXVW_ATLAS_OFF_C 0        // .. 8
#define NXVW_ATLAS_OFF_SRC 9
#define NXVW_ATLAS_OFF_PACK 10    // gen | flags<<16 | res_level<<24
#define NXVW_ATLAS_OFF_RSVD 11    // .. 15, zero

#define NXVW_ATLAS_FLAG_VALID 1u
#define NXVW_ATLAS_FLAG_STATIC 2u
// ADR-0029 flags bit 2.  SYNTAX 13.12.1 still says "bits 2-7 reserved, zero";
// the ADR owner is making this normative and the reference already emits it.
// A stream that never imports a base-layer tile never sets it, so a decoder
// built with it is byte-identical to one without on every non-hybrid vector.
#define NXVW_ATLAS_FLAG_BASE_SOURCED 4u

// ------------------------------------------------- the composition guard
// [SYN] 13.12.2, "Width of `P[k] << 29`".  If any |P[k]| >= 2^33 the
// composition FAILS and the entry is invalidated, BEFORE the shift is
// evaluated.  2^33 << 29 is 2^62, so every shift the normative path evaluates
// fits int64.  This is not a tunable threshold: a legal composed matrix is
// bounded by kWarpEntryMax (2^30), so 2^33 is already eight times outside the
// envelope.
//
// A 128-bit implementation must apply the SAME guard.  Without it a 128-bit
// decoder accepts a composition an int64 decoder rejects -- a conformance
// difference, not an optimisation.  The GPU kernel is emulated 64-bit and
// tests the guard as `abs64(P).hi >= NXVW_ATLAS_PGUARD_HI`, which is the same
// predicate: 2^33 is (hi = 2, lo = 0).
#define NXVW_ATLAS_PGUARD_HI 2u
#ifdef __cplusplus
inline constexpr int64_t kAtlasPGuard = (int64_t)1 << 33;
#endif

// --------------------------------------------------------- the `H` ring
// The decoder retains the last NXVW_ATLAS_HRING frames' per-eye homographies
// so a lazily advanced entry can be walked forward one step at a time, in
// frame order, exactly as 13.12.3's informative note requires.
//
// Depth 64.  `vk.atlas.compose` measures the envelope: at 1.37 deg a frame --
// 123 deg/s at 90 Hz, the fastest rotation the paper measures -- a tile's `C`
// survives 19 compositions and then invalidates itself.  64 clears that by
// more than 3x, so the ring is never the binding constraint and a tile is
// always invalidated for a reason the atlas can state, never because the
// decoder forgot an `H`.  The ring depth is NOT what makes the lazy advance
// safe; the per-step envelope check is.  A matrix at the very edge of what
// 3.1.1 permits survives two compositions, and nothing forbids an encoder
// emitting one.
//
// Ten uints per (slot, eye): nine matrix words and one flags word whose bit 0
// is `warp_present`.  A frame with `warp_present == 0` contributes NO step at
// all -- not even a `gen` increment ([SYN] 13.12.3 step 1 is conditioned on
// it, and [REF] atlas_advance_frame() returns early) -- so the bit has to
// travel with the slot and cannot be inferred from the matrix, whose h22 is
// 2^29 for every legal value.
//
// 64 slots x 2 eyes x 10 uints = 5120 B.  (ATLAS-DECODER.md's "4.6 kB" counts
// the nine matrix words only; the flags word is the eleventh percent.)
#define NXVW_ATLAS_HRING 64u
#define NXVW_ATLAS_HSLOT_UINTS 10u
#define NXVW_ATLAS_HFLAG_WARP_PRESENT 1u

NXVW_AFNU nxvw_atlas_hslot(uint frame, uint eye, uint eyes) {
    return ((frame % NXVW_ATLAS_HRING) * eyes + eye) * NXVW_ATLAS_HSLOT_UINTS;
}

NXVW_AFNU nxvw_atlas_pack(uint gen, uint flags, uint res_level) {
    return (gen & 0xffffu) | ((flags & 0xffu) << 16) | ((res_level & 0xffu) << 24);
}
NXVW_AFNU nxvw_atlas_gen(uint pack) { return pack & 0xffffu; }
NXVW_AFNU nxvw_atlas_flags(uint pack) { return (pack >> 16) & 0xffu; }
NXVW_AFNU nxvw_atlas_res_level(uint pack) { return (pack >> 24) & 0xffu; }
NXVW_AFN nxvw_atlas_valid(uint pack) {
    return (nxvw_atlas_flags(pack) & NXVW_ATLAS_FLAG_VALID) != 0u ? 1 : 0;
}
NXVW_AFN nxvw_atlas_static(uint pack) {
    return (nxvw_atlas_flags(pack) & NXVW_ATLAS_FLAG_STATIC) != 0u ? 1 : 0;
}

// ------------------------------------------------------- the tile index
// [SYN] 3.3, and it is row-major and eye-MINOR: the two eyes' entries
// INTERLEAVE within each row, so the table is NOT two contiguous per-eye
// halves and neither is the tile order.  Anything that walks it linearly as
// one eye is wrong.  `build_tile_order()` in the decoder already computes this
// same index; what it reorders is the dispatch.
//
//   cols = eyes * cols_per_eye
//   n    = row * cols + eye * cols_per_eye + col
NXVW_AFN nxvw_atlas_index(int row, int eye, int col, int cols_per_eye,
                          int eyes) {
    return row * (eyes * cols_per_eye) + eye * cols_per_eye + col;
}
NXVW_AFN nxvw_atlas_eye_of(int n, int cols_per_eye, int eyes) {
    return (n % (eyes * cols_per_eye)) / cols_per_eye;
}
NXVW_AFN nxvw_atlas_row_of(int n, int cols_per_eye, int eyes) {
    return n / (eyes * cols_per_eye);
}
NXVW_AFN nxvw_atlas_col_of(int n, int cols_per_eye, int eyes) {
    return (n % (eyes * cols_per_eye)) % cols_per_eye;
}

// ------------------------------------------------- the compose dispatch
// ONE dispatch per frame over EVERY entry of BOTH eyes.  289 entries an eye is
// already in the starved region of the workgroup-count curve
// (passA/README.md), and splitting the dispatch per eye would halve the
// occupancy of each half for no gain: the thread reads its own eye out of its
// index and picks H[eye], so two eyes' worth of entries in one dispatch needs
// no eye arithmetic beyond `nxvw_atlas_eye_of()`.
//
// The SAME kernel serves the eager and the lazy path.  An entry is advanced
// from its private `advanced_to` up to `targetFrame`, one step at a time, in
// frame order.  The eager (frame-complete) path is the case where every
// entry's `advanced_to` is already `targetFrame - 1`, so every thread takes
// exactly one step -- there is no second kernel and no second transcription
// of 13.12.2 to keep in agreement.
#define NXVW_ATLAS_SEL_ALL 0u    // one thread per table entry, index == id
#define NXVW_ATLAS_SEL_LIST 1u   // one thread per element of the index list

#ifdef __cplusplus
struct NxvwAtlasPush {
    uint entryCount;    // SEL_ALL: entries to cover.  SEL_LIST: list length.
    uint colsPerEye;
    uint eyes;
    int lumaW, lumaH;   // per-eye luma dimensions, for 3.1.1 condition 3
    uint genMax;        // 0 = no cap.  The reference DECODER passes 0.
    uint targetFrame;   // advance every selected entry up to this frame
    uint sel;           // NXVW_ATLAS_SEL_*
    uint pad0;
};
#endif

#ifdef __cplusplus
}  // namespace nxvw
#endif

#undef NXVW_AFN
#undef NXVW_AFNU

#endif  // NXVW_ATLAS_LAYOUT_H
