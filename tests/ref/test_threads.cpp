// Encoder threading: `nxvc_config::threads` may not change a single byte.
//
// The reference encoder codes a frame's tiles on a pool of worker threads
// (ref/README.md "Encoder threading").  That is only legitimate because tiles
// are independent by construction -- own rANS lanes, no cross-tile prediction
// -- and because the two places where they are NOT independent, the motion
// search's left/above vector seeds and a STEREO tile's read of this frame's
// eye-0 reconstruction, are respected by the schedule rather than dropped.
//
// This file is the check that the claim holds: the same source through the
// same configuration at threads = 1, 2, 4 and 0 (auto) has to produce the same
// bytes, and the same per-tile records, on every tool combination that changes
// what the schedule has to preserve.  A failure here is a correctness bug, not
// a performance regression: it means a stream depends on how many cores the
// encoder happened to have.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "test_util.h"

namespace {

struct Clip {
    int w = 0, h = 0, cw = 0, ch = 0, eyes = 1;
    std::vector<std::vector<uint8_t>> Y, U, V;
};

// Moving texture plus a travelling disc: enough structure that the mode
// decision picks every mode (skip, static, warp, near-skip) somewhere, and
// enough motion that the left/above vector seeds actually matter.
Clip make_clip(int eye_w, int h, int eyes, bool c444, int nframes) {
    Clip c;
    c.w = eye_w * eyes;
    c.h = h;
    c.eyes = eyes;
    c.cw = c444 ? c.w : (c.w + 1) / 2;
    c.ch = c444 ? h : (h + 1) / 2;
    for (int f = 0; f < nframes; ++f) {
        std::vector<uint8_t> Y((size_t)c.w * h), U((size_t)c.cw * c.ch, 128),
            V((size_t)c.cw * c.ch, 128);
        const double px = f * 2.3, py = f * 1.1;
        const int ox = 20 + f * 5, oy = 30 + f * 3;
        for (int e = 0; e < eyes; ++e)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < eye_w; ++x) {
                    const double sx = x + px + e * 7, sy = y + py;
                    double v = 128 + 52 * std::sin(sx * 0.033) *
                                         std::cos(sy * 0.029) +
                               26 * std::sin((sx * 3 + sy * 5) * 0.12);
                    v += ((int)(sx / 11 + sy / 9) % 2) ? 14 : -14;
                    const double dx = x - ox, dy = y - oy;
                    if (dx * dx + dy * dy < 15.0 * 15.0) v = 240 - (int)(dx * dx) % 80;
                    Y[(size_t)y * c.w + e * eye_w + x] =
                        (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
        const int cew = c444 ? eye_w : eye_w / 2;
        for (int e = 0; e < eyes; ++e)
            for (int y = 0; y < c.ch; ++y)
                for (int x = 0; x < cew; ++x) {
                    const int sx = x + (int)px, sy = y + (int)py;
                    U[(size_t)y * c.cw + e * cew + x] =
                        (uint8_t)(110 + ((sx * 5 + sy * 3) % 60));
                    V[(size_t)y * c.cw + e * cew + x] =
                        (uint8_t)(140 - ((sx * 3 + sy * 7) % 60));
                }
        c.Y.push_back(std::move(Y));
        c.U.push_back(std::move(U));
        c.V.push_back(std::move(V));
    }
    return c;
}

// Everything one encode of the clip produced: the bytes of every frame, and
// the per-tile records, which carry the decisions the bytes only imply.
struct Result {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<std::vector<nxvc_tile_info>> tiles;
    bool ok = false;
};

Result encode(const nxvc_config &base, const Clip &c, uint32_t threads) {
    Result r;
    nxvc_config cfg = base;
    cfg.threads = threads;
    nxvc_status st = NXVC_OK;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) return r;
    const uint32_t ntiles = nxvc_encoder_tile_count(e);
    std::vector<uint8_t> buf((size_t)c.w * c.h * 4 + (1 << 20));
    for (size_t f = 0; f < c.Y.size(); ++f) {
        // A slowly turning head, so warp_ext() is a real matrix rather than
        // the identity and WARP_MV is reachable.
        nxvc_view v[2];
        for (int i = 0; i < 2; ++i) {
            const double a = 0.004 * (double)f;
            v[i] = nxvc_view{0, std::sin(a), 0, std::cos(a),
                             -0.8, 0.8, 0.8, -0.8};
        }
        nxvc_encoder_set_views(e, v, (uint32_t)c.eyes);
        nxvc_image img{};
        img.plane[0] = const_cast<uint8_t *>(c.Y[f].data());
        img.plane[1] = const_cast<uint8_t *>(c.U[f].data());
        img.plane[2] = const_cast<uint8_t *>(c.V[f].data());
        img.stride[0] = c.w;
        img.stride[1] = c.cw;
        img.stride[2] = c.cw;
        size_t n = 0;
        if (nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, buf.data(),
                                      buf.size(), &n) != NXVC_OK) {
            nxvc_encoder_destroy(e);
            return r;
        }
        r.frames.emplace_back(buf.begin(), buf.begin() + n);
        const nxvc_tile_info *ti = nxvc_encoder_tiles(e, nullptr);
        r.tiles.emplace_back(ti, ti + ntiles);
    }
    nxvc_encoder_destroy(e);
    r.ok = true;
    return r;
}

