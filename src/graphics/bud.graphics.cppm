module;

#include <cmath>
#include <string>
#include <memory>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>


export module bud.graphics;

import bud.math;
import bud.platform;
import bud.threading;
import bud.graphics.defs;

export namespace bud::graphics {

	export enum class Backend {
		Vulkan,
		D3D12,
		Metal
	};

	export struct MemoryAllocation {
		void* internal_handle = nullptr;
		uint64_t offset = 0;
		uint64_t size = 0;
		void* mapped_ptr = nullptr;
	};

	export class MemoryManager {
	public:
		virtual ~MemoryManager() = default;

		virtual void init() = 0;
		virtual void cleanup() = 0;

		virtual bool allocate_staging(uint64_t size, MemoryAllocation& out_alloc) = 0;

		virtual void mark_submitted(const MemoryAllocation& alloc, void* sync_handle) = 0;
	};

	export struct CascadeData {
		bud::math::mat4 view_proj_matrix;
		float split_depth;
	};

	export struct RenderConfig {
		uint32_t shadow_map_size = 4096;
		float shadow_bias_constant = 1.25f;
		float shadow_bias_slope = 1.75f;
		float shadow_ortho_size = 35.0f;
		float shadow_near_plane = 0.1f;
		float shadow_far_plane = 100.0f;

		bud::math::vec3 directional_light_position = { 5.0f, 15.0f, 5.0f };
		bud::math::vec3 directional_light_color = { 1.0f, 1.0f, 1.0f };
		float directional_light_intensity = 5.0f;
		float ambient_strength = 0.05f;

		bool enable_soft_shadows = true;

		int shadow_cascade_count = 3;
	};

	export struct SceneView {
		bud::math::mat4 view_matrix;
		bud::math::mat4 proj_matrix;
		bud::math::mat4 view_proj_matrix;

		bud::math::vec3 camera_position;
		float fov;
		float near_plane;
		float far_plane;

		float viewport_width;
		float viewport_height;

		float time;
		float delta_time;

		void update_matrices() {
			view_proj_matrix = proj_matrix * view_matrix;
		}
	};


	export class RHITexture {
	public:
		virtual ~RHITexture() = default;
	};

	export class RHI {
	public:
		virtual ~RHI() = default;
		virtual void init(bud::platform::Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation) = 0;

		virtual CommandHandle begin_frame() = 0;
		virtual void end_frame(CommandHandle cmd) = 0;

		virtual void cmd_resource_barrier(CommandHandle cmd, RHITexture* texture, ResourceState old_state, ResourceState new_state) = 0;

		virtual void cmd_bind_pipeline(CommandHandle cmd, void* pipeline) = 0;

		virtual void cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) = 0;

		virtual RHITexture* get_current_swapchain_texture() = 0;

		virtual uint32_t get_current_image_index() = 0;

		virtual void update_global_uniforms(uint32_t image_index, const SceneView& scene_view) = 0;

		virtual void cmd_push_constants(CommandHandle cmd, void* pipeline_layout, uint32_t size, const void* data) = 0;

		virtual void render_shadow_pass(CommandHandle cmd, uint32_t image_index) = 0;

		virtual void render_main_pass(CommandHandle cmd, uint32_t image_index) = 0;

		virtual void draw_frame(const bud::math::mat4& view, const bud::math::mat4& proj) = 0;
		virtual void wait_idle() = 0;
		virtual void cleanup() = 0;
		virtual void reload_shaders_async() = 0;
		virtual void load_model_async(const std::string& filepath) = 0;
		virtual void set_config(const RenderConfig& new_settings) = 0;
	};

	export std::unique_ptr<RHI> create_rhi(Backend backend);

	
}
