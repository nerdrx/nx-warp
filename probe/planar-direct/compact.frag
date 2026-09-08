#version 450
// Compact PLANAR display proof: one bit per 8x8 cell selects one of two
// packed RGBA8 palette entries.  The record buffer is tile-major, four uints
// per 64x64 tile: words 0/1 are the 64-bit cell label map, words 2/3 are the
// two RGBA8 palette colours.  Frame QP and chromaQPOffset are reserved push
// fields so this fragment can share the direct probe's interface.
layout(location = 0) out vec4 outColor;
layout(std430, binding = 1) readonly buffer Records { uint records[]; };
layout(push_constant) uniform PC {
    uint frameQP;
    uint chromaQPOffset;
    uint width;
    uint height;
} pc;

vec4 rgba8(uint x) {
    return vec4(float(x & 255u), float((x >> 8u) & 255u),
                float((x >> 16u) & 255u), float(x >> 24u)) / 255.0;
}

void main() {
    // Vulkan fragment coordinates use the top-left raster origin.
    if (gl_FragCoord.x >= float(pc.width) ||
        gl_FragCoord.y >= float(pc.height)) { outColor = vec4(0.0); return; }
    uint x = uint(gl_FragCoord.x);
    uint y = uint(gl_FragCoord.y);
    uint tilesX = (pc.width + 63u) / 64u;
    uint tile = (y >> 6u) * tilesX + (x >> 6u);
    uint cell = ((y & 63u) >> 3u) * 8u + ((x & 63u) >> 3u);
    uint map = records[tile * 4u + (cell >> 5u)];
    uint label = (map >> (cell & 31u)) & 1u;
    outColor = rgba8(records[tile * 4u + 2u + label]);
}
