// Shared PBR lighting functions — included by resolve.frag and main.frag.
// Requires: ubo, tex_samplers, shadow_map, MaterialBuffer to be defined in the including shader.

const float PI = 3.14159265359;

// ACES 电影级色调映射
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return num / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec2 poissonDisk[16] = vec2[](
   vec2( -0.94201624, -0.39906216 ),
   vec2( 0.94558609, -0.76890725 ),
   vec2( -0.094184101, -0.92938870 ),
   vec2( 0.34495938, 0.29387760 ),
   vec2( -0.91588581, 0.45771432 ),
   vec2( -0.81544232, -0.87912464 ),
   vec2( -0.38277543, 0.27676845 ),
   vec2( 0.97484398, 0.75648379 ),
   vec2( 0.44323325, -0.97511554 ),
   vec2( 0.53742981, -0.47373420 ),
   vec2( -0.26496911, -0.41893023 ),
   vec2( 0.79197514, 0.19090188 ),
   vec2( -0.24188840, 0.99706507 ),
   vec2( -0.81409955, 0.91437590 ),
   vec2( 0.19984126, 0.78641367 ),
   vec2( 0.14383161, -0.14100790 )
);

float SampleCascadeRaw(int layer, vec3 world_pos, vec3 N, vec3 L) {
    // cascade_texel_size == 0 marks an unconfigured cascade (cascade_count < 4):
    // bail out instead of sampling a layer that does not exist in the map array.
    float texel = ubo.cascade_texel_size[layer];
    if (texel <= 0.0) {
        return -1.0;
    }

    float ndl = clamp(dot(N, L), 0.0, 1.0);

    // (1) Shadow Normal Offset, in WORLD metres. Sized from this cascade's own texel
    //     footprint, so it is the same physical bias on cascade 0 and cascade 3.
    vec3 biased_pos = world_pos + N * (texel * ubo.shadow_normal_offset_texels *
                                       (0.5 + 1.5 * (1.0 - ndl)));

    vec4 frag_pos_light_space = ubo.cascade_view_proj[layer] * vec4(biased_pos, 1.0);
    vec3 proj_coords = frag_pos_light_space.xyz / frag_pos_light_space.w;

    // NDC -> [0, 1]
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    // Check if within bounds of this cascade
    if (proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0 ||
        proj_coords.z < 0.0 || proj_coords.z > 1.0) {
        return -1.0;
    }

    // (2) Residual depth bias, expressed as a number of shadow texels of light-space
    //     thickness and converted with THIS cascade's own depth slab.
    float bias = ubo.shadow_receiver_bias_texels * texel / max(ubo.cascade_depth_range[layer], 1e-4);

    // PCF
    float shadow_sum = 0.0;
    vec2 texel_size = 1.0 / textureSize(shadow_map, 0).xy;

    // Spread: normalize world-space filter size across cascade layers
    float spread = max(1.0, 2.5 / (1.0 + float(layer) * 0.4));

    bool is_reversed = ubo.reversed_z != 0u;
    for(int i = 0; i < 16; ++i) {
        vec2 offset = poissonDisk[i] * texel_size * spread;
        float pcf_depth = is_reversed ? (proj_coords.z + bias) : (proj_coords.z - bias);
        shadow_sum += texture(shadow_map, vec4(proj_coords.xy + offset, float(layer), pcf_depth));
    }

    return 1.0 - (shadow_sum / 16.0);
}

float SampleCascade(int layer, vec3 world_pos, vec3 N, vec3 L) {
    return SampleCascadeRaw(layer, world_pos, N, L);
}

float ShadowCalculation(vec3 world_pos, vec3 N, vec3 L) {
    // 1. View-space depth for cascade selection (must match the view-space
    //    split depths computed by update_cascades in renderer.cpp).
    vec4 view_pos = ubo.view * vec4(world_pos, 1.0);
    float depth = -view_pos.z;

    int layer = 3;
    for (int i = 0; i < 4; ++i) {
        if (depth < ubo.cascade_split_depths[i]) {
            layer = i;
            break;
        }
    }

    // Try sampling the selected cascade. If the fragment lies slightly outside this cascade's
    // light ortho bounds (e.g. at the far boundary or due to normal bias), fallback to the next
    // coarser cascade layer so we never leak unshadowed light on seams.
    for (int l = layer; l < 4; ++l) {
        float shadow = SampleCascade(l, world_pos, N, L);
        if (shadow >= 0.0) {
            return shadow;
        }
    }

    return 0.0;
}

