#version 450
// GPU palette variant: upload the validated PLANAR body and TileRec, then
// dequantize each R2/coarse tile's two constant colours in the vertex stage.
layout(std430, binding = 0) readonly buffer Planar { uint planar[]; } plan;
layout(std430, binding = 1) readonly buffer TileRec { uint records[]; } tiles;
layout(push_constant) uniform PC { int frameQP; int chromaQPOffset; int width; int height; } pc;
layout(location = 0) flat out uvec2 tileMap;
layout(location = 1) flat out vec4 color0;
layout(location = 2) flat out vec4 color1;

const int qstep[64] = int[64](
    16,18,20,23,25,29,32,36,40,45,51,57,64,72,81,91,102,114,
    128,144,161,181,203,228,256,287,323,362,406,456,512,575,
    645,724,813,912,1024,1149,1290,1448,1625,1825,2048,2299,2580,
    2896,3251,3649,4096,4598,5161,5793,6502,7298,8192,9195,10321,
    11585,13004,14596,16384,18390,20643,23170);

int signedByte(uint word, int lane) {
    int q = int((word >> uint(lane * 8)) & 255u);
    return q >= 128 ? q - 256 : q;
}

int dequant(int q, int qp) {
    int dcqp = clamp(qp, 0, 63) >> 1;
    int step = (qstep[dcqp] * 16 + 8) >> 4;
    return clamp((q * step + 8) >> 4, -32768, 32767);
}

uint roundedByte(float x) {
    return uint(clamp(int(x + 0.5), 0, 255));
}

uint paletteColor(uint base, int region, int qp, int cqp) {
    int v[3];
    for (int plane = 0; plane < 3; ++plane) {
        int i = (region * 3 + plane) * 3;
        int q = signedByte(plan.planar[base + 17u + uint(i >> 2)], i & 3);
        v[plane] = clamp(128 + dequant(q, plane == 0 ? qp : cqp), 0, 255);
    }
    float cb = float(v[1]) - 128.0;
    float cr = float(v[2]) - 128.0;
    return roundedByte(float(v[0]) + 1.5748 * cr) |
           (roundedByte(float(v[0]) - 0.1873 * cb - 0.4681 * cr) << 8) |
           (roundedByte(float(v[0]) + 1.8556 * cb) << 16) | 0xff000000u;
}

vec4 rgba8(uint x) {
    return vec4(float(x & 255u), float((x >> 8u) & 255u),
                float((x >> 16u) & 255u), float(x >> 24u)) / 255.0;
}

void main() {
    uint tilesX = (uint(pc.width) + 63u) / 64u;
    uint tile = uint(gl_InstanceIndex);
    uint tileX = tile % tilesX, tileY = tile / tilesX;
    vec2 corner = vec2((gl_VertexIndex == 1 || gl_VertexIndex == 4 || gl_VertexIndex == 5) ? 1.0 : 0.0,
                       (gl_VertexIndex == 2 || gl_VertexIndex == 3 || gl_VertexIndex == 5) ? 1.0 : 0.0);
    vec2 pixel = (vec2(tileX, tileY) + corner) * 64.0;
    gl_Position = vec4(pixel / vec2(float(pc.width), float(pc.height)) * 2.0 - 1.0, 0.0, 1.0);

    uint base = tile * 26u;
    uint rec = tiles.records[tile * 4u + 1u];
    int delta = int((rec >> 8u) & 63u);
    if (delta >= 32) delta -= 64;
    int qp = clamp(pc.frameQP + delta, 0, 63);
    int cqp = clamp(qp + pc.chromaQPOffset, 0, 63);
    tileMap = uvec2(plan.planar[base + 1u], plan.planar[base + 2u]);
    uint c0 = paletteColor(base, 0, qp, cqp);
    uint c1 = paletteColor(base, 1, qp, cqp);
    color0 = rgba8(c0);
    color1 = rgba8(c1);
}
