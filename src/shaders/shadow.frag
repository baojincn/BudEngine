#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 1) flat in uint frag_material_id;

layout(push_constant) uniform PushConsts {
    mat4 light_view_proj;
    vec4 light_dir;
} push;

layout(binding = 1) uniform sampler2D tex_samplers[];

void main() {
    uint tex_id = frag_material_id;

    // Page-streamed / untextured meshes (tex_id == 0) are opaque. Skip the
    // alpha test so we don't sample the uninitialized fallback texture, which
    // would cause garbage discards in the shadow map.
    if (tex_id == 0 || tex_id >= 1000u)
        return;

    float alpha = texture(tex_samplers[nonuniformEXT(tex_id)], frag_tex_coord).a;
    if (alpha < 0.5) {
        discard; 
    }
}
