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
    mat4 prev_view_proj;
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
	uint debug_cluster;

	// --- CSM receiver metrics (must match vg_common.glsl / UniformBufferObject) ---
	vec4 cascade_texel_size;   // world metres per shadow-map texel, per cascade
	vec4 cascade_depth_range;  // light-space slab thickness (metres), per cascade
	float shadow_receiver_bias_texels; // residual depth bias, in shadow texels
	float shadow_normal_offset_texels; // shadow normal offset, in shadow texels

	// --- TAA (appended, std140 offsets locked by C++ static_assert) ---
	mat4 unjittered_inv_view_proj;
	mat4 prev_unjittered_view_proj;
	vec4 jitter_offset; // xy: pixel offset, zw: ndc offset
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
#include "translucency_common.glsl"

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

    if (frag_blend_factor > 0.0 && frag_blend_factor < 1.0 && mat.alpha_mode != 2u) {
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
    roughness = clamp(roughness, 0.02, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    // Stable Screen UV computation
    ivec2 screen_res = textureSize(tex_samplers[997], 0);
    if (screen_res.x <= 1) screen_res = textureSize(tex_samplers[998], 0);
    if (screen_res.x <= 1) screen_res = textureSize(tex_samplers[995], 0);
    if (screen_res.x <= 1) screen_res = textureSize(tex_samplers[996], 0);
    vec2 screen_uv = gl_FragCoord.xy / vec2(max(ivec2(1), screen_res));

    // Screen-space AO sampling from bindless slot 998
    ivec2 ao_tex_size = textureSize(tex_samplers[998], 0);
    float ao = 1.0;
    if (ao_tex_size.x > 1 && ao_tex_size.y > 1) {
        ao = texture(tex_samplers[998], screen_uv).r;
    }

    // Screen-Space Global Illumination (SSGI) from bindless slot 996
    vec4 ssgi_sample = vec4(0.0);
    ivec2 ssgi_tex_size = textureSize(tex_samplers[996], 0);
    if (ssgi_tex_size.x > 1 && ssgi_tex_size.y > 1) {
        ssgi_sample = texture(tex_samplers[996], screen_uv);
    }

    // Screen-Space Reflections (SSR) from bindless slot 997
    vec4 ssr_sample = vec4(0.0);
    ivec2 ssr_tex_size = textureSize(tex_samplers[997], 0);
    if (ssr_tex_size.x > 1 && ssr_tex_size.y > 1) {
        ssr_sample = texture(tex_samplers[997], screen_uv);
    }

    vec3 geom_N = normalize(frag_normal);
    if (!gl_FrontFacing) {
        geom_N = -geom_N;
    }
    vec3 N = geom_N;
    if (mat.normal_texture_id > 0u && mat.normal_texture_id < 1000u) {
        vec3 normal_sample = texture(tex_samplers[nonuniformEXT(mat.normal_texture_id)], frag_tex_coord).xyz * 2.0 - 1.0;
        vec3 dp1 = dFdx(frag_world_pos);
        vec3 dp2 = dFdy(frag_world_pos);
        vec2 duv1 = dFdx(frag_tex_coord);
        vec2 duv2 = dFdy(frag_tex_coord);
        duv1 -= round(duv1);
        duv2 -= round(duv2);

        vec3 dp2perp = cross(dp2, geom_N);
        vec3 dp1perp = cross(geom_N, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

        float det = max(dot(T, T), dot(B, B));
        if (det > 1e-12) {
            float invmax = inversesqrt(det);
            mat3 TBN = mat3(T * invmax, B * invmax, geom_N);
            vec3 perturbed_N = TBN * normal_sample;
            float n_len_sq = dot(perturbed_N, perturbed_N);
            if (n_len_sq > 1e-12) {
                N = perturbed_N * inversesqrt(n_len_sq);
            }
        }
    }

    vec3 V = normalize(ubo.cam_pos - frag_world_pos);
    float mat_opacity = clamp(albedo_sample.a, 0.0, 1.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NdotV = clamp(dot(N, V), 0.001, 1.0);
    vec3 F = FresnelSchlick(NdotV, F0);

    vec3 L = normalize(ubo.light_dir);
    float NdotL = max(dot(N, L), 0.0);
    vec3 H = normalize(V + L);

    // Direct Cook-Torrance Specular (Sun / Directional Light)
    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3 F_dir = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 direct_specular = (NDF * G * F_dir) / (4.0 * NdotV * NdotL + 0.0001);
    float shadow = ShadowCalculation(frag_world_pos, geom_N, L);
    direct_specular *= ubo.light_color * ubo.light_intensity * NdotL * (1.0 - shadow);

    // Hemispheric Ambient Irradiance
    vec3 sky_ambient = vec3(0.7, 0.8, 1.0) * max(ubo.ambient_strength, 0.45);
    vec3 ground_ambient = vec3(0.5, 0.42, 0.35) * max(ubo.ambient_strength, 0.45);
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ambient_irradiance = mix(ground_ambient, sky_ambient, hemi);
    float ao_factor = mix(0.4, 1.0, clamp(ao, 0.0, 1.0));

    // Directional Environmental Specular Reflection
    vec3 refl_dir = reflect(-V, N);
    float hemi_spec = clamp(refl_dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 refl_ambient = mix(ground_ambient * 0.8, sky_ambient * 1.8, hemi_spec);

    // Double-layer Physical Fresnel (Reflections from both outer and inner glass surfaces)
    vec3 F_double = eval_double_layer_fresnel(F);
    vec3 ambient_specular = F_double * refl_ambient * (1.0 - roughness * 0.5) * 1.5;

    // Stable Screen-Space Reflections (SSR) from compute pass
    vec3 ssr_reflection = eval_ssr_reflection(ssr_sample, F_double, roughness, albedo, metallic);

    vec3 total_specular = direct_specular + ambient_specular + ssr_reflection;

    // PBR Diffuse Lighting (for translucent body, colored glass, bottle labels)
    vec3 direct_diffuse = (vec3(1.0) - F) * (1.0 - metallic) * (albedo / PI) * ubo.light_color * ubo.light_intensity * NdotL * (1.0 - shadow);
    vec3 ambient_diffuse = (vec3(1.0) - F) * (1.0 - metallic) * albedo * ambient_irradiance * ao_factor;
    vec3 ssgi_diffuse = ssgi_sample.rgb * albedo * (1.0 - metallic);
    vec3 total_diffuse = direct_diffuse + ambient_diffuse + ssgi_diffuse;

    // Emissive contribution
    vec3 emissive = vec3(0.0);
    if (mat.emissive_texture_id > 0u && mat.emissive_texture_id < 900u) {
        emissive = texture(tex_samplers[nonuniformEXT(mat.emissive_texture_id)], frag_tex_coord).rgb * 5.0;
    }

    // Material Classification & Physical Glass Volume via shared common function
    float texture_alpha = (mat.albedo_texture_id > 0u) ? albedo_sample.a : 0.0;
    float base_opacity = (mat.albedo_texture_id > 0u) ? 0.0 : mat.base_color_factor.a;

    TranslucentShadingResult glass_res = eval_translucent_body_and_alpha(
        albedo,
        base_opacity,
        texture_alpha,
        roughness,
        ambient_irradiance,
        direct_diffuse,
        total_diffuse,
        F_double
    );

    // Final Color: Premultiplied Specular + Material-tinted Body Diffuse + Emissive
    vec3 final_color = total_specular + glass_res.body_color + emissive;

    // Output Premultiplied Linear Color (Vulkan sRGB attachment handles sRGB conversion)
    out_color = vec4(ACESFilm(final_color * frag_color), glass_res.effective_alpha);
}
