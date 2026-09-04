// nxvc-stereosim: per-tile mode measurement and aggregation.
#pragma once

#include <string>
#include <vector>

#include "nxs_common.h"
#include "predict.h"
#include "scene.h"

namespace nxs {

enum Mode {
    kIntra = 0,      // DC-plane intra, no reference
    kStereoD,        // right from left, per-tile disparity from the depth buffer
    kStereoDMv,      // ... plus a quarter-pel refinement vector
    kStereoEst,      // ... disparity from an encoder-side coarse search instead
    kStereoRec,      // right from the *reconstructed* left eye, depth seed
    kStereoRecMv,    // ... plus a quarter-pel refinement vector
    kWarp,           // pose-warped right(N-1), zero MV
    kWarpMv,         // ... plus a quarter-pel refinement vector
    kModeCount
};

const char* mode_name(int m);

struct TileResult {
    int tx = 0, ty = 0;
    i64 sad[kModeCount] = {0};
    i64 sse[kModeCount] = {0};
    double bits[kModeCount] = {0};  // residual bits only, at the run's q
    double side[kModeCount] = {0};  // side information bits
    double disp_seed_px = 0;        // f*IPD/z from the depth buffer, median over the tile
    double disp_est_px = 0;         // encoder coarse-search estimate
    double disocc_frac = 0;         // fraction of pixels the left eye cannot see
    double edge_frac = 0;           // fraction whose left source falls outside the frame
    double mean_z = 0;
    double activity = 0;  // mean |Laplacian|, a flatness proxy
    i32 mv_stereo[2] = {0, 0};
    i32 mv_stereo_rec[2] = {0, 0};
    i32 mv_warp[2] = {0, 0};
};

struct RunConfig {
    int width = 1024;
    int height = 1024;
    double fov_deg = 95.0;
    double ipd = 0.064;
    double motion_scale = 1.0;
    double q = 8.0;
    bool use_depth = true;   // app-supplied depth available for the disparity seed
    int search_range = 4;    // integer refinement radius, px
};

struct SceneResult {
    std::string scene;
    std::vector<TileResult> tiles;
    // Sanity numbers about the render itself.
    u64 digest_left = 0, digest_right = 0;
    double mean_disparity = 0;
};

// Render frames N-1 and N for both eyes, then measure every right-eye tile.
SceneResult analyze_scene(const Scene& sc, const RunConfig& cfg);

// One-line digest of a scene result, used by the determinism test.
u64 digest(const SceneResult& r);

}  // namespace nxs
