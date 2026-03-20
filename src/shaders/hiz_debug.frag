#version 460

layout (location = 0) in vec2 in_uv;
layout (location = 0) out vec4 out_color;

layout (set = 0, binding = 1) uniform sampler2D hiz_pyramid;

layout (push_constant) uniform PushConstants {
    uint mip_level;
} pc;

void main() {
    float depth = textureLod(hiz_pyramid, in_uv, pc.mip_level).r;
    
    // Visualize depth: 1.0 is near, 0.0 is far (Standardizing visual mapping)
    // For Standard Z (0=near, 1=far), we invert it.
    // We use a power function to enhance contrast since Sponza depth is usually near 1.0.
    float v = 1.0 - depth; 
    v = pow(v, 10.0); // Enhance contrast for small depth range
    
    out_color = vec4(vec3(v), 1.0);
}
