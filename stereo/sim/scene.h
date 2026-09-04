// nxvc-stereosim: synthetic stereo scene generation.
//
// A pinhole stereo pair (parallel cameras, no toe-in) rendered by primary-ray
// casting against axis-aligned boxes with procedural face textures.  Ray
// casting rather than triangle rasterisation because we need exact per-pixel
// eye-space depth and exact occlusion for both eyes, which is what the
// disparity and disocclusion measurements depend on; the visible result is the
// same as a z-buffered rasteriser of the same boxes.
#pragma once

#include <string>
#include <vector>

#include "nxs_common.h"

namespace nxs {

enum class Tex {
    kFlat,      // near-constant, worst case for any predictor to be worth bits
    kNoise,     // fbm value noise, broadband detail
    kChecker,   // hard edges at a fixed pitch
    kStripes,   // oriented high-frequency
    kBrick,     // structured mid-frequency with mortar lines
    kText,      // 5x7 bitmap text on a light panel
    kSkin,      // low-contrast smooth with pores; the "hand"
};

struct Box {
    Vec3 lo, hi;      // world-space AABB, metres
    Tex tex = Tex::kNoise;
    double texel_per_m = 64.0;  // texture density
    double tint = 1.0;          // luma multiplier
    u32 seed = 0;
};

// Rendered output for one eye at one time instant.
struct View {
    Image luma;
    DepthMap depth;
};

struct Camera {
    Mat3 rot = Mat3::identity();  // world-from-camera rotation
    Vec3 pos{0, 0, 0};            // world-space eye position (metres)
    double f = 0;                 // focal length, pixels
    double cx = 0, cy = 0;
    int w = 0, h = 0;
};

struct Scene {
    std::string name;
    std::vector<Box> boxes;
    double bg_luma = 8.0;  // luma where no box is hit
};

// The five scenes the sim reports on.  See RESULTS.md for what each is for.
std::vector<Scene> all_scenes();
Scene scene_by_name(const std::string& name);

// Head pose for a frame index.  frame 0 is N-1, frame 1 is N.  The motion is a
// 300 deg/s-class yaw with roll and a walking-speed translation, i.e. the
// regime where the pose warp is doing real work (PAPER 2.2).
struct Pose {
    Mat3 rot;
    Vec3 pos;
};
Pose pose_for_frame(int frame, double motion_scale);

// Build the camera for one eye.  eye = 0 left, 1 right.  ipd in metres.
Camera make_camera(const Pose& head, int eye, double ipd, int w, int h, double fov_deg);

// Render one view.  Deterministic given (scene, cam).
View render(const Scene& sc, const Camera& cam);

// Procedural texture lookup, luma 0..255.  Exposed for tests.
double sample_texture(Tex t, u32 seed, double u, double v, double dens);

}  // namespace nxs
