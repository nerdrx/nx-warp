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
    uint8_t flags;        /* bit 0 valid, 1 static, 2 base_sourced; 3-7 zero */
    uint8_t res_level;    /* res_level the position was last coded at       */
    uint8_t reserved[20]; /* Phase 2 depth; zero in v1                      */
};
static_assert(sizeof(AtlasEntry) == 64, "[SYN] 13.12.1 fixes the entry at 64 B");

/* [SYN] 13.12.1 flags.  Bit 2 `base_sourced` is NORMATIVE in v1 (13.12.9): it
 * is written, and conformance compares it like every other bit of the 64.  It
 * is what lets a receiver, a rate controller and a conformance vector tell the
 * two patch sources apart, so an encoder that kept the provenance to itself
 * would produce a shadow that differs from the decoder's atlas on exactly the
 * tiles the two exist to agree about.  Bits 3-7 stay reserved and zero. */
enum : uint8_t {
    kAtlasValid = 1u,
    kAtlasStatic = 2u,
    kAtlasBaseSourced = 4u,
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
 * Returns false ALSO when any `|P[k]| >= 2^33`.  That is the guard of 13.12.2:
 * the normative arithmetic is `int64`, and it is the guard rather than a wider
 * type that keeps it safe -- a composition past the bound FAILS and the entry
 * is invalidated, before the shift is evaluated.
 *
 * `P[i][j] << 29` is then formed in 128 bits, which the clause permits only
 * WITH that same guard: without it a 128-bit implementation would accept
 * compositions an `int64` one rejects, and that is a conformance difference
 * rather than an optimisation.  Guarded, the wider intermediate is merely
 * another way of writing the same values, so this is not a second
 * arithmetic. */
bool atlas_renorm(const int64_t P[9], int32_t out[9]);

/* Both of the above: `out := renorm(C . H)`.  Returns false for the degenerate
 * `P[2][2] == 0` and for a product that trips the 2^33 guard. */
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

    /* [SYN] 13.12.9's write-back for a tile filled from the BASE LAYER, which
     * is 13.12.3 step 3's with one bit added: identity `C`, `frame_number` as
     * the source, generation 0, valid, never static, res_level 0, and
     * `base_sourced` SET.  The table says WHERE the pixels are and at which
     * pose, which is the same statement however they were produced; the bit is
     * what tells the two sources apart.  `code_tile()` clears it by writing
     * `flags` whole, which is 13.12.9's "cleared by any subsequent coded-tile
     * write to the same position, because that write replaces the pixels".
     *
     * The SUPERSEDE rule of 13.12.9 is the caller's: this is the write itself
     * and assumes the position has already been found writable. */
    void write_base_tile(uint32_t t, uint32_t frame_number);

    /* Whether the position was last filled from the base layer -- the flag
     * itself, not a shadow of it, so there is one place it can be wrong. */
    bool is_base_sourced(uint32_t t) const {
        return t < e.size() && (e[t].flags & kAtlasBaseSourced) != 0;
    }

    /* ------------------------------------------------ the per-frame mode
     *
     * A frame is either an ATLAS frame or a PICTURE frame.  Both settle the
     * pending transform into the pixels -- `C := I`, `gen := 0` -- and they
     * differ in exactly one field, which is the field that decides whether the
     * frame's own coded tiles survive.
     *
     * ORDER IS THE WHOLE OF IT.  Both of these run AFTER this frame's coded
     * tiles have been written back, never before.  The supersede test of
     * 13.12.6 is `>=` against the src_frame a position ALREADY holds, so
     * stamping the frame's own number first would make every coded tile of the
     * frame satisfy `src_frame >= frame_number` and be dropped -- measured in
     * the reference as 35 of 46 coded tiles silently discarded, with encoder
     * and decoder still agreeing on the frame header and disagreeing on the
     * picture.  The test itself is NOT relaxed; the ordering is what keeps it
     * from firing.  This mirrors nxvw::AtlasHostState on the decoder side,
     * where `apply()` runs before `rebase_picture()` for the same reason. */

