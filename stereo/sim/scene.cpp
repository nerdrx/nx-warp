#include "scene.h"

#include <algorithm>

namespace nxs {
namespace {

// ------------------------------------------------------------------ 5x7 font
// Row-major, 7 rows of 5 bits, bit 4 leftmost.
struct Glyph {
    char c;
    u8 row[7];
};

// clang-format off
const Glyph kFont[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
};
// clang-format on

const char* kTextLines[] = {
    "NX WARP STEREO SIM",
    "TILE 64X64  IPD 64MM",
    "MODE: STEREO / INTRA",
    "DISPARITY = F IPD / Z",
    "RIGHT EYE FROM LEFT",
    "1234567890 - : . /",
    "RESIDUAL ENERGY TEST",
    "DECODE ROW R THEN R",
};
constexpr int kTextLineCount = 8;

const Glyph* find_glyph(char c) {
    for (const Glyph& g : kFont)
        if (g.c == c) return &g;
    return &kFont[0];
}

// ------------------------------------------------------------------ noise

double value_noise(double x, double y, u32 seed) {
    int xi = static_cast<int>(std::floor(x));
    int yi = static_cast<int>(std::floor(y));
    double fx = x - xi, fy = y - yi;
    double sx = fx * fx * (3.0 - 2.0 * fx);
    double sy = fy * fy * (3.0 - 2.0 * fy);
    double a = hash01(xi, yi, static_cast<int>(seed));
    double b = hash01(xi + 1, yi, static_cast<int>(seed));
    double c = hash01(xi, yi + 1, static_cast<int>(seed));
    double d = hash01(xi + 1, yi + 1, static_cast<int>(seed));
    return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}

double fbm(double x, double y, u32 seed, int oct) {
    double s = 0, amp = 0.5, f = 1.0, norm = 0;
    for (int i = 0; i < oct; ++i) {
        s += amp * value_noise(x * f, y * f, seed + static_cast<u32>(i) * 7919u);
        norm += amp;
        amp *= 0.55;
        f *= 2.07;
    }
    return s / norm;
}

}  // namespace

double sample_texture(Tex t, u32 seed, double u, double v, double dens) {
    const double tu = u * dens, tv = v * dens;
    switch (t) {
        case Tex::kFlat:
            return 120.0 + 6.0 * (fbm(tu * 0.05, tv * 0.05, seed, 2) - 0.5);
        case Tex::kNoise:
            return 40.0 + 180.0 * fbm(tu * 0.25, tv * 0.25, seed, 5);
        case Tex::kChecker: {
            int cu = static_cast<int>(std::floor(tu / 8.0));
            int cv = static_cast<int>(std::floor(tv / 8.0));
            double base = ((cu + cv) & 1) ? 210.0 : 45.0;
            return base + 14.0 * (fbm(tu * 0.5, tv * 0.5, seed, 3) - 0.5);
        }
        case Tex::kStripes: {
            double s = std::sin(tu * 0.9) * std::cos(tv * 0.31);
            return 128.0 + 90.0 * s + 12.0 * (fbm(tu * 0.7, tv * 0.7, seed, 3) - 0.5);
        }
        case Tex::kBrick: {
            double bh = 6.0, bw = 16.0;
            int row = static_cast<int>(std::floor(tv / bh));
            double ou = tu + (row & 1 ? bw * 0.5 : 0.0);
            double fu = ou - std::floor(ou / bw) * bw;
            double fv = tv - std::floor(tv / bh) * bh;
            bool mortar = fu < 1.2 || fv < 1.0;
            if (mortar) return 175.0 + 8.0 * (fbm(tu, tv, seed, 2) - 0.5);
            int col = static_cast<int>(std::floor(ou / bw));
            double tone = 70.0 + 60.0 * hash01(col, row, static_cast<int>(seed));
            return tone + 24.0 * (fbm(tu * 1.5, tv * 1.5, seed + 11u, 3) - 0.5);
        }
        case Tex::kSkin: {
            double base = 150.0 + 28.0 * (fbm(tu * 0.06, tv * 0.06, seed, 3) - 0.5);
            double pore = 10.0 * (fbm(tu * 0.9, tv * 0.9, seed + 3u, 2) - 0.5);
            return base + pore;
        }
        case Tex::kText: {
            // Glyph cell is 6x8 texels (5x7 glyph plus 1 texel of spacing).
            const int cw = 6, ch = 10;
            int gx = static_cast<int>(std::floor(tu));
            int gy = static_cast<int>(std::floor(tv));
            if (gx < 0 || gy < 0) return 232.0;
            int line = gy / ch;
            int col = gx / cw;
            int py = gy - line * ch;
            int px = gx - col * cw;
            if (line >= kTextLineCount) return 232.0;
            const char* s = kTextLines[line];
            int len = static_cast<int>(std::strlen(s));
            if (col >= len || py >= 7 || px >= 5) return 232.0;
            const Glyph* g = find_glyph(s[col]);
            bool on = (g->row[py] >> (4 - px)) & 1;
            return on ? 18.0 : 232.0;
        }
    }
    return 128.0;
}

// ------------------------------------------------------------------ scenes

namespace {

Box mk(Vec3 lo, Vec3 hi, Tex t, double dens, double tint, u32 seed) {
    Box b;
    b.lo = lo;
    b.hi = hi;
    b.tex = t;
    b.texel_per_m = dens;
    b.tint = tint;
    b.seed = seed;
    return b;
}

void add_room(std::vector<Box>& v, double back_z) {
    v.push_back(mk({-4.0, -1.25, -3.0}, {4.0, -1.20, back_z}, Tex::kChecker, 40.0, 0.9, 11));   // floor
    v.push_back(mk({-4.0, 1.60, -3.0}, {4.0, 1.65, back_z}, Tex::kFlat, 20.0, 1.0, 12));        // ceiling
    v.push_back(mk({-4.05, -1.25, -3.0}, {-4.0, 1.65, back_z}, Tex::kBrick, 60.0, 1.0, 13));    // left
    v.push_back(mk({4.0, -1.25, -3.0}, {4.05, 1.65, back_z}, Tex::kBrick, 60.0, 0.95, 14));     // right
    v.push_back(mk({-4.0, -1.25, back_z}, {4.0, 1.65, back_z + 0.05}, Tex::kNoise, 55.0, 1.0, 15));  // back
}

}  // namespace

std::vector<Scene> all_scenes() {
    std::vector<Scene> out;

    {  // 1. room: mid-depth world, the ordinary case
        Scene s;
        s.name = "room";
        add_room(s.boxes, 6.0);
        s.boxes.push_back(mk({-1.6, -1.2, 2.2}, {-0.6, -0.2, 3.2}, Tex::kNoise, 90.0, 1.0, 21));
        s.boxes.push_back(mk({0.5, -1.2, 1.6}, {1.4, 0.3, 2.4}, Tex::kStripes, 70.0, 1.0, 22));
        s.boxes.push_back(mk({-0.4, -0.1, 4.0}, {0.6, 0.9, 4.8}, Tex::kBrick, 80.0, 1.0, 23));
        s.boxes.push_back(mk({1.8, -1.2, 3.4}, {2.6, 0.6, 4.0}, Tex::kNoise, 60.0, 0.85, 24));
        s.boxes.push_back(mk({-2.8, -1.2, 1.2}, {-2.2, -0.4, 1.8}, Tex::kChecker, 120.0, 1.0, 25));
        out.push_back(s);
    }
    {  // 2. panel: a head-locked-looking text quad at reading distance
        Scene s;
        s.name = "panel";
        add_room(s.boxes, 6.0);
        s.boxes.push_back(mk({-0.62, -0.42, 1.50}, {0.62, 0.42, 1.53}, Tex::kText, 156.0, 1.0, 31));
        s.boxes.push_back(mk({-1.9, -1.2, 2.6}, {-1.0, -0.1, 3.4}, Tex::kNoise, 90.0, 1.0, 32));
        s.boxes.push_back(mk({1.2, -1.2, 2.2}, {2.0, 0.2, 2.9}, Tex::kBrick, 80.0, 1.0, 33));
        out.push_back(s);
    }
    {  // 3. hand: near-field object, disparity far outside the +-16 px search
        Scene s;
        s.name = "hand";
        add_room(s.boxes, 6.0);
        // palm and three finger boxes at 0.30 to 0.42 m
        s.boxes.push_back(mk({-0.10, -0.34, 0.34}, {0.10, -0.14, 0.40}, Tex::kSkin, 900.0, 1.0, 41));
        s.boxes.push_back(mk({-0.09, -0.14, 0.31}, {-0.04, -0.02, 0.36}, Tex::kSkin, 900.0, 1.0, 42));
        s.boxes.push_back(mk({-0.02, -0.14, 0.30}, {0.03, 0.01, 0.35}, Tex::kSkin, 900.0, 1.0, 43));
        s.boxes.push_back(mk({0.05, -0.14, 0.32}, {0.10, -0.01, 0.37}, Tex::kSkin, 900.0, 1.0, 44));
        s.boxes.push_back(mk({-0.9, -1.2, 2.0}, {0.0, -0.2, 2.8}, Tex::kNoise, 90.0, 1.0, 45));
        s.boxes.push_back(mk({0.8, -1.2, 2.6}, {1.7, 0.4, 3.3}, Tex::kStripes, 70.0, 1.0, 46));
        out.push_back(s);
    }
    {  // 4. far: nearly everything beyond 8 m, sub-pixel disparity
        Scene s;
        s.name = "far";
        add_room(s.boxes, 25.0);
        s.boxes.push_back(mk({-4.0, -1.2, 9.0}, {-1.5, 1.0, 11.0}, Tex::kBrick, 60.0, 1.0, 51));
        s.boxes.push_back(mk({1.0, -1.2, 14.0}, {3.5, 1.4, 16.0}, Tex::kNoise, 55.0, 1.0, 52));
        s.boxes.push_back(mk({-1.0, -1.2, 19.0}, {1.0, 0.8, 20.5}, Tex::kChecker, 45.0, 1.0, 53));
        s.boxes.push_back(mk({-3.0, -1.2, 7.5}, {-2.4, -0.2, 8.0}, Tex::kStripes, 60.0, 1.0, 54));
        out.push_back(s);
    }
    {  // 5. clutter: many depth discontinuities, the disocclusion stress case
        Scene s;
        s.name = "clutter";
        add_room(s.boxes, 8.0);
        for (int i = 0; i < 14; ++i) {
            double z = 0.45 + 0.55 * i + 1.4 * hash01(i, 7, 61);
            double x = -2.6 + 5.2 * hash01(i, 11, 62);
            double y = -1.2 + 2.2 * hash01(i, 13, 63);
            double sx = 0.12 + 0.55 * hash01(i, 17, 64);
            double sy = 0.12 + 0.70 * hash01(i, 19, 65);
            double sz = 0.10 + 0.40 * hash01(i, 23, 66);
            Tex t = (i % 3 == 0) ? Tex::kNoise : (i % 3 == 1 ? Tex::kChecker : Tex::kBrick);
            s.boxes.push_back(mk({x, y, z}, {x + sx, y + sy, z + sz}, t, 70.0 + 40.0 * (i % 4),
                                 0.8 + 0.3 * hash01(i, 29, 67), static_cast<u32>(70 + i)));
        }
        out.push_back(s);
    }
    return out;
}

Scene scene_by_name(const std::string& name) {
    for (const Scene& s : all_scenes())
        if (s.name == name) return s;
    return Scene{};
}

Pose pose_for_frame(int frame, double motion_scale) {
    // One frame at 90 Hz of a brisk head turn: 3.3 deg yaw (300 deg/s), 0.8 deg
    // pitch, 0.6 deg roll, and 11 mm of lateral translation (walking speed).
    const double yaw = 3.3 * (M_PI / 180.0) * motion_scale;
    const double pitch = 0.8 * (M_PI / 180.0) * motion_scale;
    const double roll = 0.6 * (M_PI / 180.0) * motion_scale;
    Pose p;
    double t = static_cast<double>(frame);
    p.rot = mul(mul(rot_y(yaw * t), rot_x(pitch * t)), rot_z(roll * t));
    p.pos = Vec3{0.011 * motion_scale * t, 0.002 * motion_scale * t, 0.004 * motion_scale * t};
    return p;
}

Camera make_camera(const Pose& head, int eye, double ipd, int w, int h, double fov_deg) {
    Camera c;
    c.rot = head.rot;
    // Eye offset is in head space; +x is to the right, so eye 0 (left) is -ipd/2.
    Vec3 off = mul(head.rot, Vec3{(eye == 0 ? -0.5 : 0.5) * ipd, 0, 0});
    c.pos = head.pos + off;
    c.w = w;
    c.h = h;
    c.cx = w * 0.5;
    c.cy = h * 0.5;
    c.f = (w * 0.5) / std::tan(fov_deg * 0.5 * (M_PI / 180.0));
    return c;
}

// ------------------------------------------------------------------ render

namespace {

struct Hit {
    double t = 1e30;
    int axis = 0;      // 0=x,1=y,2=z face normal axis
    const Box* box = nullptr;
};

// Slab test.  Returns entry distance and the axis crossed last on entry.
bool intersect(const Box& b, const Vec3& o, const Vec3& d, double tmax, double* t_out,
               int* axis_out) {
    double tmin = 1e-4;
    double tm = tmax;
    int axis = 0;
    const double ol[3] = {o.x, o.y, o.z};
    const double dl[3] = {d.x, d.y, d.z};
    const double lo[3] = {b.lo.x, b.lo.y, b.lo.z};
    const double hi[3] = {b.hi.x, b.hi.y, b.hi.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dl[i]) < 1e-12) {
            if (ol[i] < lo[i] || ol[i] > hi[i]) return false;
            continue;
        }
        double inv = 1.0 / dl[i];
        double t0 = (lo[i] - ol[i]) * inv;
        double t1 = (hi[i] - ol[i]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        if (t0 > tmin) {
            tmin = t0;
            axis = i;
        }
        tm = t1 < tm ? t1 : tm;
        if (tm < tmin) return false;
    }
    *t_out = tmin;
    *axis_out = axis;
    return true;
}

Hit trace(const Scene& sc, const Vec3& o, const Vec3& d) {
    Hit best;
    for (const Box& b : sc.boxes) {
        double t;
        int axis;
        if (intersect(b, o, d, best.t, &t, &axis) && t < best.t) {
            best.t = t;
            best.axis = axis;
            best.box = &b;
        }
    }
    return best;
}

double shade(const Scene& sc, const Hit& h, const Vec3& p) {
    if (!h.box) return sc.bg_luma;
    // World-locked UV from the two axes that are not the face normal, so the
    // texture is identical in both eyes and both frames.  This is the property
    // that makes inter-view prediction meaningful at all.
    double u, v;
    if (h.axis == 0) {
        u = p.z;
        v = p.y;
    } else if (h.axis == 1) {
        u = p.x;
        v = p.z;
    } else {
        u = p.x;
        v = p.y;
    }
    double s = sample_texture(h.box->tex, h.box->seed + static_cast<u32>(h.axis) * 977u, u, v,
                              h.box->texel_per_m);
    // Lambert-ish shading from a fixed world light, so faces differ in tone.
    static const double kFaceGain[3] = {0.82, 1.06, 0.94};
    return clampd(s * h.box->tint * kFaceGain[h.axis], 0.0, 255.0);
}

}  // namespace

