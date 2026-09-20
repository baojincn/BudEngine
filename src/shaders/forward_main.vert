#version 460
#extension GL_GOOGLE_include_directive : enable
#extension GL_ARB_shader_draw_parameters : enable

#include "common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec3 in_color;

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_normal;
layout(location = 2) out vec2 frag_tex_coord;
layout(location = 3) out vec3 frag_color;
layout(location = 4) flat out uint frag_material_id;
layout(location = 5) flat out float frag_blend_factor;

layout(binding = 0) uniform UniformBufferObject {
	mat4 view;
	mat4 proj;
	mat4 prev_view_proj;
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

layout(push_constant) uniform PushConstants {
	mat4 model;
	uint material_id;
	uint is_indirect;
} push_consts;

void main() {
	mat4 model_mat;
	uint material_id;
	float blend_factor = 0.0;

	if (push_consts.is_indirect != 0) {
		HierarchyInstance instance = instance_buffer.data[gl_InstanceIndex];
		bool is_cloth = bool(instance.flags & 8u);
		model_mat = is_cloth ? mat4(1.0) : instance.model_matrix;
		material_id = instance.material_id;
		blend_factor = 0.0;
	} else {
		model_mat = push_consts.model;
		material_id = push_consts.material_id;
	}

	vec4 world_pos = model_mat * vec4(in_position, 1.0);
	frag_world_pos = world_pos.xyz;
	gl_Position = ubo.proj * ubo.view * world_pos;

	mat3 normal_matrix = transpose(inverse(mat3(model_mat)));
	frag_normal = normalize(normal_matrix * in_normal);

	frag_tex_coord = in_tex_coord;
	frag_color = in_color;
	frag_material_id = material_id;
	frag_blend_factor = blend_factor;
}
