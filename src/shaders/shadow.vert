#version 450

layout(location = 0) in vec3 inPos;
// location 1 (Color) 跳过
// location 2 (Normal) 跳过
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in float inTexIndex;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out flat float fragTexIndex;

layout(push_constant) uniform PushConsts {
    mat4 lightMVP; // LightProj * LightView * Model
} pushConsts;

void main() {
    gl_Position = pushConsts.lightMVP * vec4(inPos, 1.0);
	fragTexCoord = inTexCoord;
    fragTexIndex = inTexIndex;
}
