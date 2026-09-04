// warp_tile_fuzz -- nxvc::warp::warp_tile on random homographies, motion
// vectors and tile coordinates.
//
// Normative reference: warp/include/nxvc/warp.h and docs/WARP.md.  The
// predictor is pure integer arithmetic with a fixed-iteration restoring
// divide, so the two things that can go wrong are exactly the two this target
// checks:
//
//   1. an out-of-bounds read of the reference picture, or a write outside the
//      64x64 output tile.  Corner coordinates are saturated to +-2^18 in Q.6
//      (kCornerClamp) and the homogeneous denominator is required to stay in
//      [kDenMin, kDenMax); an encoder that violates the denominator range is
//      explicitly allowed to exist, and the decoder must *saturate*, not
//      index.  So the fuzzer supplies denominators outside the range on
//      purpose.
//
//   2. an output sample outside [0, max_value].  Every filter tap set sums to
//      a known constant (Catmull-Rom rows sum to 64), so a sample above
//      max_value means a missing clamp, and Catmull-Rom's negative lobes mean
//      the clamp is genuinely reachable, not theoretical.
//
// UBSan is the third check and needs no harness code: signed overflow in the
// Q10.21 / Q2.29 arithmetic, or a shift wider than the type, aborts the run.
//
// Input layout (the custom mutator keeps it in range):
//   [0..35]   h[9]        int32 little endian, rows 0-1 Q10.21, row 2 Q2.29
//   [36..39]  ox, oy      int16 each
//   [40..43]  tile_x, tile_y   int16 each
//   [44..47]  mv_qpel[2]  int16 each, Q.2
//   [48]      filter      0 bilinear, 1 Catmull-Rom
//   [49]      mode        0 warp, 1 static
//   [50]      ref width   in tiles-ish units, see below
//   [51]      ref height
//   [52]      channels    1..4
//   [53]      bit depth selector
//   [54..55]  pad
//   [rest]    reference picture samples
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <vector>

#include "nxvc/warp.h"

#include "common/nxfuzz.h"

namespace {

constexpr size_t kPrefix = 56;
constexpr int32_t kTileSide = nxvc::warp::kTile;

int32_t rd_i32(const uint8_t *p) { return int32_t(nxf::get32(p)); }
int16_t rd_i16(const uint8_t *p) { return int16_t(nxf::get16(p)); }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    using namespace nxvc::warp;
    if (size < kPrefix + 4) return 0;

    Homography H{};
    for (int i = 0; i < 9; ++i) H.h[i] = rd_i32(data + 4 * i);
    H.ox = rd_i16(data + 36);
    H.oy = rd_i16(data + 38);

    int32_t tile_x = rd_i16(data + 40);
    int32_t tile_y = rd_i16(data + 42);
    int32_t mv[2] = {rd_i16(data + 44), rd_i16(data + 46)};

    Filter filter = (data[48] & 1) ? kFilterCatmullRom : kFilterBilinear;
    Mode mode = (data[49] & 1) ? kModeStatic : kModeWarp;

    // A reference picture between 1x1 and 192x192: big enough that a warp can
    // land inside it, small enough that a five-minute run covers the space.
    int32_t rw = 1 + (data[50] % 192);
    int32_t rh = 1 + (data[51] % 192);
    int32_t channels = 1 + (data[52] % 4);
    int32_t max_value = (data[53] & 1) ? 1023 : 255;

    const uint8_t *src = data + kPrefix;
    size_t src_len = size - kPrefix;

    std::vector<uint16_t> ref(static_cast<size_t>(rw) * static_cast<size_t>(rh) * static_cast<size_t>(channels));
    for (size_t i = 0; i < ref.size(); ++i) {
        uint16_t v = uint16_t(src[(2 * i) % src_len] | (uint16_t(src[(2 * i + 1) % src_len]) << 8));
        ref[i] = uint16_t(v % uint16_t(max_value + 1));
    }

    RefImage img{};
    img.data = ref.data();
    img.width = rw;
    img.height = rh;
    img.stride = rw * channels;
    img.channels = channels;
    img.max_value = max_value;

    // Output with a guard margin on every side.  warp_tile writes 64 rows of
    // 64*channels samples at `out_stride`; anything outside that rectangle is
    // an overflow, and the guard catches it in a plain build as well as under
    // ASan.
    const int32_t stride = kTileSide * channels + 8;
    const size_t guard_rows = 2;
    std::vector<uint16_t> out(static_cast<size_t>(stride) * (static_cast<size_t>(kTileSide) + 2 * guard_rows), 0xBEEF);
    uint16_t *base = out.data() + guard_rows * static_cast<size_t>(stride);

    warp_tile(img, tile_x, tile_y, H, mv, filter, mode, base, stride);

