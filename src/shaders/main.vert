#version 460
#extension GL_ARB_shader_draw_parameters : enable

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_tex_coord;
layout(location = 3) in vec4 in_tangent;

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
	uint page_slot;
	uint padding[2];
};

layout(std430, binding = 3) readonly buffer InstanceBuffer {
	InstanceData data[];
} instance_buffer;

struct PageTableEntry {
	uint valid;
	uint padding;
	uint pool_offset;
};

layout(std430, binding = 4) readonly buffer PageTableBuffer {
	PageTableEntry data[];
} page_table;

layout(std430, binding = 5) readonly buffer PagePoolBuffer {
	uint data[];
} page_pool;

uint vg_read_bits(uint base_word_idx, uint bit_offset, uint num_bits) {
	uint word_idx = base_word_idx + (bit_offset >> 5);
	uint bit_in_word = bit_offset & 31u;
	uint w0 = page_pool.data[word_idx];
	uint mask = (num_bits == 32u) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);
	if (bit_in_word + num_bits <= 32u) {
		return (w0 >> bit_in_word) & mask;
	} else {
		uint w1 = page_pool.data[word_idx + 1u];
		uint low_count = 32u - bit_in_word;
		uint high_count = num_bits - low_count;
		uint low_bits = (w0 >> bit_in_word) & ((1u << low_count) - 1u);
		uint high_bits = (w1 & ((1u << high_count) - 1u));
		return low_bits | (high_bits << low_count);
	}
}

float vg_snorm8(uint byte_val) {
	int s8 = int(byte_val & 0xFFu);
	if (s8 >= 128) s8 -= 256;
	return float(s8) / 127.0;
}

void main() {
	InstanceData instance = instance_buffer.data[gl_InstanceIndex];
	vec3 pos = in_position;
	vec3 norm = in_normal;
	vec2 uv = in_tex_coord;

	uint page_slot = instance.page_slot;
	if (page_slot != 0xFFFFFFFFu) {
		uint pool_bytes = page_slot * 131072u;
		uint base_word = pool_bytes / 4u;
		uint magic = page_pool.data[base_word + 0u];

		if (magic == 0x50475642u) { // "BVGP" Virtual Geometry Page Data Magic
			uint vertex_count = page_pool.data[base_word + 3u];
			uint v_stream_off = page_pool.data[base_word + 5u];
			uint bits = page_pool.data[base_word + 9u];
			if (bits == 0u) bits = 12u;

			uint v_idx = gl_VertexIndex;
			if (v_idx < vertex_count) {
				vec3 poff = vec3(
					uintBitsToFloat(page_pool.data[base_word + 10u]),
					uintBitsToFloat(page_pool.data[base_word + 11u]),
					uintBitsToFloat(page_pool.data[base_word + 12u]));
				vec3 pext = vec3(
					uintBitsToFloat(page_pool.data[base_word + 13u]),
					uintBitsToFloat(page_pool.data[base_word + 14u]),
					uintBitsToFloat(page_pool.data[base_word + 15u]));

				uint bit_offset = v_stream_off * 8u + v_idx * (bits * 3u);
				uint qx = vg_read_bits(base_word, bit_offset, bits);
				uint qy = vg_read_bits(base_word, bit_offset + bits, bits);
				uint qz = vg_read_bits(base_word, bit_offset + bits * 2u, bits);

				float max_q = float((1u << bits) - 1u);
				pos = poff + (vec3(float(qx), float(qy), float(qz)) / max_q) * pext;

				uint pos_bytes = (vertex_count * bits * 3u + 7u) / 8u;
				uint pos_bytes_aligned = (pos_bytes + 3u) & ~3u;
				uint attr_word = base_word + (v_stream_off + pos_bytes_aligned + v_idx * 16u) / 4u;
				uint n_raw = page_pool.data[attr_word + 0u];
				uint uv_raw = page_pool.data[attr_word + 2u];

				norm = vec3(vg_snorm8(n_raw), vg_snorm8(n_raw >> 8u), vg_snorm8(n_raw >> 16u));
				uv = unpackHalf2x16(uv_raw);
			}
		}
	}

	vec4 world_pos = instance.model * vec4(pos, 1.0);
	frag_world_pos = world_pos.xyz;
	frag_normal = normalize(mat3(instance.model) * norm);
	frag_color = vec3(1.0);
	frag_material_id = instance.material_id;
	gl_Position = ubo.proj * ubo.view * world_pos;
	frag_tex_coord = uv;
}
