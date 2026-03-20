#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_normal;
layout(location = 2) flat out uint instance_seed;
layout(location = 3) flat out uint cluster_seed;

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
	uint reversed_z;
	uint padding[3];
} ubo;

layout(push_constant) uniform PushConsts {
    mat4 model;
    uint material_id;
    uint padding[3];
} push_consts;

void main() {
    vec4 world_pos = push_consts.model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;
    frag_normal = mat3(transpose(inverse(push_consts.model))) * in_normal;
    
    // Create stable instance seed based on world position
    instance_seed = floatBitsToUint(push_consts.model[3].x) ^ floatBitsToUint(push_consts.model[3].y) ^ floatBitsToUint(push_consts.model[3].z);
    cluster_seed = uint(gl_VertexIndex) / 64u; // Stable seed for cluster coloration

    gl_Position = ubo.proj * ubo.view * world_pos;
}
