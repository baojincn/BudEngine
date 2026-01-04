#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal; 
layout(location = 3) in vec2 inTexCoord; 
layout(location = 4) in float inTexIndex;

layout(location = 0) out vec3 fragWorldPos; // PBR 需要世界坐标
layout(location = 1) out vec3 fragNormal;   // 法线
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out flat float fragTexIndex;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 camPos;
    vec3 lightDir;
} ubo;

void main() {
    // 计算世界坐标
    vec4 worldPos = ubo.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    // 计算法线 (简单起见先直接乘 model，如果有非等比缩放需要用逆转置矩阵)
    fragNormal = mat3(ubo.model) * inNormal;

    gl_Position = ubo.proj * ubo.view * worldPos;
    fragTexCoord = inTexCoord;
    fragTexIndex = inTexIndex;
}
