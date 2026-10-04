#include "astc_encode_colour_tables.glsl"
// Try one independent colour plane on chroma-rich tiles; retain ordinary ASTC unless decoded error
// improves.
void put(inout uvec4 b, uint offset, uint count, uint value);
uint endpointDecode(uint code) { return ep160[code]; }
uint nearestWeight4(float x) { return nearestWeight4Code(uint(x)); }
uint quint22(uint a, uint b, uint c) {
    uint t = astcQuintEncode[(a >> 5u) + 5u * (b >> 5u) + 25u * (c >> 5u)];
    return (a & 31u) | ((t & 7u) << 5u) | ((b & 31u) << 8u) | (((t >> 3u) & 3u) << 13u) |
           ((c & 31u) << 15u) | (((t >> 5u) & 3u) << 20u);
}
uint interp4(uint grid[16], uint texel) {
    uvec4 r = astcInterp4[texel];
    uint v = 8u;
    for (int k = 0; k < 4; k++) {
        uint p = r[k], w = p >> 5u;
        if (w != 0u)
            v += grid[p & 31u] * w;
    }
    return v >> 4;
}
bool dualCandidate(vec3 pix[64], float baselineError, vec3 mean, mat3 cov,
                   out uvec4 result, out float score) {
    float residual[3];
    for (int c = 0; c < 3; c++) {
        int a = (c + 1) % 3, b = (c + 2) % 3;
        float det = cov[a][a] * cov[b][b] - cov[a][b] * cov[a][b];
        float explained = 0;
        if (det > 1e-6) {
            float x = cov[c][a], y = cov[c][b];
            explained = (x * x * cov[b][b] - 2 * x * y * cov[a][b] + y * y * cov[a][a]) / det;
        }
        residual[c] = max(cov[c][c] - explained, 0.0);
    }
    uint ccs = residual[0] >= residual[1] && residual[0] >= residual[2] ? 0u
               : residual[1] >= residual[2]                             ? 1u
                                                                        : 2u;
    float chroma = 0, meanRG = 0, meanBG = 0;
    for (uint i = 0u; i < 64u; i++) {
        float rg = pix[i].r - pix[i].g, bg = pix[i].b - pix[i].g;
        chroma += rg * rg + bg * bg;
        meanRG += rg / 64.0;
        meanBG += bg / 64.0;
    }
    chroma = chroma / 64.0 - meanRG * meanRG - meanBG * meanBG;
    if (chroma < 500.0)
        return false;
    uint ca = (ccs + 1u) % 3u, cb = (ccs + 2u) % 3u;
    float ma = mean[ca], mb = mean[cb], aa = cov[ca][ca], ab = cov[ca][cb], bb = cov[cb][cb];
    float ax = 1.0, ay = 0.0;
    for (int it = 0; it < 8; it++) {
        float nx = aa * ax + ab * ay, ny = ab * ax + bb * ay, n = max(length(vec2(nx, ny)), 1e-9);
        ax = nx / n;
        ay = ny / n;
    }
    float loT = 1e30, hiT = -1e30;
    vec3 lo = vec3(0), hi = vec3(0);
    float cLo = 255.0, cHi = 0.0;
    for (uint i = 0u; i < 64u; i++) {
        float t = (pix[i][ca] - ma) * ax + (pix[i][cb] - mb) * ay;
        if (t < loT) {
            loT = t;
            lo = pix[i];
        }
        if (t > hiT) {
            hiT = t;
            hi = pix[i];
        }
        cLo = min(cLo, pix[i][ccs]);
        cHi = max(cHi, pix[i][ccs]);
    }
    lo[ccs] = cLo;
    hi[ccs] = cHi;
    uint ep[6];
    vec3 e0, e1;
    for (uint c = 0u; c < 3u; c++) {
        ep[c * 2u] = endpointCodeFrom6(lo[c]);
        ep[c * 2u + 1u] = endpointCodeFrom6(hi[c]);
        e0[c] = float(endpointDecode(ep[c * 2u]));
        e1[c] = float(endpointDecode(ep[c * 2u + 1u]));
    }
    // CEM8 decodes blue-contracted endpoints if sums reverse. Canonicalize before fitting.
    if (e0.r + e0.g + e0.b > e1.r + e1.g + e1.b) {
        vec3 t = e0;
        e0 = e1;
        e1 = t;
        for (uint c = 0u; c < 3u; c++) {
            uint q = ep[c * 2u];
            ep[c * 2u] = ep[c * 2u + 1u];
            ep[c * 2u + 1u] = q;
        }
    }
    vec3 axis = e1 - e0;
    float rhs0[16], rhs1[16];
    for (int j = 0; j < 16; j++) {
        rhs0[j] = 0;
        rhs1[j] = 0;
    }
    for (uint i = 0u; i < 64u; i++) {
        float denShared = max(axis[ca] * axis[ca] + axis[cb] * axis[cb], 1e-8);
        float t0 = clamp(((pix[i][ca] - e0[ca]) * axis[ca] + (pix[i][cb] - e0[cb]) * axis[cb]) /
                             denShared,
                         0.0, 1.0),
              span = e1[ccs] - e0[ccs],
              t1 = abs(span) > 1e-8 ? clamp((pix[i][ccs] - e0[ccs]) / span, 0.0, 1.0) : 0.0;
        uvec4 r = astcInterp4[i];
        for (int k = 0; k < 4; k++) {
            uint v = r[k], wt = v >> 5u;
            if (wt != 0u) {
                float w = float(wt) * (1.0 / 16.0);
                rhs0[v & 31u] += w * t0;
                rhs1[v & 31u] += w * t1;
            }
        }
    }
    uint g0[16], g1[16];
    for (int j = 0; j < 16; j++) {
        float x = 0, y = 0;
        for (int k = 0; k < 16; k++) {
            x += astc4x4Inverse[j * 16 + k] * rhs0[k];
            y += astc4x4Inverse[j * 16 + k] * rhs1[k];
        }
        g0[j] = wt4[nearestWeight4(round(clamp(x, 0.0, 1.0) * 64.0))];
        g1[j] = wt4[nearestWeight4(round(clamp(y, 0.0, 1.0) * 64.0))];
    }
    float err = 0;
    for (uint i = 0u; i < 64u; i++) {
        uint w0 = interp4(g0, i), w1 = interp4(g1, i);
        vec3 q;
        for (uint c = 0u; c < 3u; c++) {
            uint w = c == ccs ? w1 : w0;
            q[c] = floor((e0[c] * float(64u - w) + e1[c] * float(w) + 32.0) / 64.0);
        }
        vec3 d = pix[i] - q;
        err += dot(d, d);
    }
    score = err;
    if (err >= baselineError * 0.95)
        return false;
    uvec4 outb = uvec4(0), weights = uvec4(0);
    put(outb, 0u, 17u, 0x10442u);
    put(outb, 17u, 22u, quint22(ep[0], ep[1], ep[2]));
    put(outb, 39u, 22u, quint22(ep[3], ep[4], ep[5]));
    put(outb, 62u, 2u, ccs);
    for (uint j = 0u; j < 16u; j++) {
        put(weights, 4u * j, 2u, nearestWeight4(float(g0[j])));
        put(weights, 4u * j + 2u, 2u, nearestWeight4(float(g1[j])));
    }
    result = outb | uvec4(0u, 0u, bitfieldReverse(weights.y), bitfieldReverse(weights.x));
    return true;
}

const uint weightDecode[8] = uint[8](0u, 9u, 18u, 27u, 37u, 46u, 55u, 64u);
uint interpolate5(in uint grid[25], uint texel) {
    uvec4 r = astcInterp5[texel];
    uint v = 8u;
    for (int k = 0; k < 4; k++) {
        uint p = r[k], w = p >> 5u;
        if (w != 0u)
            v += grid[p & 31u] * w;
    }
    return v >> 4;
}