void compare(const char *name, const nxvc_config &cfg, const Clip &c) {
    const Result one = encode(cfg, c, 1);
    CHECK(one.ok, "%s: threads=1 encode failed", name);
    if (!one.ok) return;
    size_t total = 0;
    for (const auto &f : one.frames) total += f.size();
    CHECK(total > 0, "%s: produced no bytes", name);
    for (uint32_t n : {2u, 4u, 0u}) {
        const Result many = encode(cfg, c, n);
        CHECK(many.ok, "%s: threads=%u encode failed", name, n);
        if (!many.ok) continue;
        CHECK(many.frames.size() == one.frames.size(),
              "%s: threads=%u produced %zu frames, not %zu", name, n,
              many.frames.size(), one.frames.size());
        for (size_t f = 0; f < one.frames.size() && f < many.frames.size(); ++f) {
            CHECK(many.frames[f].size() == one.frames[f].size(),
                  "%s: threads=%u frame %zu is %zu bytes, not %zu", name, n, f,
                  many.frames[f].size(), one.frames[f].size());
            if (many.frames[f].size() != one.frames[f].size()) continue;
            const size_t bad =
                (size_t)(std::mismatch(one.frames[f].begin(),
                                       one.frames[f].end(),
                                       many.frames[f].begin())
                             .first -
                         one.frames[f].begin());
            CHECK(bad == one.frames[f].size(),
                  "%s: threads=%u frame %zu differs at byte %zu", name, n, f,
                  bad);
            // The tile records say WHICH decision moved when a byte moves,
            // which is the difference between a schedule bug and a coding bug.
            CHECK(std::memcmp(one.tiles[f].data(), many.tiles[f].data(),
                              one.tiles[f].size() * sizeof(nxvc_tile_info)) == 0,
                  "%s: threads=%u frame %zu tile records differ", name, n, f);
        }
    }
}

}  // namespace

