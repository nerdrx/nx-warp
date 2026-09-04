// NX Warp -- homography derivation from two OpenXR poses and per-eye FOV.
//
// This is ENCODER-SIDE and runs in double precision. It is not on the
// normative decode path: its only output that crosses the wire is the nine
// quantised int32 of the Homography struct. Both encoder and decoder then use
// the quantised matrix, so the quantisation error lands in the residual and
// never causes drift.
//
// SPDX-License-Identifier: Apache-2.0

#include "nxvc/warp.h"

#include <cmath>

namespace nxvc::warp {

namespace {

// Rotation matrix (camera-to-world) from a unit quaternion, row-major.
void quat_to_mat(const Quat& q, double m[9]) {
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1.0 - 2.0 * (yy + zz);
    m[1] = 2.0 * (xy - wz);
    m[2] = 2.0 * (xz + wy);
    m[3] = 2.0 * (xy + wz);
    m[4] = 1.0 - 2.0 * (xx + zz);
    m[5] = 2.0 * (yz - wx);
    m[6] = 2.0 * (xz - wy);
    m[7] = 2.0 * (yz + wx);
    m[8] = 1.0 - 2.0 * (xx + yy);
}

void mat3_mul(const double a[9], const double b[9], double out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] +
                             a[r * 3 + 2] * b[2 * 3 + c];
}

void mat3_transpose(const double a[9], double out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = a[c * 3 + r];
}

// K maps an OpenXR camera-space direction (x right, y up, -z forward) to a
// homogeneous *centred integer sample index*:
//     (X, Y, W) = K * v,   xc = X/W,  yc = Y/W
// where xc = x_index - ox and yc = y_index - oy, and x_index counts sample
// centres (the +0.5 of the pixel-centre convention is folded in here so the
// integer decode path never sees it).
void make_K(const Fov& f, int32_t W, int32_t H, double ox, double oy, double k[9]) {
    const double tl = std::tan(f.angle_left);
    const double tr = std::tan(f.angle_right);
    const double tu = std::tan(f.angle_up);
    const double td = std::tan(f.angle_down);
    const double sx = static_cast<double>(W) / (tr - tl);
    const double sy = static_cast<double>(H) / (tu - td);
    k[0] = sx;   k[1] = 0.0;  k[2] = sx * tl + 0.5 + ox;
    k[3] = 0.0;  k[4] = -sy;  k[5] = -(sy * tu - 0.5 - oy);
    k[6] = 0.0;  k[7] = 0.0;  k[8] = -1.0;
}

// Analytic inverse of the K above (it has the form [[a,0,c],[0,b,e],[0,0,-1]]).
void invert_K(const double k[9], double inv[9]) {
    const double a = k[0], b = k[4], c = k[2], e = k[5];
    inv[0] = 1.0 / a;  inv[1] = 0.0;      inv[2] = c / a;
    inv[3] = 0.0;      inv[4] = 1.0 / b;  inv[5] = e / b;
    inv[6] = 0.0;      inv[7] = 0.0;      inv[8] = -1.0;
}

int32_t round_to_int(double v) {
    return static_cast<int32_t>(std::llround(v));
}

}  // namespace

void exact_homography(const Quat& r_prev,
                      const Fov& fov_prev,
                      const Quat& r_cur,
                      const Fov& fov_cur,
                      int32_t frame_width,
                      int32_t frame_height,
                      double out[9]) {
    const double ox = frame_width / 2;
    const double oy = frame_height / 2;

    double Rp[9], Rc[9], Rpt[9], Rrel[9];
    quat_to_mat(r_prev, Rp);
    quat_to_mat(r_cur, Rc);
    mat3_transpose(Rp, Rpt);
    mat3_mul(Rpt, Rc, Rrel);  // world-free: camera_cur -> camera_prev

    double Kp[9], Kc[9], Kci[9], t[9];
    make_K(fov_prev, frame_width, frame_height, ox, oy, Kp);
    make_K(fov_cur, frame_width, frame_height, ox, oy, Kc);
    invert_K(Kc, Kci);

    mat3_mul(Rrel, Kci, t);
    mat3_mul(Kp, t, out);

    // Normalise so h22 == 1. h22 is ~1 for any plausible head rotation.
    if (out[8] != 0.0) {
        const double s = 1.0 / out[8];
        for (int i = 0; i < 9; ++i) out[i] *= s;
    }
}

bool derive_homography(const Quat& r_prev,
                       const Fov& fov_prev,
                       const Quat& r_cur,
                       const Fov& fov_cur,
                       int32_t frame_width,
                       int32_t frame_height,
                       Homography* out) {
    const int32_t ox = frame_width / 2;
    const int32_t oy = frame_height / 2;
    *out = identity_homography(ox, oy);

    double Hd[9];
    exact_homography(r_prev, fov_prev, r_cur, fov_cur, frame_width, frame_height, Hd);
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(Hd[i])) return false;
    }

    // Quantise: rows 0 and 1 in Q10.21, row 2 in Q2.29 with h22 == 1.0 exactly.
    const double num_scale = static_cast<double>(1 << kQNum);
    const double den_scale = static_cast<double>(1 << kQDen);
    double q[9];
    for (int i = 0; i < 6; ++i) q[i] = Hd[i] * num_scale;
    for (int i = 6; i < 8; ++i) q[i] = Hd[i] * den_scale;
    q[8] = den_scale;

    for (int i = 0; i < 9; ++i) {
        if (!(q[i] >= -2147483648.0 && q[i] <= 2147483647.0)) return false;
        out->h[i] = round_to_int(q[i]);
    }
    out->h[8] = 1 << kQDen;
    out->ox = ox;
    out->oy = oy;

    // Validate the denominator over the whole picture. den is bilinear in
    // (cx, cy) so its extrema are at the picture corners; check all four,
    // extended by one tile so that the corner at (W, H) is covered too.
    const int32_t cxs[2] = {-ox, frame_width - ox};
    const int32_t cys[2] = {-oy, frame_height - oy};
    for (int a = 0; a < 2; ++a) {
        for (int b = 0; b < 2; ++b) {
            const double den = static_cast<double>(out->h[6]) * cxs[a] +
                               static_cast<double>(out->h[7]) * cys[b] +
                               static_cast<double>(out->h[8]);
            if (!(den >= static_cast<double>(kDenMin) && den < static_cast<double>(kDenMax))) {
                *out = identity_homography(ox, oy);
                return false;
            }
        }
    }
    return true;
}

}  // namespace nxvc::warp
