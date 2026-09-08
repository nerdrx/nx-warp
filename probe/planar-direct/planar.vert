#version 450
// Standalone PLANAR proof-of-concept.  The fragment shader receives a full
// stereo raster; gl_VertexIndex draws one full-screen triangle.
layout(location=0) out vec2 uv;
void main() {
  vec2 p = (gl_VertexIndex == 1) ? vec2(3.0,-1.0) :
           (gl_VertexIndex == 2) ? vec2(-1.0,3.0) : vec2(-1.0,-1.0);
  uv = p * 0.5 + 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
}
