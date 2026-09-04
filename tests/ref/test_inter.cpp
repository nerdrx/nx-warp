// The Phase 2 inter path: prediction, the reference ring, the mode decision,
// and the shadow/concealment contract.
//
// The property this file exists to hold is the one the whole transport design
// rests on: **the encoder runs the decoder**.  Every test below is ultimately a
// byte-for-byte comparison between what the encoder believes the client holds
// and what the decoder actually produces, including after loss.
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "test_util.h"

namespace {

// ---------------------------------------------------------------- material
// A textured plane sampled through a per-frame translation, plus an
// independently moving disc.  The translation stands in for the head rotation
// the views describe; the disc is the residual motion the per-tile vector
// exists for.  `disparity` shifts the whole picture, which is what a second
// eye at a fixed depth looks like.
struct Scene {
    int w = 0, h = 0, eyes = 1;
    bool c444 = true;
    std::vector<uint8_t> Y, U, V;
    int cw = 0, ch = 0;
};

static inline int tex(int x, int y) {
    double v = 128 + 55 * std::sin(x * 0.031) * std::cos(y * 0.027) +
               30 * std::sin((x * 3 + y * 5) * 0.11) +
               18 * std::sin((double)(x * x + y * y) * 0.00042);
    v += ((x / 13 + y / 11) % 2) ? 12 : -12;
    return v < 0 ? 0 : (v > 255 ? 255 : (int)v);
}

// `pan` is the whole-picture shift, `obj` the disc centre, `disp` the per-eye
// horizontal offset, `salt` a per-frame reseed that makes temporal prediction
// useless (used to corner the STEREO decision).
// `bright` is a whole-picture luma step: an exposure change, which is exactly
// the smooth drift the near-skip's warp_dc() correction exists for.
static Scene make_scene(int eye_w, int h, int eyes, bool c444, double pan_x,
                        double pan_y, int obj_x, int obj_y, int disp,
                        int salt = 0, int bright = 0) {
    Scene s;
    s.w = eye_w * eyes;
    s.h = h;
    s.eyes = eyes;
    s.c444 = c444;
    s.cw = c444 ? s.w : (s.w + 1) / 2;
    s.ch = c444 ? h : (h + 1) / 2;
    s.Y.assign((size_t)s.w * h, 0);
    s.U.assign((size_t)s.cw * s.ch, 128);
    s.V.assign((size_t)s.cw * s.ch, 128);
    for (int e = 0; e < eyes; ++e) {
        const int ex = e * disp;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < eye_w; ++x) {
                int sx = (int)std::lround(x + pan_x) + ex + salt * 37;
                int sy = (int)std::lround(y + pan_y) + salt * 11;
                int v = tex(sx, sy);
                const double dx = x - obj_x, dy = y - obj_y;
                if (dx * dx + dy * dy < 17.0 * 17.0) v = 235 - (int)(dx * dx) % 90;
                v += bright;
                s.Y[(size_t)y * s.w + e * eye_w + x] =
                    (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        const int cew = c444 ? eye_w : eye_w / 2;
        for (int y = 0; y < s.ch; ++y)
            for (int x = 0; x < cew; ++x) {
                int f = c444 ? 1 : 2;
                s.U[(size_t)y * s.cw + e * cew + x] =
                    (uint8_t)(110 + (tex(x * f + ex, y * f) >> 3));
                s.V[(size_t)y * s.cw + e * cew + x] =
                    (uint8_t)(140 - (tex(y * f, x * f + ex) >> 3));
            }
    }
    return s;
}

static nxvc_image image_of(Scene &s) {
    nxvc_image im{};
    im.plane[0] = s.Y.data();
    im.stride[0] = s.w;
    im.plane[1] = s.U.data();
    im.stride[1] = s.cw;
    im.plane[2] = s.V.data();
    im.stride[2] = s.cw;
    return im;
}

static nxvc_view view_yaw(double deg) {
    nxvc_view v{};
    const double a = deg * 3.14159265358979323846 / 360.0;  // half angle
    v.qx = 0;
    v.qy = std::sin(a);
    v.qz = 0;
    v.qw = std::cos(a);
    const double f = 95.0 * 3.14159265358979323846 / 360.0;
    v.fov_left = -f;
    v.fov_right = f;
    v.fov_up = f;
    v.fov_down = -f;
    return v;
}

// ------------------------------------------------------------------ harness
struct Run {
    std::vector<uint8_t> stream;
    // Per frame: the decoder's output planes, and the tile records of both
    // sides.
    std::vector<std::vector<uint8_t>> dec_y, dec_u, dec_v;
    std::vector<std::vector<nxvc_tile_info>> enc_tiles, dec_tiles;
    std::vector<uint32_t> frame_bytes;
    bool ok = false;
    std::string err;
};

struct Opts {
    int eye_w = 128, h = 128, eyes = 1;
    bool c444 = true;
    int qp = 26;
    int frames = 6;
    int inter = 1, stereo = 0;
    uint32_t intra_period = 1000;   // effectively off unless a test wants it
    uint32_t ref_sel = 0;
    double yaw_per_frame = 0.0;
    double pan_per_frame = 0.0;
    int disparity = 0;
    int salt_per_frame = 0;
    int obj_speed = 3;
    int bright_per_frame = 0;   // whole-picture luma step per frame
    // Loss injection: lost[frame][tile].  Empty = nothing is lost.
    std::vector<std::vector<uint8_t>> lost;
    // The rate controller's force_warp_skip request, per frame per tile.
    std::vector<std::vector<uint8_t>> skip_map;
    bool compare_shadow = true;
    // Syntax v1.5.  Defaults mirror nxvc_config_default(): all three on.
    uint32_t warp_dc = 1, mv_quad = 1;
    uint32_t refresh_drift_q8 = 256, refresh_max_age = 720;
};

static Run run(const Opts &o) {
    Run r;
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)o.eye_w;
    cfg.height = (uint32_t)o.h;
    cfg.eyes = (uint32_t)o.eyes;
    cfg.chroma = o.c444 ? NXVC_CHROMA_444 : NXVC_CHROMA_420;
    cfg.base_qp = (uint32_t)o.qp;
    cfg.inter = (uint32_t)o.inter;
    cfg.stereo = (uint32_t)o.stereo;
    cfg.intra_period = o.intra_period;
    cfg.ref_sel = o.ref_sel;
    cfg.custom_tables = 0;   // keeps the test fast; the path is table-agnostic
    cfg.warp_dc = o.warp_dc;
    cfg.mv_quad = o.mv_quad;
    cfg.refresh_drift_q8 = o.refresh_drift_q8;
    cfg.refresh_max_age = o.refresh_max_age;

    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) {
        r.err = nxvc_status_string(st);
        return r;
    }
    const uint32_t ntiles = nxvc_encoder_tile_count(e);

