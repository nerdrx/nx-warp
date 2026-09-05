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
