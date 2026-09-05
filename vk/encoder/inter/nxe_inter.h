/* nxe_inter.h -- the encoder's half of the Phase 2 inter path: the reference
 * ring, the warp parameter buffer, and the per-tile records Pass W reads.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everything here is HOST work that sets up shaders belonging to the decoder.
 * The encoder has no predictor of its own and no reconstruction of its own:
 * Pass W is `vk/decoder/inter/warp_pred.comp`, compiled from the decoder's
 * source into the encoder's build and pinned identical by
 * `vk.encoder.passw.same`, and the layouts below are the decoder's
 * `inter_layout.h`, included rather than transcribed.  The rule that makes
 * that non-negotiable is in vk/encoder/README.md: the encoder must never hold
 * a reference the decoder cannot reproduce.
 *
 * What this file adds is the part a decoder does not have, because a decoder
 * is told the answers: the frame's homography has to be DERIVED from a pose
 * pair, the ring slot has to be chosen, and every tile's record has to be
 * filled from a mode decision that has not been made yet when the buffer is
 * built.  The records are therefore written in two steps -- geometry and mode
 * -- and `mode` is the only field the decision pass writes.
 *
 * See docs/adr/0028-gpu-inter-needs-an-integer-mode-decision.md.
 */

#ifndef NXE_INTER_H
#define NXE_INTER_H

#include <cstdint>
#include <vector>

#include "../../decoder/passB/passB_layout.h"
#include "../../decoder/passB/syntax_constants.h"
#include "inter_layout.h"

namespace nxe {

/* The nine quantised coefficients of one eye's warp_ext(), exactly as they
 * travel in the frame header.  Identity is what a frame with no reference
 * carries, and what the encoder falls back to if a pose pair is degenerate --
 * an identity warp predicts badly, which the mode decision then notices,
 * where a wrong matrix would predict confidently and be wrong. */
struct WarpMatrix {
    int32_t h[9] = {1 << 21, 0, 0, 0, 1 << 21, 0, 0, 0, 1 << 29};
};

/* Geometry of the ring, computed once per stream from the picture shape.
 * Four slots, addressed by `frame_number & 3`; samples are u16 in the CODED
 * domain (Y/Co/Cg before any inverse colour transform), packed two per uint. */
struct RingLayout {
    int off[4] = {};     /* u16 element offset of plane p inside a slot */
    int stride[4] = {};  /* u16 row stride of plane p, padded even      */
    int planeW[4] = {};  /* per-eye sample width of plane p             */
    int slot_u16 = 0;    /* u16 elements one slot occupies              */
    int nplanes = 3;

    size_t bytes() const {
        /* Four slots of u16.  Never zero: an unbound descriptor is illegal,
         * and an intra-only stream still has to bind something. */
        const size_t n = (size_t)slot_u16 * 4u * 2u;
        return n ? n : 4u;
    }
};

void ring_layout(int width, int height, int cw, int ch, int eyes, int nplanes,
                 RingLayout &out);

/* Which slot a frame writes, and which it predicts from.  The bitstream's own
 * rule (SYNTAX.md 3.1, Annex D D-10): a frame writes `frame_number & 3` and
 * an inter tile with ref_sel d predicts from `(frame_number - 1 - d) & 3`,
 * whose stored frame number must be `frame_number - 1 - d`.  The encoder holds
 * the same four-entry validity record the decoder does, so that "there is a
 * reference" means the same thing on both sides. */
struct RingState {
    uint8_t valid[4] = {};
    uint32_t frame_number[4] = {};
    void reset() {
        for (int i = 0; i < 4; ++i) { valid[i] = 0; frame_number[i] = 0; }
    }
    int resolve(uint32_t now, int ref_sel) const {
        if (now < (uint32_t)(1 + ref_sel)) return -1;
        const uint32_t want = now - 1u - (uint32_t)ref_sel;
        const int s = (int)(want & 3u);
        if (!valid[s] || frame_number[s] != want) return -1;
        return s;
    }
    void publish(uint32_t now) {
        const int s = (int)(now & 3u);
        valid[s] = 1;
        frame_number[s] = now;
    }
};

/* The rolling intra refresh of PAPER 2.6, byte for byte the reference's
 * (`refresh_stagger` / `refresh_due` in ref/src/codec_impl.inc).  A fixed
 * pseudo-random permutation of the tile index, so the 1/T of tiles forced
 * INTRA each frame are scattered over the picture instead of sweeping across
 * it as a visible band. */
uint32_t refresh_stagger(uint32_t tile);
bool refresh_due(uint32_t tile, uint32_t frame, uint32_t period);

/* The parameter buffer Pass W reads: a 64-uint header (four conjugated matrix
 * records, then the ring geometry) followed by one 12-uint record per tile.
 *
 * `build_warp_params` fills the header and every tile record EXCEPT the mode
 * bits, which the decision pass writes.  `mode_words` is where those live, so
 * a caller can point the decision pass at them without knowing the layout. */
struct WarpParams {
    std::vector<uint32_t> w;
    uint32_t tile_word(uint32_t tile) const {
        return (uint32_t)NXVW_WARP_HDR_UINTS + tile * NXVW_WARP_TILE_UINTS;
    }
    size_t bytes() const { return w.size() * sizeof(uint32_t); }
};

struct WarpBuildInfo {
    int width = 0, height = 0;   /* per eye, luma */
    int cw = 0, ch = 0;          /* per eye, chroma */
    int eyes = 1;
    int cols_per_eye = 0, rows = 0;
    int chroma420 = 1;
    int nplanes = 3;
    uint32_t frame_number = 0;
    int ref_slot = -1;           /* -1 = no reference: every tile is INTRA */
    const WarpMatrix *warp = nullptr;  /* one per eye */
};

void build_warp_params(const WarpBuildInfo &bi, const RingLayout &rl,
                       WarpParams &out);

/* Pass W's push block, filled from the same information. */
nxvw::NxvwWarpPush warp_push(const WarpBuildInfo &bi, const RingLayout &rl);

/* Set one tile's mode in an already-built parameter buffer.  `inter` clears
 * for INTRA, so Pass W writes nothing for that tile and Pass B's hook never
 * reads its WPred slot. */
void set_tile_mode(WarpParams &wp, uint32_t tile, int mode, int mv_x, int mv_y);

/* i16 elements one tile occupies in the WPred buffer, and the buffer's size. */
inline int wpred_stride_i16(int chroma420, int alpha) {
    return nxvw::nxvw_wpred_stride_i16(chroma420, alpha);
}
inline size_t wpred_bytes(uint32_t ntiles, int chroma420, int alpha) {
    const size_t n =
        (size_t)ntiles * (size_t)wpred_stride_i16(chroma420, alpha) * 2u;
    return n ? n : 4u;
}

}  // namespace nxe

#endif /* NXE_INTER_H */
