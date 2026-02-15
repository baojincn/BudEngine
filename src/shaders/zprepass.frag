#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 frag_tex_coord;

layout(push_constant) uniform PushConsts {
    mat4 model;
    uint material_id;
    uint padding[3];
} push_consts;

layout(binding = 1) uniform sampler2D tex_samplers[];

void main() {
    uint tex_id = push_consts.material_id;
    float alpha = texture(tex_samplers[nonuniformEXT(tex_id)], frag_tex_coord).a;
    if (alpha < 0.5) {
        discard;
    }
}
