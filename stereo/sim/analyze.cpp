#include "analyze.h"

#include <algorithm>
#include <cmath>

namespace nxs {

const char* mode_name(int m) {
    switch (m) {
        case kIntra: return "INTRA";
        case kStereoD: return "STEREO";
        case kStereoDMv: return "STEREO_MV";
        case kStereoEst: return "STEREO_EST";
        case kStereoRec: return "STEREO_REC";
        case kStereoRecMv: return "STEREO_REC_MV";
        case kWarp: return "WARP_SKIP";
        case kWarpMv: return "WARP_MV";
    }
    return "?";
}

namespace {

// Median of a small vector, by value.
double median_of(std::vector<double>& v) {
    if (v.empty()) return 0;
    size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + n, v.end());
    return v[n];
}

// Integer + quarter-pel refinement around a seed, minimising SAD.
// pred_fn(dx_q2, dy_q2, out) fills the prediction for a candidate vector.
template <typename PredFn>
void refine(const Image& target, int tx, int ty, i32 seed_x_q2, i32 seed_y_q2, int range,
            PredFn pred_fn, i32* best_x, i32* best_y, i64* best_sad, std::vector<i32>* scratch) {
    i32 bx = seed_x_q2, by = seed_y_q2;
    pred_fn(bx, by, scratch);
    i64 bs = tile_sad(target, tx, ty, *scratch);

    // Integer step, full square search of the given radius.
    for (int dy = -range; dy <= range; ++dy) {
        for (int dx = -range; dx <= range; ++dx) {
            if (dx == 0 && dy == 0) continue;
            i32 cx = seed_x_q2 + dx * 4, cy = seed_y_q2 + dy * 4;
            pred_fn(cx, cy, scratch);
            i64 s = tile_sad(target, tx, ty, *scratch);
            if (s < bs) {
                bs = s;
                bx = cx;
                by = cy;
            }
        }
    }
    // Quarter-pel, the eight neighbours of the integer winner.
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            i32 cx = bx + dx, cy = by + dy;
            pred_fn(cx, cy, scratch);
            i64 s = tile_sad(target, tx, ty, *scratch);
            if (s < bs) {
                bs = s;
                bx = cx;
                by = cy;
            }
        }
    }
    *best_x = bx;
    *best_y = by;
    *best_sad = bs;
    pred_fn(bx, by, scratch);
}

double tile_activity(const Image& img, int tx, int ty) {
    const int x0 = tx * kTile, y0 = ty * kTile;
    double s = 0;
    for (int j = 0; j < kTile; ++j)
        for (int i = 0; i < kTile; ++i) {
            int c = img.clamped(x0 + i, y0 + j);
            int l = 4 * c - img.clamped(x0 + i - 1, y0 + j) - img.clamped(x0 + i + 1, y0 + j) -
                    img.clamped(x0 + i, y0 + j - 1) - img.clamped(x0 + i, y0 + j + 1);
            s += std::abs(l);
        }
    return s / (kTile * kTile);
}

}  // namespace

u64 digest(const SceneResult& r) {
    u64 h = fnv1a(r.scene.data(), r.scene.size());
    h = fnv1a(&r.digest_left, sizeof(r.digest_left), h);
    h = fnv1a(&r.digest_right, sizeof(r.digest_right), h);
    for (const TileResult& t : r.tiles) {
        h = fnv1a(t.sad, sizeof(t.sad), h);
        h = fnv1a(t.mv_stereo, sizeof(t.mv_stereo), h);
        h = fnv1a(t.mv_stereo_rec, sizeof(t.mv_stereo_rec), h);
        h = fnv1a(t.mv_warp, sizeof(t.mv_warp), h);
        // Bits are doubles produced by a fixed sequence of operations; quantise
        // before hashing so the digest does not depend on the last mantissa bit.
        for (int m = 0; m < kModeCount; ++m) {
            i64 q = static_cast<i64>(std::llround(t.bits[m] * 16.0));
            h = fnv1a(&q, sizeof(q), h);
        }
    }
    return h;
}

