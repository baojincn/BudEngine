#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec2 in_tex_coord;

layout(location = 0) out vec2 frag_tex_coord;

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 cascade_view_proj[4];
    vec4 cascade_split_depths;

    vec3 cam_pos;
    vec3 light_dir;
    vec3 light_color;
    float light_intensity;
    float ambient_strength;
    uint cascade_count;
    uint debug_cascades;
    uint reversed_z;
    uint padding[3];
} ubo;

layout(push_constant) uniform PushConsts {
    mat4 model;
    uint material_id;
    uint padding[3];
} push_consts;

void main() {
    vec4 world_pos = push_consts.model * vec4(in_position, 1.0);
    frag_tex_coord = in_tex_coord;
    gl_Position = ubo.proj * ubo.view * world_pos;
}
