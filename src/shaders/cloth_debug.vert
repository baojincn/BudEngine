#version 460

// GPU-driven cloth debug wireframe: pull-model line list. Every constraint in the
// simulation world becomes one line segment; the vertex shader fetches the endpoint
// particle straight from the SSBOs (no index buffer, no CPU readback).
//   gl_VertexIndex >> 1  -> constraint index
//   gl_VertexIndex & 1   -> 0 = p1, 1 = p2

layout(location = 0) out vec3 frag_color;

layout(std140, set = 0, binding = 0) uniform ViewUBO {
    mat4 view_proj;
};

struct SimParticle {
    vec4 position_inv_mass; // xyz: position, w: inv_mass (0 = pinned)
    vec4 prev_position;
};

struct DistanceConstraint {
    uint  p1;
    uint  p2;
    float rest_length;
    float compliance;
};

layout(std430, set = 0, binding = 1) readonly buffer ParticleBuffer {
    SimParticle particles[];
};

layout(std430, set = 0, binding = 2) readonly buffer ConstraintBuffer {
    DistanceConstraint constraints[];
};

void main() {
    DistanceConstraint c = constraints[gl_VertexIndex >> 1];
    uint pi = ((gl_VertexIndex & 1) != 0) ? c.p2 : c.p1;
    vec4 pm = particles[pi].position_inv_mass;

    gl_Position = view_proj * vec4(pm.xyz, 1.0);

    if (pm.w <= 0.0) {
        frag_color = vec3(1.0, 0.15, 0.15);   // pinned particle: red
    } else if (c.compliance < 1e-3) {
        frag_color = vec3(0.2, 1.0, 0.35);    // structural edge: green
    } else {
        frag_color = vec3(1.0, 0.6, 0.1);     // bending edge: orange
    }
}
