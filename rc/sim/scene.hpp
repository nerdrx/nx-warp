// Synthetic per-tile scene for nxvc-rcsim.
//
// The scene is a per-tile material map.  Each material's statistics are
// measured once from a real 64x64 synthetic tile (nxrc::synth) so the
// classifier is exercised on genuine numbers; per frame the simulator only
// varies complexity, retinal slip and a little statistical jitter.  That is
// the shape a real per-tile stats dump from tools/ would have, so
// Scene::load_dump() can replace the generator when one exists.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef NXRC_SIM_SCENE_HPP
#define NXRC_SIM_SCENE_HPP

#include "nxrc/nxrc.hpp"
#include "nxfov/foveation.hpp"

#include <string>
#include <vector>

namespace rcsim {

enum Material : uint8_t {
    MAT_SKY = 0,      // flat gradient
    MAT_WALL,         // band-limited texture
    MAT_GROUND,       // coarser texture
    MAT_OUTLINE,      // straight edges (geometry silhouettes)
    MAT_UI,           // text panel, UI stencil set
    MAT_FOLIAGE,      // high-activity texture, fast moving
    MAT_COUNT
};

struct Scene {
    int tiles_x = 0, tiles_y = 0;      // one eye
    int eyes    = 2;
    size_t n    = 0;                   // tiles_x * tiles_y * eyes

    std::vector<uint8_t> material;     // per tile
    std::vector<float>   a_true;       // ground-truth bit model, per tile
    nxrc::TileStats      stats;        // per tile, per frame (jittered)
    std::vector<float>   complexity;   // warped SAD, per tile, per frame
    std::vector<float>   slip;         // deg/s, per tile, per frame
    float                head_speed_deg_s = 0.0f;
    float                intra_ratio      = 0.0f;

    // Frames at which the content jumps.  A cut re-rolls a_true and drives
    // intra_ratio above the threshold for one frame.
    std::vector<int>     cuts;

    void build(int tiles_x, int tiles_y, int eyes, uint32_t seed);
    // Advance to frame `f`; fills stats, complexity, slip, head speed.
    void step(int f, uint32_t seed);

    // Try to load a real per-tile stats dump written by tools/.  Returns
    // false and leaves the synthetic scene untouched when there is none.
    bool load_dump(const std::string& path);

    const char* material_name(uint8_t m) const;
};

// A stereo tile array: one eye's foveation map duplicated so the tile count
// matches the paper's 2312 tiles per stereo frame.
nxfov::FoveationMap stereo_map(const nxfov::FoveationMap& one_eye, int eyes);

} // namespace rcsim

#endif
