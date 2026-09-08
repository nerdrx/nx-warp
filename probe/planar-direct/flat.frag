#version 450
// Flat PLANAR palette experiment.  Scope and ABI match planar.frag:
// binding 0 is 26 uints/tile (header,map[16],coefficients); binding 1 is four
// packed RGBA8 palette entries per tile, indexed by region.  The CPU has
// dequantized each region's constant colour into this palette; coefficients,
// slopes and colour conversion are intentionally absent from this shader.
// Push constants: { int frameQP; int chromaQPOffset; int width; int height; }.
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0,std430) readonly buffer Planar { uint w[]; } plan;
layout(set=0,binding=1,std430) readonly buffer Palette { uint rgba[]; } palette;
layout(push_constant) uniform PC { int frameQP; int chromaQPOffset; int width; int height; } pc;

int regionAt(int base, int cell, int bitsPerLabel) {
  // Labels are byte-packed in map[16], with 1 or 2 bits per cell.
  int bit = cell * bitsPerLabel;
  int byte = bit >> 3;
  return int((plan.w[base + 1 + (byte >> 2)] >>
              (((byte & 3) * 8) + (bit & 7))) &
             uint((1 << bitsPerLabel) - 1));
}

void main() {
  ivec2 p = ivec2(clamp(uv * vec2(pc.width, pc.height), vec2(0),
                        vec2(pc.width - 1, pc.height - 1)));
  int tilesX = (pc.width + 63) >> 6;
  int tile = (p.y >> 6) * tilesX + (p.x >> 6);
  int base = tile * 26;
  uint header = plan.w[base];
  int regions = int(header & 3u) + 2;
  int mlog = ((header >> 3) & 1u) != 0u ? 4 : 3;
  int cells = 1 << mlog;
  int labels = regions > 2 ? 2 : 1;
  int localX = p.x & 63;
  int localY = p.y & 63;
  int cell = (localY >> (6 - mlog)) * cells + (localX >> (6 - mlog));
  int region = clamp(regionAt(base, cell, labels), 0, regions - 1);
  outColor = unpackUnorm4x8(palette.rgba[tile * 4 + region]);
}
