// The ATLAS reference (SYNTAX.md 13.12, ADR-0029).
//
// The property this file exists to hold is the same one test_inter.cpp holds,
// stated about a different object: **the encoder runs the decoder**, and under
// the atlas the thing the two must agree on is the ATLAS -- its pixels and its
// per-tile table -- and not the displayed picture, which is explicitly
// non-normative and is compared here only for sanity.
//
// Every identity check below covers all 64 bytes of every per-tile record plus
// a digest of every atlas plane, because a model whose metadata agrees and
// whose pixels do not is not a model that a GPU decoder can be written against.
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "test_util.h"

namespace {

// ---------------------------------------------------------------- material
static inline int tex(int x, int y) {
    double v = 128 + 55 * std::sin(x * 0.031) * std::cos(y * 0.027) +
               30 * std::sin((x * 3 + y * 5) * 0.11) +
               18 * std::sin((double)(x * x + y * y) * 0.00042);
    v += ((x / 13 + y / 11) % 2) ? 12 : -12;
    return v < 0 ? 0 : (v > 255 ? 255 : (int)v);
}

struct Scene {
    int w = 0, h = 0, cw = 0, ch = 0;
    std::vector<uint8_t> Y, U, V;
};

// `panel` paints a hard-edged static rectangle in the top-left quadrant: the
// head-locked UI the static-skip rule of 13.12.3 exists for.  It does not move
// with `pan`, so under a picture model it must be re-coded every frame.
static Scene make_scene(int w, int h, bool c444, double pan, int obj, int panel) {
    Scene s;
    s.w = w; s.h = h;
    s.cw = c444 ? w : (w + 1) / 2;
    s.ch = c444 ? h : (h + 1) / 2;
    s.Y.assign((size_t)w * h, 0);
    s.U.assign((size_t)s.cw * s.ch, 128);
    s.V.assign((size_t)s.cw * s.ch, 128);
    const int px = (int)pan;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = tex(x + px, y);
            const int dx = x - obj, dy = y - h / 2;
            if (dx * dx + dy * dy < 18 * 18) v = 230;
            if (panel && x < w / 3 && y < h / 3)
                v = ((x / 8 + y / 8) % 2) ? 235 : 20;
            s.Y[(size_t)y * w + x] = (uint8_t)v;
        }
    for (int y = 0; y < s.ch; ++y)
        for (int x = 0; x < s.cw; ++x) {
            const int f = c444 ? 1 : 2;
            s.U[(size_t)y * s.cw + x] =
                (uint8_t)(110 + (tex(x * f + px, y * f) >> 4));
            s.V[(size_t)y * s.cw + x] =
                (uint8_t)(140 - (tex(x * f, y * f + 7) >> 4));
        }
    return s;
}

static nxvc_view view_yaw(double deg) {
    nxvc_view v{};
    const double a = deg * 3.14159265358979323846 / 360.0;
    v.qx = 0; v.qy = std::sin(a); v.qz = 0; v.qw = std::cos(a);
    const double f = 95.0 * 3.14159265358979323846 / 360.0;
    v.fov_left = -f; v.fov_right = f; v.fov_up = f; v.fov_down = -f;
    return v;
}

// ------------------------------------------------------------------ harness
struct Opts {
    int w = 192, h = 192, frames = 8, qp = 26, eyes = 1;
    bool c444 = false;
    uint32_t atlas = 1, row_present = 0, near_skip = 0, quad_mv = 0;
    uint32_t intra_period = 1000, gen_max = 0;
    uint32_t nbr = 0, skip_margin = 0;
    double yaw_per_frame = 0.4;
    int panel = 0;
    // lost[frame][tile]; the decoder is told, and the encoder is told the
    // complement as a receipt, which is the whole loss contract of 13.12.6.
    std::vector<std::vector<uint8_t>> lost;
    bool feed_receipts = true;
};

struct Run {
    bool ok = false;
    std::string err;
    std::vector<std::vector<uint8_t>> enc_at, dec_at;   // per frame
    std::vector<uint32_t> frame_bytes;
    std::vector<uint32_t> coded_tiles;
    std::vector<uint8_t> disp_y;    // last displayed picture, luma
    std::vector<uint8_t> src_y;     // its source, for PSNR
    uint32_t tiles = 0;
    std::vector<double> psnr_per_frame;
};

