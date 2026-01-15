#version 450

layout(location = 0) in vec3 inPos;
// location 1 (Color) 跳过
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in float inTexIndex;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out flat float fragTexIndex;

layout(push_constant) uniform PushConsts {
    mat4 lightMVP;
	vec3 lightDir;
} pushConsts;

void main() {
	float NdotL = max(dot(normalize(inNormal), -pushConsts.lightDir), 0.0);
	float sine = sqrt(1.0 - NdotL * NdotL);

	float biasAmount = 0.005 * sine;

	vec3 posOffset = inPos - inNormal * biasAmount;

    gl_Position = pushConsts.lightMVP * vec4(posOffset, 1.0);
	fragTexCoord = inTexCoord;
    fragTexIndex = inTexIndex;
}
