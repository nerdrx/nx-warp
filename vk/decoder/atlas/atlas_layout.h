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

#ifdef __cplusplus
}  // namespace nxvw
#endif

#undef NXVW_AFN
#undef NXVW_AFNU

#endif  // NXVW_ATLAS_LAYOUT_H
