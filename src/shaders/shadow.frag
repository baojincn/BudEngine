#version 450
#extension GL_EXT_nonuniform_qualifier : enable


layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in flat float fragTexIndex;

layout(binding = 1) uniform sampler2D texSamplers[];

void main() {
    int texID = int(fragTexIndex + 0.5);
    float alpha = texture(texSamplers[nonuniformEXT(texID)], fragTexCoord).a;
    
    if (alpha < 0.5) {
        discard;
    }
}
