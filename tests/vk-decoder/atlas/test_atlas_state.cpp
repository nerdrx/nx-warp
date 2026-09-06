// The ATLAS host bookkeeping: monotonicity, the lazy-advance selection, and
// the `H` ring window.  No Vulkan and no device -- it is host arithmetic, the
// same way nxvc_vkdec_parse is.
//
// The property under test is the one docs/ATLAS-DECODER.md calls "the required
// equivalence test", reduced to its host half: the same tiles applied in
// arrival order and applied frame-complete must leave the same state, because
// the SET of tiles applied is the same and only the order differs.
//
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <random>
#include <vector>

#include "atlas_state.h"

using namespace nxvw;

namespace {
int g_fail = 0;
void fail(const char *what, long long a = 0, long long b = 0) {
    std::printf("FAIL %s (%lld vs %lld)\n", what, a, b);
    ++g_fail;
}
}  // namespace

int main() {
    const uint32_t E = 578;   // the v1 stereo configuration

    // 1. Frame 0 is a real frame number.  An entry that has never been coded
    //    must ACCEPT it -- which is why `src_frame` starts at -1 and not at 0,
    //    and `0 >= 0` would have dropped every tile of the first frame.
    {
        AtlasHostState s;
        s.reset(E);
        const uint32_t t[3] = {0, 17, 577};
        AtlasApply r = s.apply(t, 3, 0);
        if (r.accepted.size() != 3) fail("frame 0 dropped", r.accepted.size(), 3);
        if (!r.superseded.empty()) fail("frame 0 superseded", r.superseded.size(), 0);
    }

    // 2. Monotonicity.  A tile of frame N arriving after a LATER frame already
    //    coded that position is dropped and reported.
    {
        AtlasHostState s;
        s.reset(E);
        const uint32_t t[1] = {42};
        s.commit(t, 1, 10);
        // an older frame's tile: superseded
        AtlasApply late = s.apply(t, 1, 7);
        if (late.superseded.size() != 1 || late.superseded[0] != 42)
            fail("an older frame's tile was not superseded");
        if (!late.accepted.empty()) fail("an older frame's tile was accepted");
        // the SAME frame again -- a retransmit -- is also superseded.  `>=`,
        // not `>`: re-running the write-back over an entry already reset is
        // harmless for the metadata and wrong for the pixels the moment a
        // NEAR_SKIP correction has landed on top of them.
        AtlasApply same = s.apply(t, 1, 10);
        if (same.superseded.size() != 1) fail("a retransmit was accepted");
        // a newer frame: accepted
        AtlasApply next = s.apply(t, 1, 11);
        if (next.accepted.size() != 1) fail("a newer frame's tile was dropped");
        // and a position that has never been coded is untouched by any of it
        const uint32_t u[1] = {43};
        if (s.apply(u, 1, 7).accepted.size() != 1)
            fail("an uncoded position was superseded by its neighbour");
    }

    // 3. The `H` ring window.  A frame inside the ring is live, one that has
    //    fallen out is not, and a frame in the FUTURE is not either -- the
    //    modulo would otherwise make frame N+64 look like frame N.
    {
        AtlasHostState s;
        s.reset(E);
        for (uint32_t f = 0; f <= 200; ++f) s.record_frame(f);
        if (!s.h_live(200, 200)) fail("the current frame is not live");
        if (!s.h_live(200, 200 - (NXVW_ATLAS_HRING - 1))) fail("the oldest ring frame is not live");
        if (s.h_live(200, 200 - NXVW_ATLAS_HRING)) fail("a frame past the ring is live");
        if (s.h_live(200, 201)) fail("a future frame is live");
    }

    // 4. Arrival order against frame-complete.  Fifty frames; in one run every
    //    tile of a frame is applied at once, in the other the frame's tiles are
    //    split into runs and the runs are INTERLEAVED across frames -- which is
    //    what a lossy link produces.  The same tiles are applied either way, so
    //    the same `src_frame` must come out.
    {
        std::mt19937_64 rng(0xa71a55ull);
        AtlasHostState eager, lazy;
        eager.reset(E);
        lazy.reset(E);
        // (frame, tile) pairs, applied in frame order in one run and shuffled
        // within a window in the other.
        std::vector<std::pair<uint32_t, uint32_t>> arrivals;
        for (uint32_t f = 0; f < 50; ++f) {
            eager.record_frame(f);
            lazy.record_frame(f);
            std::vector<uint32_t> tiles;
            for (uint32_t t = 0; t < E; ++t)
                if ((rng() % 100u) < 15u) tiles.push_back(t);
            AtlasApply r = eager.apply(tiles.data(), (uint32_t)tiles.size(), f);
            eager.commit(r.accepted.data(), (uint32_t)r.accepted.size(), f);
            for (uint32_t t : tiles) arrivals.push_back({f, t});
        }
        // Shuffle inside a sliding window, so a tile can arrive after a LATER
        // frame's tile at the same position -- which is what makes anything
        // superseded at all.  A full shuffle would do too; a window is what
        // the transport actually produces.
        for (size_t i = 1; i < arrivals.size(); ++i) {
            const size_t j = i - (size_t)(rng() % (i < 4000 ? i : 4000));
            std::swap(arrivals[i], arrivals[j]);
        }
        uint32_t nSuperseded = 0;
        for (auto &a : arrivals) {
            const uint32_t t = a.second;
            AtlasApply r = lazy.apply(&t, 1, a.first);
            nSuperseded += (uint32_t)r.superseded.size();
            lazy.commit(r.accepted.data(), (uint32_t)r.accepted.size(), a.first);
        }
        for (uint32_t t = 0; t < E; ++t)
            if (eager.src_frame(t) != lazy.src_frame(t)) {
                fail("arrival order changed src_frame", lazy.src_frame(t),
                     eager.src_frame(t));
                break;
            }
        std::printf("-- %zu arrivals shuffled, %u superseded, and the surviving "
                    "src_frame is identical to the frame-complete run\n",
                    arrivals.size(), nSuperseded);
        if (nSuperseded == 0)
            fail("the shuffle never reordered two tiles of one position, so it "
                 "never tested the rule");
    }

    // 5. The advance selection: the tiles about to be read, PLUS every entry
    //    about to fall out of the ring.  The second set is what keeps the ring
    //    from ever being the reason a tile is invalidated.
    {
        AtlasHostState s;
        s.reset(E);
        std::vector<uint32_t> sel;
        const uint32_t named[2] = {5, 6};
        // At frame 1 nothing has fallen behind, so only the named tiles.
        s.select_for_advance(named, 2, 1, sel);
        if (sel.size() != 2) fail("frame 1 selected more than the named tiles",
                                  sel.size(), 2);
        // At frame HRING-1 every entry is exactly at the edge and all of them
        // must be selected, named or not.
        s.select_for_advance(named, 2, NXVW_ATLAS_HRING - 1u, sel);
        if (sel.size() != E) fail("the ring edge did not select every entry",
                                  sel.size(), E);
        // After a flush nothing is behind at all.
        s.mark_all_advanced(NXVW_ATLAS_HRING - 1u);
        s.select_for_flush(NXVW_ATLAS_HRING - 1u, sel);
        if (!sel.empty()) fail("a flushed table still had entries behind",
                               sel.size(), 0);
    }

    // 6. The PICTURE frame ([SYN] 13.12.10 and the per-frame MODE).  Every
    //    entry the frame does NOT code is re-posed and materialised, so it
    //    takes `src_frame := N`; the entries it DOES code take it through the
    //    ordinary coded path.  After such a frame EVERY entry has age 0.
    //
    //    The ordering is the whole point and it is asserted, not assumed: a
    //    coded tile is tested for supersede against the PREVIOUS frame's
    //    `src_frame`, so `apply()` must run BEFORE the rebase.  Doing it the
    //    other way makes every coded tile of the rebasing frame satisfy
    //    `src_frame >= N` and be dropped -- the 35-of-46 failure 13.12.10
    //    names -- and this test fails if that regresses.
    {
        AtlasHostState s;
        s.reset(E);
        const uint32_t coded0[3] = {1, 2, 3};
        AtlasApply r0 = s.apply(coded0, 3, 10);
        s.commit(r0.accepted.data(), (uint32_t)r0.accepted.size(), 10);

        // Frame 20 is a PICTURE frame that also codes tiles 1 and 4.
        const uint32_t coded1[2] = {1, 4};
        AtlasApply r1 = s.apply(coded1, 2, 20);
        if (r1.accepted.size() != 2)
            fail("a PICTURE frame's own coded tiles were superseded -- the "
                 "rebase ran before apply()", r1.accepted.size(), 2);
        s.rebase_picture(coded1, 2, 20);
        s.commit(r1.accepted.data(), (uint32_t)r1.accepted.size(), 20);

        for (uint32_t t = 0; t < E; ++t)
            if (s.src_frame(t) != 20) {
                fail("a PICTURE frame left an entry with a stale src_frame",
                     s.src_frame(t), 20);
                break;
            }
        // And a later frame's tiles are NOT superseded by it: age 0 is the
        // truth, not a block on everything that follows.
        const uint32_t coded2[1] = {7};
        AtlasApply r2 = s.apply(coded2, 1, 21);
        if (r2.accepted.size() != 1)
            fail("a PICTURE frame superseded the NEXT frame's coded tile",
                 r2.accepted.size(), 1);

        // The ATLAS-frame rebase is the other rule and must NOT move
        // `src_frame`: it settles a pending transform and makes nothing newer.
        AtlasHostState t2;
        t2.reset(E);
        const uint32_t c3[1] = {2};
        AtlasApply r3 = t2.apply(c3, 1, 5);
        t2.commit(r3.accepted.data(), (uint32_t)r3.accepted.size(), 5);
        t2.rebase_settle(9);
        if (t2.src_frame(2) != 5)
            fail("an ATLAS-frame rebase moved src_frame; 13.12.10 says it must "
                 "not", t2.src_frame(2), 5);
        std::printf("-- PICTURE frame: every entry at age 0, the frame's own "
                    "coded tiles kept, and an ATLAS-frame rebase leaves "
                    "src_frame alone\n");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
