#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 1) flat in uint frag_material_id;

layout(binding = 1) uniform sampler2D tex_samplers[];

struct GPUMaterialData {
    vec4 base_color_factor;
    uint albedo_texture_id;
    uint normal_texture_id;
    uint metallic_roughness_id;
    uint emissive_texture_id;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint alpha_mode;
};

layout(std430, set = 0, binding = 7) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

void main() {
    if (frag_material_id >= materials.length())
        return;

    GPUMaterialData mat = materials[frag_material_id];
    if (mat.alpha_mode == 1u) {
        if (mat.albedo_texture_id > 0u && mat.albedo_texture_id < 1000u) {
            float alpha = texture(tex_samplers[nonuniformEXT(mat.albedo_texture_id)], frag_tex_coord).a;
            if (alpha < mat.alpha_cutoff) {
                discard;
            }
        }
    }
}
