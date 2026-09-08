#version 450
layout(std430, binding = 1) readonly buffer Records { uint records[]; };
layout(push_constant) uniform PC { int frameQP; int chromaQPOffset; int width; int height; } pc;
layout(location = 0) flat out uvec2 tileMap;
layout(location = 1) flat out vec4 color0;
layout(location = 2) flat out vec4 color1;
vec4 rgba8(uint x) { return vec4(float(x & 255u), float((x >> 8u) & 255u), float((x >> 16u) & 255u), float(x >> 24u)) / 255.0; }
void main() {
    uint tilesX = (uint(pc.width) + 63u) / 64u, tile = uint(gl_InstanceIndex);
    uint tileX = tile % tilesX, tileY = tile / tilesX;
    vec2 corner = vec2((gl_VertexIndex == 1 || gl_VertexIndex == 4 || gl_VertexIndex == 5) ? 1.0 : 0.0,
                       (gl_VertexIndex == 2 || gl_VertexIndex == 3 || gl_VertexIndex == 5) ? 1.0 : 0.0);
    vec2 pixel = (vec2(tileX, tileY) + corner) * 64.0;
    gl_Position = vec4(pixel / vec2(float(pc.width), float(pc.height)) * 2.0 - 1.0, 0.0, 1.0);
    uint base = tile * 4u;
    tileMap = uvec2(records[base], records[base + 1u]);
    color0 = rgba8(records[base + 2u]);
    color1 = rgba8(records[base + 3u]);
}
