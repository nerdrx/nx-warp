// Pose-warp CONVENTION test: the first warped frame of a pure-rotation pair.
//
// This file exists because of docs/WARP-AUDIT.md.  Every quantity the warp
// depends on -- the quaternion component order, the handedness, the direction
// of the relative rotation, the sign of the FOV tangents, the image row
// direction, the coordinate origin, the frame pairing -- is a *convention*,
// and every one of them is silently wrong-able.  A convention error does not
// crash, does not produce an illegal bitstream, and does not fail any of the
// `warp.*` tests, because those all check the predictor against its own
// arithmetic rather than against a picture the world actually produced.  It
// shows up only as a picture that is worse than it should be, which is
// exactly what nothing in the suite was measuring.
//
// The measurement here is deliberately end-to-end and deliberately absolute:
//
//   1. Render two views of the SAME analytic world through the SAME projection
//      the harness generator uses (tools/quality/capture/synth.py, _ray_grid
//      and render_view), separated by a known pure rotation and nothing else.
//   2. Encode losslessly, force WARP_SKIP on every tile of the second frame,
//      decode.  The decoded second frame is then *nothing but* the predictor
//      applied to a bit-exact copy of the first.
//   3. Require its PSNR against the true second frame to clear a threshold.
//
// The world is a smooth analytic function of the world-space ray direction, so
// the ground truth is band-limited by construction and the ceiling is set by
// the predictor rather than by the material.  That is the whole point: on the
// generator's aliased panorama the *ideal float* warp only reaches 24 dB, so a
// test written on that material would pass with the rotation applied backwards
// -- which is precisely why the 24.40 dB of ref/RESULTS-inter.md sat unexamined
// for a phase.  Here the correct convention measures 50 to 58 dB.
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "nxvc/nxvc.h"
#include "test_util.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kEye = 256;
constexpr double kFovDeg = 95.0;

// ------------------------------------------------------------------- world
// A band-limited scalar field on the sphere: a short sum of low-frequency
// plane waves in the direction vector.  Smooth at every scale the eye can
// resolve, so a geometrically correct warp reproduces it to within the
// predictor's own coordinate and filter error and nothing else.
double world_sample(double x, double y, double z) {
    static const double n[6][3] = {
        {0.8020, 0.2673, 0.5345}, {-0.3244, 0.8111, 0.4867},
        {0.4558, -0.5698, 0.6838}, {-0.6547, -0.3273, 0.6813},
        {0.1690, 0.9296, -0.3273}, {0.5774, -0.5774, -0.5774},
    };
    static const double w[6] = {3.1, 4.7, 6.3, 8.1, 5.2, 2.4};
    static const double a[6] = {34.0, 22.0, 15.0, 9.0, 18.0, 26.0};
    static const double p[6] = {0.31, 1.72, 2.55, 0.94, 3.61, 1.18};
    double v = 128.0;
    for (int k = 0; k < 6; ++k)
        v += a[k] * std::sin(w[k] * (n[k][0] * x + n[k][1] * y + n[k][2] * z) +
                             p[k]);
    return v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
}

