// nxvc_ref: the parts of the inter path that are not header-inline.
#include "inter.h"

namespace nxvc {

void warp_plane_tile(const nw::RefImage &ref, int tile_x, int tile_y,
                     const nw::Homography &H, const i32 mv[2], nw::Mode mode,
                     int extent, i32 *out) {
    const i32 mv4[4][2] = {{mv[0], mv[1]},
                           {mv[0], mv[1]},
                           {mv[0], mv[1]},
                           {mv[0], mv[1]}};
    warp_plane_tile_quad(ref, tile_x, tile_y, H, mv4, mode, extent, out);
}

// SYNTAX.md 13.10.  A quadrant vector changes the tile's VECTOR and nothing
// else: the four corner source coordinates, and therefore the in-tile
// interpolation basis, remain the tile's own, and the vector is added per
// sample after that interpolation.
//
// There is ONE predictor loop, in warp/ref/warp_ref.cpp, and it is the
// quadrant one; a single-vector tile is four equal quadrant vectors.  The
// reference used to run the whole tile once per quadrant and discard three
// quarters of each pass, which is four times the work and -- worse -- leaves
// "four equal vectors are exactly no quadrant vectors" as a property of two
// code paths agreeing rather than of there being one.  `warp.quad` pins both
// halves of the equivalence across 128 cases x 2 modes x 2 filters x 4
// splits.
void warp_plane_tile_quad(const nw::RefImage &ref, int tile_x, int tile_y,
                          const nw::Homography &H, const i32 mv[4][2],
                          nw::Mode mode, int extent, i32 *out) {
    // One 64x64 scratch, always: warp_tile's output block is fixed at
    // nxvc::warp::kTile and this file does not get to change that.
    static thread_local u16 tmp[nw::kTile * nw::kTile];
    // The split is the PLANE's own half extent, so a 4:2:0 chroma plane at
    // 32x32 splits at 16 -- the quadrants are the tile's quadrants in every
    // plane, not a fixed sample count.
    nw::warp_tile_quad(ref, tile_x, tile_y, H, mv, extent / 2,
                       nw::kFilterBilinear, mode, tmp, nw::kTile);
    for (int y = 0; y < extent; ++y)
        for (int x = 0; x < extent; ++x)
            out[(size_t)y * extent + x] = (i32)tmp[(size_t)y * nw::kTile + x];
}

}  // namespace nxvc
