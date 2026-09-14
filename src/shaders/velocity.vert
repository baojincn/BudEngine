#version 460 core

layout(location = 0) in vec3 in_position;

layout(location = 0) out vec4 out_curr_clip;
layout(location = 1) out vec4 out_prev_clip;

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

	vec4 cascade_texel_size;
	vec4 cascade_depth_range;
	float shadow_receiver_bias_texels;
	float shadow_normal_offset_texels;

	mat4 unjittered_inv_view_proj;
	mat4 prev_unjittered_view_proj;
	vec4 jitter_offset;
	mat4 unjittered_view_proj;
} ubo;

layout(std430, set = 0, binding = 8) readonly buffer ClothPrevPositionBuffer {
	vec4 cloth_prev_positions[];
};

layout(std430, push_constant) uniform PushConstants {
	mat4 model;
	mat4 prev_model;
	uint is_cloth;
	uint cloth_binding_offset;
	uint cloth_vertex_offset;
	uint padding;
} pc;

void main() {
	vec4 curr_world;
	vec4 prev_world;

	if (pc.is_cloth != 0u) {
		// XPBD Cloth vertices are simulated and stored in world space.
		// in_position = current world pos, previous world pos is read from ClothPrevPositionBuffer
		curr_world = vec4(in_position, 1.0);
		uint local_idx = uint(gl_VertexIndex) >= pc.cloth_vertex_offset ? (uint(gl_VertexIndex) - pc.cloth_vertex_offset) : 0u;
		prev_world = cloth_prev_positions[pc.cloth_binding_offset + local_idx];
		prev_world.w = 1.0;
	} else {
		curr_world = pc.model * vec4(in_position, 1.0);
		prev_world = pc.prev_model * vec4(in_position, 1.0);
	}

	// Raster clip position uses jittered view-projection to test against the depth buffer
	gl_Position = ubo.proj * ubo.view * curr_world;

	// Motion vector clip positions use unjittered camera matrices to eliminate jitter
	out_curr_clip = ubo.unjittered_view_proj * curr_world;
	out_prev_clip = ubo.prev_unjittered_view_proj * prev_world;
}
