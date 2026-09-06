/* nxe_atlas.h -- the encoder's shadow of the ATLAS reference (tool bit 31).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NORMATIVE SOURCE: docs/SYNTAX.md 13.12 and 3.1.1, and
 * docs/adr/0029-atlas-reference.md.  Where this file and those disagree, they
 * win; this is a model of them, not a second definition of them.
 *
 * Under `ATLAS` the reference is not the previous decoded picture.  For each
 * tile POSITION the atlas holds the pixels of the most recent frame that CODED
 * that position, plus the composed warp from the current frame's pose back to
 * that frame's pose.  A skipped tile writes nothing and costs nothing.
 *
 * Two objects, and this header owns the second:
 *
 *   * the atlas PIXELS, which are byte-for-byte a reference-ring picture
 *     (13.2 / vk/decoder/inter/inter_layout.h) -- so the existing ring storage
 *     hosts them, at ONE generation rather than four, and nothing about the
 *     pixel layout is new;
 *   * the per-tile TABLE, 64 bytes per tile position, which is `AtlasEntry`
 *     below and is compared byte for byte by conformance.
 *
 * Everything here is integer and is bit-exact by construction: the composition
 * is the two-partial-sum form of 13.12.2 and the renormalisation is the same
 * sign-magnitude half-away-from-zero division the corner derivation of 3.1.1
 * already mandates.  There is no floating point in this file and there must
 * never be: the decoder computes the same table from the same warp_ext() and
 * the same skip map, and a divergence here is a reference the client does not
 * hold.
 */

#ifndef NXE_ATLAS_H
#define NXE_ATLAS_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../decoder/passB/syntax_constants.h"
#include "inter_layout.h"

