// NX Warp -- determinism probe.
//
// Runs a fixed corpus of tiles through warp_tile() and prints one FNV-1a hash
// of every output sample. The corpus contains no floating point at all: the
// nine homography entries are synthesised directly inside their fixed-point
// formats, so the hash depends only on the normative integer path.
//
// The ctest `warp.determinism` builds this file at -O0 and -O3, with g++ and
// with clang++ where both exist, and requires one identical hash from all of
// them. Any difference is a bitstream-breaking bug, not a rounding curiosity:
// the encoder runs the decoder to build its references, so a single differing
// pixel drifts forever.
//
// SPDX-License-Identifier: Apache-2.0

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "warp_corpus.h"

using namespace nxvc::warp;
using namespace nxvc::warp::test;

int main(int argc, char** argv) {
    const int tiles = argc > 1 ? std::atoi(argv[1]) : 4096;
    const int W = 512, H = 512, CH = 4;

    Picture pic = make_picture(W, H, CH, 255, 0x5EEDull);
    std::vector<uint16_t> out(static_cast<size_t>(kTile) * kTile * CH);
    Rng rng(0xA11CEull);
    Hash hash;
    Case cs{};

    for (int i = 0; i < tiles; ++i) {
        make_case_int(rng, W, H, &cs);
        warp_tile(pic.img, cs.tile_x, cs.tile_y, cs.H, cs.mv, cs.filter, cs.mode, out.data(),
                  kTile * CH);
        // Mix the inputs in too, so a corpus-generation change is also caught.
        for (int k = 0; k < 9; ++k) hash.i32(cs.H.h[k]);
        hash.i32(cs.tile_x);
        hash.i32(cs.tile_y);
        hash.i32(cs.mv[0]);
        hash.i32(cs.mv[1]);
        hash.i32(static_cast<int32_t>(cs.filter));
        hash.i32(static_cast<int32_t>(cs.mode));
        for (uint16_t s : out) hash.u16(s);
    }
    std::printf("%d %016" PRIx64 "\n", tiles, hash.h);
    return 0;
}
