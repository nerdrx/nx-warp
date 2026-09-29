#version 450
layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    color = texture(source_image, uv);
}