// Intrinsic Y-X-Z rotation, Y up, -Z forward -- synth.py rot_matrix().
void rot_ypr(double yaw, double pitch, double roll, double m[9]) {
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cr = std::cos(roll), sr = std::sin(roll);
    const double ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    const double rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
    const double rz[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
    double t[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            t[r * 3 + c] = ry[r * 3] * rx[c] + ry[r * 3 + 1] * rx[3 + c] +
                           ry[r * 3 + 2] * rx[6 + c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r * 3 + c] = t[r * 3] * rz[c] + t[r * 3 + 1] * rz[3 + c] +
                           t[r * 3 + 2] * rz[6 + c];
}

// (x, y, z, w) for the intrinsic Y-X-Z rotation -- synth.py _quat_from_ypr().
std::array<double, 4> quat_ypr(double yaw, double pitch, double roll) {
    const double cy = std::cos(yaw * 0.5), sy = std::sin(yaw * 0.5);
    const double cp = std::cos(pitch * 0.5), sp = std::sin(pitch * 0.5);
    const double cr = std::cos(roll * 0.5), sr = std::sin(roll * 0.5);
    auto mul = [](const std::array<double, 4> &a,
                  const std::array<double, 4> &b) {
        return std::array<double, 4>{
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    return mul(mul(std::array<double, 4>{0, sy, 0, cy},
                   std::array<double, 4>{sp, 0, 0, cp}),
               std::array<double, 4>{0, 0, sr, cr});
}

// One eye's luma, rendered exactly as synth.py _ray_grid + render_view do:
// x right, y UP (so the image row index runs downward), -z forward, and the
// ray for sample (i, j) passes through its centre (i + 0.5, j + 0.5).
std::vector<uint8_t> render(double yaw_deg, double pitch_deg, double roll_deg) {
    double R[9];
    rot_ypr(yaw_deg * kPi / 180.0, pitch_deg * kPi / 180.0,
            roll_deg * kPi / 180.0, R);
    const double t = std::tan(kFovDeg * kPi / 360.0);
    std::vector<uint8_t> out((size_t)kEye * kEye);
    for (int j = 0; j < kEye; ++j) {
        const double dy = -(((j + 0.5) / kEye) * 2.0 - 1.0) * t;
        for (int i = 0; i < kEye; ++i) {
            const double dx = (((i + 0.5) / kEye) * 2.0 - 1.0) * t;
            const double dz = -1.0;
            const double inv = 1.0 / std::sqrt(dx * dx + dy * dy + dz * dz);
            const double hx = dx * inv, hy = dy * inv, hz = dz * inv;
            // world = R * head   (synth.py: dirs @ R.T)
            const double wx = R[0] * hx + R[1] * hy + R[2] * hz;
            const double wy = R[3] * hx + R[4] * hy + R[5] * hz;
            const double wz = R[6] * hx + R[7] * hy + R[8] * hz;
            out[(size_t)j * kEye + i] =
                (uint8_t)std::lround(world_sample(wx, wy, wz));
        }
    }
    return out;
}

nxvc_view view_of(double yaw_deg, double pitch_deg, double roll_deg) {
    const auto q = quat_ypr(yaw_deg * kPi / 180.0, pitch_deg * kPi / 180.0,
                            roll_deg * kPi / 180.0);
    nxvc_view v{};
    v.qx = q[0];
    v.qy = q[1];
    v.qz = q[2];
    v.qw = q[3];
    const double f = kFovDeg * kPi / 360.0;
    v.fov_left = -f;
    v.fov_right = f;
    v.fov_up = f;
    v.fov_down = -f;
    return v;
}

double psnr(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    double se = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = (double)a[i] - (double)b[i];
        se += d * d;
    }
    const double mse = se / (double)a.size();
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Encode the pair losslessly with WARP_SKIP forced on every tile of frame 1,
// decode, and return the PSNR of the decoded frame 1 -- which is then purely
// the predictor's output -- against the true frame 1.  Returns < 0 on error.
double warp_prediction_psnr(double yaw, double pitch, double roll,
                            std::string *err) {
    const std::vector<uint8_t> f0 = render(0.0, 0.0, 0.0);
    const std::vector<uint8_t> f1 = render(yaw, pitch, roll);
    const std::vector<uint8_t> chroma((size_t)kEye * kEye, 128);

    nxvc_config cfg;
    nxvc_config_default(&cfg);
    cfg.width = kEye;
    cfg.height = kEye;
    cfg.eyes = 1;
    cfg.chroma = NXVC_CHROMA_444;
    cfg.lossless = 1;
    cfg.inter = 1;
    cfg.intra_period = 1000000;   // no rolling refresh inside the pair
    cfg.custom_tables = 0;

    nxvc_status st;
    nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
    if (!e) {
        *err = nxvc_status_string(st);
        return -1.0;
    }
    const uint32_t ntiles = nxvc_encoder_tile_count(e);

    std::vector<uint8_t> stream(4096);
    size_t hl = 0;
    st = nxvc_encoder_stream_header(e, stream.data(), stream.size(), &hl);
    if (st != NXVC_OK) {
        *err = nxvc_status_string(st);
        nxvc_encoder_destroy(e);
        return -1.0;
    }
    stream.resize(hl);

    std::vector<uint8_t> fbuf((size_t)kEye * kEye * 8 + (1u << 20));
    const std::vector<uint8_t> *src[2] = {&f0, &f1};
    const std::array<std::array<double, 3>, 2> poses = {
        std::array<double, 3>{0.0, 0.0, 0.0},
        std::array<double, 3>{yaw, pitch, roll}};
    for (int f = 0; f < 2; ++f) {
        nxvc_view v = view_of(poses[f][0], poses[f][1], poses[f][2]);
        nxvc_encoder_set_views(e, &v, 1);
        if (f == 1) {
            std::vector<uint8_t> smap(ntiles, 1);
            nxvc_encoder_set_skip_map(e, smap.data(), ntiles);
        }
        nxvc_image img{};
        img.plane[0] = const_cast<uint8_t *>(src[f]->data());
        img.stride[0] = kEye;
        img.plane[1] = const_cast<uint8_t *>(chroma.data());
        img.stride[1] = kEye;
        img.plane[2] = const_cast<uint8_t *>(chroma.data());
        img.stride[2] = kEye;
        size_t ol = 0;
        st = nxvc_encoder_encode_frame(e, &img, nullptr, nullptr, fbuf.data(),
                                       fbuf.size(), &ol);
        if (st != NXVC_OK) {
            *err = std::string("encode: ") + nxvc_status_string(st);
            nxvc_encoder_destroy(e);
            return -1.0;
        }
        stream.insert(stream.end(), fbuf.begin(), fbuf.begin() + ol);
    }
    nxvc_encoder_destroy(e);

    nxvc_decoder *d = nxvc_decoder_create(&st);
    size_t off = 0, consumed = 0;
    st = nxvc_decoder_parse_stream_header(d, stream.data(), stream.size(),
                                          &consumed);
    if (st != NXVC_OK) {
        *err = std::string("stream header: ") + nxvc_status_string(st);
        nxvc_decoder_destroy(d);
        return -1.0;
    }
    off = consumed;
    std::vector<uint8_t> Y((size_t)kEye * kEye), U(Y.size()), V(Y.size());
    std::vector<std::vector<uint8_t>> dec;
    while (off < stream.size()) {
        nxvc_image oi{};
        oi.plane[0] = Y.data();
        oi.stride[0] = kEye;
        oi.plane[1] = U.data();
        oi.stride[1] = kEye;
        oi.plane[2] = V.data();
        oi.stride[2] = kEye;
        st = nxvc_decoder_decode_frame(d, stream.data() + off,
                                       stream.size() - off, &oi, &consumed);
        if (st != NXVC_OK) {
            *err = std::string("decode: ") + nxvc_status_string(st);
            nxvc_decoder_destroy(d);
            return -1.0;
        }
        dec.push_back(Y);
        off += consumed;
    }
    nxvc_decoder_destroy(d);

    if (dec.size() != 2) {
        *err = "expected 2 decoded frames";
        return -1.0;
    }
    // Frame 0 must be bit-exact, or frame 1's number is measuring the intra
    // coder rather than the warp.
    if (dec[0] != f0) {
        *err = "frame 0 is not lossless";
        return -1.0;
    }
    return psnr(f1, dec[1]);
}

struct Case {
    const char *name;
    double yaw, pitch, roll;
    double floor_db;
};

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int fails = 0;

    // Thresholds.  Measured on this material: the correct convention gives
    // 50.0 to 57.7 dB, and each of the three conventions deliberately broken
    // while writing this test (quaternion conjugated; components read as
    // (w,x,y,z); fov_up/fov_down signs swapped) drops at least one row to
    // 25-36 dB.  The floors sit about 7 dB under the correct value and at
    // least 1 dB over the best broken value, so every row has margin in both
    // directions.
    //
    // Which row catches what is not uniform, and the set is chosen for that
    // rather than for coverage of angles: a pure-yaw pair cannot see an error
    // in the row direction or in the vertical FOV at all, so `pitch 2 deg` and
    // `roll 2 deg` are load-bearing and must not be dropped as redundant.
    // Conversely the small-angle rows are weak discriminators by nature --
    // every convention error shrinks with the angle -- and they are here for
    // the sub-pixel case, which is what the real pose logs mostly contain.
    //
    // 1 degree per frame is 90 deg/s at the codec's frame rate, 2 is 180, and
    // 5 is 450 -- past the operational envelope, where the 64x64 corner
    // interpolation of docs/WARP.md 7 legitimately costs about half a pixel,
    // which is why that row's floor is lower.
    //
    // This test's job is to catch a convention that has been inverted,
    // transposed or dropped, not to police the predictor's last decibel --
    // warp.oracle and warp.interior do that, against the matrix rather than
    // against a picture.
    static const Case cases[] = {
        {"yaw 0.25 deg (sub-pixel, 22 deg/s)", 0.25, 0.0, 0.0, 50.0},
        {"yaw 1 deg (90 deg/s)", 1.0, 0.0, 0.0, 50.0},
        {"yaw 2 deg (180 deg/s)", 2.0, 0.0, 0.0, 48.0},
        {"pitch 2 deg (row direction)", 0.0, 2.0, 0.0, 48.0},
        {"roll 2 deg", 0.0, 0.0, 2.0, 50.0},
        {"yaw 2 pitch 0.8 roll 0.5 deg (mixed)", 2.0, 0.8, 0.5, 48.0},
        {"yaw 5 deg (450 deg/s, outside the envelope)", 5.0, 0.0, 0.0, 44.0},
    };

    for (const Case &c : cases) {
        std::string err;
        const double db = warp_prediction_psnr(c.yaw, c.pitch, c.roll, &err);
        if (db < 0.0) {
            std::printf("FAIL %-46s %s\n", c.name, err.c_str());
            ++fails;
            continue;
        }
        const bool ok = db >= c.floor_db;
        std::printf("%s %-46s %7.2f dB  (floor %.0f)\n", ok ? "ok  " : "FAIL",
                    c.name, db, c.floor_db);
        if (!ok) ++fails;
    }

    // An identity pose must be a bit-exact copy.  This is warp.identity seen
    // from the far end of the codec: if the frame pairing is off by one, or
    // the views are read from the wrong slot, this is the row that says so.
    {
        std::string err;
        const double db = warp_prediction_psnr(0.0, 0.0, 0.0, &err);
        const bool ok = db >= 98.0;
        std::printf("%s %-46s %7.2f dB  (floor 98)\n", ok ? "ok  " : "FAIL",
                    "identity pose (bit-exact copy)", db);
        if (!ok) ++fails;
    }

    if (fails) {
        std::printf("\n%d convention check(s) failed.  A first warped frame "
                    "this far below its\nceiling is a geometry error, not a "
                    "quality one -- see docs/WARP-AUDIT.md.\n", fails);
        return 1;
    }
    std::printf("\nall warp convention checks passed\n");
    return 0;
}
