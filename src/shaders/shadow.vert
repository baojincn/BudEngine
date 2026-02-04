#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec2 in_tex_coord;
//layout(location = 4) in float in_tex_index;

layout(location = 0) out vec2 frag_tex_coord;
//layout(location = 1) out flat float frag_tex_index;

layout(push_constant) uniform PushConsts {
    mat4 light_view_proj;
	mat4 model;
	vec4 light_dir;
	uint material_id;
} push_consts;

void main() {
    gl_Position = push_consts.light_view_proj * push_consts.model * vec4(in_position, 1.0);
	frag_tex_coord = in_tex_coord;
    //frag_tex_index = in_tex_index;
}