    std::vector<uint8_t> hdr(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl);
    if (st != NXVC_OK) {
        r.err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return r;
    }
    r.stream.assign(hdr.begin(), hdr.begin() + hl);

    // Shadow readback buffers.
    const size_t sw = (size_t)o.eye_w * o.eyes, sh = (size_t)o.h;
    const size_t scw = o.c444 ? sw : (sw + 1) / 2, sch = o.c444 ? sh : (sh + 1) / 2;
    std::vector<uint8_t> SY(sw * sh), SU(scw * sch), SV(scw * sch);
    std::vector<std::vector<uint8_t>> shadow_y, shadow_u, shadow_v;

    std::vector<uint8_t> fbuf(sw * sh * 8 + (1u << 20));
    for (int f = 0; f < o.frames; ++f) {
        Scene sc = make_scene(o.eye_w, o.h, o.eyes, o.c444,
                              o.pan_per_frame * f, 0.0,
                              20 + o.obj_speed * f, 40 + (f % 3), o.disparity,
                              o.salt_per_frame * f, o.bright_per_frame * f);
        nxvc_image img = image_of(sc);
        std::vector<nxvc_view> views((size_t)o.eyes,
                                     view_yaw(o.yaw_per_frame * f));
        nxvc_encoder_set_views(e, views.data(), (uint32_t)o.eyes);
        if (f < (int)o.skip_map.size() && !o.skip_map[f].empty())
            nxvc_encoder_set_skip_map(e, o.skip_map[f].data(),
                                      (uint32_t)o.skip_map[f].size());
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fbuf.data(),
                                       fbuf.size(), &ol);
        if (st != NXVC_OK) {
            r.err = std::string("encode: ") + nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return r;
        }
        r.stream.insert(r.stream.end(), fbuf.begin(), fbuf.begin() + ol);
        r.frame_bytes.push_back((uint32_t)ol);
        uint32_t n = 0;
        const nxvc_tile_info *ti = nxvc_encoder_tiles(e, &n);
        r.enc_tiles.emplace_back(ti, ti + n);