    /* PICTURE frame `frame_number` -- [SYN] 13.12.11 step 3, the write-back
     * that turns the reconstructed picture into the atlas.
     *
     * The frame was decoded by the ORDINARY non-ATLAS process, so the whole
     * picture was reconstructed and EVERY position now holds pixels of this
     * frame.  So every position, not merely every valid one, ends up
     * `C = I`, `gen = 0`, `valid = 1`, `base_sourced = 0`, `res_level = 0`
     * and `src_frame = frame_number`.  Age 0 everywhere is the truth here and
     * this is the one case in which `src_frame` moves for a position the
     * frame did not code -- because its pixels really are new, having been
     * warped from the assembled picture this frame.
     *
     * `coded_mode` is one byte per tile: the nxvw mode this frame coded the
     * position with, or `kPictureNotCoded` where it coded none.  It decides
     * `static`, which per 13.12.11 is set exactly when THIS frame coded the
     * position `STATIC_MV` and is NOT inherited: after a PICTURE frame the
     * atlas is one coherent picture at one time.  A null pointer means the
     * frame coded nothing, so nothing is static. */
    static const uint8_t kPictureNotCoded = 0xFFu;
    void picture_frame(const uint8_t *coded_mode, uint32_t frame_number);

    /* ATLAS-frame rebase ([SYN] 13.12.10, tool bit 34).  The pending
     * transform is settled into the pixels and `C := I`, `gen := 0` -- but the
     * content is no newer than it was, so `src_frame` is UNTOUCHED and age
     * stays `frame_number - src_frame`.  That is what keeps the two rules
     * which key on provenance working: the supersede rule of 13.12.6 and the
     * base-patch monotonicity of 13.12.9.
     *
     * A `static` entry is excluded: its `C` is already the identity because
     * its content is head-locked to the viewer, so re-posing it would move
     * content that is by definition not supposed to move. */
    void rebase_settle();

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
        /* This frame MATERIALISED the atlas pixels -- a PICTURE frame, or an
         * ATLAS-frame rebase.  It is a GENERATION BOUNDARY for the undo log:
         * a snapshot taken before it can never be restored, because the
         * pixels it names were overwritten by the materialisation and the
         * metadata replay cannot put them back.  `rollback()` refuses across
         * one, which sends the caller down the invalidate path and codes the
         * tile INTRA -- the safe direction, and the same one the log already
         * takes when a frame's matrix has fallen out of the ring. */
        uint8_t materialised = 0;
    };
    Step step[kDepth];
    AtlasGeom g{};
    uint32_t gen_max = 0;

    void reset(const AtlasGeom &geom);

    /* Record this frame's advance, before it is applied.  `advanced` is
     * `warp_present`. */
    void note_frame(uint32_t frame_number, const int32_t H[2][9],
                    bool advanced);

    /* Mark frame `frame_number` as having materialised the atlas pixels -- a
     * PICTURE frame, or an ATLAS-frame rebase.  Call it after note_frame() for
     * that frame.  It makes the frame a generation boundary the log will not
     * roll back across; see `Step::materialised`. */
    void note_materialised(uint32_t frame_number);

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
/* --------------------------------------------------- display (NOT normative)
 *
 * [SYN] 13.12.5.  A client displays a tile by warping its atlas pixels from
 * its source pose to the pose it wants, IN ONE STEP, with whatever arithmetic
 * it likes -- the hardware sampler, floating point of any precision, any
 * filter.  Nothing here affects the atlas and nothing here is tested by
 * conformance; a decoder is conforming if its atlas matches, whatever it puts
 * on the panel.
 *
 * `C` is already exactly the map this needs: it takes THIS frame's centred
 * sample indices to the source frame's, which is the one-step warp from the
 * displayed pose back to the pixels.  So displaying at frame N is a gather
 * through C and nothing else.
 *
 * Doubles and bilinear, deliberately.  This is the reference display, used to
 * ask what the atlas would LOOK like -- which is the only way to price the
 * model at equal rate -- and using the normative integer warp here would
 * measure the predictor a second time instead of the picture.
 *
 * `atlas` is the luma plane of the atlas at `stride` u16 per row, holding
 * `eyes` sub-pictures side by side, exactly as the ring stores it.  `out` is
 * filled TILE-MAJOR, 64x64 per tile in the table's own tile order, because
 * that is the layout the encoder's source is already in and un-tiling one of
 * them only to compare would be work for nothing.  An invalid tile is left at
 * mid-grey: it has no pixels a client could show.
 */