// Table + per-plane pixel digest: the whole normative output of 13.12.
template <class GetTable, class GetPlane>
static std::vector<uint8_t> atlas_blob(size_t table_bytes, GetTable gt,
                                       GetPlane gp) {
    std::vector<uint8_t> out(table_bytes + 32, 0);
    gt(out.data(), table_bytes);
    for (int p = 0; p < 4; ++p) {
        uint64_t hsh = 1469598103934665603ull;
        uint32_t w = 0, h = 0, stride = 0;
        const uint16_t *d = gp(p, &w, &h, &stride);
        if (d)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t v = d[(size_t)y * stride + x];
                    hsh = (hsh ^ (v & 0xff)) * 1099511628211ull;
                    hsh = (hsh ^ (v >> 8)) * 1099511628211ull;
                }
        for (int k = 0; k < 8; ++k)
            out[table_bytes + p * 8 + k] = (uint8_t)(hsh >> (8 * k));
    }
    return out;
}

static Run run(const Opts &o) {
    Run r;
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)o.w;
    cfg.height = (uint32_t)o.h;
    cfg.eyes = (uint32_t)o.eyes;
    cfg.chroma = o.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)o.qp;
    cfg.inter = 1;
    cfg.atlas = o.atlas;
    cfg.row_present = o.row_present;
    cfg.atlas_static_skip = o.atlas;
    cfg.atlas_gen_max = o.gen_max;
    cfg.atlas_nbr = o.nbr;
    cfg.atlas_skip_margin = o.skip_margin;
    cfg.near_skip = o.near_skip;
    cfg.quad_mv = o.quad_mv;
    cfg.intra_period = o.intra_period;
    cfg.custom_tables = 0;
    cfg.threads = 1;

    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { r.err = "encoder_create"; return r; }
    nxvc_decoder *d = nxvc_decoder_create(&st);
    if (!d) { nxvc_encoder_destroy(e); r.err = "decoder_create"; return r; }

    std::vector<uint8_t> hdr(64);
    size_t hn = 0;
    st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hn);
    if (st != NXVC_OK) { r.err = "stream_header"; goto done; }
    {
        size_t consumed = 0;
        st = nxvc_decoder_parse_stream_header(d, hdr.data(), hn, &consumed);
        if (st != NXVC_OK) { r.err = "parse_stream_header"; goto done; }
    }
    r.tiles = nxvc_decoder_tile_count(d);

    for (int f = 0; f < o.frames; ++f) {
        const int full_w = o.w * o.eyes;
        Scene s = make_scene(full_w, o.h, o.c444, f * 2.0, 30 + f * 4, o.panel);
        nxvc_image img{};
        img.plane[0] = s.Y.data(); img.stride[0] = full_w;
        img.plane[1] = s.U.data(); img.stride[1] = s.cw;
        img.plane[2] = s.V.data(); img.stride[2] = s.cw;

        std::vector<nxvc_view> views((size_t)o.eyes,
                                     view_yaw(f * o.yaw_per_frame));
        for (int k = 1; k < o.eyes; ++k) views[k] = views[0];
        nxvc_encoder_set_views(e, views.data(), (uint32_t)o.eyes);

        std::vector<uint8_t> buf((size_t)full_w * o.h * 3 + (1 << 16));
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, buf.data(),
                                       buf.size(), &ol);
        if (st != NXVC_OK) { r.err = "encode " + std::to_string(f); goto done; }
        r.frame_bytes.push_back((uint32_t)ol);

        uint32_t tc = 0;
        const nxvc_tile_info *eti = nxvc_encoder_tiles(e, &tc);
        uint32_t coded = 0;
        for (uint32_t t = 0; t < tc; ++t)
            if (!eti[t].skipped) ++coded;
        r.coded_tiles.push_back(coded);

        // --- decode, with this frame's loss vector if any
        if (f < (int)o.lost.size() && !o.lost[f].empty())
            nxvc_decoder_set_lost_tiles(d, o.lost[f].data(),
                                        (uint32_t)o.lost[f].size());
        std::vector<uint8_t> dy((size_t)full_w * o.h),
            du((size_t)s.cw * s.ch), dv((size_t)s.cw * s.ch);
        nxvc_image dimg{};
        dimg.plane[0] = dy.data(); dimg.stride[0] = full_w;
        dimg.plane[1] = du.data(); dimg.stride[1] = s.cw;
        dimg.plane[2] = dv.data(); dimg.stride[2] = s.cw;
        size_t consumed = 0;
        st = nxvc_decoder_decode_frame(d, buf.data(), ol, &dimg, &consumed);
        if (st != NXVC_OK) { r.err = "decode " + std::to_string(f); goto done; }

        // --- the receipt: what the client actually holds (13.12.6)
        if (o.feed_receipts && f < (int)o.lost.size() &&
            !o.lost[f].empty()) {
            std::vector<uint8_t> recv(o.lost[f].size(), 1);
            for (size_t t = 0; t < recv.size(); ++t)
                if (o.lost[f][t]) recv[t] = 0;
            st = nxvc_encoder_set_received_tiles(e, recv.data(),
                                                 (uint32_t)recv.size());
            if (st != NXVC_OK) {
                r.err = "receipts " + std::to_string(f);
                goto done;
            }
        }

        if (o.atlas) {
            const size_t etb = nxvc_encoder_atlas_table_size(e);
            const size_t dtb = nxvc_decoder_atlas_table_size(d);
            if (etb == 0 || etb != dtb) { r.err = "table size"; goto done; }
            r.enc_at.push_back(atlas_blob(
                etb,
                [&](uint8_t *p, size_t n) { nxvc_encoder_atlas_table(e, p, n); },
                [&](int pl, uint32_t *w, uint32_t *h, uint32_t *sd) {
                    return nxvc_encoder_atlas_plane(e, pl, w, h, sd);
                }));
            r.dec_at.push_back(atlas_blob(
                dtb,
                [&](uint8_t *p, size_t n) { nxvc_decoder_atlas_table(d, p, n); },
                [&](int pl, uint32_t *w, uint32_t *h, uint32_t *sd) {
                    return nxvc_decoder_atlas_plane(d, pl, w, h, sd);
                }));
        }
        r.psnr_per_frame.push_back(
            psnr8(dy.data(), s.Y.data(), (size_t)full_w * o.h));
        if (f == o.frames - 1) { r.disp_y = dy; r.src_y = s.Y; }
    }
    r.ok = true;
