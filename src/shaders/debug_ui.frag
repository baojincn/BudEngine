#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec4 fragment_color;
layout(location = 1) in vec2 fragment_uv;

layout(set = 0, binding = 1) uniform sampler2D bindless_textures[];

layout(push_constant) uniform push_constants {
    vec2 scale;
    vec2 translate;
    uint texture_id;
} push_data;

layout(location = 0) out vec4 output_color;

void main() {
    output_color = fragment_color * texture(bindless_textures[nonuniformEXT(push_data.texture_id)], fragment_uv);
}
