#version 450

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) flat in uint instance_seed;
layout(location = 3) flat in uint cluster_seed;

layout(location = 0) out vec4 out_color;

// Simple PCG hash to generate pseudo-random numbers
uint pcg_hash(uint seed) {
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 get_random_color(uint seed) {
    uint h = pcg_hash(seed);
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

void main() {
    // Mix instance_seed and cluster_seed for stable color
    uint seed = instance_seed * 1337u + cluster_seed;
    
    vec3 debug_color = get_random_color(seed);

    // Basic lighting to see shapes
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    float ndotl = max(dot(normalize(frag_normal), light_dir), 0.2); 

    out_color = vec4(debug_color * ndotl, 1.0);
}