    // Guard rows above and below.
    for (size_t i = 0; i < guard_rows * static_cast<size_t>(stride); ++i)
        if (out[i] != 0xBEEF) __builtin_trap();
    for (size_t i = out.size() - guard_rows * static_cast<size_t>(stride); i < out.size(); ++i)
        if (out[i] != 0xBEEF) __builtin_trap();
    // Guard columns: the 8 samples past the end of each written row.
    for (int32_t y = 0; y < kTileSide; ++y)
        for (int32_t x = kTileSide * channels; x < stride; ++x)
            if (base[static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)] != 0xBEEF) __builtin_trap();

    // Every written sample must be a legal sample value.
    for (int32_t y = 0; y < kTileSide; ++y)
        for (int32_t x = 0; x < kTileSide * channels; ++x)
            if (base[static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)] > uint16_t(max_value))
                __builtin_trap();

    // The corner primitive on its own: it is what saturates, and it is exposed
    // for conformance testing precisely so it can be checked in isolation.
    TileCorners c = warp_tile_corners(H, tile_x, tile_y, mode);
    for (int i = 0; i < 4; ++i) {
        // warp.h says corners are saturated to +-kCornerClamp.  They are not,
        // at least in kModeStatic (FINDINGS.md F2), and the overflow argument
        // the constant exists for is what that saturation protects.  Counted
        // rather than trapped so it does not mask memory-safety findings for
        // the rest of a run; the reproducer is in fuzz/regressions/.
        if (c.x[i] > kCornerClamp || c.x[i] < -kCornerClamp ||
            c.y[i] > kCornerClamp || c.y[i] < -kCornerClamp)
            nxf::note_soft_violation("warp_tile_corners left kCornerClamp "
                                     "(FINDINGS.md F2)");
    }

    // The emulated 64-bit primitives and the restoring divide, driven straight
    // from the input.  nxvc_warp_div requires d != 0 and n.hi < d; anything
    // else is the caller's bug, so only legal arguments are passed.
    uint32_t a = nxf::get32(data), b = nxf::get32(data + 4);
    U64 p = nxvc_umul_ext(a, b);
    U64 q = nxvc_imul_ext(int32_t(a), int32_t(b));
    U64 s = nxvc_add64(p, q);
    s = nxvc_shl64(s, nxf::get32(data + 8) & 31u);
    s = nxvc_neg64(s);
    uint32_t d = b | 1u;
    if (s.hi < d) {
        volatile uint32_t quot = nxvc_warp_div(s, d);
        (void)quot;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Structure-aware mutator.  The parameter block is mutated as typed fields,
// and the homography is kept near the legal manifold most of the time: a
// uniformly random h[9] almost always has a denominator outside
// [kDenMin, kDenMax) at every pixel, so the saturation path is all a byte-level
// mutator ever reaches and the interpolation itself is never exercised.
// ---------------------------------------------------------------------------
extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned seed) {
    using namespace nxf;
    using namespace nxvc::warp;
    Rng rng(mix_seed(seed, size));

    uint8_t p[kPrefix];
    std::vector<uint8_t> body;
    if (size >= kPrefix) {
        std::memcpy(p, data, kPrefix);
        body.assign(data + kPrefix, data + size);
    } else {
        std::memset(p, 0, kPrefix);
        // Start from the identity, which is always legal.
        Homography I = identity_homography(64, 64);
        for (int i = 0; i < 9; ++i) set32(p + 4 * i, uint32_t(I.h[i]));
        set16(p + 36, 64);
        set16(p + 38, 64);
    }

    switch (rng.below(9)) {
        case 0: {  // perturb one homography entry by a small amount
            // Deliberately wrapping: the entry is a fuzzer-controlled int32 and
            // the step may push it past the end of the range.  Done in uint32,
            // because signed overflow here is UB in the *mutator* -- which is
            // how this line was found, by the mutator rounds the regression
            // runner executes.
            int i = int(rng.below(9));
            uint32_t v = get32(p + 4 * i);
            uint32_t step = rng.below(1u << (10 + rng.below(12))) - (1u << 9);
            set32(p + 4 * i, v + step);
            break;
        }
        case 1: {  // replace one entry outright, including illegal magnitudes
            int i = int(rng.below(9));
            set32(p + 4 * i, rng.edge_u32());
            break;
        }
        case 2: {  // reset to a legal-by-construction matrix
            Homography I = identity_homography(int32_t(rng.below(2048)), int32_t(rng.below(2048)));
            for (int i = 0; i < 9; ++i) set32(p + 4 * i, uint32_t(I.h[i]));
            break;
        }
        case 3:  // denominator row: straddle kDenMin / kDenMax deliberately
            switch (rng.below(4)) {
                case 0: set32(p + 4 * 8, uint32_t(kDenMin)); break;
                case 1: set32(p + 4 * 8, uint32_t(kDenMax)); break;
                case 2: set32(p + 4 * 8, uint32_t(1 << kQDen)); break;
                default: set32(p + 4 * 8, rng.edge_u32()); break;
            }
            set32(p + 4 * 6, rng.chance(2) ? 0u : rng.edge_u32());
            set32(p + 4 * 7, rng.chance(2) ? 0u : rng.edge_u32());
            break;
        case 4:  // origin
            set16(p + 36, rng.edge_u16());
            set16(p + 38, rng.edge_u16());
            break;
        case 5: {  // tile coordinates: on, off and far outside the picture
            static const uint16_t v[] = {0, 64, 128, 192, 0xffc0, 0x8000, 0x7fc0, 0xffff};
            set16(p + 40, rng.chance(2) ? v[rng.below(8)] : rng.edge_u16());
            set16(p + 42, rng.chance(2) ? v[rng.below(8)] : rng.edge_u16());
            break;
        }
        case 6:  // motion vector, Q.2
            set16(p + 44, rng.edge_u16());
            set16(p + 46, rng.edge_u16());
            break;
        case 7:  // filter / mode / geometry
            p[48] = uint8_t(rng.below(2));
            p[49] = uint8_t(rng.below(2));
            p[50] = uint8_t(rng.chance(3) ? 63 : rng.u8());
            p[51] = uint8_t(rng.chance(3) ? 63 : rng.u8());
            p[52] = uint8_t(rng.below(4));
            p[53] = uint8_t(rng.below(2));
            break;
        default: break;
    }

    mutate_bytes(body, 8192, rng);
    if (body.size() < 4) body.resize(4, uint8_t(rng.u8()));

    std::vector<uint8_t> out;
    putn(out, p, kPrefix);
    putn(out, body.data(), body.size());
    return emit(out, data, max_size);
}
