#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in flat float fragTexIndex;
layout(location = 4) in vec4 fragPosLightSpace;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
	mat4 lightSpaceMatrix;
    vec3 camPos;
    vec3 lightDir;
    vec3 lightColor;
    float lightIntensity;
    float ambientStrength;
} ubo;

layout(binding = 1) uniform sampler2D texSamplers[];
layout(binding = 2) uniform sampler2DShadow shadowMap;

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

float ShadowCalculation(vec4 fragPosLightSpace, vec3 N, vec3 L) {
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    
    // [0, 1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // 超出视锥体
    if(projCoords.z > 1.0)
		return 0.0;

    // 解决 Shadow Acne
    // Bias 越大角度越倾斜
    float currentDepth = projCoords.z;
    float bias = max(0.005 * (1.0 - dot(N, L)), 0.001); 

    // Percentage-closer filtering
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
	float spread = 0.8;

    for(int i = 0; i < 16; ++i) {
			vec2 offset = poissonDisk[i] * texelSize * spread;
            float pcf = texture(shadowMap, vec3(projCoords.xy + offset, currentDepth - bias));
			
            shadow += pcf;        
    }

    shadow = 1.0 - (shadow / 16.0);
    return shadow;
}

void main() {
    int texID = int(fragTexIndex + 0.5);
    vec4 albedoSample = texture(texSamplers[nonuniformEXT(texID)], fragTexCoord);

	if (albedoSample.a < 0.5)
		discard;

    vec3 albedo = albedoSample.rgb; 

    float metallic = 0.1; 
    float roughness = 0.5;
    float ao = 1.0;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(ubo.camPos - fragWorldPos);
    vec3 L = normalize(ubo.lightDir); 
    vec3 H = normalize(V + L);

    vec3 F0 = vec3(0.04); 
    F0 = mix(F0, albedo, metallic);

    vec3 lightColor = ubo.lightColor; 
    float lightIntensity = ubo.lightIntensity;
    vec3 radiance = lightColor * lightIntensity; 

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

	float shadow = ShadowCalculation(fragPosLightSpace, N, L);

    // 应用阴影到 Diffuse 和 Specular
    Lo *=  (1.0 - shadow);

    vec3 ambient = vec3(ubo.ambientStrength) * albedo * ao;
    vec3 color = ambient + Lo;

    color = ACESFilm(color);

    // Gamma Correction (Linear -> sRGB)
    color = pow(color, vec3(1.0/2.2)); 

    outColor = vec4(color, albedoSample.a);
}
