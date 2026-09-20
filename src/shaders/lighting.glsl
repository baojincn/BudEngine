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

// Roughness-dependent Fresnel for ambient/IBL (UE4 Epic split-sum approximation)
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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

    // (1) Shadow Normal Offset in WORLD metres. Sized from this cascade's own texel
    //     footprint. Scaled tightly with (1.0 - ndl) to protect grazing surfaces while
    //     anchoring contact shadows firmly to caster edges without detachment (Peter Panning).
    float normal_scale = clamp(1.0 - ndl, 0.15, 1.0);
    vec3 biased_pos = world_pos + N * (texel * ubo.shadow_normal_offset_texels * normal_scale);

    vec4 frag_pos_light_space = ubo.cascade_view_proj[layer] * vec4(biased_pos, 1.0);
    vec3 proj_coords = frag_pos_light_space.xyz / frag_pos_light_space.w;

    // NDC -> [0, 1]
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    // PCF texel size and filter spread
    vec2 texel_size = 1.0 / textureSize(shadow_map, 0).xy;
    float spread = max(1.0, 2.5 / (1.0 + float(layer) * 0.4));

    // Check if within bounds of this cascade, accounting for PCF filter radius
    float pcf_margin = spread * max(texel_size.x, texel_size.y) * 1.5;
    if (proj_coords.x < pcf_margin || proj_coords.x > (1.0 - pcf_margin) ||
        proj_coords.y < pcf_margin || proj_coords.y > (1.0 - pcf_margin) ||
        proj_coords.z < 0.0 || proj_coords.z > 1.0)
        return -1.0;

    // (2) Residual depth bias with slope scaling to keep contact shadows tight.
    float slope_factor = clamp(1.0 - ndl, 0.0, 1.0);
    float bias = ubo.shadow_receiver_bias_texels * (0.25 + 0.75 * slope_factor) * texel / max(ubo.cascade_depth_range[layer], 1e-4);

    float shadow_sum = 0.0;
    bool is_reversed = ubo.reversed_z != 0u;
    for(int i = 0; i < 16; ++i) {
        vec2 offset = poissonDisk[i] * texel_size * spread;
        vec2 sample_uv = clamp(proj_coords.xy + offset, vec2(0.5 * texel_size), vec2(1.0 - 0.5 * texel_size));
        float pcf_depth = is_reversed ? (proj_coords.z + bias) : (proj_coords.z - bias);
        shadow_sum += texture(shadow_map, vec4(sample_uv, float(layer), pcf_depth));
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

    // All cascade bounds missed (fragment very far or at edge). Force-sample the coarsest
    // cascade without the bounds check to avoid light-leak on seam gaps.
    return SampleCascadeRaw(3, world_pos, N, L) < 0.0 ? 0.0
           : SampleCascadeRaw(3, world_pos, N, L);
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
vec3 calculate_lighting(vec3 world_pos, vec3 normal, vec3 geom_normal, vec2 tex_coord,
                        GPUMaterialData mat, vec3 albedo, float ao,
                        float metallic, float roughness, float receive_shadow) {
    vec3 N = normalize(normal);
    vec3 geom_N = normalize(geom_normal);
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

    // For dielectrics (cloth, fabric, plaster, wood), soft micro-fiber scattering suppresses
    // harsh mirror specular highlights so the material looks like fabric rather than shiny plastic.
    if (metallic < 0.2)
        specular *= (1.0 - roughness) * (1.0 - roughness);

    // Dual-Layer Clearcoat (Automotive metallic car paint / luxury robotics finish)
    // Clearcoat creates a razor-sharp glassy outer reflection over the deep metallic base
    if (metallic > 0.5) {
        float cc_roughness = 0.08;
        float cc_NDF = DistributionGGX(N, H, cc_roughness);
        float cc_G   = GeometrySmith(N, V, L, cc_roughness);
        vec3 cc_F    = FresnelSchlick(max(dot(H, V), 0.0), vec3(0.04));
        vec3 cc_specular = (cc_NDF * cc_G * cc_F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);
        specular += cc_specular * 0.45;
    }

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    float NdotL = max(dot(N, L), 0.0);

    vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    // receive_shadow == 0 means this surface opted out (per-instance flag): skip the
    // shadow term entirely - which also skips every PCF sample, so opting out is free.
    // Use geometric normal (geom_N) for shadow normal bias to avoid normal-map self-shadow acne.
    float shadow = (receive_shadow > 0.0) ? ShadowCalculation(world_pos, geom_N, L) : 0.0;

    // Apply Shadow
    Lo *= (1.0 - shadow);

    // ---------------------------------------------------------------------
    // Physically-Based Ambient Lighting
    // ---------------------------------------------------------------------
    float NdotV = max(dot(N, V), 0.001);

    // Hemispheric Sky/Ground Ambient Irradiance (proportional to scene ambient_strength)
    vec3 sky_ambient = vec3(0.55, 0.65, 0.85) * ubo.ambient_strength;
    vec3 ground_ambient = vec3(0.35, 0.28, 0.22) * ubo.ambient_strength;
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ambient_irradiance = mix(ground_ambient, sky_ambient, hemi);

    // Ambient Diffuse:
    // For dielectrics (cloth, stone, wood), Lambertian multi-scattering preserves diffuse energy
    // at grazing angles without artificial darkening. Only metals extinguish diffuse.
    float ao_factor = mix(0.4, 1.0, clamp(ao, 0.0, 1.0));
    vec3 ambient_diffuse = (1.0 - metallic) * ambient_irradiance * albedo * ao_factor;

    // Environmental Specular Reflection:
    vec3 refl_dir = reflect(-V, N);
    float hemi_spec = clamp(refl_dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 refl_radiance = mix(ground_ambient * 0.6, sky_ambient * 1.2, hemi_spec);

    vec3 ambient_specular = vec3(0.0);

    if (metallic > 0.1) {
        // Metallic surfaces (G1 robotics finish / chrome / car paint)
        vec3 metal_refl_radiance = mix(ground_ambient * 0.8, sky_ambient * 1.8, hemi_spec);
        float sun_spec_bounce = pow(max(dot(refl_dir, L), 0.0), 8.0) * (1.0 - roughness);
        vec3 metal_radiance = metal_refl_radiance + ubo.light_color * ubo.light_intensity * sun_spec_bounce * 0.25 * (1.0 - shadow);

        vec3 F_env = FresnelSchlickRoughness(NdotV, F0, roughness);
        float spec_roughness_fade = (1.0 - roughness) * (1.0 - roughness);
        ambient_specular += metal_radiance * F_env * spec_roughness_fade * ao_factor * metallic;
    } else {
        // Dielectrics (cloth, marble, wood, plaster):
        // Only very smooth dielectrics (roughness < 0.3, e.g. glass, glossy tiles) exhibit subtle environmental specular.
        // Rough dielectrics like cloth (roughness >= 0.5) produce negligible ambient reflection, eliminating white haze.
        float dielectric_spec_fade = pow(clamp(1.0 - roughness, 0.0, 1.0), 3.0);
        ambient_specular += refl_radiance * 0.04 * dielectric_spec_fade * ao_factor;
    }

    vec3 color = ambient_diffuse + ambient_specular + Lo;

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

// Convenience overload: single normal provided
vec3 calculate_lighting(vec3 world_pos, vec3 normal, vec2 tex_coord,
                        GPUMaterialData mat, vec3 albedo, float ao,
                        float metallic, float roughness, float receive_shadow) {
    return calculate_lighting(world_pos, normal, normal, tex_coord, mat, albedo, ao,
                              metallic, roughness, receive_shadow);
}

// Convenience overload for receivers that have no per-instance flag available (the
// forward translucent pass): shadows are always received.
vec3 calculate_lighting(vec3 world_pos, vec3 normal, vec2 tex_coord,
                        GPUMaterialData mat, vec3 albedo, float ao,
                        float metallic, float roughness) {
    return calculate_lighting(world_pos, normal, normal, tex_coord, mat, albedo, ao,
                              metallic, roughness, 1.0);
}

vec3 apply_tonemap_and_gamma(vec3 linear_color) {
    vec3 mapped = ACESFilm(linear_color);
    return pow(mapped, vec3(1.0 / 2.2));
}

// Public common function: Screen-Space Reflections evaluation
vec3 eval_ssr_reflection(vec4 ssr_sample, vec3 F, float roughness, vec3 albedo, float metallic) {
    if (roughness > 0.25 || ssr_sample.a <= 1e-4)
        return vec3(0.0);
    // Non-metals (stone, brick, cloth) must be mirror-smooth (< 0.05) to reflect
    if (metallic < 0.20 && roughness > 0.05)
        return vec3(0.0);
    float roughness_fade = 1.0 - smoothstep(0.04, 0.25, roughness);
    vec3 specular_tint = mix(vec3(1.0), albedo, metallic);
    return ssr_sample.rgb * F * roughness_fade * specular_tint * ssr_sample.a;
}