int main() {
    // 384x384 is 6x6 tiles: wide enough that a worker pool has something to
    // do and that the decision wavefront has interior diagonals, small enough
    // to stay a unit test.
    const Clip c420 = make_clip(384, 384, 1, false, 5);
    const Clip c444 = make_clip(256, 256, 1, true, 2);
    const Clip cst = make_clip(192, 192, 2, false, 4);

    nxvc_config cfg;

    // 1. Phase 1 intra, the default tool set.  No mode decision at all: every
    //    tile is independent, and this is the case the pool codes flat out.
    nxvc_config_default(&cfg);
    cfg.width = 384;
    cfg.height = 384;
    cfg.base_qp = 26;
    compare("intra/420", cfg, c420);

    // 2. Intra at 4:4:4 with the per-tile QP and weighting-matrix searches on,
    //    which is the heaviest per-tile path there is.
    nxvc_config_default(&cfg);
    cfg.width = 256;
    cfg.height = 256;
    cfg.chroma = NXVC_CHROMA_444;
    cfg.base_qp = 18;
    cfg.qp_search = 2;
    cfg.wm_id = 255;
    cfg.preset = NXVC_PRESET_SLOW;
    compare("intra/444/qp-wm-search", cfg, c444);

    // 3. The inter path.  This is the one with a cross-tile dependency: the
    //    motion search seeds from the vectors the LEFT and ABOVE tiles of this
    //    frame chose, so the wavefront has to reproduce the raster order's
    //    seeds exactly or the vectors -- and the bytes -- move.
    nxvc_config_default(&cfg);
    cfg.width = 384;
    cfg.height = 384;
    cfg.base_qp = 26;
    cfg.inter = 1;
    cfg.intra_period = 4;
    compare("inter/420", cfg, c420);

    // 4. The same with the inter-efficiency tools that put bytes in the ROW
    //    header (near-skip) and extra vectors in the tile (quad-mv), because
    //    those are what the row-header emission and the tile concatenation
    //    have to keep in step.
    cfg.near_skip = 1;
    cfg.quad_mv = 1;
    cfg.drift_refresh = 1;
    cfg.me_effort = 3;
    compare("inter/420/near-skip+quad-mv", cfg, c420);

    // 4b. The GPU encoder's integer mode decision (ADR-0028).  It has the same
    //     cross-tile dependency and a stricter reason to care: its search seeds
    //     from the left and above vectors and it breaks ties by the order the
    //     candidates are visited, so an order the pool does not reproduce
    //     exactly moves a vector, and a moved vector moves the bytes.  This is
    //     the decision the Vulkan encoder has to match byte for byte, so a
    //     threading-dependent one would make that test unpassable rather than
    //     merely flaky.
    nxvc_config_default(&cfg);
    cfg.width = 384;
    cfg.height = 384;
    cfg.base_qp = 26;
    cfg.inter = 1;
    cfg.intra_period = 4;
    cfg.inter_int_decision = 1;
    compare("inter/420/int-decision", cfg, c420);

    // 5. Custom tables: the frame's eight table sets are trained on a symbol
    //    histogram the workers accumulate separately and sum afterwards.  If
    //    that sum were order-dependent the tables would be, and every tile in
    //    the frame would then code differently.
    nxvc_config_default(&cfg);
    cfg.width = 384;
    cfg.height = 384;
    cfg.base_qp = 30;
    cfg.custom_tables = 1;
    cfg.tab_v2 = 1;
    cfg.ctx_v3 = 1;
    cfg.table_iters = 3;
    cfg.table_iters_set = 1;
    cfg.inter = 1;
    cfg.intra_period = 8;
    compare("inter/custom-tables/ctx-v3", cfg, c420);

    // 6. Stereo.  A STEREO tile predicts from THIS frame's eye-0
    //    reconstruction, the one intra-frame dependency in the format, so the
    //    encoder keeps such a stream on the serial path whatever `threads`
    //    says.  The bytes must be identical for that reason rather than in
    //    spite of it -- and the point of the case is that asking for threads
    //    on a stereo stream is not an error and does not change the output.
    nxvc_config_default(&cfg);
    cfg.width = 192;
    cfg.height = 192;
    cfg.eyes = 2;
    cfg.base_qp = 26;
    cfg.inter = 1;
    cfg.stereo = 1;
    cfg.intra_period = 4;
    compare("stereo/420/eyes2", cfg, cst);

    // 7. Two eyes without STEREO: the eyes are then independent and the pool
    //    does run, so the eye-minor tile order has to survive it.
    nxvc_config_default(&cfg);
    cfg.width = 192;
    cfg.height = 192;
    cfg.eyes = 2;
    cfg.base_qp = 26;
    cfg.inter = 1;
    cfg.stereo = 0;
    cfg.intra_period = 4;
    compare("eyes2/no-stereo", cfg, cst);

    // 8. Entropy-lite, which replaces the rANS coder and the tables entirely.
    nxvc_config_default(&cfg);
    cfg.width = 384;
    cfg.height = 384;
    cfg.base_qp = 26;
    cfg.entropy_lite = 1;
    compare("intra/entropy-lite", cfg, c420);

    return test_report("threads");
}