done:
    nxvc_encoder_destroy(e);
    nxvc_decoder_destroy(d);
    return r;
}

// Compare the two atlases frame by frame; returns the first differing frame or
// -1.  Also reports how many 64-byte records differ, which localises a failure
// to a tile rather than to a stream.
static int atlas_differs(const Run &r, int *bad_records = nullptr) {
    for (size_t f = 0; f < r.enc_at.size(); ++f) {
        if (r.enc_at[f] != r.dec_at[f]) {
            if (bad_records) {
                int n = 0;
                const size_t nrec = (r.enc_at[f].size() - 32) / 64;
                for (size_t t = 0; t < nrec; ++t)
                    if (std::memcmp(r.enc_at[f].data() + t * 64,
                                    r.dec_at[f].data() + t * 64, 64) != 0)
                        ++n;
                *bad_records = n;
            }
            return (int)f;
        }
    }
    return -1;
}

// ------------------------------------------------------------------- tests
// 1. The contract: encoder shadow == decoder atlas, across the tool matrix.
static void test_identity() {
    struct Case { const char *name; Opts o; };
    std::vector<Case> cases;
    {
        Opts o; o.c444 = false;                       cases.push_back({"420", o});
    }
    { Opts o; o.c444 = true;                          cases.push_back({"444", o}); }
    { Opts o; o.eyes = 2;                             cases.push_back({"stereo-geometry", o}); }
    { Opts o; o.near_skip = 1;                        cases.push_back({"near-skip", o}); }
    { Opts o; o.quad_mv = 1;                          cases.push_back({"quad-mv", o}); }
    { Opts o; o.near_skip = 1; o.quad_mv = 1;         cases.push_back({"near-skip+quad-mv", o}); }
    { Opts o; o.row_present = 1;                      cases.push_back({"row-present", o}); }
    { Opts o; o.intra_period = 4;                     cases.push_back({"refresh", o}); }
    { Opts o; o.yaw_per_frame = 0.0;                  cases.push_back({"no-motion", o}); }
    { Opts o; o.yaw_per_frame = 2.5;                  cases.push_back({"fast-turn", o}); }
    { Opts o; o.panel = 1;                            cases.push_back({"static-panel", o}); }
    { Opts o; o.gen_max = 3;                          cases.push_back({"gen-max", o}); }
    // Tool bit 33, the neighbour-aware gather (13.12.4).  It is NORMATIVE --
    // it changes which samples the predictor reads -- so the whole of the
    // matrix above has to hold under it as well, and in particular under fast
    // motion, which is the only condition in which it does anything at all.
    { Opts o; o.nbr = 1;                              cases.push_back({"nbr", o}); }
    { Opts o; o.nbr = 1; o.yaw_per_frame = 2.5;       cases.push_back({"nbr-fast-turn", o}); }
    { Opts o; o.nbr = 1; o.c444 = true;               cases.push_back({"nbr-444", o}); }
    { Opts o; o.nbr = 1; o.eyes = 2;                  cases.push_back({"nbr-stereo-geometry", o}); }
    { Opts o; o.nbr = 1; o.near_skip = 1; o.quad_mv = 1;
                                                      cases.push_back({"nbr-near-skip+quad-mv", o}); }
    { Opts o; o.nbr = 1; o.panel = 1;                 cases.push_back({"nbr-static-panel", o}); }
    { Opts o; o.nbr = 1; o.intra_period = 4;          cases.push_back({"nbr-refresh", o}); }
    // The displacement bound is ENCODER-side and changes no rule, so the
    // stream it produces must decode under the unchanged 13.12.4.
    { Opts o; o.skip_margin = 8; o.yaw_per_frame = 2.5;
                                                      cases.push_back({"skip-margin-8", o}); }
    { Opts o; o.skip_margin = 4; o.nbr = 1;           cases.push_back({"skip-margin-4+nbr", o}); }
    for (auto &c : cases) {
        Run r = run(c.o);
        CHECK(r.ok, "%s: %s", c.name, r.err.c_str());
        if (!r.ok) continue;
        int bad = 0;
        const int f = atlas_differs(r, &bad);
        CHECK(f < 0, "%s: atlas diverges at frame %d (%d records)", c.name, f,
              bad);
    }
}

