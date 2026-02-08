#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_tex_coord;
//layout(location = 3) in flat float frag_tex_index;
// [CSM] fragPosLightSpace removed, we use worldPos per cascade

layout(location = 0) out vec4 out_color;


layout(push_constant) uniform PushConsts {
    mat4 model;
    uint material_id;
	uint padding[3];
} push;

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
        return 1.0;

    float bias = 0.0; 

    float shadow_sum = 0.0;
    vec2 texel_size = 1.0 / textureSize(shadow_map, 0).xy;
    float spread = 2.5; 

    for(int i = 0; i < 16; ++i) {
        vec2 offset = poissonDisk[i] * texel_size * spread;
        float pcf_depth = proj_coords.z - bias;
        shadow_sum += texture(shadow_map, vec4(proj_coords.xy + offset, float(layer), pcf_depth));
    }

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

	int cascade_count = int(clamp(ubo.cascade_count, 1u, 4u));
	for (int i = 0; i < cascade_count; ++i) {
		if (depth < ubo.cascade_split_depths[i]) {
			layer = i;

			float split_dist = ubo.cascade_split_depths[i];
			float dist_to_edge = split_dist - depth;

			if (dist_to_edge < blend_band && i + 1 < cascade_count) {
				next_layer = i + 1;
				blend_factor = 1.0 - (dist_to_edge / blend_band);
			}

			break;
		}
	}

	if (layer == -1)
		layer = cascade_count - 1;

    float shadow = SampleCascade(layer, world_pos, N, L);

    if (blend_factor > 0.001 && next_layer >= 0) {
        float next_shadow = SampleCascade(next_layer, world_pos, N, L);
        shadow = mix(shadow, next_shadow, blend_factor);
    }

    return shadow;
}

void main() {
	uint tex_id = push.material_id;
    vec4 albedo_sample;
    
    if (tex_id <= 0) {
        albedo_sample = vec4(0.8, 0.1, 0.8, 1.0);
    } else {
        albedo_sample = texture(tex_samplers[nonuniformEXT(tex_id)], frag_tex_coord);
    }

    vec3 albedo = albedo_sample.rgb; 

    float metallic = 0.1; 
    float roughness = 0.5;
    float ao = 1.0;

    vec3 N = normalize(frag_normal);
    vec3 V = normalize(ubo.cam_pos - frag_world_pos);
    vec3 L = normalize(ubo.light_dir); 
    vec3 H = normalize(V + L);

    vec3 F0 = vec3(0.04); 
    F0 = mix(F0, albedo, metallic);

    vec3 light_color = ubo.light_color; 
    float light_intensity = ubo.light_intensity;
    vec3 radiance = light_color * light_intensity; 

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

    Lo *= (1.0 - shadow);

    vec3 ambient = vec3(ubo.ambient_strength) * albedo * ao;
    vec3 color = ambient + Lo;

    if (ubo.debug_cascades > 0) {
        int debugLayer = -1;
        vec4 debugViewPos = ubo.view * vec4(frag_world_pos, 1.0);
        float debugDepth = -debugViewPos.z;

		for (int i = 0; i < cascade_count; ++i) {
			if (debugDepth < ubo.cascade_split_depths[i]) {
				debugLayer = i;
				break;
			}
		}

        vec2 texel_size = 1.0 / textureSize(shadow_map, 0).xy;
        vec2 uv = debugViewPos.xy * 0.5 * texel_size;

        // 显示调试纹理
        if (ubo.debug_cascades == 1)
            color = texture(shadow_map, vec3(uv, 0)).rgb;
        else if (ubo.debug_cascades == 2)
            color = texture(shadow_map, vec3(uv, 1)).rgb;
        else if (ubo.debug_cascades == 3)
            color = texture(shadow_map, vec3(uv, 2)).rgb;
        else if (ubo.debug_cascades == 4)
            color = texture(shadow_map, vec3(uv, 3)).rgb;
        else {
            float test = 0.0;
            for (int i = 0; i < 4; ++i) {
                vec3 texColor = texture(shadow_map, vec3(uv, float(i))).rgb;
                color += texColor * cascadeColors[i] * 0.25;
            }
        }
    }

    out_color = vec4(color, albedo_sample.a);
}
