#version 450
// Fragment companion: select an already-unpacked flat palette colour.
layout(location = 0) out vec4 outColor;
layout(location = 0) flat in uvec2 tileMap;
layout(location = 1) flat in vec4 color0;
layout(location = 2) flat in vec4 color1;
layout(push_constant) uniform PC { int frameQP; int chromaQPOffset; int width; int height; } pc;
void main() {
    if (gl_FragCoord.x >= float(pc.width) || gl_FragCoord.y >= float(pc.height)) { outColor = vec4(0.0); return; }
    uint cell = ((uint(gl_FragCoord.y) & 63u) >> 3u) * 8u + ((uint(gl_FragCoord.x) & 63u) >> 3u);
    uint map = cell < 32u ? tileMap.x : tileMap.y;
    uint label = (map >> (cell & 31u)) & 1u;
    outColor = label == 0u ? color0 : color1;
}