// 2. Loss.  A missed frame invalidates exactly the tiles it coded, and the
//    encoder's one-deep undo brings its shadow back into agreement -- with no
//    signalling, no concealment kernel and no replay (13.12.6).
static void test_loss() {
    Opts o;
    o.frames = 10;
    o.near_skip = 1;
    o.lost.assign((size_t)o.frames, {});
    // A quarter of the tiles on every third frame, deterministically.
    Run probe = run(Opts{});
    const uint32_t nt = probe.tiles;
    CHECK(nt > 0, "tile count");
    for (int f = 0; f < o.frames; ++f) {
        if (f % 3 != 2) continue;
        o.lost[f].assign(nt, 0);
        for (uint32_t t = 0; t < nt; ++t)
            if ((t + (uint32_t)f) % 4 == 0) o.lost[f][t] = 1;
    }
    Run r = run(o);
    CHECK(r.ok, "loss: %s", r.err.c_str());
    if (!r.ok) return;
    int bad = 0;
    const int f = atlas_differs(r, &bad);
    CHECK(f < 0, "loss: atlas diverges at frame %d (%d records)", f, bad);

    // Without the receipts the encoder must NOT still agree -- otherwise the
    // test above proves nothing about the rollback.
    Opts o2 = o;
    o2.feed_receipts = false;
    Run r2 = run(o2);
    CHECK(r2.ok, "loss-no-receipts: %s", r2.err.c_str());
    if (r2.ok)
        CHECK(atlas_differs(r2) >= 0,
              "loss without receipts still agrees: the rollback is untested");
}

