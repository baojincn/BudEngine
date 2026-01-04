#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in flat float fragTexIndex;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 camPos;
    vec3 lightDir;
} ubo;

layout(binding = 1) uniform sampler2D texSamplers[];

const float PI = 3.14159265359;

// --- [新增] ACES 电影级色调映射 (对比度更高) ---
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

// ... DistributionGGX, GeometrySchlickGGX, GeometrySmith, FresnelSchlick 保持不变 ...
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

void main() {
    int texID = int(fragTexIndex + 0.5);
    vec4 albedoSample = texture(texSamplers[nonuniformEXT(texID)], fragTexCoord);
    
    // 【修改 1】直接使用 rgb，不要再 pow(2.2) 了！
    vec3 albedo = albedoSample.rgb; 

    // 【修改 2】让材质稍微光滑一点，增加对比
    float metallic = 0.1; 
    float roughness = 0.5; // 从 0.8 改为 0.5，让高光更明显一点
    float ao = 1.0;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(ubo.camPos - fragWorldPos);
    vec3 L = normalize(ubo.lightDir); 
    vec3 H = normalize(V + L);

    // F0 ... (保持不变)
    vec3 F0 = vec3(0.04); 
    F0 = mix(F0, albedo, metallic);

    // 【修改 3】增加一点光照强度
    vec3 lightColor = vec3(1.0, 1.0, 1.0); 
    float lightIntensity = 5.0; // 加强亮度
    vec3 radiance = lightColor * lightIntensity; 

    // --- Cook-Torrance BRDF (保持不变) ---
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

    // 环境光 (稍微提亮一点暗部)
    vec3 ambient = vec3(0.05) * albedo * ao;
    vec3 color = ambient + Lo;

    // 【修改 4】使用 ACES Tone Mapping 替代 Reinhard
    color = ACESFilm(color);

    // Gamma Correction (Linear -> sRGB 输出)
    // 这一步必须保留，因为显示器是 sRGB 的
    color = pow(color, vec3(1.0/2.2)); 

    outColor = vec4(color, albedoSample.a);
}
