#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_tex_coord;
layout(location = 3) in flat float frag_tex_index;
// [CSM] fragPosLightSpace removed, we use worldPos per cascade

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
} ubo;

layout(binding = 1) uniform sampler2D tex_samplers[];
layout(binding = 2) uniform sampler2DArrayShadow shadow_map;

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

float SampleCascade(int layer, vec3 world_pos, vec3 N, vec3 L) {
    vec4 frag_pos_light_space = ubo.cascade_view_proj[layer] * vec4(world_pos, 1.0);
    vec3 proj_coords = frag_pos_light_space.xyz / frag_pos_light_space.w;
    
    // NDC -> [0, 1]
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    // 超出视锥体范围，视作无阴影
    if(proj_coords.z > 1.0 || proj_coords.x < 0.0 || proj_coords.x > 1.0 || proj_coords.y < 0.0 || proj_coords.y > 1.0)
        return 0.0;

    // Bias: 既然 C++ 端已经设置了硬件 DepthBias，这里保持 0.0 或者非常小的值即可
    // 如果发现波纹，可以在这里加一点点: max(0.0005 * (1.0 - dot(N, L)), 0.00005)
    float bias = 0.0; 

    // PCF
    float shadow_sum = 0.0;
    vec2 texel_size = 1.0 / textureSize(shadow_map, 0).xy;
    
    // Spread: 控制软阴影程度
    // 技巧：远处的级联 (layer 越大) 纹素覆盖的世界面积越大，
    // 如果保持 spread 不变，远处阴影会显得非常软（模糊）。这是期望的效果，能掩盖锯齿。
    float spread = 2.5; 

    for(int i = 0; i < 16; ++i) {
        vec2 offset = poissonDisk[i] * texel_size * spread;
        // 注意：sampler2DArrayShadow 期望的坐标是 vec4(uv.x, uv.y, layer_index, depth_ref)
        float pcf_depth = proj_coords.z - bias;
        shadow_sum += texture(shadow_map, vec4(proj_coords.xy + offset, float(layer), pcf_depth));
    }

    // texture 返回的是 "是否通过测试" (1.0 = 被照亮, 0.0 = 在阴影中)
    // 所以累加后 shadow_sum 是 "光照强度"
    // 我们最后需要返回 "阴影强度" (1.0 = 全黑)
    return 1.0 - (shadow_sum / 16.0);
}


float ShadowCalculation(vec3 world_pos, vec3 N, vec3 L) {
	// 1. Cascade Selection
	vec4 view_pos = ubo.view * vec4(world_pos, 1.0);
	float depth = -view_pos.z;

	int layer = -1;
	float blend_band = 1.5f;
	float blend_factor = 0.0f;
	int next_layer = -1;

	for (int i = 0; i < 4; ++i) {
		if (depth < ubo.cascade_split_depths[i]) {
			layer = i;

			// Blend between cascades
			float split_dist = ubo.cascade_split_depths[i];
			float dist_to_edge = split_dist - depth;

			if (dist_to_edge < blend_band && i < 3) {
				next_layer = i + 1;
				blend_factor = 1.0 - (dist_to_edge / blend_band);
			}

			break;
		}
	}

	if (layer == -1)
		layer = 3;

	// 2. 采样当前层级
    float shadow = SampleCascade(layer, world_pos, N, L);

    // 3. 如果处于混合带，采样下一层级并插值
    if (blend_factor > 0.001) {
        float next_shadow = SampleCascade(layer + 1, world_pos, N, L);
        
        // 线性插值：blend_factor 越大，越倾向于 next_shadow
        shadow = mix(shadow, next_shadow, blend_factor);
    }

    return shadow;
}

void main() {
    int tex_id = int(frag_tex_index + 0.5);
    vec4 albedo_sample;
    
    // [FIX] Handle unbound texture index 0 or missing textures
    if (tex_id <= 0) {
        albedo_sample = vec4(0.8, 0.1, 0.8, 1.0); // Highlight non-textured walls in Magenta
    } else {
        albedo_sample = texture(tex_samplers[nonuniformEXT(tex_id)], frag_tex_coord);
    }

	// [FIX] Disable discard as Sponza wall textures use alpha for Specular/Gloss, not Opacity
	if (albedo_sample.a < 0.5)
		discard;

    vec3 albedo = albedo_sample.rgb; 

    float metallic = 0.1; 
    float roughness = 0.5;
    float ao = 1.0;

    vec3 N = normalize(frag_normal);
    vec3 V = normalize(ubo.cam_pos - frag_world_pos);
    vec3 L = normalize(ubo.light_dir); 
    vec3 H = normalize(V + L);

    vec3 cascadeColors[4] = vec3[](
        vec3(1.0, 0.1, 0.1),
        vec3(0.1, 1.0, 0.1),
        vec3(0.1, 0.1, 1.0),
        vec3(1.0, 1.0, 0.1)
    );

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

	float shadow = ShadowCalculation(frag_world_pos, N, L);

    // Apply Shadow
    Lo *= (1.0 - shadow);

    vec3 ambient = vec3(ubo.ambient_strength) * albedo * ao;
    vec3 color = ambient + Lo;

    // [DEBUG] Toggle this to visualize cascades
    if (ubo.debug_cascades > 0) {
        int debugLayer = -1;
        vec4 debugViewPos = ubo.view * vec4(frag_world_pos, 1.0);
        float debugDepth = -debugViewPos.z;
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

    color = ACESFilm(color);

    // Gamma Correction (Linear -> sRGB)
    color = pow(color, vec3(1.0/2.2)); 

    out_color = vec4(color, albedo_sample.a);
}