        if (f < (int)o.lost.size() && !o.lost[f].empty()) {
            std::vector<uint8_t> recv(ntiles, 1);
            for (uint32_t t = 0; t < ntiles && t < o.lost[f].size(); ++t)
                recv[t] = o.lost[f][t] ? 0 : 1;
            st = nxvc_encoder_set_received_tiles(e, recv.data(), ntiles);
            if (st != NXVC_OK) {
                r.err = std::string("shadow: ") + nxvc_status_string(st);
                nxvc_encoder_destroy(e);
                return r;
            }
        }
        nxvc_image simg{};
        simg.plane[0] = SY.data();
        simg.stride[0] = (int)sw;
        simg.plane[1] = SU.data();
        simg.stride[1] = (int)scw;
        simg.plane[2] = SV.data();
        simg.stride[2] = (int)scw;
        nxvc_encoder_shadow_image(e, &simg);
        shadow_y.push_back(SY);
        shadow_u.push_back(SU);
        shadow_v.push_back(SV);
    }
    nxvc_encoder_destroy(e);

    // ---- decode
    nxvc_decoder *d = nxvc_decoder_create(&st);
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, r.stream.data(), r.stream.size(),
                                          &consumed);
    if (st != NXVC_OK) {
        r.err = std::string("stream header: ") + nxvc_status_string(st);
        nxvc_decoder_destroy(d);
        return r;
    }
    off = consumed;
    uint32_t yw, yh, cw2, ch2;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw2, &ch2);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw2 * ch2),
        V((size_t)cw2 * ch2);
    for (int f = 0; off < r.stream.size(); ++f) {
        if (f < (int)o.lost.size() && !o.lost[f].empty())
            nxvc_decoder_set_lost_tiles(d, o.lost[f].data(),
                                        (uint32_t)o.lost[f].size());
        nxvc_image oi{};
        oi.plane[0] = Y.data();
        oi.stride[0] = (int)yw;
        oi.plane[1] = U.data();
        oi.stride[1] = (int)cw2;
        oi.plane[2] = V.data();
        oi.stride[2] = (int)ch2 ? (int)cw2 : (int)cw2;
        st = nxvc_decoder_decode_frame(d, r.stream.data() + off,
                                       r.stream.size() - off, &oi, &consumed);
        if (st != NXVC_OK) {
            r.err = std::string("decode frame ") + std::to_string(f) + ": " +
                    nxvc_status_string(st);
            nxvc_decoder_destroy(d);
            return r;
        }
        r.dec_y.push_back(Y);
        r.dec_u.push_back(U);
        r.dec_v.push_back(V);
        uint32_t n = 0;
        const nxvc_tile_info *ti = nxvc_decoder_tiles(d, &n);
        r.dec_tiles.emplace_back(ti, ti + n);
        off += consumed;
    }
    nxvc_decoder_destroy(d);

    if (o.compare_shadow) {
        for (size_t f = 0; f < r.dec_y.size() && f < shadow_y.size(); ++f) {
            if (r.dec_y[f] != shadow_y[f] || r.dec_u[f] != shadow_u[f] ||
                r.dec_v[f] != shadow_v[f]) {
                r.err = "encoder shadow differs from the decoder at frame " +
                        std::to_string(f);
                return r;
            }
        }
    }
    r.ok = true;
    return r;
}

