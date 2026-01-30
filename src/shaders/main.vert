#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec3 in_normal; 
layout(location = 3) in vec2 in_tex_coord; 
layout(location = 4) in float in_tex_index;

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_normal;
layout(location = 2) out vec2 frag_tex_coord;
layout(location = 3) out flat float frag_tex_index;
// [CSM] fragPosLightSpace removed, we calculate it in frag shader per cascade

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
} ubo;

layout(push_constant) uniform PushConsts {
    mat4 model;
} push_consts;

void main() {
    vec4 world_pos = push_consts.model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;
	// [CSM] No more single lightSpaceMatrix projection here

    frag_normal = mat3(push_consts.model) * in_normal;

    gl_Position = ubo.proj * ubo.view * world_pos;

    frag_tex_coord = in_tex_coord;
    frag_tex_index = in_tex_index;
}
