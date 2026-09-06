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

/* ------------------------------------------------------- what the CLIENT holds
 *
 * `RingState` answers "did this encoder produce that picture", which is what
 * decides whether a stream is well formed in the abstract.  It is not what
 * decides whether the stream this client is being sent is decodable, and the
 * two are different the moment the client drops a frame: the decoder's own
 * ring then has a hole, and docs/SYNTAX.md 4.1 makes an inter tile naming an
 * absent slot a BITSTREAM error -- ref/src/codec_impl.inc `ref_for` returns
 * NXVC_ERR_BITSTREAM, and vk/decoder/nxvc_vkdec_parse.cpp does the same.  The
 * encoder therefore has to keep a SECOND record: which frames the headset is
 * believed to be able to reconstruct.
 *
 * Two rules make that record, and the second is the one that is easy to miss:
 *
 *   1. a frame the client reports it did not decode is not held;
 *   2. a frame coded from a reference that is not held is not held either,
 *      however cleanly it arrived -- unless it was coded with no temporal
 *      reference at all.  Holding is transitive backwards along the
 *      prediction chain, so a single report invalidates every frame that
 *      descends from it.
 *
 * The history is deeper than the four ring slots on purpose.  A report names
 * a frame by number and arrives a round trip late, by which time the frame
 * may have left the ring; forgetting it would silently promote its
 * descendants back to "held".  Sixteen entries is about 180 ms at 90 Hz,
 * comfortably past the round trip the transport is designed for, and a frame
 * older than that can no longer be referenced anyway (`ref_sel` reaches three
 * frames back at most).
 *
 * A frame this record has never seen is NOT held.  That is the safe direction:
 * the cost of being wrong is one intra frame, and the cost of the other
 * mistake is a frame the headset refuses.
 */
struct HeldState {
    static const int kDepth = 16;
    struct Rec {
        uint32_t fn = 0;
        int64_t pred_fn = -1;   /* the frame it predicts from; -1 = intra */
        uint8_t used = 0;
        uint8_t held = 0;
        /* The client SAID it reconstructed this frame.  Ground truth, not an
         * inference: `held` above is what the encoder can deduce from the
         * prediction chain, and it is optimistic between a drop and the report
         * that names it -- which is a round trip, and which is exactly when
         * the encoder codes the frames that then get refused.  A confirmation
         * has no such window and needs no cascade: a frame the headset says it
         * reconstructed is one it can predict from, whatever happened before
         * or after it. */
        uint8_t confirmed = 0;
    };
    Rec r[kDepth];
    uint32_t newest = 0;
    bool any = false;
    /* Whether a confirmation has EVER arrived.  Until one has, the caller is
     * one that does not send them -- an older client, or a harness -- and the
     * chain-derived `held` is the only evidence there is, so that is what
     * select_reference() uses.  From the first confirmation on it uses
     * confirmations only, which is what makes a refusal impossible rather than
     * merely rarer.  There is deliberately no timeout back the other way: a
     * client that has stopped confirming is a client whose held set is unknown,
     * and coding INTRA for it is correct where guessing is not -- and an INTRA
     * frame it does reconstruct starts the confirmations again. */
    bool any_confirmed = false;
    uint32_t newest_confirmed = 0;
    /* The caller has SAID its client confirms, so confirmations are required
     * from the first frame rather than from the first one that arrives.
     *
     * Without it there is a startup window -- before any confirmation has
     * landed the encoder has nothing but the chain to go on, and the chain is
     * optimistic, so the first frames after the initial INTRA can still be
     * refused.  A caller that knows its client's protocol can close that
     * window by saying so, and pays for it with INTRA frames until the first
     * confirmation, which is a handful of frames once. */
    bool require_confirmed = false;
    bool confirmation_required() const {
        return require_confirmed || any_confirmed;
    }

    void reset() {
        for (int i = 0; i < kDepth; ++i) r[i] = Rec{};
        newest = 0;
        any = false;
        any_confirmed = false;
        newest_confirmed = 0;
    }

    const Rec *find(uint32_t fn) const {
        const Rec &e = r[fn % (uint32_t)kDepth];
        return (e.used && e.fn == fn) ? &e : nullptr;
    }
    Rec *find(uint32_t fn) {
        Rec &e = r[fn % (uint32_t)kDepth];
        return (e.used && e.fn == fn) ? &e : nullptr;
    }
    bool holds(uint32_t fn) const {
        const Rec *e = find(fn);
        return e && e->held;
    }
    bool confirms(uint32_t fn) const {
        const Rec *e = find(fn);
        return e && e->confirmed;
    }

    /* The client said it DID reconstruct `fn`.  Monotonic: nothing later can
     * take it back, because it is a statement about a picture that exists on
     * the device rather than about a chain the encoder is reasoning over.
     *
     * A confirmation for a frame the history no longer covers is accepted and
     * recorded as "confirmations are flowing" without a record to hang it on,
     * because that is all a frame that old can contribute -- `ref_sel` reaches
     * three frames back. */
    void confirm(uint32_t fn) {
        if (Rec *e = find(fn)) {
            e->confirmed = 1;
            /* A frame the client reconstructed is one it holds, whatever the
             * chain deduced.  Saying so keeps the two records from
             * contradicting each other in a log. */
            e->held = 1;
        }
        if (!any_confirmed || fn > newest_confirmed) newest_confirmed = fn;
        any_confirmed = true;
    }

