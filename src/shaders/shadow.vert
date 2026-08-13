#version 460
#extension GL_ARB_shader_draw_parameters : enable

layout(location = 0) in vec3 in_position;
//layout(location = 1) in vec3 in_color;
//layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec2 in_tex_coord;
//layout(location = 4) in float in_tex_index;

layout(location = 0) out vec2 frag_tex_coord;
layout(location = 1) flat out uint frag_material_id;

struct InstanceData {
	mat4 model;
	uint material_id;
	uint page_slot;
	uint padding[2];
};

layout(std430, set = 0, binding = 6) readonly buffer InstanceBuffer {
	InstanceData data[];
} instance_buffer;

layout(push_constant) uniform PushConsts {
    mat4 light_view_proj;
	mat4 model;
    uint material_id;
    uint use_gpu_driven;
    uint page_slot;
} push_consts;

struct PageTableEntry {
	uint valid;
	uint padding;
	uint pool_offset;
};

layout(std430, set = 0, binding = 4) readonly buffer PageTableBuffer {
	PageTableEntry data[];
} page_table;

layout(std430, set = 0, binding = 5) readonly buffer PagePoolBuffer {
	uint data[];
} page_pool;

uint nanite_read_bits(uint base_word_idx, uint bit_offset, uint num_bits) {
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

void main() {
    mat4 model;
    uint mat_id;
    vec3 pos = in_position;
    vec2 uv = in_tex_coord;
    
        uint page_slot;

        if (push_consts.use_gpu_driven == 1) {
            InstanceData instance = instance_buffer.data[gl_InstanceIndex];
            model = instance.model;
            mat_id = instance.material_id;
            page_slot = instance.page_slot;
        } else {
            model = push_consts.model;
            mat_id = push_consts.material_id;
            page_slot = push_consts.page_slot;
        }

        if (page_slot != 0xFFFFFFFFu && page_slot < page_table.data.length() && page_table.data[page_slot].valid != 0u) {
            uint pool_bytes = page_table.data[page_slot].pool_offset;
            uint base_word = pool_bytes / 4u;
            uint magic = page_pool.data[base_word + 0u];

            if (magic == 0x50474142u) { // "BAGP" Nanite Page Data Magic
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
                    uint qx = nanite_read_bits(base_word, bit_offset, bits);
                    uint qy = nanite_read_bits(base_word, bit_offset + bits, bits);
                    uint qz = nanite_read_bits(base_word, bit_offset + bits * 2u, bits);

                    float max_q = float((1u << bits) - 1u);
                    pos = poff + (vec3(float(qx), float(qy), float(qz)) / max_q) * pext;

                    uint pos_bytes = (vertex_count * bits * 3u + 7u) / 8u;
                    uint pos_bytes_aligned = (pos_bytes + 3u) & ~3u;
                    uint attr_word = base_word + (v_stream_off + pos_bytes_aligned + v_idx * 16u) / 4u;
                    uint uv_raw = page_pool.data[attr_word + 2u];
                    uv = unpackHalf2x16(uv_raw);
                }
            }
        }

    frag_tex_coord = uv;
    frag_material_id = mat_id;
    gl_Position = push_consts.light_view_proj * model * vec4(pos, 1.0);
}
