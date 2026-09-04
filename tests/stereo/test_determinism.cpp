// stereo.determinism - the sim's numbers are only worth quoting if the same
// build produces them twice.  Also pins the mode-cost bookkeeping.
#include <cstdio>
#include <string>
#include <vector>

#include "analyze.h"
#include "scene.h"

using namespace nxs;

static int g_fail = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++g_fail;                                        \
        }                                                    \
    } while (0)

int main() {
    RunConfig cfg;
    cfg.width = 192;
    cfg.height = 128;
    cfg.q = 8.0;
    cfg.search_range = 2;

    Scene sc = scene_by_name("room");
    CHECK(!sc.boxes.empty(), "scene 'room' not found");

    SceneResult a = analyze_scene(sc, cfg);
    SceneResult b = analyze_scene(sc, cfg);
    CHECK(digest(a) == digest(b), "two runs differ: %016llx vs %016llx",
          (unsigned long long)digest(a), (unsigned long long)digest(b));
    CHECK(a.tiles.size() == static_cast<size_t>((192 / kTile) * (128 / kTile)), "tile count %zu",
          a.tiles.size());

    // Rendering the same scene twice must produce the same pixels.
    Pose head;
    Camera cam = make_camera(head, 0, cfg.ipd, 128, 128, 95.0);
    View v1 = render(sc, cam), v2 = render(sc, cam);
    CHECK(v1.luma.px == v2.luma.px, "render is not deterministic");
    CHECK(v1.depth.z == v2.depth.z, "depth render is not deterministic");

    // Scene identity: changing the scene changes the digest.
    Scene other = scene_by_name("clutter");
    SceneResult c = analyze_scene(other, cfg);
    CHECK(digest(a) != digest(c), "different scenes hash the same");

    // Every mode must have produced a cost, and INTRA must never be free.
    for (const TileResult& t : a.tiles) {
        for (int m = 0; m < kModeCount; ++m) {
            CHECK(t.bits[m] > 0.0, "mode %s produced %f bits", mode_name(m), t.bits[m]);
            CHECK(t.side[m] >= 3.0, "mode %s side info %f bits", mode_name(m), t.side[m]);
        }
        CHECK(t.disp_seed_px >= 0.0, "negative disparity seed %f", t.disp_seed_px);
        CHECK(t.disocc_frac >= 0.0 && t.disocc_frac <= 1.0, "disocc fraction %f", t.disocc_frac);
    }

    // A larger quantiser must never cost more bits.
    RunConfig coarse = cfg;
    coarse.q = 32.0;
    SceneResult d = analyze_scene(sc, coarse);
    double fine_bits = 0, coarse_bits = 0;
    for (size_t i = 0; i < a.tiles.size(); ++i) {
        fine_bits += a.tiles[i].bits[kIntra];
        coarse_bits += d.tiles[i].bits[kIntra];
    }
    CHECK(coarse_bits < fine_bits, "q=32 cost %.0f bits, q=8 cost %.0f", coarse_bits, fine_bits);

    if (g_fail == 0) std::printf("stereo.determinism: OK\n");
    return g_fail ? 1 : 0;
}
