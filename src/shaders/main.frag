#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_tex_coord;
layout(location = 3) in vec3 frag_color;
layout(location = 4) flat in uint frag_material_id;
layout(location = 5) flat in float frag_blend_factor;

layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
	// [CSM]
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
	float shadow_bias_constant;
	float shadow_bias_slope;
	uint padding[1];
} ubo;

layout(binding = 1) uniform sampler2D tex_samplers[];
layout(binding = 2) uniform sampler2DArrayShadow shadow_map;

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

#include "lighting.glsl"

// Golden-angle based dithering threshold for LOD transitions
float golden_noise(ivec2 coord) {
    const float PHI = 1.61803398874989484820459;
    return fract(sin(dot(vec2(coord), vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    GPUMaterialData mat;
    if (frag_material_id < materials.length())
        mat = materials[frag_material_id];
    else {
        mat.base_color_factor = vec4(0.8, 0.8, 0.8, 1.0);
        mat.albedo_texture_id = 0u;
        mat.normal_texture_id = 0u;
        mat.metallic_roughness_id = 0u;
        mat.emissive_texture_id = 0u;
        mat.metallic_factor = 0.0;
        mat.roughness_factor = 0.5;
        mat.alpha_cutoff = 0.5;
        mat.alpha_mode = 0u;
    }

    vec4 albedo_sample = mat.base_color_factor;
    if (mat.albedo_texture_id > 0u && mat.albedo_texture_id < 1000u)
        albedo_sample *= texture(tex_samplers[nonuniformEXT(mat.albedo_texture_id)], frag_tex_coord);

    if (mat.alpha_mode == 1u && albedo_sample.a < mat.alpha_cutoff)
        discard;

    // LOD dithering: when blend_factor is between 0 and 1, use screen-space
    // golden noise to smoothly transition between LOD levels.
    // blend_factor = 0.0 → full high LOD (keep all fragments)
    // blend_factor = 1.0 → full low LOD (discard all fragments)
    // In transition, noise < blend_factor → discard (low LOD pixels fade out)
    if (frag_blend_factor > 0.0 && frag_blend_factor < 1.0) {
        // Golden noise (better spectral properties than IGN, reduces moire)
        float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        if (noise < frag_blend_factor)
            discard;
    }

    vec3 albedo = albedo_sample.rgb; 
    float metallic = mat.metallic_factor; 
    float roughness = mat.roughness_factor;

    if (mat.metallic_roughness_id > 0u && mat.metallic_roughness_id < 1000u) {
        vec4 mr_sample = texture(tex_samplers[nonuniformEXT(mat.metallic_roughness_id)], frag_tex_coord);
        roughness *= mr_sample.g;
        metallic *= mr_sample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    // Screen-space AO sampling from bindless slot 998 (slot 999 is reserved for ImGui Font Atlas)
    ivec2 ao_tex_size = textureSize(tex_samplers[998], 0);
    float ao = 1.0;
    if (ao_tex_size.x > 1 && ao_tex_size.y > 1) {
        vec2 screen_uv = gl_FragCoord.xy / vec2(ao_tex_size);
        ao = texture(tex_samplers[998], screen_uv).r;
    }

    vec3 N = normalize(frag_normal);
    if (mat.normal_texture_id > 0u && mat.normal_texture_id < 1000u) {
        vec3 normal_sample = texture(tex_samplers[nonuniformEXT(mat.normal_texture_id)], frag_tex_coord).xyz * 2.0 - 1.0;
        vec3 Q1 = dFdx(frag_world_pos);
        vec3 Q2 = dFdy(frag_world_pos);
        vec2 st1 = dFdx(frag_tex_coord);
        vec2 st2 = dFdy(frag_tex_coord);
        vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
        vec3 B = -normalize(cross(N, T));
        mat3 TBN = mat3(T, B, N);
        N = normalize(TBN * normal_sample);
    }

        vec3 color = calculate_lighting(frag_world_pos, N, frag_tex_coord, mat, albedo, ao, metallic, roughness);

    out_color = vec4(color * frag_color, albedo_sample.a);
}
