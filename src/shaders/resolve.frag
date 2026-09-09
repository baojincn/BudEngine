#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_nonuniform_qualifier : enable

#include "common.glsl"
#include "vg_common.glsl"
// Recompile with spherical rotation-invariant CSM shadow calculation

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform usampler2D visibility_tex;

layout(set = 1, binding = 1) uniform sampler2D tex_samplers[];
layout(set = 1, binding = 2) uniform sampler2DArrayShadow shadow_map;

layout(std430, set = 1, binding = 7) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

layout(std430, set = 1, binding = 3) readonly buffer InstanceBuffer {
    HierarchyInstance data[];
} instance_buffer;

#include "lighting.glsl"

uint pcg_hash(uint seed) {
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 get_random_cluster_color(uint seed) {
    uint h = pcg_hash(seed + 1u);
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

void main() {
    ivec2 texel_coord = ivec2(gl_FragCoord.xy);
    uvec4 vis = texelFetch(visibility_tex, texel_coord, 0);

    if (vis.r == 0u && vis.a == 0u)
        discard;

    uint cluster_seed = vis.r & 0xFFFFu;
    uint material_id = (vis.r >> 16u) & 0xFFFFu;

    if (cluster_seed == 0xFFFFu && material_id == 0xFFFFu)
        discard;

    float depth = uintBitsToFloat(vis.g);
    vec2 uv = unpackHalf2x16(vis.b);
    vec4 normal_raw = unpackSnorm4x8(vis.a);
    vec3 N = normal_raw.xyz;
    // vis.a.w carries the per-instance receive-shadow flag (written by visibility.frag).
    float receive_shadow = (normal_raw.w < 0.0) ? 0.0 : 1.0;

    if (ubo.debug_cluster > 0u) {
        vec3 cluster_color = get_random_cluster_color(cluster_seed);
        vec3 L = normalize(ubo.light_dir.y < 0.0 ? -ubo.light_dir : vec3(0.5, 1.0, 0.3));
        float ndotl = max(dot(normalize(N), L), 0.25);
        out_color = vec4(cluster_color * ndotl, 1.0);
        return;
    }

    GPUMaterialData mat;
    if (material_id < materials.length())
        mat = materials[material_id];
    else {
        mat.base_color_factor = vec4(1.0, 1.0, 1.0, 1.0);
        mat.albedo_texture_id = 0u;
        mat.normal_texture_id = 0u;
        mat.metallic_roughness_id = 0u;
        mat.emissive_texture_id = 0u;
        mat.metallic_factor = 0.0;
        mat.roughness_factor = 0.5;
        mat.alpha_cutoff = 0.5;
        mat.alpha_mode = 0u;
    }

    vec2 screen_size = vec2(textureSize(visibility_tex, 0));
    vec2 screen_uv = gl_FragCoord.xy / screen_size;
    vec2 ndc_xy = screen_uv * 2.0 - 1.0;
    vec4 clip_pos = vec4(ndc_xy, depth, 1.0);
    vec4 world_pos_h = inverse(ubo.proj * ubo.view) * clip_pos;
    vec3 world_pos = world_pos_h.xyz / world_pos_h.w;

    vec3 geom_N = N;
    if (length(geom_N) < 0.1) {
        vec3 pos_dx = dFdx(world_pos);
        vec3 pos_dy = dFdy(world_pos);
        vec3 cross_n = cross(pos_dx, pos_dy);
        geom_N = (length(cross_n) > 1e-5) ? normalize(cross_n) : vec3(0.0, 1.0, 0.0);
    } else {
        geom_N = normalize(geom_N);
    }

    vec3 V = normalize(ubo.cam_pos - world_pos);
    if (dot(geom_N, V) < 0.0)
        geom_N = -geom_N;
    N = geom_N;

    vec2 uv_dx = dFdx(uv);
    vec2 uv_dy = dFdy(uv);
    // Wrap correction for repeating textures across [0, 1] boundaries
    uv_dx -= round(uv_dx);
    uv_dy -= round(uv_dy);
    const float max_derivative = 0.5;
    uv_dx = clamp(uv_dx, vec2(-max_derivative), vec2(max_derivative));
    uv_dy = clamp(uv_dy, vec2(-max_derivative), vec2(max_derivative));

    vec4 albedo_sample = mat.base_color_factor;
    if (mat.albedo_texture_id > 0u && mat.albedo_texture_id < 1000u) {
        albedo_sample *= textureGrad(tex_samplers[nonuniformEXT(mat.albedo_texture_id)], uv, uv_dx, uv_dy);
    }

    if (ubo.debug_cascades > 0u) {
        vec4 debug_view_pos = ubo.view * vec4(world_pos, 1.0);
        float debug_depth = -debug_view_pos.z;
        int debug_layer = 3;
        for (int i = 0; i < 4; ++i) {
            if (debug_depth < ubo.cascade_split_depths[i]) {
                debug_layer = i;
                break;
            }
        }
        vec3 cascade_colors[4] = vec3[](
            vec3(1.0, 0.2, 0.2), // Cascade 0: Red
            vec3(0.2, 1.0, 0.2), // Cascade 1: Green
            vec3(0.2, 0.4, 1.0), // Cascade 2: Blue
            vec3(1.0, 1.0, 0.2)  // Cascade 3: Yellow
        );
        vec3 L = normalize(ubo.light_dir);
        float ndotl = max(dot(geom_N, L), 0.25);
        vec3 debug_color = mix(albedo_sample.rgb * ndotl, cascade_colors[debug_layer] * ndotl, 0.7);
        out_color = vec4(debug_color, 1.0);
        return;
    }

    if (mat.normal_texture_id > 0u && mat.normal_texture_id < 1000u) {
        vec3 normal_sample = textureGrad(tex_samplers[nonuniformEXT(mat.normal_texture_id)], uv, uv_dx, uv_dy).xyz * 2.0 - 1.0;
        vec3 dp1 = dFdx(world_pos);
        vec3 dp2 = dFdy(world_pos);
        vec2 duv1 = dFdx(uv);
        vec2 duv2 = dFdy(uv);
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
    if (dot(N, V) < 0.0)
        N = -N;

    float metallic = mat.metallic_factor;
    float roughness = mat.roughness_factor;
    if (mat.metallic_roughness_id > 0u && mat.metallic_roughness_id < 1000u) {
        vec4 mr_sample = textureGrad(tex_samplers[nonuniformEXT(mat.metallic_roughness_id)], uv, uv_dx, uv_dy);
        roughness *= mr_sample.g;
        metallic *= mr_sample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    float ao = 1.0;
    ivec2 ao_tex_size = textureSize(tex_samplers[998], 0);
    if (ao_tex_size.x > 1 && ao_tex_size.y > 1) {
        ao = texture(tex_samplers[998], screen_uv).r;
    }

    // Screen-Space Global Illumination (SSGI) from bindless slot 996
    vec4 ssgi_sample = vec4(0.0);
    ivec2 ssgi_tex_size = textureSize(tex_samplers[996], 0);
    if (ssgi_tex_size.x > 1 && ssgi_tex_size.y > 1) {
        ssgi_sample = texture(tex_samplers[996], screen_uv);
    }
    vec3 ssgi_diffuse = ssgi_sample.rgb * albedo_sample.rgb * (1.0 - metallic);

    // Screen-Space Reflections (SSR) from bindless slot 997
    vec4 ssr_sample = vec4(0.0);
    ivec2 ssr_tex_size = textureSize(tex_samplers[997], 0);
    if (ssr_tex_size.x > 1 && ssr_tex_size.y > 1) {
        ssr_sample = texture(tex_samplers[997], screen_uv);
    }

    vec3 F0 = mix(vec3(0.04), albedo_sample.rgb, metallic);
    vec3 F_ssr = FresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 ssr_reflection = eval_ssr_reflection(ssr_sample, F_ssr, roughness, albedo_sample.rgb, metallic);

    vec3 albedo = albedo_sample.rgb;
    vec3 color = calculate_lighting(world_pos, N, geom_N, uv, mat, albedo, ao, metallic, roughness, receive_shadow);
    color += ssr_reflection + ssgi_diffuse;
    color = apply_tonemap_and_gamma(color);
    out_color = vec4(color, albedo_sample.a);
}