void atlas_display_luma(const AtlasTable &at, const uint16_t *atlas, int stride,
                        int eye_w, int height, std::vector<uint16_t> &out);

/* ------------------------------------------- the displacement-bounded skip
 *
 * ADR-0029's open defect.  A skipped tile is displayed and predicted by
 * reading the atlas at `C(x)`; for a displacement `d` those samples land `d`
 * pixels outside the tile's own position, in NEIGHBOURING entries whose
 * content was coded at a different frame and therefore at a different pose.
 * The contamination is proportional to `d`, and the encoder's error threshold
 * cannot bound it because the encoder measures the same contaminated predictor
 * and cannot see that it is contaminated.
 *
 * So bound `d` directly.  This returns the largest displacement, in luma
 * samples, over the tile's FOUR CORNERS -- the corners rather than a sample
 * grid because `C` is a homography and a homography maps a quadrilateral's
 * extremes to its corners, which is the same argument 3.1.1's own corner
 * derivation rests on.
 *
 * The caller skips only when this is under its margin.  A margin of 64 is the
 * tile itself and is therefore no bound at all; the useful range is a small
 * fraction of that, and the cost of a tight one is FORCED REFRESH -- a tile
 * that would have been skipped is coded instead, so the rule trades bytes for
 * the defect rather than removing it.
 *
 * An invalid entry returns 0: it cannot be skipped for a different reason and
 * the caller has already refused it. */
double atlas_corner_disp(const AtlasTable &at, uint32_t tile, int eye_w,
                         int height);

/* ------------------------------------------- Cheat 3: refresh priority
 *
 * The rolling refresh of `refresh_due()` re-codes a fixed, staggered fraction
 * of tiles every frame regardless of where the eye is or how stale the tile
 * is.  Cheat 3 orders them instead: given a per-frame CAP on how many
 * refresh-driven tiles may be coded, code the highest-priority ones and let
 * the rest keep their atlas entry for another frame.
 *
 * Priority is fovea distance plus age, both normalised, because the two say
 * different things -- a stale tile in the periphery can wait, a fresh one at
 * the fovea usually can too, and the tile that must not wait is the one that
 * is both old and central.  `fovea_x`/`fovea_y` are in tile units within the
 * eye; passing the eye's centre is the fixed-foveation case and is what a
 * headset without eye tracking has.
 *
 * `out_refresh` is filled with `ntiles` bytes: 1 = code this tile now, 0 =
 * leave it for a later frame.  Tiles the caller did not mark as refresh
 * candidates are always 0.  With `cap == 0` the hook is OFF and every
 * candidate is passed through unchanged, which is the default and is what
 * keeps every existing stream byte-identical.
 *
 * This changes the bitstream.  It is a rate-shaping hook, not a conformance
 * rule: the decoder neither knows nor cares which tiles an encoder chose to
 * refresh. */
void atlas_refresh_priority(const AtlasTable &at, const uint8_t *candidate,
                            uint32_t ntiles, uint32_t frame_number,
                            uint32_t cap, double fovea_x, double fovea_y,
                            uint8_t *out_refresh);

/* PSNR of a tile-major luma picture against a tile-major source, over the
 * tiles the caller names (`nullptr` = all of them).  Returns 99.0 for an exact
 * match, which is the convention every other tool in this tree uses. */
double luma_psnr_tilemajor(const uint16_t *a, const int32_t *b, uint32_t ntiles,
                           int maxval);

struct WarpParams;
void atlas_build_matrices(const AtlasTable &at, int width, int height, int cw,
                          int ch, WarpParams &wp);

}  // namespace nxe

#endif /* NXE_ATLAS_H */
