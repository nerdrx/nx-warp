// nxvc-stereosim: measure what STEREO mode is worth on synthetic stereo pairs.
//
//   nxvc-stereosim [--scene NAME] [--w N] [--h N] [--q Q[,Q...]] [--ipd M]
//                  [--motion S] [--range N] [--json FILE] [--csv FILE] [--quiet]
//
// Exit code is 0 unless a scene fails to render.  See stereo/RESULTS.md.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "analyze.h"

using namespace nxs;

namespace {

struct Policy {
    const char* name;
    std::vector<int> modes;
};

double tile_cost(const TileResult& t, int m) { return t.bits[m] + t.side[m]; }

int best_mode_in(const TileResult& t, const std::vector<int>& modes, double* cost_out) {
    int best = modes[0];
    double bc = tile_cost(t, modes[0]);
    for (size_t i = 1; i < modes.size(); ++i) {
        double c = tile_cost(t, modes[i]);
        if (c < bc) {
            bc = c;
            best = modes[i];
        }
    }
    *cost_out = bc;
    return best;
}

struct Agg {
    double bits_base = 0;    // no STEREO
    double bits_stereo = 0;  // STEREO with app depth
    double bits_est = 0;     // STEREO with encoder-estimated disparity
    double bits_rec = 0;     // STEREO against the reconstructed left eye
    long tiles = 0;
    long stereo_beats_intra = 0;
    long stereo_chosen = 0;
    long base_intra_tiles = 0;
    double base_intra_bits = 0;
    double stereo_on_intra_bits = 0;
    long est_chosen = 0;
    long rec_chosen = 0;
    double stereo_rec_on_intra_bits = 0;
};

void accumulate(Agg* a, const SceneResult& r, const std::vector<int>& base,
                const std::vector<int>& with_stereo, const std::vector<int>& with_est,
                const std::vector<int>& with_rec) {
    for (const TileResult& t : r.tiles) {
        double cb = 0, cs = 0, ce = 0, cr = 0;
        int mb = best_mode_in(t, base, &cb);
        int ms = best_mode_in(t, with_stereo, &cs);
        int me = best_mode_in(t, with_est, &ce);
        int mr = best_mode_in(t, with_rec, &cr);
        a->bits_base += cb;
        a->bits_stereo += cs;
        a->bits_est += ce;
        a->bits_rec += cr;
        ++a->tiles;
        if (mr == kStereoRec || mr == kStereoRecMv) ++a->rec_chosen;
        if (tile_cost(t, kStereoD) < tile_cost(t, kIntra) ||
            tile_cost(t, kStereoDMv) < tile_cost(t, kIntra))
            ++a->stereo_beats_intra;
        if (ms == kStereoD || ms == kStereoDMv) ++a->stereo_chosen;
        if (me == kStereoEst) ++a->est_chosen;
        if (mb == kIntra) {
            ++a->base_intra_tiles;
            a->base_intra_bits += cb;
            a->stereo_on_intra_bits += cs;
            a->stereo_rec_on_intra_bits += cr;
        }
    }
}

double pct(double a, double b) { return b > 0 ? 100.0 * a / b : 0.0; }

}  // namespace