namespace nxe {

/* ---------------------------------------------------------------- the entry
 *
 * [SYN] 13.12.1.  64 bytes per tile position, in the tile order of Annex D
 * D-3 (row-major over `eyes * cols_per_eye`).  A v1 encoder writes bytes 0..43
 * and zeroes 44..63; conformance compares all 64, which is why `reserved` is a
 * member and not padding the compiler is free to leave uninitialised. */
struct AtlasEntry {
    /* The composed homography, THIS frame -> the source frame.  Rows 0-1
     * Q10.21, row 2 Q2.29, in the element order of 3.1.1: h00 h01 h02 h10 h11
     * h12 h20 h21 h22.  This is the LUMA matrix, unconjugated -- the per-plane
     * conjugation S H S^-1 is done where it already is, in
     * nxe_inter.cpp::plane_matrix, because the table is per tile and not per
     * plane. */
    int32_t C[9];
    uint32_t src_frame;   /* the frame number that last coded this position */
    uint16_t gen;         /* composition steps since src_frame              */
    uint8_t flags;        /* bit 0 valid, bit 1 static, bits 2-7 reserved   */
    uint8_t res_level;    /* res_level the position was last coded at       */
    uint8_t reserved[20]; /* Phase 2 depth; zero in v1                      */
};
static_assert(sizeof(AtlasEntry) == 64, "[SYN] 13.12.1 fixes the entry at 64 B");

enum : uint8_t {
    kAtlasValid = 1u,
    kAtlasStatic = 2u,
};

/* The identity, in the wire scales.  [SYN] 13.12.3 step 3 writes exactly
 * this. */
void atlas_identity(int32_t C[9]);

/* ---------------------------------------------------------- the arithmetic
 *
 * [SYN] 13.12.2, one step of `P = C . H` with two independently rounded
 * partial sums.  `P` comes back in the row scales of its inputs: rows 0-1
 * Q21, row 2 Q29.  Every product and sum is exact in 64 bits for any pair of
 * matrices inside the envelope of 3.1.1. */
void atlas_compose_partial(const int32_t C[9], const int32_t H[9],
                           int64_t P[9]);

/* `sdiv_round(a, b)`: sign-magnitude, half away from zero, over a division
 * truncating toward zero.  [SYN] 13.12.2.  `b` must be nonzero. */
int64_t atlas_sdiv_round(int64_t a, int64_t b);

/* The renormalisation that puts `P` back on `C[2][2] == 0x20000000`, so the
 * composed matrix lives in exactly the envelope the wire format defines and
 * warp_plane_tile() consumes it unchanged.
 *
 * Returns false when `P[2][2]` is zero -- which no pair of matrices satisfying
 * 3.1.1 condition 3 can produce, and which is therefore a caller error or an
 * already-invalid entry rather than a stream condition.
 *
 * `P[i][j] << 29` is formed in 128 bits.  Inside the envelope it fits `int64`
 * with room to spare, and both the reference codec and the GPU may use `int64`
 * for it; the wider intermediate exists so that a composition which the
 * envelope check on the NEXT line is about to reject is DEFINED rather than
 * undefined behaviour.  The two agree on every value either can represent, so
 * this is not a second arithmetic. */
bool atlas_renorm(const int64_t P[9], int32_t out[9]);

/* Both of the above: `out := renorm(C . H)`.  Returns false only for the
 * degenerate `P[2][2] == 0`. */
bool atlas_compose(const int32_t C[9], const int32_t H[9], int32_t out[9]);

/* [SYN] 3.1.1 conditions 2 and 3, which under 13.12.3 step 1 are the staleness
 * bound: a composition that leaves the envelope invalidates the tile, and an
 * invalid tile cannot be skipped or inter-predicted, so the encoder must code
 * it.  `width`/`height` are the eye's LUMA dimensions; the origin is derived
 * as 3.1.1 derives it.  Condition 1 is guaranteed by the renormalisation and
 * is asserted rather than tested. */
bool atlas_envelope_ok(const int32_t C[9], int width, int height);

/* ----------------------------------------------------------- the geometry */
struct AtlasGeom {
    int width = 0, height = 0;   /* per eye, luma */
    int cols_per_eye = 0, rows = 0;
    int eyes = 1;
    uint32_t ntiles() const {
        return (uint32_t)(cols_per_eye * rows * eyes);
    }
    /* Annex D D-3: row-major over `eyes * cols_per_eye`. */
    int eye_of(uint32_t tile) const {
        const int cols = cols_per_eye * eyes;
        return (int)((tile % (uint32_t)cols) / (uint32_t)cols_per_eye);
    }
};

/* ------------------------------------------------------------- the table
 *
 * The per-tile table and the frame process of 13.12.3.  The PIXELS are not
 * here: they live in the ring buffer the encoder already owns, at one
 * generation, and the write-back that puts a coded tile's reconstruction into
 * them is Pass B's ring store, unchanged.  What this object owns is the
 * metadata, the part that has to be derived identically on both sides. */
struct AtlasTable {
    std::vector<AtlasEntry> e;
    AtlasGeom g{};
    /* [SYN] 13.12.3 step 1, "an implementation's declared cap".  0 is no cap,
     * which is the v1 default: `gen_max` and tool bit 32 ATLAS_DRIFT are the
     * Cheats-8 experiment and are NOT built. */
    uint32_t gen_max = 0;

    /* `tile_map_reset` clears the whole table: every entry zero, so every
     * entry is invalid and every tile must be INTRA.  [SYN] 13.12.1. */
    void reset(const AtlasGeom &geom);
    void reset() { reset(g); }

    bool valid(uint32_t t) const { return (e[t].flags & kAtlasValid) != 0; }
    bool is_static(uint32_t t) const { return (e[t].flags & kAtlasStatic) != 0; }

    /* Step 1, the advance.  `H[eye]` is this frame's warp_ext() for that eye.
     * Called once per frame, before any tile is decided, and only when
     * `warp_present`; a frame with no reference does not advance, it resets.
     *
     * For every entry with `valid == 1`: `gen` increments; a non-static entry
     * composes; and the entry is invalidated if the result leaves the envelope
     * or `gen` exceeds `gen_max`.  A static entry keeps `C` at the identity,
     * which is the whole of the static-skip cheat: a head-locked tile is not
     * warped, so it may be SKIPPED at zero bits until its content changes,
     * where the picture-based model had to code it every frame. */
    void advance(const int32_t H[2][9]);

    /* Step 3, the write-back, for a tile whose mode produced reconstructed
     * samples.  The PIXELS are written by Pass B; this is the metadata half.
     * `mode` is an nxvw::kMode* value. */
    void code_tile(uint32_t t, uint32_t frame_number, int mode, int res_level);

