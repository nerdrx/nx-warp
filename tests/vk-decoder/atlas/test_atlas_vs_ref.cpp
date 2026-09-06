// The decoder's ATLAS model against the REFERENCE CODEC's, in one binary.
//
// vk/decoder/atlas/atlas_model.cpp is the oracle the GPU kernel is checked
// against, and conformance is byte-identity with nxv-dec's atlas.  So the
// model's own claim -- "pinned to ref/src/inter.h" -- has to be a TEST and not
// a comment: everything downstream of it inherits whatever it gets wrong.
//
// This links both and runs them side by side over the same inputs.  It is
// deliberately not a paraphrase of either: it calls `nxvc::compose_warp` and
// `nxvc::atlas_advance` directly, exactly as ref/src/codec_impl.inc does, and
// `nxvw::atlas_renorm` and `nxvw::atlas_advance_entry`, exactly as
// atlas_compose.comp's host does.
//
// The interesting inputs are the ILLEGAL ones.  Two implementations of a
// well-behaved composition agree by construction; they diverge at the guard,
// at the cap, and at invalidation -- which is exactly where four real
// divergences were found:
//
//   * a 2^34 renorm guard instead of 13.12.2's normative 2^33;
//   * `gen` wrapping a u16 field instead of saturating;
//   * `gen_max` tested after the composition instead of before it, leaving a
//     different `C` behind under the same `valid == 0`;
//   * invalidation clearing bit 0 instead of the whole flags byte.
//
// Every one of those is invisible to a well-behaved input and visible in the
// 64 bytes conformance compares.
//
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <random>

#include "atlas_model.h"   // the decoder's
#include "inter.h"         // the reference's

namespace {

int g_fail = 0;
void fail(const char *what, long long a = 0, long long b = 0) {
    std::printf("FAIL %s (%lld vs %lld)\n", what, a, b);
    ++g_fail;
}

// A homography in the wire's scales satisfying 3.1.1 for a w x h eye.  `scale`
// pushes it toward the edge of the envelope; above about 4 a composition chain
// trips within a handful of steps, which is where the two implementations have
// anything to disagree about.
void make_legal(std::mt19937_64 &rng, int32_t w, int32_t h, double scale,
                int32_t H[9]) {
    const double one = (double)(1 << nxvw::kWarpQNum);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (;;) {
        const double a = 1.0 + u(rng) * 0.05 * scale;
        const double b = u(rng) * 0.05 * scale;
        const double c = u(rng) * 0.05 * scale;
        const double d = 1.0 + u(rng) * 0.05 * scale;
        const double tx = u(rng) * 64.0 * scale;
        const double ty = u(rng) * 64.0 * scale;
        const double lim =
            0.25 * (double)nxvw::kWarpH22 / (double)(w / 2 + h / 2);
        H[0] = (int32_t)llround(a * one);
        H[1] = (int32_t)llround(b * one);
        H[2] = (int32_t)llround(tx * one);
        H[3] = (int32_t)llround(c * one);
        H[4] = (int32_t)llround(d * one);
        H[5] = (int32_t)llround(ty * one);
        H[6] = (int32_t)llround(u(rng) * lim * scale);
        H[7] = (int32_t)llround(u(rng) * lim * scale);
        H[8] = nxvw::kWarpH22;
        if (nxvw::atlas_in_envelope(H, w, h)) return;
    }
}

}  // namespace

