#version 450

layout(location = 0) in vec2 vertex_position;
layout(location = 1) in vec2 vertex_uv;
layout(location = 2) in vec4 vertex_color;

layout(push_constant) uniform push_constants {
    vec2 scale;
    vec2 translate;
    uint texture_id;
} push_data;

layout(location = 0) out vec4 fragment_color;
layout(location = 1) out vec2 fragment_uv;

void main() {
    fragment_color = vertex_color;
    fragment_uv = vertex_uv;
    gl_Position = vec4(vertex_position * push_data.scale + push_data.translate, 0.0, 1.0);
}