View render(const Scene& sc, const Camera& cam) {
    View out;
    out.luma = Image(cam.w, cam.h);
    out.depth = DepthMap(cam.w, cam.h);
    const int kSS = 2;  // 2x2 supersampling; rendered frames are anti-aliased
    for (int y = 0; y < cam.h; ++y) {
        for (int x = 0; x < cam.w; ++x) {
            double acc = 0;
            for (int sy = 0; sy < kSS; ++sy) {
                for (int sx = 0; sx < kSS; ++sx) {
                    double px = x + (sx + 0.5) / kSS;
                    double py = y + (sy + 0.5) / kSS;
                    Vec3 dc = normalize(Vec3{(px - cam.cx) / cam.f, (py - cam.cy) / cam.f, 1.0});
                    Vec3 d = mul(cam.rot, dc);
                    Hit h = trace(sc, cam.pos, d);
                    acc += h.box ? shade(sc, h, cam.pos + d * h.t) : sc.bg_luma;
                }
            }
            out.luma.at(x, y) = static_cast<u8>(clampi(
                static_cast<i32>(acc / (kSS * kSS) + 0.5), 0, 255));

            // Depth from a single centre ray: eye-space z, which is what the
            // disparity relation D = f*IPD/z is written against.
            Vec3 dc = normalize(Vec3{(x + 0.5 - cam.cx) / cam.f, (y + 0.5 - cam.cy) / cam.f, 1.0});
            Vec3 d = mul(cam.rot, dc);
            Hit h = trace(sc, cam.pos, d);
            if (h.box) {
                // z along the camera's own forward axis, not the ray length.
                Vec3 pc = mul(transpose(cam.rot), (cam.pos + d * h.t) - cam.pos);
                out.depth.at(x, y) = static_cast<float>(pc.z);
            } else {
                out.depth.at(x, y) = kFarZ;
            }
        }
    }
    return out;
}

}  // namespace nxs
