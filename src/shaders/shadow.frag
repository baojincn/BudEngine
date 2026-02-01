#version 450
#extension GL_EXT_nonuniform_qualifier : enable


layout(location = 0) in vec2 frag_tex_coord;
layout(location = 1) in flat float frag_tex_index;

layout(binding = 1) uniform sampler2D tex_samplers[];

void main() {
    int tex_id = int(frag_tex_index + 0.5);
    float alpha = texture(tex_samplers[nonuniformEXT(tex_id)], frag_tex_coord).a;
    
    if (alpha < 0.5) {
        discard; 
    }
}
