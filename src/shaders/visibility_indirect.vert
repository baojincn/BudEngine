#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_ARB_shader_draw_parameters : enable

#include "common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_color;

layout(location = 0) flat out uint frag_material_id;
layout(location = 1) flat out float frag_blend_factor;
layout(location = 2) flat out uint frag_instance_id;
layout(location = 3) out vec2 frag_tex_coord;
layout(location = 4) out vec3 frag_normal;
layout(location = 5) flat out uint frag_cluster_id;
layout(location = 6) flat out uint frag_instance_flags;

layout(std140, set = 0, binding = 0) uniform UniformBufferObject {
	mat4 view;
	mat4 proj;
	mat4 prev_view_proj;
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
	float shadow_bias_constant;
	float shadow_bias_slope;
	uint debug_cluster;
} ubo;

struct HierarchyInstance {
	mat4 model_matrix;
	uint mesh_id;
	uint material_id;
	uint root_group_index;
	uint flags;
	float global_sphere_center_x;
	float global_sphere_center_y;
	float global_sphere_center_z;
	float global_sphere_radius;
	float error_threshold;
	uint base_virtual_page;
	uint pad0;
	uint pad1;
};

layout(std430, set = 0, binding = 3) readonly buffer InstanceBuffer {
	HierarchyInstance data[];
} instance_buffer;

void main() {
	HierarchyInstance instance = instance_buffer.data[gl_InstanceIndex];
	vec3 pos = in_position;
	vec3 norm = in_normal;
	vec2 uv = in_tex_coord;

	bool is_cloth = bool(instance.flags & 8u);
	vec4 world_pos = is_cloth ? vec4(pos, 1.0) : (instance.model_matrix * vec4(pos, 1.0));
	frag_material_id = instance.material_id;
	frag_blend_factor = 0.0;
	frag_instance_id = gl_InstanceIndex;
	frag_tex_coord = uv;
	float n_len = length(norm);
	vec3 safe_norm = (n_len > 1e-4) ? (norm / n_len) : vec3(0.0, 1.0, 0.0);
	frag_normal = is_cloth ? safe_norm : normalize(mat3(instance.model_matrix) * safe_norm);
	frag_cluster_id = gl_InstanceIndex;
	frag_instance_flags = instance.flags;
	gl_Position = ubo.proj * ubo.view * world_pos;
}