// Main lighting entry point — called from resolve.frag and main.frag.
// Parameters:
//   world_pos  - fragment world position
//   normal     - fragment world normal (already perturbed by normal map if any)
//   tex_coord  - UV coordinate
//   mat        - material data from the material buffer
//   albedo     - base color (with alpha already sampled)
//   ao         - ambient occlusion factor (1.0 = no occlusion)
//   metallic   - metallic factor
//   roughness  - roughness factor
// Returns: final linear color (before tone-mapping & gamma)
vec3 calculate_lighting(vec3 world_pos, vec3 normal, vec2 tex_coord,
                        GPUMaterialData mat, vec3 albedo, float ao,
                        float metallic, float roughness, float receive_shadow) {
    vec3 N = normalize(normal);
    vec3 V = normalize(ubo.cam_pos - world_pos);
    vec3 L = normalize(ubo.light_dir);
    vec3 H = normalize(V + L);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    vec3 light_color = ubo.light_color;
    float light_intensity = ubo.light_intensity;
    vec3 radiance = light_color * light_intensity;

    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3 F    = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator    = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    float NdotL = max(dot(N, L), 0.0);

    vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    // receive_shadow == 0 means this surface opted out (per-instance flag): skip the
    // shadow term entirely - which also skips every PCF sample, so opting out is free.
    float shadow = (receive_shadow > 0.0) ? ShadowCalculation(world_pos, N, L) : 0.0;



    // Apply Shadow
    Lo *= (1.0 - shadow);

    // Hemispheric Sky/Ground Ambient Irradiance (prevents indoor pitch-black shadows)
    vec3 sky_ambient = vec3(0.7, 0.8, 1.0) * max(ubo.ambient_strength, 0.45);
    vec3 ground_ambient = vec3(0.5, 0.42, 0.35) * max(ubo.ambient_strength, 0.45);
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ambient_irradiance = mix(ground_ambient, sky_ambient, hemi);

    float ao_factor = mix(0.4, 1.0, clamp(ao, 0.0, 1.0));
    vec3 ambient = ambient_irradiance * albedo * ao_factor;

    // Ambient Specular Reflection for smooth surfaces
    vec3 ambient_specular = mix(vec3(0.04), albedo, metallic) * ambient_irradiance * (1.0 - roughness) * 0.5;

    vec3 color = ambient + ambient_specular + Lo;

    // Emissive contribution (e.g. fire pit)
    if (mat.emissive_texture_id > 0u && mat.emissive_texture_id < 900u) {
        vec3 emissive_sample = texture(tex_samplers[nonuniformEXT(mat.emissive_texture_id)], tex_coord).rgb;
        color += emissive_sample * 5.0;
    }

    // [DEBUG] Toggle this to visualize cascades
    if (ubo.debug_cascades > 0) {
        int debugLayer = -1;
        vec4 debugViewPos = ubo.view * vec4(world_pos, 1.0);
        float debugDepth = -debugViewPos.z;
        vec3 cascadeColors[4] = vec3[](
            vec3(1.0, 0.1, 0.1),
            vec3(0.1, 1.0, 0.1),
            vec3(0.1, 0.1, 1.0),
            vec3(1.0, 1.0, 0.1)
        );
        for (int i = 0; i < 4; ++i) {
            if (debugDepth < ubo.cascade_split_depths[i]) {
                debugLayer = i;
                break;
            }
        }
        if (debugLayer == -1)
            debugLayer = 3;
        color = mix(color, albedo * cascadeColors[debugLayer], 0.5);
    }

    return color;
}

// Convenience overload for receivers that have no per-instance flag available (the
// forward translucent pass): shadows are always received.
vec3 calculate_lighting(vec3 world_pos, vec3 normal, vec2 tex_coord,
                        GPUMaterialData mat, vec3 albedo, float ao,
                        float metallic, float roughness) {
    return calculate_lighting(world_pos, normal, tex_coord, mat, albedo, ao,
                              metallic, roughness, 1.0);
}

vec3 apply_tonemap_and_gamma(vec3 linear_color) {
    vec3 mapped = ACESFilm(linear_color);
    return pow(mapped, vec3(1.0 / 2.2));
}

// Public common function: Screen-Space Reflections evaluation
vec3 eval_ssr_reflection(vec4 ssr_sample, vec3 F, float roughness, vec3 albedo, float metallic) {
    float roughness_fade = smoothstep(0.25, 0.05, roughness);
    vec3 specular_tint = mix(vec3(1.0), albedo, metallic);
    return ssr_sample.rgb * F * roughness_fade * specular_tint * ssr_sample.a;
}