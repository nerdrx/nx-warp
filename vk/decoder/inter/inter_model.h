// CPU model of Pass W, the inter predictor kernel.
//
// This is to warp_pred.comp what passB_model.{h,cpp} is to reconstruct.comp:
// the same formulas, the same constants (both include inter_layout.h), the
// same rounding and the same clamps, executed sequentially instead of by 256
// threads.  It is the oracle the kernel is tested against.
//
// **The warp itself is not re-implemented here.**  `nxvc_warp_ref` is the
// normative predictor, warp/glsl/warp_tile.comp is its GLSL twin, and
// warp_pred.comp's corner derivation, corner interpolation and bilinear tap
// are a line-for-line copy of that twin.  So this model calls
// `nxvc::warp::warp_tile_quad()` directly, exactly as ref/src/inter.cpp does,
// and what it models is everything the kernel adds AROUND the library: the
// plane loop, the conjugated matrix, the halved vectors, the reference-ring
// addressing, the res_level box average and the near-skip mean field.  A
// model that copied the arithmetic a third time would test the copy, not the
// kernel.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXVW_INTER_MODEL_H
#define NXVW_INTER_MODEL_H

#include <cstdint>
#include <vector>

#include "inter_layout.h"

namespace nxvw {

struct InterModelInput {
    NxvwWarpPush push{};
    // The parameter block exactly as the kernel's binding 1 sees it:
    // NXVW_WARP_HDR_UINTS of header, then one NxvwWarpTile per tile.
    const uint32_t *params = nullptr;
    // The reference ring exactly as binding 0 sees it: u16 samples packed two
    // per uint, four slots of `push.ringSlotU16` elements each.
    const uint32_t *ring = nullptr;
};

// Fill `wpred` -- `push.tileCount * push.wpredStrideI16` int16 -- for every
// tile the kernel would write with these push constants (`push.eyeFilter`
// included).  Slots the kernel does not write are left untouched, exactly as
// the kernel leaves them.
void inter_model_predict(const InterModelInput &in, int16_t *wpred);

// One sample of the ring, for a test that wants to read it back the way the
// predictor does.  `plane_off` and `stride` are the header's.
int inter_model_ring_at(const uint32_t *ring, int base, int stride, int w,
                        int h, int x, int y);

}  // namespace nxvw

#endif  // NXVW_INTER_MODEL_H
