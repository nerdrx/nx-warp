// Shared helpers for the nxvc_ref tests: assertions, PSNR, MD5, test images.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond);      \
            std::printf(__VA_ARGS__);                                         \
            std::printf("\n");                                                \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

static inline int test_report(const char *name) {
    std::printf("%s: %s (%d failures)\n", name, g_failures ? "FAIL" : "ok",
                g_failures);
    return g_failures ? 1 : 0;
}

// --------------------------------------------------------------------- PSNR
static inline double psnr8(const uint8_t *a, const uint8_t *b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; ++i) {
        double d = (double)a[i] - (double)b[i];
        se += d * d;
    }
    if (se == 0) return 1000.0;
    return 10.0 * std::log10(255.0 * 255.0 * (double)n / se);
}

// ---------------------------------------------------------------------- MD5
// Compact MD5 (RFC 1321) used only to pin conformance vectors.
struct MD5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t buflen = 0;

    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

    void block(const uint8_t *p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
            0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
            0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
            0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
            0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
            0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
            0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
            0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
            0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
                   ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
            else { F = C ^ (B | ~D); g = (7 * i) & 15; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rol(F, S[i]);
        }
        a += A; b += B; c += C; d += D;
    }

    void update(const void *data, size_t n) {
        const uint8_t *p = (const uint8_t *)data;
        len += n;
        while (n) {
            size_t take = 64 - buflen;
            if (take > n) take = n;
            std::memcpy(buf + buflen, p, take);
            buflen += take;
            p += take;
            n -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    std::string hex() {
        MD5 t = *this;
        uint64_t bits = t.len * 8;
        uint8_t pad = 0x80;
        t.update(&pad, 1);
        uint8_t z = 0;
        while (t.buflen != 56) t.update(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (8 * i));
        t.update(l, 8);
        uint32_t v[4] = {t.a, t.b, t.c, t.d};
        char out[33];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                std::snprintf(out + (i * 4 + j) * 2, 3, "%02x",
                              (unsigned)((v[i] >> (8 * j)) & 0xff));
        return std::string(out, 32);
    }
};

static inline std::string md5_hex(const void *p, size_t n) {
    MD5 m;
    m.update(p, n);
    return m.hex();
}

// ------------------------------------------------------------- test images
struct TestImage {
    int w = 0, h = 0, cw = 0, ch = 0;
    bool c444 = false;
    std::vector<uint8_t> p[4];
};

// Deterministic pseudo-random generator (xorshift), so vectors are stable.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
};

// kind: 0 gradient, 1 textured, 2 checker/graphics, 3 noise, 4 flat
static inline TestImage make_image(int w, int h, bool c444, int kind,
                                   uint32_t seed = 12345) {
    TestImage im;
    im.w = w; im.h = h; im.c444 = c444;
    im.cw = c444 ? w : (w + 1) / 2;
    im.ch = c444 ? h : (h + 1) / 2;
    im.p[0].resize((size_t)w * h);
    im.p[1].resize((size_t)im.cw * im.ch);
    im.p[2].resize((size_t)im.cw * im.ch);
    im.p[3].resize((size_t)w * h, 255);
    Rng rng(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v;
            switch (kind) {
                case 0: v = (x * 255) / (w - 1) / 2 + (y * 255) / (h - 1) / 2; break;
                case 1:
                    v = (int)(128 + 60 * std::sin(x * 0.02) * std::cos(y * 0.017) +
                              22 * std::sin((x * x + y * y) * 0.0007)) +
                        (((x / 16) + (y / 16)) % 2 ? 14 : -14);
                    break;
                case 2: v = ((x / 9 + y / 7) % 2) ? 235 : 16; break;
                case 3: v = (int)(rng.next() & 255); break;
                default: v = 137; break;
            }
            im.p[0][(size_t)y * w + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            im.p[3][(size_t)y * w + x] =
                (uint8_t)((x < w / 3) ? 255 : ((y < h / 2) ? 96 : 200));
        }
    for (int y = 0; y < im.ch; ++y)
        for (int x = 0; x < im.cw; ++x) {
            int u, v;
            switch (kind) {
                case 0: u = 90 + x % 64; v = 160 - y % 64; break;
                case 1:
                    u = (int)(128 + 45 * std::sin(x * 0.03 + y * 0.02));
                    v = (int)(128 + 40 * std::cos(x * 0.04 - y * 0.01));
                    break;
                case 2: u = ((x / 9) % 2) ? 210 : 40; v = ((y / 7) % 2) ? 30 : 220; break;
                case 3: u = (int)(rng.next() & 255); v = (int)(rng.next() & 255); break;
                default: u = 128; v = 128; break;
            }
            im.p[1][(size_t)y * im.cw + x] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            im.p[2][(size_t)y * im.cw + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    return im;
}
