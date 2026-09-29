#version 450
layout(location = 0) out vec2 uv;
void main() {
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
    uv = (p[gl_VertexIndex] + 1.0) * 0.5;
}
