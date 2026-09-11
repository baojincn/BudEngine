#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_nonuniform_qualifier : enable

#include "common.glsl"

layout(location = 0) flat in uint frag_material_id;
layout(location = 1) flat in float frag_blend_factor;
layout(location = 2) flat in uint frag_instance_id;
layout(location = 3) in vec2 frag_tex_coord;
layout(location = 4) in vec3 frag_normal;
layout(location = 5) flat in uint frag_cluster_id;
layout(location = 6) flat in uint frag_instance_flags;

layout(location = 0) out uvec4 out_visibility;

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

layout(set = 0, binding = 1) uniform sampler2D tex_samplers[];
layout(std430, set = 0, binding = 7) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

void main() {
    if (frag_material_id < materials.length()) {
        GPUMaterialData mat = materials[frag_material_id];
        if (mat.alpha_mode == 1u) {
            float alpha = mat.base_color_factor.a;
            if (mat.albedo_texture_id > 0u && mat.albedo_texture_id < 1000u) {
                alpha *= texture(tex_samplers[nonuniformEXT(mat.albedo_texture_id)], frag_tex_coord).a;
            }
            if (alpha < mat.alpha_cutoff)
                discard;
        } else if (mat.alpha_mode == 2u) {
            discard;
        }
    }

    if (frag_blend_factor > 0.0 && frag_blend_factor < 1.0) {
        float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        if (noise < frag_blend_factor)
            discard;
    }

    uint instance_id = frag_instance_id & 0xFFFFu;
    uint material_id = frag_material_id & 0xFFFFu;

    out_visibility.r = (frag_cluster_id & 0xFFFFu) | (material_id << 16u);
    out_visibility.g = floatBitsToUint(gl_FragCoord.z);
    out_visibility.b = packHalf2x16(frag_tex_coord);
    float receive_shadow = ((frag_instance_flags & 4u) != 0u) ? -1.0 : 1.0;
    float n_len = length(frag_normal);
    vec3 safe_n = (n_len > 1e-4) ? (frag_normal / n_len) : vec3(0.0, 1.0, 0.0);
    out_visibility.a = packSnorm4x8(vec4(safe_n, receive_shadow));
}
