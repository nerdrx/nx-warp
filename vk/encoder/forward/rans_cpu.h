/* rans_cpu.h -- bit-exact CPU model of E4 (`rans_encode.comp`) and of the
 * layout E5 (`packetize.comp`) produces.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ---------------------------------------------------------------------------
 * Why this is not just "run the reference encoder"
 * ---------------------------------------------------------------------------
 * `ref/src/entropy.cpp` encodes a tile by materialising the whole global
 * operation list, then walking it backwards.  A GPU cannot: the list is tens
 * of thousands of entries per tile and the whole point of eight lanes is that
 * they run at once.  The model below is the shader's algorithm, and it is the
 * reference's answer for a reason that is worth writing down, because it is
 * the only non-obvious thing in this file.
 *
 * `encode_units` drives the lane machines in rounds: one `while (any)` pass
 * asks every lane that is not finished for exactly one operation, in lane
 * order.  A lane's machine reaches `kDone` once and never restarts, so lane l
 * contributes to rounds 0 .. nops[l]-1 and to no others.  Therefore
 *
 *     the global operation order is (round ascending, lane ascending),
 *
 * and lane l's k-th operation is exactly the one it contributes in round k.
 * The rANS encoder walks that list backwards, so the shader walks
 * (round descending, lane descending) -- eight lanes in lockstep, one round per
 * step, no list.  Each lane's rANS state depends only on its own operations,
 * so the lanes update in parallel; only the *byte* order is global, and that is
 * settled by a running emission counter plus an eight-entry rank within the
 * round.
 *
 * The output layout follows from `encode_ops`'s reversed buffer:
 *
 *     [lane 0 state, u32 LE] .. [lane n-1 state, u32 LE]
 *     [emission E-1, u16 BE] .. [emission 0, u16 BE]
 *
 * where emission 0 is the first one the backward sweep produces (the highest
 * global index).  So the emission with counter `e` lands at byte offset
 * 4*nlanes + 2*(E - 1 - e), which is why E4 runs the sweep twice: once to
 * count (that count is also the tile's byte size for E2's prefix sum) and once
 * to place.  The sweep is deterministic, so the two agree by construction.
 *
 * Everything a lane needs is materialised one *coding unit* at a time.  A unit
 * cannot emit more than NXE_UNIT_MAX_OPS operations -- that bound is exact, not
 * a guess -- so the per-lane scratch is fixed and E4 has no overflow path.
 */

#ifndef NXE_RANS_CPU_H
#define NXE_RANS_CPU_H

#include <stdint.h>

#include "nxe_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One coding unit, as ref/src/entropy.h's `Unit` reduced to what the encoder
 * reads.  `coef_off` indexes the tile's level array; a mode unit has -1. */
typedef struct nxe_unit {
    int32_t coef_off;
    uint16_t ncoef;
    uint8_t tskip;        /* selects the scan for a 64-coefficient unit */
    uint8_t kind;         /* 0 = UNIT_COEF, 1 = UNIT_MODE */
    uint8_t ctx_cbf;
    uint8_t ctx_last;
    uint8_t ctx_level;    /* NXE_CTX_NONE = the banded LEVEL contexts */
    uint8_t ctx_mode;     /* NXE_CTX_NONE = bypass coded */
    uint8_t nbx;          /* UNIT_MODE: blocks per edge */
    uint8_t sdh;
    uint8_t mode_off;     /* UNIT_MODE: plane index into the mode array */
    uint8_t pad;
} nxe_unit;

typedef struct nxe_tile_units {
    nxe_unit u[NXE_TILE_UNITS_MAX];
    int nunits;
    int nlanes;   /* 1 << nsub_log2 */
    int active;   /* min(nlanes, nunits) */
} nxe_tile_units;

/* The frame's probability tables, flattened: freq[set][ctx][sym] and the
 * exclusive cumulative frequencies cum[set][ctx][sym].  This is exactly the
 * buffer the shader binds; `cum` has NXE_NUM_SYM entries per context, not
 * NXE_NUM_SYM+1, because the encoder never needs the final total. */
/* 32-bit entries, not 16: this struct is uploaded verbatim as the shader's
 * table buffer, and a std430 uint array is what E4 indexes. */
typedef struct nxe_tables {
    uint32_t freq[8][NXE_MAX_CTX][NXE_NUM_SYM];
    uint32_t cum[8][NXE_MAX_CTX][NXE_NUM_SYM];
} nxe_tables;

/* Build the tile's unit list, mirroring TileCoder::build_units. */
void nxe_build_units(const nxe_frame_params *fp, const nxe_tile_job *job,
                     nxe_tile_units *tu);

/* Materialise unit `ui`'s operations into `ops` (at most NXE_UNIT_MAX_OPS).
 * Returns the count.  `modes` is the tile's per-plane mode array, 64 entries
 * per plane, or NULL when directional intra is off. */
int nxe_unit_ops(const nxe_tile_units *tu, int ui, const int16_t *coef,
                 const uint8_t *modes, uint32_t *ops);

/* E4 over one tile.  Writes the 8-byte tile header followed by the payload
 * into `out` (at least NXE_TILE_SLOT_BYTES) when `out` is non-null; when it is
 * null only the length is computed.  Returns the payload length in bytes, or
 * -1 if it exceeds 65535 (the tile header's field width). */
int nxe_e4_tile(const nxe_frame_params *fp, const nxe_tile_job *job,
                const nxe_tile_units *tu, const int16_t *coef,
                const uint8_t *modes, const nxe_tables *tabs, uint8_t *out);

/* Pack the 8-byte tile header, ref's pack_tile_header restricted to what an
 * intra tile of this pipeline can carry. */
void nxe_pack_tile_header(const nxe_frame_params *fp, const nxe_tile_job *job,
                          uint8_t out[8]);

/* ------------------------------------------------------------------- E5
 *
 * Byte offset of tile `t`'s header inside the frame.  Tiles are in the
 * normative order (row-major, eye-minor, column) so the row group of tile t is
 * t / tiles_x and every row header before and including it is accounted for by
 * a multiplication -- no second scan.
 */
uint32_t nxe_e5_tile_offset(const nxe_frame_params *fp, uint32_t t,
                            const uint32_t *tile_prefix);
uint32_t nxe_e5_frame_bytes(const nxe_frame_params *fp, uint32_t total_tile_bytes);
void nxe_e5_row_header(const nxe_frame_params *fp, uint32_t rowgroup,
                       uint8_t out[NXE_ROW_HEADER_BYTES]);
void nxe_e5_frame_header(const nxe_frame_params *fp, const uint8_t pose[26],
                         uint32_t total, uint8_t out[NXE_FRAME_HEADER_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* NXE_RANS_CPU_H */
