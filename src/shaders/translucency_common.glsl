#ifndef TRANSLUCENCY_COMMON_GLSL
#define TRANSLUCENCY_COMMON_GLSL

// 计算电介质菲涅尔反射系数 (Dielectric Fresnel - Schlick Approximation)
// ior: 介质折射率 (玻璃约为 1.52, 水约为 1.33)
float eval_dielectric_fresnel(float NdotV, float ior) {
    float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    return f0 + (1.0 - f0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
}

// 计算双层电介质表面菲涅尔反射 (Double-layer Physical Fresnel for Thin/Hollow Glass)
// 考虑光线在内外玻璃表面的二次反射
vec3 eval_double_layer_fresnel(vec3 F) {
    return F * (vec3(2.0) - F);
}

// 计算屏幕空间折射 UV 偏移 (Pixel Normal Offset Refraction)
// screen_uv: 当前像素归一化屏幕坐标 [0, 1]
// N: 世界空间/视空间法线
// roughness: 材质粗糙度 (粗糙度越小折射越锐利)
// ior: 折射率
vec2 eval_refraction_uv(vec2 screen_uv, vec3 N, float roughness, float ior) {
    // 根据法线在投影平面的方向进行光线偏折估算
    vec2 offset = N.xy * (ior - 1.0) * 0.04 * (1.0 - roughness * 0.5);
    return clamp(screen_uv + offset, vec2(0.001), vec2(0.999));
}

// 比尔-朗伯定律介质吸收 (Beer-Lambert Law Absorption)
// incoming_light: 穿透玻璃的背景光
// absorption_tint: 玻璃介质的本征颜色 (BaseColor)
// thickness: 光线穿过介质的估算厚度
vec3 eval_beer_lambert(vec3 incoming_light, vec3 absorption_tint, float thickness) {
    // 将基础色转化为吸收系数 sigma_a
    vec3 sigma_a = max(vec3(0.0), vec3(1.0) - absorption_tint) * 2.0;
    return incoming_light * exp(-sigma_a * thickness);
}

// 半透明物体屏幕空间光线追踪反射 (Screen-Space Raymarching for Translucents with Temporal Reprojection)
// view_pos: 视空间表面坐标
// view_refl: 视空间物理反射光线向量
// proj: 当前投影矩阵
// prev_view_proj: 前一帧 ViewProj 矩阵 (用于重投影消除拖影)
// inv_view: 当前 View 矩阵的逆矩阵 (将视空间命中点转换为世界空间)
// scene_color_tex: 历史场景颜色缓冲 (HistorySceneColor)
// roughness: 表面粗糙度
vec4 eval_translucent_ssr(vec3 view_pos, vec3 view_refl, mat4 proj, mat4 prev_view_proj, mat4 inv_view, sampler2D scene_color_tex, float roughness) {
    vec3 hit_color = vec3(0.0);
    float hit_weight = 0.0;
    ivec2 tex_sz = textureSize(scene_color_tex, 0);
    if (tex_sz.x > 1 && tex_sz.y > 1) {
        float step_d = 0.15;
        for (int s = 1; s <= 12; ++s) {
            vec3 p = view_pos + view_refl * (float(s) * step_d);
            vec4 clip_p = proj * vec4(p, 1.0);
            if (clip_p.w <= 0.001) break;
            vec2 s_uv = (clip_p.xy / clip_p.w) * 0.5 + 0.5;
            if (s_uv.x < 0.0 || s_uv.x > 1.0 || s_uv.y < 0.0 || s_uv.y > 1.0) break;

            // Reprojection to previous frame's screen UV:
            // Converts current view hit point to world position, then projects with prev_view_proj
            vec4 world_p = inv_view * vec4(p, 1.0);
            vec4 prev_clip = prev_view_proj * world_p;
            if (prev_clip.w <= 0.001) break;
            vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
            if (prev_uv.x < 0.0 || prev_uv.x > 1.0 || prev_uv.y < 0.0 || prev_uv.y > 1.0) break;

            vec4 c = texture(scene_color_tex, prev_uv);
            if (length(c.rgb) > 0.01) {
                vec2 edge = smoothstep(vec2(0.0), vec2(0.08), prev_uv) * smoothstep(vec2(1.0), vec2(0.92), prev_uv);
                hit_color = c.rgb;
                hit_weight = edge.x * edge.y * (1.0 - roughness * 1.5);
                break;
            }
        }
    }
    return vec4(hit_color, hit_weight);
}

// 半透明物理体着色与混合透明度估算 (Physical Glass Body & Effective Premultiplied Alpha)
struct TranslucentShadingResult {
    vec3 body_color;
    float effective_alpha;
};

TranslucentShadingResult eval_translucent_body_and_alpha(
    vec3 albedo,
    float base_opacity,
    float texture_alpha,
    float roughness,
    vec3 ambient_irradiance,
    vec3 direct_diffuse,
    vec3 total_diffuse,
    vec3 F_double
) {
    TranslucentShadingResult res;
    
    // 计算色彩饱和度 (用于区分无色透明玻璃与有色深色玻璃瓶)
    float max_c = max(albedo.r, max(albedo.g, albedo.b));
    float min_c = min(albedo.r, min(albedo.g, albedo.b));
    float saturation = (max_c > 0.001) ? clamp((max_c - min_c) / max_c, 0.0, 1.0) : 0.0;

    // 无色透明玻璃无漫反射散射 (100% 背景透射)；有色玻璃根据饱和度产生内部透光色泽
    vec3 glass_body = albedo * (ambient_irradiance * 0.4 + direct_diffuse * 0.35) * (1.0 - roughness * 0.4) * saturation;
    res.body_color = mix(glass_body, total_diffuse, texture_alpha);

    // 预乘混合模式下的有效 Alpha:
    // 透明玻璃为纯物理双层菲涅尔反射；有色玻璃叠加吸收厚度不透明度
    float glass_alpha = clamp(F_double.r + saturation * base_opacity * 0.6, 0.04, 0.98);
    res.effective_alpha = clamp(mix(glass_alpha, 1.0, texture_alpha), 0.04, 1.0);
    
    return res;
}

#endif // TRANSLUCENCY_COMMON_GLSL