SceneResult analyze_scene(const Scene& sc, const RunConfig& cfg) {
    SceneResult res;
    res.scene = sc.name;

    const Pose p_prev = pose_for_frame(0, cfg.motion_scale);
    const Pose p_cur = pose_for_frame(1, cfg.motion_scale);

    const Camera cam_l = make_camera(p_cur, 0, cfg.ipd, cfg.width, cfg.height, cfg.fov_deg);
    const Camera cam_r = make_camera(p_cur, 1, cfg.ipd, cfg.width, cfg.height, cfg.fov_deg);
    const Camera cam_r_prev = make_camera(p_prev, 1, cfg.ipd, cfg.width, cfg.height, cfg.fov_deg);
    const Camera cam_l_prev = make_camera(p_prev, 0, cfg.ipd, cfg.width, cfg.height, cfg.fov_deg);

    const View left = render(sc, cam_l);
    const View right = render(sc, cam_r);
    const View right_prev = render(sc, cam_r_prev);
    const View left_prev = render(sc, cam_l_prev);

    res.digest_left = fnv1a(left.luma.px.data(), left.luma.px.size());
    res.digest_right = fnv1a(right.luma.px.data(), right.luma.px.size());

    const WarpQ warp = quantize_warp(p_prev.rot, p_cur.rot, cam_r.f, cam_r.cx, cam_r.cy);

    // What the decoder actually holds for the left eye of this frame: the
    // pose-warped left(N-1) plus a quantised residual.  STEREO_REC predicts
    // from this; STEREO predicts from the pristine render.  Truth is the
    // former, and the gap between them is the honest error bar.
    const Image left_recon =
        reconstruct_frame(left.luma, left_prev.luma,
                          quantize_warp(p_prev.rot, p_cur.rot, cam_l.f, cam_l.cx, cam_l.cy), cfg.q,
                          cfg.search_range);

    const int tiles_x = cfg.width / kTile;
    const int tiles_y = cfg.height / kTile;
    const double f_ipd = cam_r.f * cfg.ipd;

    std::vector<i32> pred, scratch;
    double disp_acc = 0;
    int disp_n = 0;

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            TileResult tr;
            tr.tx = tx;
            tr.ty = ty;
            const int x0 = tx * kTile, y0 = ty * kTile;

            // ---- geometry statistics: disparity seed, disocclusion, edges
            std::vector<double> disps;
            disps.reserve(kTile * kTile);
            double zsum = 0;
            int disocc = 0, edge = 0;
            for (int j = 0; j < kTile; ++j) {
                for (int i = 0; i < kTile; ++i) {
                    const double z = right.depth.at(x0 + i, y0 + j);
                    const double zc = z >= kFarZ * 0.5 ? 1.0e4 : z;
                    zsum += zc;
                    const double d = f_ipd / zc;
                    disps.push_back(d);
                    // Ground-truth visibility: where does this surface land in
                    // the left eye, and does the left eye actually see it?
                    const int ul = static_cast<int>(std::lround(x0 + i + d));
                    if (ul < 0 || ul >= cfg.width) {
                        ++edge;
                        continue;
                    }
                    const double zl = left.depth.at(ul, y0 + j);
                    const double zlc = zl >= kFarZ * 0.5 ? 1.0e4 : zl;
                    if (std::fabs(zlc - zc) > std::max(0.02 * zc, 0.01)) ++disocc;
                }
            }
            tr.mean_z = zsum / (kTile * kTile);
            tr.disocc_frac = static_cast<double>(disocc) / (kTile * kTile);
            tr.edge_frac = static_cast<double>(edge) / (kTile * kTile);
            tr.disp_seed_px = median_of(disps);
            tr.activity = tile_activity(right.luma, tx, ty);
            disp_acc += tr.disp_seed_px;
            ++disp_n;

            // ---- INTRA
            intra_plane(right.luma, tx, ty, &pred);
            tr.sad[kIntra] = tile_sad(right.luma, tx, ty, pred);
            tr.sse[kIntra] = tile_sse(right.luma, tx, ty, pred);
            tr.bits[kIntra] = tile_bits(right.luma, tx, ty, pred, cfg.q);
            tr.side[kIntra] = mode_side_bits("INTRA");

            // ---- STEREO with the depth-derived per-tile disparity.
            // The right eye samples the left at x + D, D = f*IPD/z >= 0, and
            // the vertical component is zero by construction (parallel eyes).
            const i32 seed_q2 = static_cast<i32>(std::lround(tr.disp_seed_px * 4.0));
            auto stereo_pred = [&](i32 dx, i32 dy, std::vector<i32>* o) {
                shift_tile(left.luma, tx, ty, dx, dy, o);
            };
            stereo_pred(seed_q2, 0, &pred);
            tr.sad[kStereoD] = tile_sad(right.luma, tx, ty, pred);
            tr.sse[kStereoD] = tile_sse(right.luma, tx, ty, pred);
            tr.bits[kStereoD] = tile_bits(right.luma, tx, ty, pred, cfg.q);
            tr.side[kStereoD] = mode_side_bits("STEREO");

            // ---- STEREO plus refinement
            {
                i32 bx = 0, by = 0;
                i64 bs = 0;
                refine(right.luma, tx, ty, seed_q2, 0, cfg.search_range, stereo_pred, &bx, &by, &bs,
                       &pred);
                tr.mv_stereo[0] = bx - seed_q2;
                tr.mv_stereo[1] = by;
                tr.sad[kStereoDMv] = bs;
                tr.sse[kStereoDMv] = tile_sse(right.luma, tx, ty, pred);
                tr.bits[kStereoDMv] = tile_bits(right.luma, tx, ty, pred, cfg.q);
                tr.side[kStereoDMv] = mode_side_bits("STEREO_MV");
            }

            // ---- STEREO without depth: encoder-side coarse disparity search.
            // 4x downsampled tile, horizontal only, 0..192 px in 2 px steps,
            // then the same refinement.  This is the fallback when no
            // XR_NX_render_hints depth is supplied (docs/XR_EXT_NXWARP.md).
            {
                i64 best = -1;
                i32 best_d = 0;
                for (int d = 0; d <= 192; d += 2) {
                    i64 s = 0;
                    for (int j = 0; j < kTile; j += 4)
                        for (int i = 0; i < kTile; i += 4)
                            s += std::abs(static_cast<i32>(right.luma.at(x0 + i, y0 + j)) -
                                          left.luma.clamped(x0 + i + d, y0 + j));
                    if (best < 0 || s < best) {
                        best = s;
                        best_d = d;
                    }
                }
                tr.disp_est_px = best_d;
                i32 bx = 0, by = 0;
                i64 bs = 0;
                refine(right.luma, tx, ty, best_d * 4, 0, cfg.search_range, stereo_pred, &bx, &by,
                       &bs, &pred);
                tr.sad[kStereoEst] = bs;
                tr.sse[kStereoEst] = tile_sse(right.luma, tx, ty, pred);
                tr.bits[kStereoEst] = tile_bits(right.luma, tx, ty, pred, cfg.q);
                tr.side[kStereoEst] = mode_side_bits("STEREO_MV");
            }

            // ---- STEREO from the reconstructed left eye
            {
                auto rec_pred = [&](i32 dx, i32 dy, std::vector<i32>* o) {
                    shift_tile(left_recon, tx, ty, dx, dy, o);
                };
                rec_pred(seed_q2, 0, &pred);
                tr.sad[kStereoRec] = tile_sad(right.luma, tx, ty, pred);
                tr.sse[kStereoRec] = tile_sse(right.luma, tx, ty, pred);
                tr.bits[kStereoRec] = tile_bits(right.luma, tx, ty, pred, cfg.q);
                tr.side[kStereoRec] = mode_side_bits("STEREO");
                i32 bx = 0, by = 0;
                i64 bs = 0;
                refine(right.luma, tx, ty, seed_q2, 0, cfg.search_range, rec_pred, &bx, &by, &bs,
                       &pred);
                tr.mv_stereo_rec[0] = bx - seed_q2;
                tr.mv_stereo_rec[1] = by;
                tr.sad[kStereoRecMv] = bs;
                tr.sse[kStereoRecMv] = tile_sse(right.luma, tx, ty, pred);
                tr.bits[kStereoRecMv] = tile_bits(right.luma, tx, ty, pred, cfg.q);
                tr.side[kStereoRecMv] = mode_side_bits("STEREO_MV");
            }

            // ---- WARP of right(N-1), zero vector
            auto warp_pred = [&](i32 dx, i32 dy, std::vector<i32>* o) {
                warp_tile(right_prev.luma, warp, tx, ty, dx, dy, o);
            };
            warp_pred(0, 0, &pred);
            tr.sad[kWarp] = tile_sad(right.luma, tx, ty, pred);
            tr.sse[kWarp] = tile_sse(right.luma, tx, ty, pred);
            tr.bits[kWarp] = tile_bits(right.luma, tx, ty, pred, cfg.q);
            tr.side[kWarp] = mode_side_bits("WARP_SKIP");

            // ---- WARP plus refinement
            {
                i32 bx = 0, by = 0;
                i64 bs = 0;
                refine(right.luma, tx, ty, 0, 0, cfg.search_range, warp_pred, &bx, &by, &bs, &pred);
                tr.mv_warp[0] = bx;
                tr.mv_warp[1] = by;
                tr.sad[kWarpMv] = bs;
                tr.sse[kWarpMv] = tile_sse(right.luma, tx, ty, pred);
                tr.bits[kWarpMv] = tile_bits(right.luma, tx, ty, pred, cfg.q);
                tr.side[kWarpMv] = mode_side_bits("WARP_MV");
            }

            res.tiles.push_back(tr);
        }
    }
    res.mean_disparity = disp_n ? disp_acc / disp_n : 0.0;
    return res;
}

}  // namespace nxs
