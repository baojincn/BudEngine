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
	uint padding[3];
};

layout(std430, set = 0, binding = 6) readonly buffer InstanceBuffer {
	InstanceData data[];
} instance_buffer;

layout(push_constant) uniform PushConsts {
    mat4 light_view_proj;
	mat4 model;
    uint material_id;
    uint use_gpu_driven;
} push_consts;

void main() {
    mat4 model;
    uint mat_id;
    
    if (push_consts.use_gpu_driven == 1) {
        InstanceData instance = instance_buffer.data[gl_InstanceIndex];
        model = instance.model;
        mat_id = instance.material_id;
    } else {
        model = push_consts.model;
        mat_id = push_consts.material_id;
    }

    frag_tex_coord = in_tex_coord;
    frag_material_id = mat_id;
    gl_Position = push_consts.light_view_proj * model * vec4(in_position, 1.0);
}
