// nxvc_ref: the parts of the inter path that are not header-inline.
#include "inter.h"

namespace nxvc {

void warp_plane_tile(const nw::RefImage &ref, int tile_x, int tile_y,
                     const nw::Homography &H, const i32 mv[4][2], nw::Mode mode,
                     int extent, i32 *out) {
    // One 64x64 scratch, always: warp_tile's output block is fixed at
    // nxvc::warp::kTile and this file does not get to change that.  The
    // quadrant split is the PLANE's own half-extent, so a 4:2:0 chroma tile
    // cropped out of the 64x64 block splits at 16 and its quadrants cover the
    // same picture area as the luma tile's (docs/SYNTAX.md 13.8).
    static thread_local u16 tmp[nw::kTile * nw::kTile];
    nw::warp_tile_quad(ref, tile_x, tile_y, H, mv, extent / 2,
                       nw::kFilterBilinear, mode, tmp, nw::kTile);
    for (int y = 0; y < extent; ++y)
        for (int x = 0; x < extent; ++x)
            out[(size_t)y * extent + x] = (i32)tmp[(size_t)y * nw::kTile + x];
}

}  // namespace nxvc