int main() {
    const int32_t W = 1088, H_ = 1088;
    std::mt19937_64 rng(0xa71a5c0deull);

    // 1. One composition step, over the whole range of aggressiveness.  The
    //    two must agree on the RESULT and on whether there is one at all --
    //    a decoder that composes where the reference fails, or fails where it
    //    composes, produces a table nxv-dec never produces.
    long long nOk = 0, nFail = 0;
    for (int t = 0; t < 60000; ++t) {
        const double scale = 1.0 + 7.0 * (double)(t % 29) / 28.0;
        int32_t A[9], B[9];
        make_legal(rng, W, H_, 1.0, A);
        make_legal(rng, W, H_, scale, B);

        // the reference
        nxvc::i32 refOut[9];
        const bool refOkay =
            nxvc::compose_warp(A, B, (int)W, (int)H_, refOut);

        // the decoder's model, the same three steps in the same order
        int32_t modC[9];
        for (int k = 0; k < 9; ++k) modC[k] = A[k];
        const bool modOkay = nxvw::atlas_advance(modC, B, W, H_);

        if (refOkay != modOkay) {
            fail("compose agreement", modOkay, refOkay);
            break;
        }
        if (refOkay) {
            ++nOk;
            for (int k = 0; k < 9; ++k)
                if (modC[k] != refOut[k]) {
                    fail("composed C", modC[k], refOut[k]);
                    t = 1 << 30;
                    break;
                }
        } else {
            ++nFail;
        }
    }
    std::printf("-- one step: %lld composed, %lld refused, and the two agree "
                "on which\n", nOk, nFail);
    if (nFail == 0) fail("the sweep never left the envelope, so it never "
                         "tested the guard");

    // 2. A CHAIN, which is what the atlas actually does: the same entry
    //    right-multiplied step after step until it invalidates.  A one-step
    //    check cannot see a divergence that only appears once the entries have
    //    grown, and this is also where `gen` and `gen_max` come in.
    for (int trial = 0; trial < 400 && !g_fail; ++trial) {
        const double scale = 1.0 + 6.0 * (double)(trial % 17) / 16.0;
        const uint32_t gen_max = (trial % 4 == 0) ? (uint32_t)(1 + trial % 11)
                                                 : 0u;
        const bool isStatic = (trial % 5 == 0);

        nxvc::AtlasEntry re{};
        re.flags = nxvc::kAtlasValid | (isStatic ? nxvc::kAtlasStatic : 0u);
        re.res_level = (uint8_t)(trial % 3);
        re.src_frame = 7;

        nxvw::AtlasEntry me{};
        nxvw::atlas_identity(me.C);
        me.flags = NXVW_ATLAS_FLAG_VALID |
                   (isStatic ? NXVW_ATLAS_FLAG_STATIC : 0u);
        me.res_level = (uint32_t)(trial % 3);
        me.src_frame = 7;

        for (int step = 0; step < 40 && !g_fail; ++step) {
            int32_t Hm[9];
            make_legal(rng, W, H_, scale, Hm);
            nxvc::WarpMatrix rw{};
            for (int k = 0; k < 9; ++k) rw.h[k] = Hm[k];

            nxvc::atlas_advance(re, rw, (int)W, (int)H_, gen_max);
            nxvw::atlas_advance_entry(me, Hm, W, H_, gen_max);

            // All 64 bytes, field by field, so a failure names the field.
            for (int k = 0; k < 9; ++k)
                if (me.C[k] != re.C[k])
                    fail("chain C", me.C[k], re.C[k]);
            if (me.gen != (uint32_t)re.gen) fail("chain gen", me.gen, re.gen);
            if (me.flags != (uint32_t)re.flags)
                fail("chain flags", me.flags, re.flags);
            if (me.src_frame != re.src_frame)
                fail("chain src_frame", me.src_frame, re.src_frame);
            if (me.res_level != (uint32_t)re.res_level)
                fail("chain res_level", me.res_level, re.res_level);
        }
    }

    // 3. `gen` at the u16 ceiling.  The reference SATURATES; a model that let
    //    the field wrap would make a very old entry look freshly composed, and
    //    no random chain reaches 65535 to find out.
    {
        int32_t Hm[9];
        make_legal(rng, W, H_, 1.0, Hm);
        nxvc::WarpMatrix rw{};
        for (int k = 0; k < 9; ++k) rw.h[k] = Hm[k];

        nxvc::AtlasEntry re{};
        re.flags = nxvc::kAtlasValid | nxvc::kAtlasStatic;   // C cannot trip
        re.gen = 0xffffu;
        nxvw::AtlasEntry me{};
        nxvw::atlas_identity(me.C);
        me.flags = NXVW_ATLAS_FLAG_VALID | NXVW_ATLAS_FLAG_STATIC;
        me.gen = 0xffffu;

        for (int i = 0; i < 4; ++i) {
            nxvc::atlas_advance(re, rw, (int)W, (int)H_, 0u);
            nxvw::atlas_advance_entry(me, Hm, W, H_, 0u);
            if (me.gen != (uint32_t)re.gen)
                fail("gen saturation", me.gen, re.gen);
            if (me.flags != (uint32_t)re.flags)
                fail("gen saturation flags", me.flags, re.flags);
        }
    }

    // 4. The 2^33 guard, at the boundary, against the reference's own.  Both
    //    are handed the same `P` directly, so nothing about how `P` was
    //    reached can hide a difference in where the guard sits.
    {
        for (int k = -2; k <= 2; ++k) {
            const int64_t p = ((int64_t)1 << 33) + k;
            int64_t P[9];
            for (int i = 0; i < 9; ++i) P[i] = ((int64_t)1 << 33) - 1;
            P[0] = p;
            int32_t out[9];
            const bool modOkay = nxvw::atlas_renorm(P, out);
            // The reference's guard, transcribed from its own source rather
            // than called: compose_warp() does not take a `P`.
            bool refOkay = true;
            for (int i = 0; i < 9; ++i)
                if (P[i] >= ((int64_t)1 << 33) || P[i] <= -((int64_t)1 << 33))
                    refOkay = false;
            if (modOkay != refOkay)
                fail("2^33 guard boundary", modOkay, refOkay);
        }
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