    /* The encoder coded `fn`, predicting from `pred_fn` (-1 when the frame
     * carries no temporal reference).  Its held state follows rule 2. */
    void publish(uint32_t fn, int64_t pred_fn) {
        Rec &e = r[fn % (uint32_t)kDepth];
        e.fn = fn;
        e.pred_fn = pred_fn;
        e.used = 1;
        e.held = (pred_fn < 0) || holds((uint32_t)pred_fn) ? 1u : 0u;
        /* A frame the encoder has only just made cannot have been confirmed:
         * the slot may be carrying an older frame's verdict. */
        e.confirmed = 0;
        if (!any || fn > newest) newest = fn;
        any = true;
    }

    /* The client said it did not reconstruct `fn`.  Clear it, then sweep
     * forward once in frame order: every record is younger than the one it
     * predicts from, so a single ascending pass is the whole transitive
     * closure.  A report for a frame the history no longer covers clears
     * nothing, which is sound -- `ref_sel` reaches three frames back, so
     * nothing that old can be referenced.
     *
     * The sweep demotes a frame only when its predecessor is STILL IN THE
     * HISTORY and unusable.  A predecessor that has merely aged out says
     * nothing: its verdict was already folded into this frame's `held` at
     * publish time, when it was certainly present, and treating "gone" as
     * "not held" would make the oldest entry in the window demote its
     * successor and cascade the whole history to unheld on any report at
     * all. */
    void not_held(uint32_t fn) {
        if (Rec *e = find(fn)) e->held = 0;
        if (!any) return;
        const uint32_t first =
            newest + 1u > (uint32_t)kDepth ? newest + 1u - (uint32_t)kDepth : 0u;
        for (uint32_t k = first; k <= newest; ++k) {
            Rec *e = find(k);
            if (!e || !e->held || e->pred_fn < 0) continue;
            const Rec *pe = find((uint32_t)e->pred_fn);
            if (pe && !pe->held) e->held = 0;
        }
    }
};

/* The reference this frame should ask for: the nearest slot at or beyond
 * `base_ref_sel`, up to 2, that the encoder produced AND the client can
 * predict from.
 *
 * "Can predict from" is the whole question, and it has two answers.  Until a
 * confirmation has ever arrived it is `HeldState::holds` -- what the
 * prediction chain deduces -- which is optimistic for one round trip and is
 * why a dropped frame still costs a refusal.  From the first confirmation on
 * it is `HeldState::confirms`, the client's own statement, which cannot be
 * optimistic: a frame it says it reconstructed is one it holds.  Requiring a
 * confirmation makes a refusal structurally impossible at the cost of a
 * reference one feedback period older, and turns the storm case -- nothing
 * confirmed within reach -- into an INTRA frame, which is decodable, instead
 * of an inter frame that is refused.
 *
 * `base_ref_sel` is the configured distance -- `nxv-enc --ref-sel`'s field --
 * and it is a FLOOR rather than a fixed choice, so a stream configured at 0
 * with a client that holds everything is byte for byte the one this encoder
 * produced before any of this existed, and a stream configured at 1 is the
 * one nxv-enc --ref-sel 1 produces.  The walk only ever goes outwards:
 * nearer is better, because the warp matrix is derived from the reference's
 * view and a more distant reference is a larger inter-frame motion.
 *
 * `out_slot` receives the ring slot, `out_ref_sel` the syntax field.  Returns
 * false when none of the candidates is usable, which is the one case that
 * still costs an all-INTRA frame.
 */
bool select_reference(const RingState &ring, const HeldState &held,
                      uint32_t frame_number, int base_ref_sel,
                      int *out_ref_sel, int *out_slot);

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

/* ------------------------------------------------------------- the views
 *
 * One eye's pose and projection for one frame.  This is the ONLY
 * floating-point input the codec takes, and it is encoder-side: the result
 * reaches the decoder already quantised to the nine int32 of warp_ext().  The
 * struct is nxvc_view's fields, repeated rather than included so that
 * vk/encoder/inter does not depend on the reference codec's header.
 */
struct View {
    double qx = 0, qy = 0, qz = 0, qw = 1;
    double fov_left = -0.8, fov_right = 0.8;
    double fov_up = 0.8, fov_down = -0.8;
};

/* The view history, which is what turns a pose track into a warp.  A frame's
 * matrix is derived from the view of the frame it PREDICTS FROM and the view
 * of itself, so the encoder has to remember the view that went with each ring
 * slot -- exactly as ref/src/codec_impl.inc keeps `views_slot`.  Getting this
 * wrong does not crash and does not produce an illegal stream; it produces a
 * confident prediction of the wrong place, which is indistinguishable from a
 * codec that is merely bad. */
struct ViewState {
    View cur[2];
    View slot[4][2];
    bool have = false;

    void set(const View *v, int eyes, uint32_t frame_number) {
        for (int i = 0; i < eyes && i < 2; ++i) cur[i] = v[i];
        have = true;
        /* Before the first frame there is no history, so every slot is seeded
         * with this view and the first inter frame sees a true previous
         * view rather than the identity. */
        if (frame_number == 0)
            for (int s = 0; s < 4; ++s)
                for (int i = 0; i < 2; ++i) slot[s][i] = cur[i];
    }
    void publish(uint32_t frame_number) {
        const int s = (int)(frame_number & 3u);
        for (int i = 0; i < 2; ++i) slot[s][i] = cur[i];
    }
};

/* warp_ext() for one eye, from the reference slot's view and this frame's.
 * Falls back to the identity when derive_homography refuses -- an entry
 * outside its format, or a denominator that leaves its legal range somewhere
 * in the picture -- because an identity warp predicts badly and the mode
 * decision then notices, where a malformed matrix would be a bitstream error.
 */
WarpMatrix derive_warp(const ViewState &vs, int ref_slot, int eye, int width,
                       int height);

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