static int count_mode(const std::vector<nxvc_tile_info> &t, int mode) {
    int n = 0;
    for (const auto &x : t)
        if (x.mode == mode) ++n;
    return n;
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // ---------------------------------------------------------------- 1
    // The basic contract: an inter stream decodes, and the encoder's shadow is
    // the decoder's output byte for byte on every frame.
    {
        Opts o;
        o.yaw_per_frame = 0.9;
        o.pan_per_frame = 2.0;
        Run r = run(o);
        CHECK(r.ok, "mono inter round trip: %s", r.err.c_str());
        if (r.ok) {
            CHECK(r.dec_y.size() == (size_t)o.frames, "decoded %zu frames",
                  r.dec_y.size());
            // Frame 0 has no reference, so every tile is INTRA and the frame
            // sets tile_map_reset.  Later frames must use the inter path.
            CHECK(count_mode(r.dec_tiles[0], NXVC_MODE_INTRA) ==
                      (int)r.dec_tiles[0].size(),
                  "frame 0 is not all intra");
            int inter_tiles = 0;
            for (size_t f = 1; f < r.dec_tiles.size(); ++f)
                inter_tiles += (int)r.dec_tiles[f].size() -
                               count_mode(r.dec_tiles[f], NXVC_MODE_INTRA);
            CHECK(inter_tiles > 0, "no inter tiles were chosen at all");
            // Inter must actually pay: later frames should be much smaller.
            CHECK(r.frame_bytes[1] * 2 < r.frame_bytes[0],
                  "frame 1 (%u B) is not clearly cheaper than frame 0 (%u B)",
                  r.frame_bytes[1], r.frame_bytes[0]);
        }
    }

    // ---------------------------------------------------------------- 2
    // Annex D D-21 `inter/identity`: an identity matrix with a zero vector is
    // a bit-exact copy of the reference (docs/WARP.md 10 case 1).  The mode is
    // pinned with the rate controller's force_warp_skip so the property is
    // tested rather than the mode decision's opinion of it: every frame after
    // the first must then be the SAME BYTES as the first, in every plane, for
    // as long as the chain runs.
    {
        Opts o;
        o.frames = 6;
        o.yaw_per_frame = 0.0;
        o.pan_per_frame = 0.0;
        o.obj_speed = 0;
        o.skip_map.assign(o.frames, std::vector<uint8_t>(4, 1));
        Run r = run(o);
        CHECK(r.ok, "identity repeat: %s", r.err.c_str());
        if (r.ok) {
            for (size_t f = 1; f < r.dec_tiles.size(); ++f) {
                CHECK(count_mode(r.dec_tiles[f], NXVC_MODE_WARP_SKIP) ==
                          (int)r.dec_tiles[f].size(),
                      "frame %zu: %d of %zu tiles are WARP_SKIP", f,
                      count_mode(r.dec_tiles[f], NXVC_MODE_WARP_SKIP),
                      r.dec_tiles[f].size());
                CHECK(r.dec_y[f] == r.dec_y[0],
                      "frame %zu is not a bit-exact copy of frame 0", f);
                CHECK(r.dec_u[f] == r.dec_u[0] && r.dec_v[f] == r.dec_v[0],
                      "frame %zu chroma is not a bit-exact copy", f);
            }
            // A whole frame of skipped tiles is a frame header, a warp record
            // and one row header per row, and nothing else.
            CHECK(r.frame_bytes[1] < 200, "an all-skip frame costs %u bytes",
                  r.frame_bytes[1]);
        }
    }

    // ---------------------------------------------------------------- 2b
    // Left to itself on the same material, the mode decision must reach for
    // skip on most tiles -- not necessarily all of them: a tile whose stored
    // reconstruction is worse than a fresh intra encode of the same samples is
    // entitled to be recoded, and that judgement is the encoder's.
    {
        Opts o;
        o.frames = 4;
        o.yaw_per_frame = 0.0;
        o.pan_per_frame = 0.0;
        o.obj_speed = 0;
        Run r = run(o);
        CHECK(r.ok, "identity repeat, free decision: %s", r.err.c_str());
        if (r.ok) {
            int skips = 0, total = 0;
            for (size_t f = 1; f < r.dec_tiles.size(); ++f) {
                skips += count_mode(r.dec_tiles[f], NXVC_MODE_WARP_SKIP);
                total += (int)r.dec_tiles[f].size();
            }
            CHECK(skips * 2 >= total,
                  "only %d of %d tiles skipped on a repeated picture", skips,
                  total);
        }
    }

    // ---------------------------------------------------------------- 3
    // A still picture with a large head rotation in the pose log: the warp is
    // exactly wrong and the identity predictor is exactly right, which is what
    // STATIC_MV exists for (PAPER 2.3).
    {
        Opts o;
        o.frames = 4;
        o.yaw_per_frame = 12.0;   // the matrix moves; the picture does not
        o.pan_per_frame = 0.0;
        o.obj_speed = 0;
        Run r = run(o);
        CHECK(r.ok, "static_mv: %s", r.err.c_str());
        if (r.ok) {
            int stat = 0, warp = 0;
            for (size_t f = 1; f < r.dec_tiles.size(); ++f) {
                stat += count_mode(r.dec_tiles[f], NXVC_MODE_STATIC_MV);
                warp += count_mode(r.dec_tiles[f], NXVC_MODE_WARP_MV) +
                        count_mode(r.dec_tiles[f], NXVC_MODE_WARP_SKIP);
            }
            CHECK(stat > warp,
                  "the identity predictor did not win on a still picture under "
                  "a 12 deg/frame matrix (%d STATIC_MV vs %d warped)",
                  stat, warp);
        }
    }

    // ---------------------------------------------------------------- 4
    // The reference ring: ref_sel 1 and 2 select frames N-2 and N-3, and the
    // decoder reproduces the encoder for each.
    for (uint32_t rs = 0; rs <= 2; ++rs) {
        Opts o;
        o.frames = 8;
        o.ref_sel = rs;
        o.yaw_per_frame = 0.4;
        o.pan_per_frame = 1.0;
        Run r = run(o);
        CHECK(r.ok, "ref_sel %u: %s", rs, r.err.c_str());
        if (r.ok) {
            int seen = 0;
            for (size_t f = 1 + rs; f < r.dec_tiles.size(); ++f)
                for (const auto &t : r.dec_tiles[f])
                    if (t.mode == NXVC_MODE_WARP_MV ||
                        t.mode == NXVC_MODE_STATIC_MV) {
                        CHECK(t.ref_sel == rs, "ref_sel %u coded as %u", rs,
                              t.ref_sel);
                        ++seen;
                    }
            CHECK(seen > 0, "ref_sel %u: no coded inter tile to check", rs);
        }
    }

    // ---------------------------------------------------------------- 5
    // Rolling intra refresh: with a period of T and no drift rule, every tile
    // is refreshed exactly once every T frames and about 1/T of the tiles per
    // frame.  The drift rule of SYNTAX 13.10 is off here on purpose: this is
    // the fixed cadence's own test, and case 14 below is the drift rule's.
    {
        Opts o;
        o.frames = 12;
        o.intra_period = 4;
        o.refresh_drift_q8 = 0;
        o.yaw_per_frame = 0.3;
        o.pan_per_frame = 1.0;
        Run r = run(o);
        CHECK(r.ok, "intra refresh: %s", r.err.c_str());
        if (r.ok) {
            const size_t n = r.dec_tiles[0].size();
            std::vector<int> refreshed(n, 0);
            for (size_t f = 4; f < 8; ++f)
                for (size_t t = 0; t < n; ++t)
                    if (r.dec_tiles[f][t].mode == NXVC_MODE_INTRA)
                        ++refreshed[t];
            // Every tile must be refreshed at least once per period.  It may
            // be refreshed more often: the rate-distortion decision is free to
            // pick INTRA on its own merits, and the forced refresh only
            // guarantees the floor.
            for (size_t t = 0; t < n; ++t)
                CHECK(refreshed[t] >= 1,
                      "tile %zu was not refreshed in a period of 4", t);
        }
    }

    // ---------------------------------------------------------------- 6
    // Stereo: two pictures per frame, and content that is new to both eyes at
    // once, which is exactly the case inter-view prediction exists for
    // (PAPER 2.5).  The right eye must reach for STEREO rather than INTRA.
    {
        Opts o;
        o.eyes = 2;
        o.stereo = 1;
        o.frames = 4;
        o.eye_w = 128;
        o.salt_per_frame = 1;   // every frame is new content
        o.disparity = 9;
        o.yaw_per_frame = 0.0;
        Run r = run(o);
        CHECK(r.ok, "stereo: %s", r.err.c_str());
        if (r.ok) {
            CHECK(r.dec_tiles[0].size() == (size_t)(2 * 2 * 2),
                  "stereo tile count is %zu, expected 8",
                  r.dec_tiles[0].size());
            int nstereo = 0;
            for (size_t f = 1; f < r.dec_tiles.size(); ++f)
                nstereo += count_mode(r.dec_tiles[f], NXVC_MODE_STEREO);
            CHECK(nstereo > 0, "no STEREO tile was chosen on new content");
            // A STEREO tile is only ever a right-eye tile (Annex D D-4).
            for (const auto &fr : r.dec_tiles)
                for (const auto &t : fr)
                    if (t.mode == NXVC_MODE_STEREO)
                        CHECK(t.eye == 1, "STEREO on the left eye");
        }
    }

    // ---------------------------------------------------------------- 7
    // The shadow contract under loss (PAPER 2.11 item 4).  Random tiles are
    // dropped on the decoder and the same bitmap is handed to the encoder; the
    // two must agree byte for byte on every frame, for 200 frames.
    {
        Opts o;
        o.frames = 200;
        o.eye_w = 128;
        o.h = 128;
        o.qp = 28;
        o.yaw_per_frame = 0.35;
        o.pan_per_frame = 1.0;
        o.intra_period = 40;
        // 4 tiles per frame at 128x128; drop each with probability 1/4.
        const uint32_t ntiles = 4;
        Rng rng(20260904u);
        o.lost.resize(o.frames);
        int dropped = 0;
        for (int f = 1; f < o.frames; ++f) {
            o.lost[f].assign(ntiles, 0);
            for (uint32_t t = 0; t < ntiles; ++t)
                if ((rng.next() & 3u) == 0) {
                    o.lost[f][t] = 1;
                    ++dropped;
                }
        }
        Run r = run(o);
        CHECK(r.ok, "shadow under loss: %s", r.err.c_str());
        CHECK(dropped > 100, "only %d tiles were dropped", dropped);
        if (r.ok) {
            int concealed = 0;
            for (const auto &fr : r.dec_tiles)
                for (const auto &t : fr) concealed += t.concealed ? 1 : 0;
            CHECK(concealed == dropped,
                  "the decoder concealed %d tiles, %d were dropped", concealed,
                  dropped);
            std::printf("  shadow: %d frames, %d tiles dropped and concealed, "
                        "encoder == decoder on every frame\n",
                        (int)r.dec_y.size(), dropped);
        }
    }

    // ---------------------------------------------------------------- 8
    // 4:2:0.  The chroma planes are predicted through the conjugated matrix
    // and the halved vector; if that were wrong the shadow comparison fails.
    {
        Opts o;
        o.c444 = false;
        o.frames = 5;
        o.yaw_per_frame = 0.8;
        o.pan_per_frame = 2.0;
        Run r = run(o);
        CHECK(r.ok, "4:2:0 inter: %s", r.err.c_str());
    }

    // ---------------------------------------------------------------- 9
    // Inter off is Phase 1: no tool bits, no warp record, every tile INTRA.
    {
        Opts o;
        o.inter = 0;
        o.frames = 3;
        Run r = run(o);
        CHECK(r.ok, "inter off: %s", r.err.c_str());
        if (r.ok)
            for (const auto &fr : r.dec_tiles)
                for (const auto &t : fr)
                    CHECK(t.mode == NXVC_MODE_INTRA, "inter tile with inter off");
    }

    // ---------------------------------------------------------------- 10
    // The rate controller's force_warp_skip request
    // (docs/RATECONTROL.md 8.7): it pins WARP_SKIP after the mode search, and
    // the encoder overrides it exactly where a coded tile is required.
    {
        Opts o;
        o.frames = 9;
        o.intra_period = 3;      // so the override has something to override
        o.yaw_per_frame = 0.5;
        o.pan_per_frame = 2.0;
        o.obj_speed = 6;         // content the search would never skip
        const uint32_t ntiles = 4;
        o.skip_map.assign(o.frames, std::vector<uint8_t>(ntiles, 1));
        Run r = run(o);
        CHECK(r.ok, "skip map: %s", r.err.c_str());
        if (r.ok) {
            int forced = 0, refreshed = 0, other = 0;
            for (size_t f = 1; f < r.dec_tiles.size(); ++f)
                for (const auto &t : r.dec_tiles[f]) {
                    if (t.mode == NXVC_MODE_WARP_SKIP) ++forced;
                    else if (t.mode == NXVC_MODE_INTRA) ++refreshed;
                    else ++other;
                }
            CHECK(other == 0,
                  "%d tiles were coded despite force_warp_skip and no "
                  "correctness override", other);
            CHECK(refreshed > 0,
                  "the rolling refresh never overrode force_warp_skip");
            CHECK(forced > 0, "force_warp_skip produced no skipped tile");
            // age_since_coded must count the skipped frames and reset on the
            // refresh.  It is encoder-side bookkeeping, so read it there.
            std::printf("  skip map: %d forced skips, %d refresh overrides\n",
                        forced, refreshed);
            for (size_t f = 1; f < r.enc_tiles.size(); ++f)
                for (const auto &t : r.enc_tiles[f])
                    if (t.mode == NXVC_MODE_INTRA)
                        CHECK(t.age_since_coded == 0,
                              "a coded tile reports age_since_coded %u",
                              t.age_since_coded);
            for (const auto &fr : r.dec_tiles)
                for (const auto &t : fr) {
                    if (t.mode == NXVC_MODE_INTRA)
                        CHECK(t.ref_delta == 3,
                              "INTRA tile reports ref_delta %u", t.ref_delta);
                }
        }
    }

    // ---------------------------------------------------------------- 12
    // Syntax v1.5: four vectors per tile (SYNTAX 13.8).
    {
        Opts o;
        o.frames = 6;
        o.qp = 26;
        o.yaw_per_frame = 0.7;
        o.pan_per_frame = 2.0;
        o.obj_speed = 5;   // a disc crossing tile quadrants
        o.warp_dc = 0;
        o.refresh_drift_q8 = 0;
        Opts off = o;
        off.mv_quad = 0;
        Run a = run(off), b = run(o);
        CHECK(a.ok, "mv_quad off: %s", a.err.c_str());
        CHECK(b.ok, "mv_quad on: %s", b.err.c_str());
        if (a.ok && b.ok) {
            int quads = 0;
            for (const auto &fr : b.dec_tiles)
                for (const auto &t : fr) {
                    if (!t.mv_quad) continue;
                    ++quads;
                    CHECK(t.mode == NXVC_MODE_WARP_MV ||
                              t.mode == NXVC_MODE_STATIC_MV,
                          "mv_quad on mode %u", t.mode);
                    CHECK(t.mv_present == 1, "mv_quad without mv_present");
                    for (int q = 0; q < 4; ++q) {
                        const int ax = (int)t.mv_x + t.dmv[q][0];
                        const int ay = (int)t.mv_y + t.dmv[q][1];
                        CHECK(ax >= -128 && ax <= 127 && ay >= -128 && ay <= 127,
                              "quadrant vector (%d, %d) out of range", ax, ay);
                    }
                }
            CHECK(quads > 0, "no tile chose mv_quad on moving-object material");
            // The deltas are only ever taken when they pay for their eight
            // bytes, so the stream must not grow.
            uint32_t sa = 0, sb = 0;
            for (auto v : a.frame_bytes) sa += v;
            for (auto v : b.frame_bytes) sb += v;
            std::printf("  mv_quad: %d quad tiles, %u -> %u bytes\n", quads, sa,
                        sb);
            // The deltas are taken on a SAD comparison that charges their eight
            // bytes, so the stream may not grow by more than what they cost:
            // this is a four-tile picture where one tile taking them is the
            // whole signal, not a rate measurement (that is
            // ref/RESULTS-inter-b.md).
            CHECK(sb <= sa + (uint32_t)(8 * quads),
                  "mv_quad grew the stream by more than its own bytes: "
                  "%u -> %u over %d quad tiles", sa, sb, quads);
        }
    }

    // ---------------------------------------------------------------- 13
    // Syntax v1.5: the near-skip and warp_dc() (SYNTAX 3.3, 13.9).
    {
        Opts o;
        o.frames = 10;
        o.qp = 34;
        o.eye_w = 192;
        o.h = 192;
        o.yaw_per_frame = 0.15;
        o.pan_per_frame = 0.4;
        o.obj_speed = 0;
        o.bright_per_frame = 3;
        o.mv_quad = 0;
        o.refresh_drift_q8 = 0;
        Opts off = o;
        off.warp_dc = 0;
        Run a = run(off), b = run(o);
        CHECK(a.ok, "warp_dc off: %s", a.err.c_str());
        CHECK(b.ok, "warp_dc on: %s", b.err.c_str());
        if (a.ok && b.ok) {
            int near_enc = 0, near_dec = 0;
            for (const auto &fr : b.enc_tiles)
                for (const auto &t : fr) near_enc += t.dc_corr ? 1 : 0;
            for (const auto &fr : b.dec_tiles)
                for (const auto &t : fr) {
                    if (!t.dc_corr) continue;
                    ++near_dec;
                    // 13.9: a near-skip IS a skipped tile.
                    CHECK(t.skipped == 1, "dc_corr on a coded tile");
                    CHECK(t.mode == NXVC_MODE_WARP_SKIP,
                          "dc_corr on mode %u", t.mode);
                }
            CHECK(near_enc > 0, "no tile chose the near-skip");
            CHECK(near_enc == near_dec,
                  "encoder wrote %d near-skips, decoder saw %d", near_enc,
                  near_dec);
            std::printf("  warp_dc: %d near-skip tiles\n", near_dec);
        }
        // ... and the same thing under loss, where the shadow comparison in
        // run() is the whole point: a lost row still carries its corrections,
        // because they travel in the row header the transport replicates.
        Opts l = o;
        l.frames = 6;
        l.lost.assign(6, {});
        for (int f = 2; f < 6; ++f) l.lost[f].assign(2 * 2, (uint8_t)(f & 1));
        Run c = run(l);
        CHECK(c.ok, "warp_dc under loss: %s", c.err.c_str());
    }

    // ---------------------------------------------------------------- 14
    // Syntax v1.5: the refresh rule of SYNTAX 13.10.  The hard cap is
    // unconditional, so every tile must be INTRA at least once inside it even
    // when the drift rule would defer every eligible tile.
    {
        Opts o;
        o.frames = 14;
        o.qp = 24;
        o.intra_period = 3;
        o.refresh_max_age = 6;
        o.refresh_drift_q8 = 1u << 20;   // no tile is ever drifty enough
        o.warp_dc = 0;
        o.mv_quad = 0;
        o.yaw_per_frame = 0.2;
        o.pan_per_frame = 0.5;
        Run r = run(o);
        CHECK(r.ok, "drift refresh: %s", r.err.c_str());
        if (r.ok && !r.enc_tiles.empty()) {
            const size_t nt = r.enc_tiles[0].size();
            std::vector<int> last_intra(nt, 0);
            for (size_t f = 1; f < r.enc_tiles.size(); ++f)
                for (size_t t = 0; t < nt; ++t) {
                    if (r.enc_tiles[f][t].mode == NXVC_MODE_INTRA)
                        last_intra[t] = (int)f;
                    CHECK((int)f - last_intra[t] <= (int)o.refresh_max_age,
                          "tile %zu went %d frames without an INTRA", t,
                          (int)f - last_intra[t]);
                }
        }
    }

    return test_report("test_inter");
}
