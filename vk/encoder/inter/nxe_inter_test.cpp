/* nxe_inter_test.cpp -- the encoder's inter host module against the rules it
 * has to obey.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * None of this needs a GPU: it is the bookkeeping that decides what the GPU is
 * then asked to do, and every one of its rules is one the DECODER also
 * implements from the same normative text.  A disagreement here is a stream
 * the decoder refuses, or -- worse -- one it accepts and reconstructs from the
 * wrong slot, so the checks are against the spec's arithmetic rather than
 * against a golden dump.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nxe_inter.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond);     \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

int main() {
    namespace nw = nxvw;

    /* ---- the ring layout.  Planes are concatenated with no padding between
     * them, each row stride padded to an even number of samples so no uint
     * ever straddles two rows. */
    {
        nxe::RingLayout rl;
        nxe::ring_layout(1088, 1088, 544, 544, 1, 3, rl);
        CHECK(rl.off[0] == 0, "luma at %d", rl.off[0]);
        CHECK(rl.stride[0] == 1088, "luma stride %d", rl.stride[0]);
        CHECK(rl.stride[1] == 544 && rl.stride[2] == 544, "chroma stride %d",
              rl.stride[1]);
        CHECK(rl.off[1] == 1088 * 1088, "Co at %d", rl.off[1]);
        CHECK(rl.off[2] == 1088 * 1088 + 544 * 544, "Cg at %d", rl.off[2]);
        CHECK(rl.slot_u16 == 1088 * 1088 + 2 * 544 * 544, "slot %d",
              rl.slot_u16);
        CHECK(rl.bytes() == (size_t)rl.slot_u16 * 8, "bytes %zu", rl.bytes());

        /* An odd eye pair still pads to even, which is the property the
         * whole packed-pair store rests on. */
        nxe::RingLayout odd;
        nxe::ring_layout(200, 150, 100, 75, 1, 3, odd);
        for (int p = 0; p < 3; ++p)
            CHECK((odd.stride[p] & 1) == 0, "plane %d stride %d is odd", p,
                  odd.stride[p]);

        /* Two eyes: the stride spans the pair, because an eye's sub-picture
         * is a column range of one plane and not a plane of its own. */
        nxe::RingLayout st;
        nxe::ring_layout(512, 512, 256, 256, 2, 3, st);
        CHECK(st.stride[0] == 1024, "stereo luma stride %d", st.stride[0]);
        CHECK(st.planeW[0] == 512, "stereo planeW %d", st.planeW[0]);
    }

    /* ---- the ring's slot rule.  A frame writes `frame_number & 3`; a tile
     * with ref_sel d predicts from the slot holding `frame_number - 1 - d`,
     * and a slot whose stored number is anything else is not a reference.
     * This is the rule the decoder enforces as BITSTREAM, so the encoder has
     * to agree with it exactly or it emits streams the decoder refuses. */
    {
        nxe::RingState r;
        CHECK(r.resolve(0, 0) < 0, "frame 0 must have no reference");
        r.publish(0);
        CHECK(r.resolve(1, 0) == 0, "frame 1 should predict from slot 0");
        CHECK(r.resolve(1, 1) < 0, "ref_sel 1 at frame 1 has no slot");
        r.publish(1);
        r.publish(2);
        r.publish(3);
        CHECK(r.resolve(4, 0) == 3, "frame 4 ref_sel 0 -> slot 3");
        CHECK(r.resolve(4, 1) == 2, "frame 4 ref_sel 1 -> slot 2");
        CHECK(r.resolve(4, 2) == 1, "frame 4 ref_sel 2 -> slot 1");
        /* Slot 0 now holds frame 0, but frame 4 ref_sel 3 wants frame 0 --
         * and 0 & 3 is 0, so the slot index matches.  It is a legal
         * reference, which is exactly why the frame number is checked too:
         * after frame 4 is published, slot 0 holds 4 and the same query must
         * fail. */
        CHECK(r.resolve(4, 3) == 0, "frame 4 ref_sel 3 -> slot 0");
        r.publish(4);
        /* Slot 0 now holds frame 4.  Frame 8 with ref_sel 3 wants frame 4 and
         * 4 & 3 is 0, so that IS the slot and IS the frame: it resolves. */
        CHECK(r.resolve(8, 3) == 0, "frame 8 ref_sel 3 wants frame 4");
        /* Staleness is what the frame-number check catches, and it is not
         * hypothetical: slot 1 still holds frame 1, so frame 9 asking for
         * frame 5 lands on the right slot index with the wrong picture in it.
         * Without the second half of the test the encoder would predict from
         * a frame four older than the one it names, and the decoder -- which
         * makes the same check -- would refuse the stream. */
        CHECK(r.resolve(9, 3) < 0, "a slot holding an older frame is stale");
        r.reset();
        CHECK(r.resolve(4, 0) < 0, "reset must invalidate every slot");
    }

    /* ---- what the CLIENT holds, and which reference that leaves.
     *
     * `RingState` above says what the ENCODER produced.  This says what the
     * headset can reconstruct, which is a different question the moment a
     * frame is dropped there -- and the difference is the whole reason an
     * inter frame gets refused as malformed (docs/SYNTAX.md 4.1: a tile whose
     * `ref_sel` names a picture the decoder does not hold is a BITSTREAM
     * error).  The rules under test are in nxe_inter.h: a reported frame is
     * not held, a frame predicted from an unheld frame is not held either,
     * and a frame coded with no temporal reference is held regardless. */
    {
        nxe::HeldState h;
        CHECK(!h.holds(0), "a frame never published is not held");
        h.publish(0, -1);                       /* frame 0, intra */
        CHECK(h.holds(0), "an intra frame is held");
        h.publish(1, 0);
        h.publish(2, 1);
        h.publish(3, 2);
        CHECK(h.holds(3), "a chain over held frames is held");
        /* One report, and every descendant of it goes with it. */
        h.not_held(1);
        CHECK(h.holds(0), "the report must not reach backwards");
        CHECK(!h.holds(1), "the reported frame is not held");
        CHECK(!h.holds(2), "a frame predicted from an unheld frame is not held");
        CHECK(!h.holds(3), "the cascade must be transitive");
        /* An all-INTRA frame ends the cascade: it reconstructs from nothing,
         * so nothing its nominal reference did can make it unusable.  Getting
         * this wrong would leave the encoder in INTRA for ever after one
         * dropped frame. */
        h.publish(4, -1);
        CHECK(h.holds(4), "an intra resync frame is held whatever preceded it");
        h.publish(5, 4);
        CHECK(h.holds(5), "and its successors are held again");
        /* A report for a frame older than the history is accepted and does
         * nothing.  Nothing that old can be referenced -- ref_sel reaches
         * three frames back -- so this is sound, and it must not crash or
         * clear anything. */
        for (uint32_t k = 6; k < 40; ++k) h.publish(k, (int64_t)k - 1);
        h.not_held(2);
        CHECK(h.holds(39), "a report older than the history changes nothing");
    }

    /* ---- the reference walk.  `select_reference` is what turns the held
     * record into the `ref_sel` a frame carries: the nearest reference at or
     * beyond the configured floor that the encoder produced AND the headset
     * holds, and no reference at all when none of the three qualifies. */
    {
        nxe::RingState r;
        nxe::HeldState h;
        for (uint32_t k = 0; k < 4; ++k) {
            r.publish(k);
            h.publish(k, k == 0 ? -1 : (int64_t)k - 1);
        }
        int d = -1, slot = -1;
        CHECK(nxe::select_reference(r, h, 4, 0, &d, &slot),
              "a client holding everything must have a reference");
        CHECK(d == 0 && slot == 3, "and it must be the newest: d %d slot %d",
              d, slot);
        /* The floor is honoured, which is what makes the encoder byte-
         * identical to `nxv-enc --ref-sel 1` when the client holds all four. */
        CHECK(nxe::select_reference(r, h, 4, 1, &d, &slot) && d == 1 &&
                  slot == 2,
              "a floor of 1 must select d 1, got %d", d);
        /* Now the client drops frame 3.  Frame 4 must step out to frame 2
         * rather than resync. */
        h.not_held(3);
        CHECK(nxe::select_reference(r, h, 4, 0, &d, &slot) && d == 1 &&
                  slot == 2,
              "dropping N-1 must step out to d 1, got %d", d);
        h.not_held(2);
        CHECK(nxe::select_reference(r, h, 4, 0, &d, &slot) && d == 2 &&
                  slot == 1,
              "dropping N-2 as well must step out to d 2, got %d", d);
        /* Three gone is the case ref_sel cannot express: the fourth candidate
         * would be d 3, which the syntax reserves.  This is the one case that
         * still costs an all-INTRA frame, and it must be reported as no
         * reference rather than as d 3. */
        h.not_held(1);
        CHECK(!nxe::select_reference(r, h, 4, 0, &d, &slot),
              "three unheld frames must leave no reference");
        /* A floor above 2 is clamped rather than producing the reserved
         * value. */
        nxe::HeldState h2;
        for (uint32_t k = 0; k < 4; ++k) h2.publish(k, k == 0 ? -1 : (int64_t)k - 1);
        CHECK(nxe::select_reference(r, h2, 4, 9, &d, &slot) && d == 2,
              "a floor above 2 must clamp to 2, got %d", d);
    }

    /* ---- confirmations, which are the difference between a refusal being
     * rarer and a refusal being impossible.
     *
     * The negative report is negative: silence means "held", and silence is
     * also what a frame dropped a moment ago produces, so the chain-derived
     * record is optimistic for one round trip.  A confirmation is a statement
     * about a picture that exists on the device, so it has no such window and
     * needs no cascade. */
    {
        nxe::RingState r;
        nxe::HeldState h;
        for (uint32_t k = 0; k < 4; ++k) {
            r.publish(k);
            h.publish(k, k == 0 ? -1 : (int64_t)k - 1);
        }
        int d = -1, slot = -1;
        /* Nothing confirmed yet and no caller promise: the chain answers, as
         * it did before confirmations existed. */
        CHECK(!h.confirmation_required(), "no promise and no confirmation");
        CHECK(nxe::select_reference(r, h, 4, 0, &d, &slot) && d == 0,
              "the chain still answers before any confirmation");
        /* One confirmation and the policy changes for good: from here the
         * chain's opinion is not consulted. */
        h.confirm(2);
        CHECK(h.confirmation_required(), "one confirmation switches the policy");
        CHECK(h.confirms(2) && !h.confirms(3), "only 2 is confirmed");
        CHECK(nxe::select_reference(r, h, 4, 0, &d, &slot) && d == 1 &&
                  slot == 2,
              "frame 4 must step back to the confirmed frame 2, got d %d", d);
        /* A confirmation is monotonic: a not-held report for a DIFFERENT frame
         * cannot take it away, and the confirmed frame stays selectable. */
        h.not_held(3);
        CHECK(h.confirms(2), "a report about 3 must not unconfirm 2");
        CHECK(nxe::select_reference(r, h, 4, 0, &d, &slot) && d == 1,
              "and 2 is still the reference");
        /* Nothing confirmed within reach is an INTRA frame, which is
         * decodable -- where the inter frame it replaces would have been
         * refused.  Frame 6 can reach 5, 4 and 3; only 2 is confirmed. */
        for (uint32_t k = 4; k < 6; ++k) {
            r.publish(k);
            h.publish(k, (int64_t)k - 1);
        }
        CHECK(!nxe::select_reference(r, h, 6, 0, &d, &slot),
              "no confirmed frame within reach must mean no reference");
    }

    /* ---- the caller's promise, which closes the startup window.
     *
     * Without it the frames between the first INTRA and the first confirmation
     * are still coded on the chain's optimism, and those are exactly the ones
     * a client that is already behind refuses. */
    {
        nxe::RingState r;
        nxe::HeldState h;
        h.require_confirmed = true;
        r.publish(0);
        h.publish(0, -1);
        int d = -1, slot = -1;
        CHECK(!nxe::select_reference(r, h, 1, 0, &d, &slot),
              "with the promise, frame 1 has no reference until 0 is confirmed");
        h.confirm(0);
        CHECK(nxe::select_reference(r, h, 1, 0, &d, &slot) && d == 0,
              "and once 0 is confirmed it is the reference");
    }

    /* ---- the rolling refresh.  Every tile must be refreshed exactly once in
     * every window of `period` frames -- that is the loss-recovery bound of
     * PAPER 2.6 -- and the tiles due on one frame must be scattered rather
     * than a contiguous band, which is what the stagger buys. */
    {
        const uint32_t period = 16;
        for (uint32_t t = 0; t < 64; ++t) {
            int hits = 0;
            for (uint32_t f = 0; f < period; ++f)
                if (nxe::refresh_due(t, f, period)) ++hits;
            CHECK(hits == 1, "tile %u refreshed %d times in %u frames", t, hits,
                  period);
        }
        /* Not a band: the tiles due on a frame should not be consecutive. */
        int consecutive = 0, due = 0;
        bool prev = false;
        for (uint32_t t = 0; t < 289; ++t) {
            const bool d = nxe::refresh_due(t, 0, period);
            if (d) ++due;
            if (d && prev) ++consecutive;
            prev = d;
        }
        CHECK(due > 0, "no tile due at all");
        CHECK(consecutive * 4 < due, "%d of %d due tiles are consecutive",
              consecutive, due);
        /* period 0 means "every tile, every frame", which is how an
         * intra-only stream is expressed. */
        CHECK(nxe::refresh_due(7, 3, 0), "period 0 must force intra");
    }

    /* ---- the warp parameter buffer. */
    {
        nxe::RingLayout rl;
        nxe::ring_layout(256, 256, 128, 128, 1, 3, rl);
        nxe::WarpMatrix wm[2];
        /* A translation of 8 luma samples, Q10.21. */
        wm[0].h[2] = 8 << 21;
        wm[0].h[5] = -4 << 21;
        nxe::WarpBuildInfo bi;
        bi.width = 256; bi.height = 256; bi.cw = 128; bi.ch = 128;
        bi.eyes = 1; bi.cols_per_eye = 4; bi.rows = 4;
        bi.chroma420 = 1; bi.nplanes = 3;
        bi.frame_number = 5; bi.ref_slot = 0; bi.warp = wm;

        nxe::WarpParams wp;
        nxe::build_warp_params(bi, rl, wp);
        CHECK(wp.w.size() ==
                  (size_t)NXVW_WARP_HDR_UINTS + 16u * NXVW_WARP_TILE_UINTS,
              "buffer is %zu words", wp.w.size());

        /* The luma matrix is the matrix as given, with the origin at the
         * plane's centre. */
        CHECK((int32_t)wp.w[2] == (8 << 21), "luma h02 %d", (int32_t)wp.w[2]);
        CHECK((int32_t)wp.w[9] == 128 && (int32_t)wp.w[10] == 128,
              "luma origin %d,%d", (int32_t)wp.w[9], (int32_t)wp.w[10]);

        /* The chroma matrix is conjugated: translation halved (to nearest,
         * ties away from zero), perspective row doubled, origin at the
         * chroma plane's centre. */
        const size_t c = NXVW_WARP_MAT_UINTS;
        CHECK((int32_t)wp.w[c + 2] == (4 << 21), "chroma h02 %d",
              (int32_t)wp.w[c + 2]);
        CHECK((int32_t)wp.w[c + 5] == -(2 << 21), "chroma h12 %d",
              (int32_t)wp.w[c + 5]);
        CHECK((int32_t)wp.w[c + 9] == 64 && (int32_t)wp.w[c + 10] == 64,
              "chroma origin %d,%d", (int32_t)wp.w[c + 9],
              (int32_t)wp.w[c + 10]);
        for (int k = 0; k < 4; ++k)
            CHECK((int32_t)wp.w[k * NXVW_WARP_MAT_UINTS + 8] == nw::kWarpH22,
                  "matrix %d h22 is not normalised", k);

        /* The ring geometry the shader reads out of the header. */
        const size_t r = NXVW_WARP_HDR_RING;
        CHECK((int)wp.w[r + 0] == rl.slot_u16, "hdr slot %u", wp.w[r + 0]);
        CHECK(wp.w[r + 2] == 4u, "hdr colsPerEye %u", wp.w[r + 2]);
        CHECK(wp.w[r + 3] == 1u, "hdr curSlot %u (frame 5 -> 5 & 3)",
              wp.w[r + 3]);
        for (int p = 0; p < 3; ++p) {
            CHECK((int)wp.w[r + 4 + p] == rl.off[p], "hdr off %d", p);
            CHECK((int)wp.w[r + 8 + p] == rl.stride[p], "hdr stride %d", p);
        }

        /* Every tile starts INTRA with the inter bit clear, so a frame whose
         * decision never runs predicts nothing rather than predicting
         * garbage. */
        for (uint32_t t = 0; t < 16; ++t) {
            const uint32_t w0 = wp.w[wp.tile_word(t)];
            CHECK((w0 & 7u) == (uint32_t)nw::kModeIntra, "tile %u mode %u", t,
                  w0 & 7u);
            CHECK(((w0 >> 3) & 1u) == 0u, "tile %u inter bit set", t);
            CHECK(wp.w[wp.tile_word(t) + 1] == t % 4u, "tile %u col", t);
            CHECK(wp.w[wp.tile_word(t) + 2] == t / 4u, "tile %u row", t);
            CHECK(wp.w[wp.tile_word(t) + 6] == 0u, "tile %u refBase", t);
        }

        /* A skip sets the mode and the inter bit and keeps the eye. */
        nxe::set_tile_mode(wp, 6, nw::kModeWarpSkip, 0, 0);
        const uint32_t w6 = wp.w[wp.tile_word(6)];
        CHECK((w6 & 7u) == (uint32_t)nw::kModeWarpSkip, "mode %u", w6 & 7u);
        CHECK(((w6 >> 3) & 1u) == 1u, "inter bit clear on a skip");

        /* No reference: refBase is the sentinel, and Pass W then fills the
         * tile with mid-grey instead of reading a slot that does not hold
         * what it claims. */
        nxe::WarpBuildInfo nb = bi;
        nb.ref_slot = -1;
        nxe::WarpParams np;
        nxe::build_warp_params(nb, rl, np);
        CHECK(np.w[np.tile_word(0) + 6] == 0xffffffffu, "refBase %u",
              np.w[np.tile_word(0) + 6]);

        /* Two eyes: the eye bit is positional, from the tile index. */
        nxe::WarpBuildInfo sb = bi;
        sb.eyes = 2; sb.cols_per_eye = 4; sb.rows = 2;
        nxe::WarpParams sp;
        nxe::build_warp_params(sb, rl, sp);
        for (uint32_t t = 0; t < 16; ++t) {
            const uint32_t expect_eye = (t % 8u) / 4u;
            CHECK(((sp.w[sp.tile_word(t)] >> 4) & 1u) == expect_eye,
                  "tile %u eye", t);
        }
    }

    /* ---- the push block and the WPred stride. */
    {
        nxe::RingLayout rl;
        nxe::ring_layout(1088, 1088, 544, 544, 1, 3, rl);
        nxe::WarpBuildInfo bi;
        bi.width = 1088; bi.height = 1088; bi.cw = 544; bi.ch = 544;
        bi.eyes = 1; bi.cols_per_eye = 17; bi.rows = 17; bi.chroma420 = 1;
        const nw::NxvwWarpPush p = nxe::warp_push(bi, rl);
        CHECK(p.tileCount == 289, "tileCount %d", p.tileCount);
        CHECK(p.eyeFilter == -1, "eyeFilter %d", p.eyeFilter);
        CHECK(p.wpredStrideI16 == 4096 + 2 * 32 * 32,
              "wpred stride %d", p.wpredStrideI16);
        CHECK((p.wpredStrideI16 & 1) == 0, "wpred stride must be even");
        CHECK(nxe::wpred_bytes(289, 1, 0) ==
                  (size_t)289 * (size_t)p.wpredStrideI16 * 2,
              "wpred bytes");
        /* 4:4:4 chroma is a full 64-edge tile. */
        CHECK(nxe::wpred_stride_i16(0, 0) == 3 * 4096, "444 stride %d",
              nxe::wpred_stride_i16(0, 0));
    }

    std::printf("nxe_inter: %s (%d failures)\n", g_fail ? "FAIL" : "ok",
                g_fail);
    return g_fail ? 1 : 0;
}