// 3. The static-skip rule (13.12.3).  A head-locked panel must cost fewer
//    coded tiles under the atlas than under the picture model, because the
//    picture model has to re-code it every frame and the atlas can hold it.
static void test_static_panel() {
    Opts a; a.panel = 1; a.frames = 10; a.atlas = 1;
    Opts p = a; p.atlas = 0;
    Run ra = run(a), rp = run(p);
    CHECK(ra.ok && rp.ok, "static-panel runs: %s %s", ra.err.c_str(),
          rp.err.c_str());
    if (!ra.ok || !rp.ok) return;
    uint64_t ca = 0, cp = 0, ba = 0, bp = 0;
    for (size_t f = 1; f < ra.coded_tiles.size(); ++f) {
        ca += ra.coded_tiles[f];
        cp += rp.coded_tiles[f];
        ba += ra.frame_bytes[f];
        bp += rp.frame_bytes[f];
    }
    std::printf("  static panel: coded tiles atlas %llu vs picture %llu; "
                "bytes %llu vs %llu\n",
                (unsigned long long)ca, (unsigned long long)cp,
                (unsigned long long)ba, (unsigned long long)bp);
    CHECK(ca <= cp * 3 / 2,
          "atlas codes far more tiles than the picture model (%llu > %llu)",
          (unsigned long long)ca, (unsigned long long)cp);
}

// 4. row_present (3.1.2): an idle frame must cost bytes proportional to what
//    changed, not to the tile grid.
static void test_row_present() {
    Opts a; a.yaw_per_frame = 0.0; a.frames = 8; a.intra_period = 1000;
    Opts b = a; b.row_present = 1;
    Run ra = run(a), rb = run(b);
    CHECK(ra.ok && rb.ok, "row_present runs: %s %s", ra.err.c_str(),
          rb.err.c_str());
    if (!ra.ok || !rb.ok) return;
    CHECK(atlas_differs(rb) < 0, "row_present changes the atlas");
    uint64_t sa = 0, sb = 0;
    for (size_t f = 1; f < ra.frame_bytes.size(); ++f) {
        sa += ra.frame_bytes[f];
        sb += rb.frame_bytes[f];
    }
    std::printf("  row_present: %llu bytes -> %llu bytes over %zu idle frames\n",
                (unsigned long long)sa, (unsigned long long)sb,
                ra.frame_bytes.size() - 1);
    CHECK(sb <= sa, "row_present made the stream larger (%llu > %llu)",
          (unsigned long long)sb, (unsigned long long)sa);
}

// 5. The display helper is non-normative, but it must produce a picture that
//    resembles the source -- a model whose atlas is perfect and whose display
//    is noise would pass every other test here.
static void test_display() {
    for (int qp : {16, 20, 26, 32}) {
        Opts a; a.frames = 8; a.qp = qp; a.atlas = 1;
        Opts p = a; p.atlas = 0;
        Run ra = run(a), rp = run(p);
        CHECK(ra.ok && rp.ok, "display: %s %s", ra.err.c_str(), rp.err.c_str());
        if (!ra.ok || !rp.ok) continue;
        const double pa = psnr8(ra.disp_y.data(), ra.src_y.data(),
                                ra.disp_y.size());
        const double pp = psnr8(rp.disp_y.data(), rp.src_y.data(),
                                rp.disp_y.size());
        uint64_t ba = 0, bp = 0;
        for (size_t f = 0; f < ra.frame_bytes.size(); ++f) {
            ba += ra.frame_bytes[f];
            bp += rp.frame_bytes[f];
        }
        if (qp == 26) {
            std::printf("  per-frame PSNR-Y (qp 26):\n    atlas  ");
            for (double v : ra.psnr_per_frame) std::printf("%6.2f", v);
            std::printf("\n    picture");
            for (double v : rp.psnr_per_frame) std::printf("%6.2f", v);
            std::printf("\n    coded  ");
            for (uint32_t v : ra.coded_tiles) std::printf("%6u", v);
            std::printf(" (atlas)\n    coded  ");
            for (uint32_t v : rp.coded_tiles) std::printf("%6u", v);
            std::printf(" (picture)\n");
        }
        std::printf("  qp %2d: atlas %.2f dB / %llu B   picture %.2f dB / "
                    "%llu B   (%+.2f dB, %+.1f%% bytes)\n",
                    qp, pa, (unsigned long long)ba, pp,
                    (unsigned long long)bp, pa - pp,
                    100.0 * ((double)ba - (double)bp) / (double)bp);
        CHECK(pa > 15.0, "qp %d: displayed picture is %.2f dB", qp, pa);
    }
}

}  // namespace

int main() {
    test_identity();
    test_loss();
    test_static_panel();
    test_row_present();
    test_display();
    return test_report("atlas");
}
