#pragma once

#include <string>
#include <vector>
#include <memory>

#include "src/core/bud.math.hpp"
#include "src/platform/bud.platform.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics {

	struct RenderPassBeginInfo {
		std::vector<Texture*> color_attachments;
		Texture* depth_attachment = nullptr;
		bool clear_color = false;
		bool clear_depth = false;
		bud::math::vec4 clear_color_value = { 0, 0, 0, 1 };
		float clear_depth_value = 1.0f;
		uint32_t base_array_layer = 0;
		uint32_t layer_count = 1;
	};

	class ResourcePool;

	class RHI {
	public:
		virtual ~RHI() = default;
		virtual void init(bud::platform::Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation, uint32_t inflight_frame_count) = 0;

		virtual void resize_swapchain(uint32_t width, uint32_t height) = 0;
		virtual bool is_swapchain_out_of_date() const { return false; }

		virtual CommandHandle begin_frame() = 0;
		virtual void end_frame(CommandHandle cmd) = 0;
		virtual void wait_idle() = 0;
		virtual void cleanup() = 0;

		// 资源管理
		virtual BufferHandle create_gpu_buffer(uint64_t size, ResourceState usage_state) = 0;
		virtual BufferHandle create_upload_buffer(uint64_t size) = 0;
		virtual void copy_buffer_immediate(BufferHandle src, BufferHandle dst, uint64_t size) = 0;
		virtual void destroy_buffer(BufferHandle block) = 0;
		virtual void* create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
		virtual void* create_compute_pipeline(const ComputePipelineDesc& desc) = 0;


		// 现有接口
		virtual void resource_barrier(CommandHandle cmd, Texture* texture, ResourceState old_state, ResourceState new_state) = 0;
		virtual void resource_barrier(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) = 0;
		virtual void cmd_bind_pipeline(CommandHandle cmd, void* pipeline) = 0;
		virtual void cmd_bind_descriptor_set(CommandHandle cmd, void* pipeline, uint32_t set_index) = 0;
		virtual void cmd_bind_storage_buffer(CommandHandle cmd, void* pipeline, uint32_t binding, BufferHandle buffer) = 0;
		virtual void cmd_bind_compute_texture(CommandHandle cmd, void* pipeline, uint32_t binding, Texture* texture, uint32_t mip_level = 0, bool is_storage = false, bool is_general = false) = 0;
		virtual void cmd_bind_compute_ubo(CommandHandle cmd, void* pipeline, uint32_t binding) = 0;
		virtual void cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) = 0;
		virtual void cmd_draw_indexed_indirect(CommandHandle cmd, BufferHandle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) = 0;
		virtual void cmd_dispatch(CommandHandle cmd, uint32_t group_x, uint32_t group_y, uint32_t group_z) = 0;
		virtual Texture* get_current_swapchain_texture() = 0;
		virtual uint32_t get_current_image_index() = 0;
		virtual void update_global_uniforms(uint32_t image_index, const SceneView& scene_view) = 0;
		virtual void cmd_push_constants(CommandHandle cmd, void* pipeline_layout, uint32_t size, const void* data) = 0;

		// 动态渲染
		virtual void cmd_begin_render_pass(CommandHandle cmd, const RenderPassBeginInfo& info) = 0;
		virtual void cmd_end_render_pass(CommandHandle cmd) = 0;

		virtual void cmd_bind_vertex_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) = 0;
		virtual void cmd_bind_index_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) = 0;
		virtual void cmd_draw_indexed(CommandHandle cmd, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) = 0;
		virtual void cmd_set_viewport(CommandHandle cmd, float width, float height) = 0;
		virtual void cmd_set_scissor(CommandHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;

		// 纹理管理
		virtual Texture* create_texture(const TextureDesc& desc, const void* initial_data, uint64_t size) = 0;
		virtual void update_bindless_texture(uint32_t index, Texture* texture) = 0;
		virtual void update_bindless_image(uint32_t index, Texture* texture, uint32_t mip_level = 0, bool is_storage = false) = 0;
		virtual Texture* get_fallback_texture() = 0;
		virtual void update_global_shadow_map(Texture* texture) = 0;
		virtual void cmd_copy_image(CommandHandle cmd, Texture* src, Texture* dst) = 0; // Shadow Caching
		virtual void cmd_blit_image(CommandHandle cmd, Texture* src, Texture* dst) = 0;
		virtual void cmd_set_scissor(CommandHandle cmd, uint32_t width, uint32_t height) = 0;

		virtual void set_render_config(const RenderConfig& new_render_config) = 0;
		virtual void reload_shaders_async() = 0;
		virtual void load_model_async(const std::string& filepath) = 0;

		virtual ResourcePool* get_resource_pool() = 0;

		virtual void cmd_set_depth_bias(CommandHandle cmd, float constant, float clamp, float slope) = 0;

		virtual void cmd_begin_debug_label(CommandHandle cmd, const std::string& name, float r, float g, float b) = 0;
		virtual void cmd_end_debug_label(CommandHandle cmd) = 0;

		virtual void set_debug_name(Texture* texture, ObjectType object_type, const std::string& name) = 0;
		virtual void set_debug_name(const BufferHandle& buffer, ObjectType object_type, const std::string& name) = 0;
		virtual void set_debug_name(CommandHandle cmd, ObjectType object_type, const std::string& name) = 0;

		virtual RenderStats get_stats() const = 0;
		virtual RenderStats& get_render_stats() = 0;
		virtual void add_culling_stats(uint32_t total, uint32_t visible, uint32_t casters) = 0;

		virtual void cmd_copy_buffer(CommandHandle cmd, BufferHandle src, BufferHandle dst, uint64_t size) = 0;
		virtual void cmd_copy_to_buffer(CommandHandle cmd, bud::graphics::BufferHandle dst, uint64_t offset, uint64_t size, const void* data) = 0;
	};
}