    /* 13.12.7.  A NEAR_SKIP tile applies its correction to the atlas pixels in
     * place and changes NO metadata.  It is here as a named no-op so that the
     * one case in which a skipped tile touches the atlas is visible in the
     * model rather than implied by the absence of a call. */
    void near_skip_tile(uint32_t /*t*/) {}
};

/* --------------------------------------------------------- the undo log
 *
 * ADR-0029 section 7.  On a negative receipt for tile `t` of frame `M` the
 * encoder rolls that tile's shadow entry back to the generation before `M`,
 * from a one-deep log over the tiles it coded in the last `K` frames.  That
 * replaces the replay the picture-based model runs per lost tile, and it is
 * strictly cheaper.
 *
 * One-deep means one snapshot per tile POSITION: the state that position was
 * in immediately before its most recent coded write-back.  Rolling back at a
 * later frame `N` needs that state advanced from `M` to `N`, and the log gets
 * it by REPLAYING the advance over the frames in between, from a small ring of
 * their warp matrices -- which is bit-identical to having advanced the
 * snapshot every frame, because the advance is a fixed function of the state
 * and the matrix, and strictly cheaper because it runs only for the tiles a
 * receipt actually names.
 *
 * The PIXEL half of the rollback is a copy of the tile's previous atlas
 * pixels, which the caller owns (they are a rect of the ring buffer); this
 * object records WHICH tiles have a live snapshot and what its metadata was,
 * and `rollback` tells the caller whether a pixel restore is needed.
 */
struct AtlasUndo {
    static const int kDepth = 32;   /* K: frames of history kept */

    struct Snap {
        AtlasEntry before{};    /* the entry as it was at frame `frame`,
                                 * after that frame's advance and BEFORE the
                                 * write-back */
        uint32_t frame = 0;     /* M, the frame whose write-back it precedes */
        uint8_t used = 0;
    };
    std::vector<Snap> s;
    /* The last kDepth frames' warp matrices, indexed `frame % kDepth`, and
     * whether that frame advanced at all (a frame with no reference does
     * not). */
    struct Step {
        int32_t H[2][9] = {};
        uint32_t frame = 0;
        uint8_t used = 0;
        uint8_t advanced = 0;
    };
    Step step[kDepth];
    AtlasGeom g{};
    uint32_t gen_max = 0;

    void reset(const AtlasGeom &geom);

    /* Record this frame's advance, before it is applied.  `advanced` is
     * `warp_present`. */
    void note_frame(uint32_t frame_number, const int32_t H[2][9],
                    bool advanced);

    /* Record the state of tile `t` immediately before frame `M` codes it. */
    void note_coded(uint32_t t, uint32_t frame_number, const AtlasEntry &before);

    /* Roll tile `t` back to the generation before frame `M`, as of the current
     * frame `now`.  Returns true and fills `out` when a snapshot for exactly
     * `M` is still held; returns false when it is not, in which case the
     * caller must invalidate the entry instead -- the safe direction, because
     * an invalid tile is coded INTRA and a wrongly-held one is a prediction
     * from a picture the client does not have. */
    bool rollback(uint32_t t, uint32_t lost_frame, uint32_t now,
                  AtlasEntry &out) const;

    /* Whether tile `t`'s live snapshot is for frame `M`; the caller uses it to
     * decide whether the pixel restore is available too. */
    bool holds(uint32_t t, uint32_t lost_frame) const {
        return t < s.size() && s[t].used && s[t].frame == lost_frame;
    }
};

/* Fill the per-tile matrix area of an ATLAS frame's warp parameter buffer:
 * for every tile, its own composed `C` conjugated for sub 1 and sub 2, at the
 * offset that tile's `mat_idx` names.  build_warp_params() with
 * `WarpBuildInfo::atlas` set has already reserved the room and written the
 * offsets, so this writes matrices and never geometry.
 *
 * A tile whose entry is INVALID gets the identity rather than a stale `C`.  It
 * cannot be predicted -- [SYN] 13.12.4 makes a non-INTRA tile over an invalid
 * entry BITSTREAM, and the host has already forced it to INTRA -- so nothing
 * reads the record; writing the identity means a stray read is a still picture
 * rather than a confident prediction through a matrix that is no longer in the
 * envelope.
 *
 * Declared here rather than in nxe_inter.h because it is the atlas that owns
 * the per-tile matrix; the conjugation it calls is nxe_inter's, so the two
 * paths cannot conjugate differently. */
struct WarpParams;
void atlas_build_matrices(const AtlasTable &at, int width, int height, int cw,
                          int ch, WarpParams &wp);

}  // namespace nxe

#endif /* NXE_ATLAS_H */
