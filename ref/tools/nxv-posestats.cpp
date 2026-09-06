// Measure the DOMINANT-POSE structure of the atlas, for
// docs/COMPOSITOR-POSE-DISPLAY.md.
//
// An atlas entry's pixels were captured at the pose of the frame that last
// coded it, and [SYN] 13.12.3 step 1 advances EVERY valid entry by the same
// per-frame homography.  So two entries that share a `src_frame` have had the
// same sequence of matrices composed into them and therefore hold the same
// `C`: grouping the table by `src_frame` IS grouping it by pose, exactly, with
// no floating-point comparison anywhere.
//
// What this prints, per frame and then summarised: how many valid entries
// share the most common `src_frame` (the DOMINANT pose), and the three
// warped-tile counts the proposal is judged on.
//
// Display-only.  Nothing here reads or writes the normative atlas; it reads
// the table the decoder already publishes.
//
// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"

namespace {

struct Img {
    int w = 0, h = 0, cw = 0, ch = 0;
    std::vector<uint8_t> p[4];
};

// A FIXED scene, sampled through a horizontal shift that matches the frame's
// yaw.  This is the part that has to be right for any of these numbers to mean
// anything: content that changes every frame is unskippable and unwarpable, so
// the encoder codes every tile and the atlas degenerates to "recode
// everything" -- which is what the first run of this tool measured, and it
// said more about the generator than about the codec.  A pose-consistent pan
// is what lets WARP_SKIP actually predict, and therefore what lets an atlas
// with several poses in it exist at all.
Img make_image(int w, int h, double shift_px) {
    Img im;
    im.w = w; im.h = h; im.cw = w / 2; im.ch = h / 2;
    im.p[0].assign((size_t)w * h, 0);
    im.p[1].assign((size_t)im.cw * im.ch, 0);
    im.p[2].assign((size_t)im.cw * im.ch, 0);
    im.p[3].assign((size_t)w * h, 255);
    // The scene is a function of WORLD coordinates; the frame samples it at
    // an offset.  Several spatial frequencies, so a tile has real detail to
    // fail to predict rather than a single sinusoid a warp reproduces exactly.
    auto scene = [](double X, double Y) {
        const double a = std::sin(X * 0.041) * std::cos(Y * 0.033);
        const double b = std::sin((X + Y) * 0.011);
        const double c = std::sin(X * 0.17) * 0.35;
        return 128.0 + 70.0 * a + 30.0 * b + 25.0 * c;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int v = (int)scene((double)x + shift_px, (double)y);
            im.p[0][(size_t)y * w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    for (int y = 0; y < im.ch; ++y)
        for (int x = 0; x < im.cw; ++x) {
            const double X = 2.0 * x + shift_px, Y = 2.0 * y;
            const int u = (int)(128.0 + 40.0 * std::sin(X * 0.02));
            const int vv = (int)(128.0 + 40.0 * std::cos(Y * 0.025));
            im.p[1][(size_t)y * im.cw + x] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
            im.p[2][(size_t)y * im.cw + x] = (uint8_t)(vv < 0 ? 0 : vv > 255 ? 255 : vv);
        }
    return im;
}

struct FrameRow {
    int frame = 0;
    int picture = 0;      // was this a PICTURE frame
    int valid = 0;        // valid entries
    int dominant = 0;     // entries at the dominant pose
    int poses = 0;        // distinct poses present
    int coded = 0;        // entries this frame coded
};

bool run(const char *name, double yaw_per_frame, int frames, int W, int H,
         unsigned D, std::vector<FrameRow> &rows, std::string &err) {
    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = (uint32_t)W;
    cfg.height = (uint32_t)H;
    cfg.chroma = NXVC_CHROMA_420;
    cfg.base_qp = 28;
    cfg.inter = 1;
    cfg.atlas = 1;
    cfg.atlas_picture_disp = D;       // the 13.12.11.1 mode trigger
    cfg.atlas_picture_period = 0;
    cfg.atlas_picture_min_spacing = 0;
    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) { err = nxvc_status_string(st); return false; }
    std::vector<uint8_t> hdr(4096), stream;
    size_t hl = 0;
    if ((st = nxvc_encoder_stream_header(e, hdr.data(), hdr.size(), &hl)) !=
        NXVC_OK) {
        err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return false;
    }
    stream.assign(hdr.begin(), hdr.begin() + hl);
    std::vector<uint8_t> fbuf((size_t)W * H * 8 + (1u << 20));
    for (int f = 0; f < frames; ++f) {
        const double a = yaw_per_frame * f;
        nxvc_view v{};
        v.qy = std::sin(a * 0.5);
        v.qw = std::cos(a * 0.5);
        v.fov_left = -0.9; v.fov_right = 0.9;
        v.fov_up = 0.9; v.fov_down = -0.9;
        nxvc_encoder_set_views(e, &v, 1);
        // The pan that goes with the yaw: the horizontal FOV is 1.8 rad, so a
        // yaw of `a` radians moves the picture `a / 1.8 * W` samples.
        Img im = make_image(W, H, a / 1.8 * (double)W);
        nxvc_image img{};
        for (int p = 0; p < 4; ++p) img.plane[p] = im.p[p].data();
        img.stride[0] = im.w;
        img.stride[1] = im.cw;
        img.stride[2] = im.cw;
        img.stride[3] = im.w;
        size_t ol = 0;
        if ((st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr,
                                            fbuf.data(), fbuf.size(), &ol)) !=
            NXVC_OK) {
            err = nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return false;
        }
        stream.insert(stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);

    nxvc_decoder *d = nxvc_decoder_create(&st);
    if (!d) { err = "decoder_create"; return false; }
    size_t consumed = 0;
    if ((st = nxvc_decoder_parse_stream_header(d, stream.data(), stream.size(),
                                               &consumed)) != NXVC_OK) {
        err = nxvc_status_string(st);
        nxvc_decoder_destroy(d);
        return false;
    }
    uint32_t yw, yh, cw, ch;
    nxvc_decoder_plane_size(d, 0, &yw, &yh);
    nxvc_decoder_plane_size(d, 1, &cw, &ch);
    std::vector<uint8_t> Y((size_t)yw * yh), U((size_t)cw * ch),
        V((size_t)cw * ch);
    size_t off = consumed;
    int nf = 0;
    while (off < stream.size()) {
        nxvc_image oi{};
        oi.plane[0] = Y.data(); oi.stride[0] = (int)yw;
        oi.plane[1] = U.data(); oi.stride[1] = (int)cw;
        oi.plane[2] = V.data(); oi.stride[2] = (int)cw;
        size_t used = 0;
        if ((st = nxvc_decoder_decode_frame(d, stream.data() + off,
                                            stream.size() - off, &oi, &used)) !=
            NXVC_OK) {
            err = nxvc_status_string(st);
            nxvc_decoder_destroy(d);
            return false;
        }
        off += used;
        const size_t tb = nxvc_decoder_atlas_table_size(d);
        std::vector<uint8_t> tab(tb);
        nxvc_decoder_atlas_table(d, tab.data(), tb);
        FrameRow r;
        r.frame = nf;
        nxvc_frame_info fi{};
        if (nxvc_decoder_frame_info(d, &fi) == NXVC_OK)
            r.picture = (int)((fi.flags >> 5) & 1u);
        std::map<uint32_t, int> hist;
        const size_t entries = tb / 64;
        for (size_t i = 0; i < entries; ++i) {
            const uint8_t *p = tab.data() + i * 64;
            const uint8_t flags = p[42];
            if (!(flags & 1u)) continue;   // not valid
            uint32_t sf = 0;
            std::memcpy(&sf, p + 36, 4);
            ++hist[sf];
            ++r.valid;
            if (sf == (uint32_t)nf) ++r.coded;
        }
        for (auto &kv : hist)
            if (kv.second > r.dominant) r.dominant = kv.second;
        r.poses = (int)hist.size();
        rows.push_back(r);
        ++nf;
    }
    nxvc_decoder_destroy(d);
    (void)name;
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    int W = 1088, H = 1088, frames = 60;
    unsigned D = 8;
    bool csv = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 2 < argc) { W = atoi(argv[++i]); H = atoi(argv[++i]); }
        else if (a == "--frames" && i + 1 < argc) frames = atoi(argv[++i]);
        else if (a == "--disp" && i + 1 < argc) D = (unsigned)atoi(argv[++i]);
        else if (a == "--csv") csv = true;
        else { std::fprintf(stderr, "usage: %s [--size W H] [--frames N] [--disp D] [--csv]\n", argv[0]); return 2; }
    }
    // Rest / mid / fast, as yaw per frame.  At 90 Hz these are 0, 45 and
    // 123 deg/s -- the last being the fastest rotation docs/PAPER.md measures.
    // Degrees of yaw per frame; at 90 Hz these are 0, 9, 22.5, 45 and
    // 123 deg/s, the last being the fastest rotation docs/PAPER.md measures.
    // The two slow arms exist because the interesting regime is neither end:
    // at rest nothing is coded and the atlas holds ONE pose, and in fast
    // motion the D=8 mode trigger makes every frame a PICTURE frame, which
    // also leaves one pose.  A mosaic of several poses only exists in between.
    const double kDeg = 3.14159265358979 / 180.0;
    struct Arm { const char *name; double yaw; } arms[5] = {
        {"rest", 0.0},        {"creep", 0.10 * kDeg},
        {"slow", 0.25 * kDeg}, {"mid", 0.50 * kDeg},
        {"fast", 1.37 * kDeg}};
    if (csv) std::printf("arm,deg_per_frame,frame,picture,valid,dominant,poses,coded\n");
    else
    std::printf("# compositor-pose display: dominant-pose share, D=%u, "
                "%dx%d, %d frames\n", D, W, H, frames);
    if (!csv)
    std::printf("%-6s %6s %8s %8s %8s %9s %9s %9s %9s\n", "arm", "frames",
                "PICTURE", "entries", "poses", "dom.share", "warp:new",
                "warp:today", "warp:PIC");
    for (const Arm &a : arms) {
        std::vector<FrameRow> rows;
        std::string err;
        if (!run(a.name, a.yaw, frames, W, H, D, rows, err)) {
            std::fprintf(stderr, "%s FAILED: %s\n", a.name, err.c_str());
            return 1;
        }
        if (csv) {
            for (const FrameRow &r : rows)
                std::printf("%s,%.4f,%d,%d,%d,%d,%d,%d\n", a.name,
                            a.yaw / kDeg, r.frame, r.picture, r.valid,
                            r.dominant, r.poses, r.coded);
            continue;
        }
        // Averages over the frames after the first, which is the intra one
        // and tells nothing about steady state.
        double share = 0, wnew = 0, wtoday = 0, wpic = 0, poses = 0;
        int n = 0, npic = 0;
        for (size_t i = 1; i < rows.size(); ++i) {
            const FrameRow &r = rows[i];
            npic += r.picture;
            if (!r.valid) continue;
            share += 100.0 * r.dominant / r.valid;
            // The proposal: only entries NOT at the dominant pose are warped.
            wnew += r.valid - r.dominant;
            // Today: every entry the frame did not code is warped to the
            // current pose by the display pass.
            wtoday += r.valid - r.coded;
            // A PICTURE frame assembles every valid entry.
            wpic += r.valid;
            poses += r.poses;
            ++n;
        }
        if (!n) n = 1;
        std::printf("%-6s %6d %8d %8d %8.1f %8.1f%% %9.1f %9.1f %9.1f\n",
                    a.name, (int)rows.size(), npic,
                    rows.empty() ? 0 : rows.back().valid, poses / n, share / n,
                    wnew / n, wtoday / n, wpic / n);
        // Per-frame detail for the first frames after a PICTURE frame, which
        // is where the decay the proposal lives or dies on shows up.
        std::printf("       per-frame (frame:dom%%/poses):");
        for (size_t i = 1; i < rows.size() && i < 13; ++i)
            std::printf(" %zu:%.0f%%/%d%s", i,
                        rows[i].valid ? 100.0 * rows[i].dominant / rows[i].valid
                                      : 0.0,
                        rows[i].poses, rows[i].picture ? "P" : "");
        std::printf("\n");
    }
    return 0;
}
