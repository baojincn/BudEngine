#ifndef VG_COMMON_GLSL
#define VG_COMMON_GLSL

layout(set = 1, binding = 0) uniform UniformBufferObject {
	mat4 view;
	mat4 proj;
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
} ubo;

struct GPUMaterialData {
    vec4 base_color_factor;
    uint albedo_texture_id;
    uint normal_texture_id;
    uint metallic_roughness_id;
    uint emissive_texture_id;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint alpha_mode;
};

struct HierarchyInstance {
    mat4 model_matrix;
    uint mesh_id;
    uint material_id;
    uint root_group_index;
    uint flags;
    float global_sphere_center_x;
    float global_sphere_center_y;
    float global_sphere_center_z;
    float global_sphere_radius;
    float error_threshold;
    uint base_virtual_page;
    uint pad0;
    uint pad1;
};

struct PageTableEntry {
    uint pool_offset;
    uint valid;
    uint pad0;
    uint pad1;
};

struct VGClusterGroup {
    vec3 lod_bounds_center;
    float lod_bounds_radius;
    float lod_error;
    uint page_index_start;
    uint page_count;
    uint pad0;
};

#define VG_PAGE_SIZE_BYTES 131072u
#define VG_PAGE_MAGIC 0x50475642u
#define VG_PAGE_HEADER_DWORDS 16u
#define VG_CLUSTER_DWORDS 12u

#endif
