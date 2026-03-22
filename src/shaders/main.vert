#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec3 in_normal; 
layout(location = 3) in vec2 in_tex_coord; 
layout(location = 4) in float in_tex_index;

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_normal;
layout(location = 2) out vec2 frag_tex_coord;
layout(location = 3) out vec3 frag_color;
layout(location = 4) flat out uint frag_material_id;

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

struct InstanceData {
    mat4 model;
    uint material_id;
    uint padding[3];
};

layout(std430, binding = 3) readonly buffer InstanceBuffer {
    InstanceData data[];
} instance_buffer;

void main() {
    InstanceData instance = instance_buffer.data[gl_InstanceIndex];
    vec4 world_pos = instance.model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;

    frag_normal = in_normal;
    frag_color = in_color;
    frag_material_id = instance.material_id;

    gl_Position = ubo.proj * ubo.view * world_pos;

    frag_tex_coord = in_tex_coord;
}