int main(int argc, char** argv) {
    RunConfig cfg;
    std::string only_scene, json_path, csv_path;
    std::vector<double> qs;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--scene") only_scene = next();
        else if (a == "--w") cfg.width = std::atoi(next().c_str());
        else if (a == "--h") cfg.height = std::atoi(next().c_str());
        else if (a == "--ipd") cfg.ipd = std::atof(next().c_str());
        else if (a == "--motion") cfg.motion_scale = std::atof(next().c_str());
        else if (a == "--range") cfg.search_range = std::atoi(next().c_str());
        else if (a == "--json") json_path = next();
        else if (a == "--csv") csv_path = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "--q") {
            std::string s = next();
            size_t p = 0;
            while (p <= s.size()) {
                size_t c = s.find(',', p);
                if (c == std::string::npos) c = s.size();
                if (c > p) qs.push_back(std::atof(s.substr(p, c - p).c_str()));
                p = c + 1;
            }
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: nxvc-stereosim [--scene NAME] [--w N] [--h N] [--q Q,Q]\n"
                "                      [--ipd M] [--motion S] [--range N]\n"
                "                      [--json FILE] [--csv FILE] [--quiet]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (qs.empty()) qs = {4.0, 8.0, 16.0};

    std::vector<Scene> scenes = all_scenes();
    if (!only_scene.empty()) {
        std::vector<Scene> f;
        for (const Scene& s : scenes)
            if (s.name == only_scene) f.push_back(s);
        if (f.empty()) {
            std::fprintf(stderr, "no such scene: %s\n", only_scene.c_str());
            return 2;
        }
        scenes = f;
    }
    if (cfg.width % kTile || cfg.height % kTile) {
        std::fprintf(stderr, "resolution must be a multiple of %d\n", kTile);
        return 2;
    }

    const std::vector<int> base = {kIntra, kWarp, kWarpMv};
    const std::vector<int> with_stereo = {kIntra, kWarp, kWarpMv, kStereoD, kStereoDMv};
    const std::vector<int> with_est = {kIntra, kWarp, kWarpMv, kStereoEst};
    const std::vector<int> with_rec = {kIntra, kWarp, kWarpMv, kStereoRec, kStereoRecMv};

    FILE* jf = json_path.empty() ? nullptr : std::fopen(json_path.c_str(), "w");
    FILE* cf = csv_path.empty() ? nullptr : std::fopen(csv_path.c_str(), "w");
    if (cf)
        std::fprintf(cf,
                     "scene,q,tx,ty,mean_z,disp_seed,disp_est,disocc,edge,activity,"
                     "bits_intra,bits_stereo,bits_stereo_mv,bits_stereo_est,bits_stereo_rec_mv,"
                     "bits_warp,bits_warp_mv,sad_intra,sad_stereo_mv,sad_warp_mv,"
                     "mvx_q2,mvy_q2\n");
    if (jf) std::fprintf(jf, "{\n  \"runs\": [\n");

    bool first_json = true;
    for (double q : qs) {
        cfg.q = q;
        Agg total;
        std::map<std::string, Agg> per_scene;
        if (!quiet) {
            std::printf("\n=== q = %.0f ===\n", q);
            std::printf("%-9s %6s %9s %8s %8s %8s %6s %6s %6s %6s %6s\n", "scene", "tiles",
                        "kbit/base", "kb/ideal", "kbit/rec", "kbit/est", "save%", "rec%", "est%",
                        "win%", "chos%");
        }
        for (const Scene& sc : scenes) {
            SceneResult r = analyze_scene(sc, cfg);
            Agg a;
            accumulate(&a, r, base, with_stereo, with_est, with_rec);
            per_scene[sc.name] = a;
            total.bits_base += a.bits_base;
            total.bits_stereo += a.bits_stereo;
            total.bits_est += a.bits_est;
            total.bits_rec += a.bits_rec;
            total.tiles += a.tiles;
            total.rec_chosen += a.rec_chosen;
            total.stereo_rec_on_intra_bits += a.stereo_rec_on_intra_bits;
            total.stereo_beats_intra += a.stereo_beats_intra;
            total.stereo_chosen += a.stereo_chosen;
            total.est_chosen += a.est_chosen;
            total.base_intra_tiles += a.base_intra_tiles;
            total.base_intra_bits += a.base_intra_bits;
            total.stereo_on_intra_bits += a.stereo_on_intra_bits;

            if (!quiet)
                std::printf("%-9s %6ld %9.1f %8.1f %8.1f %8.1f %6.2f %6.2f %6.2f %6.1f %6.1f\n",
                            sc.name.c_str(), a.tiles, a.bits_base / 1000.0, a.bits_stereo / 1000.0,
                            a.bits_rec / 1000.0, a.bits_est / 1000.0,
                            pct(a.bits_base - a.bits_stereo, a.bits_base),
                            pct(a.bits_base - a.bits_rec, a.bits_base),
                            pct(a.bits_base - a.bits_est, a.bits_base),
                            pct(a.stereo_beats_intra, a.tiles), pct(a.stereo_chosen, a.tiles));

            if (cf)
                for (const TileResult& t : r.tiles)
                    std::fprintf(cf,
                                 "%s,%.0f,%d,%d,%.3f,%.2f,%.0f,%.4f,%.4f,%.2f,%.1f,%.1f,%.1f,%.1f,"
                                 "%.1f,%.1f,%.1f,%lld,%lld,%lld,%d,%d\n",
                                 sc.name.c_str(), q, t.tx, t.ty, t.mean_z, t.disp_seed_px,
                                 t.disp_est_px, t.disocc_frac, t.edge_frac, t.activity,
                                 t.bits[kIntra], t.bits[kStereoD], t.bits[kStereoDMv],
                                 t.bits[kStereoEst], t.bits[kStereoRecMv], t.bits[kWarp],
                                 t.bits[kWarpMv],
                                 (long long)t.sad[kIntra], (long long)t.sad[kStereoDMv],
                                 (long long)t.sad[kWarpMv], t.mv_stereo_rec[0],
                                 t.mv_stereo_rec[1]);

            if (jf) {
                std::fprintf(jf, "%s    {\"q\": %.1f, \"scene\": \"%s\", \"tiles\": %ld, ",
                             first_json ? "" : ",\n", q, sc.name.c_str(), a.tiles);
                std::fprintf(jf,
                             "\"bits_base\": %.1f, \"bits_stereo\": %.1f, \"bits_rec\": %.1f, "
                             "\"bits_est\": %.1f, \"save_pct\": %.3f, \"rec_save_pct\": %.3f, "
                             "\"est_save_pct\": %.3f, "
                             "\"stereo_beats_intra_pct\": %.2f, \"stereo_chosen_pct\": %.2f, "
                             "\"stereo_rec_chosen_pct\": %.2f, "
                             "\"intra_tiles\": %ld, \"intra_bits\": %.1f, "
                             "\"intra_tiles_with_stereo_bits\": %.1f, \"mean_disparity_px\": %.2f, "
                             "\"digest\": \"%016llx\"}",
                             a.bits_base, a.bits_stereo, a.bits_rec, a.bits_est,
                             pct(a.bits_base - a.bits_stereo, a.bits_base),
                             pct(a.bits_base - a.bits_rec, a.bits_base),
                             pct(a.bits_base - a.bits_est, a.bits_base),
                             pct(a.stereo_beats_intra, a.tiles), pct(a.stereo_chosen, a.tiles),
                             pct(a.rec_chosen, a.tiles), a.base_intra_tiles, a.base_intra_bits, a.stereo_on_intra_bits,
                             r.mean_disparity, (unsigned long long)digest(r));
                first_json = false;
            }
        }
        if (!quiet) {
            std::printf("%-9s %6ld %9.1f %8.1f %8.1f %8.1f %6.2f %6.2f %6.2f %6.1f %6.1f\n", "ALL",
                        total.tiles, total.bits_base / 1000.0, total.bits_stereo / 1000.0,
                        total.bits_rec / 1000.0, total.bits_est / 1000.0,
                        pct(total.bits_base - total.bits_stereo, total.bits_base),
                        pct(total.bits_base - total.bits_rec, total.bits_base),
                        pct(total.bits_base - total.bits_est, total.bits_base),
                        pct(total.stereo_beats_intra, total.tiles),
                        pct(total.stereo_chosen, total.tiles));
            std::printf(
                "  right eye: ideal-ref %.2f%%, reconstructed-ref %.2f%% (the honest one)\n"
                "  whole frame (left eye unchanged): ideal %.2f%%, reconstructed %.2f%%\n"
                "  STEREO chosen on %.1f%% of tiles (ideal ref) / %.1f%% (reconstructed ref)\n"
                "  base-INTRA tiles %ld (%.1f%%): %.1f kbit -> %.1f kbit ideal, %.1f kbit rec"
                " (%.1f%% / %.1f%% off those tiles)\n",
                pct(total.bits_base - total.bits_stereo, total.bits_base),
                pct(total.bits_base - total.bits_rec, total.bits_base),
                0.5 * pct(total.bits_base - total.bits_stereo, total.bits_base),
                0.5 * pct(total.bits_base - total.bits_rec, total.bits_base),
                pct(total.stereo_chosen, total.tiles), pct(total.rec_chosen, total.tiles),
                total.base_intra_tiles, pct(total.base_intra_tiles, total.tiles),
                total.base_intra_bits / 1000.0, total.stereo_on_intra_bits / 1000.0,
                total.stereo_rec_on_intra_bits / 1000.0,
                pct(total.base_intra_bits - total.stereo_on_intra_bits, total.base_intra_bits),
                pct(total.base_intra_bits - total.stereo_rec_on_intra_bits,
                    total.base_intra_bits));
        }
    }
    if (jf) {
        std::fprintf(jf, "\n  ]\n}\n");
        std::fclose(jf);
    }
    if (cf) std::fclose(cf);
    return 0;
}
