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
		std::vector<TextureHandle> color_attachments;
		TextureHandle depth_attachment;
		bool clear_color = false;
		bool clear_depth = false;
		bool depth_read_only = false;
		bud::math::vec4 clear_color_value = { 0, 0, 0, 1 };
		float clear_depth_value = 1.0f;
		uint32_t base_array_layer = 0;
		uint32_t layer_count = 1;
		uint32_t render_width = 0;  // 0 = auto from attachment
		uint32_t render_height = 0; // 0 = auto from attachment
	};

	class ResourcePool;
	class Allocator;

	class RHI {
	public:
		virtual ~RHI() = default;
		virtual void init(bud::platform::Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation, uint32_t inflight_frame_count, bool is_headless = false) = 0;

		virtual void resize_swapchain(uint32_t width, uint32_t height) = 0;
		virtual bool is_swapchain_out_of_date() const { return false; }
		virtual bool is_headless() const { return false; }

		virtual uint32_t get_width() const = 0;
		virtual uint32_t get_height() const = 0;

		virtual CommandHandle begin_frame() = 0;
		virtual void end_frame(CommandHandle cmd) = 0;
		virtual CommandHandle get_current_graphics_command_buffer() = 0;
		virtual void wait_idle() = 0;
		virtual void cleanup() = 0;
		virtual uint32_t get_inflight_frame_count() const = 0;

		// 统一内存分配器接口
		virtual bud::graphics::Allocator* get_allocator() = 0;

		// 资源管理
		virtual BufferHandle create_gpu_buffer(uint64_t size, ResourceState usage_state) = 0;
		virtual BufferHandle create_upload_buffer(uint64_t size) = 0;
		virtual BufferHandle create_readback_buffer(uint64_t size) = 0;
		virtual void copy_buffer_immediate(BufferHandle src, BufferHandle dst, uint64_t size) = 0;
		virtual void copy_buffer_immediate_offset(BufferHandle src, BufferHandle dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) = 0;
		virtual void destroy_buffer(BufferHandle block) = 0;
		virtual PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
		virtual PipelineHandle create_compute_pipeline(const ComputePipelineDesc& desc) = 0;
		virtual void destroy_pipeline(PipelineHandle pipeline) = 0;

		// 现有接口
		virtual void resource_barrier(CommandHandle cmd, TextureHandle texture, ResourceState old_state, ResourceState new_state) = 0;
		virtual void resource_barrier(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) = 0;
		virtual void resource_barrier_release(CommandHandle cmd, TextureHandle texture, ResourceState old_state, ResourceState new_state, uint32_t src_queue_family, uint32_t dst_queue_family) = 0;
		virtual void resource_barrier_release(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state, uint32_t src_queue_family, uint32_t dst_queue_family) = 0;
		virtual void resource_barrier_acquire(CommandHandle cmd, TextureHandle texture, ResourceState old_state, ResourceState new_state, uint32_t src_queue_family, uint32_t dst_queue_family) = 0;
		virtual void resource_barrier_acquire(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state, uint32_t src_queue_family, uint32_t dst_queue_family) = 0;
		virtual uint32_t get_graphics_queue_family() const = 0;
		virtual uint32_t get_compute_queue_family() const = 0;
		virtual uint32_t get_transfer_queue_family() const = 0;
		virtual void cmd_bind_pipeline(CommandHandle cmd, PipelineHandle pipeline) = 0;
		virtual void cmd_bind_descriptor_set(CommandHandle cmd, PipelineHandle pipeline, uint32_t set_index) = 0;
		virtual void cmd_bind_descriptor_set(CommandHandle cmd, PipelineHandle pipeline, uint32_t set_index, uint64_t descriptor_set) = 0;

		// Descriptor set management for per-pass custom descriptor sets
		// Returns a VkDescriptorSetLayout as uint64_t
		virtual uint64_t create_descriptor_set_layout(const std::vector<DescriptorBinding>& bindings) = 0;
		// Returns a VkDescriptorSet as uint64_t
		virtual uint64_t create_descriptor_set(uint64_t layout) = 0;
		virtual void update_descriptor_set_buffer(uint64_t set, uint32_t binding, BufferHandle buffer, uint32_t descriptor_type = 0) = 0;
		virtual void update_descriptor_set_image(uint64_t set, uint32_t binding, TextureHandle texture, uint32_t mip_level = 0, uint32_t descriptor_type = 0) = 0;
		virtual void destroy_descriptor_set_layout(uint64_t layout) = 0;
		virtual void destroy_descriptor_set(uint64_t set) = 0;
		virtual void cmd_bind_storage_buffer(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding, BufferHandle buffer) = 0;
		virtual void cmd_bind_compute_texture(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding, TextureHandle texture, uint32_t mip_level = 0, bool is_storage = false, bool is_general = false) = 0;
		virtual void cmd_bind_compute_ubo(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding) = 0;
		virtual void cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) = 0;
		virtual void cmd_draw_indexed_indirect(CommandHandle cmd, BufferHandle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) = 0;
		virtual void cmd_dispatch(CommandHandle cmd, uint32_t group_x, uint32_t group_y, uint32_t group_z) = 0;
		// Async compute
		virtual CommandHandle begin_async_compute() = 0;
		virtual void end_async_compute() = 0;
		virtual void wait_compute_timeline(uint64_t value) = 0;
		virtual uint64_t get_compute_timeline_value() const = 0;
		virtual uint64_t get_graphics_timeline_value() const = 0;
		virtual uint64_t get_graphics_timeline_completed_value() const = 0;
		virtual uint64_t get_transfer_timeline_value() const { return 0; }
		virtual void wait_transfer_timeline(uint64_t value) {}
		virtual bool has_dedicated_compute_queue() const = 0;
		virtual bool has_dedicated_transfer_queue() const { return false; }
		virtual TextureHandle get_current_swapchain_texture() = 0;
		virtual uint32_t get_current_image_index() = 0;
		virtual uint32_t get_current_frame_index() const = 0;
		virtual void update_global_uniforms(uint32_t image_index, const SceneView& scene_view) = 0;
		virtual void cmd_push_constants(CommandHandle cmd, PipelineHandle pipeline, uint32_t size, const void* data) = 0;

		// 动态渲染
		virtual void cmd_begin_render_pass(CommandHandle cmd, const RenderPassBeginInfo& info) = 0;
		virtual void cmd_end_render_pass(CommandHandle cmd) = 0;

		virtual void cmd_bind_vertex_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) = 0;
		virtual void cmd_bind_index_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer, bool is_u16 = false) = 0;
		virtual void cmd_draw_indexed(CommandHandle cmd, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) = 0;
		virtual void cmd_draw_mesh_tasks(CommandHandle cmd, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) = 0;
		virtual void cmd_set_viewport(CommandHandle cmd, float width, float height) = 0;
		virtual void cmd_set_scissor(CommandHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;

		// 纹理管理
		virtual TextureHandle create_texture(const TextureDesc& desc, const void* initial_data, uint64_t size) = 0;
		virtual TextureHandle create_texture_async(const TextureDesc& desc, const void* initial_data, uint64_t size, uint32_t bindless_slot) = 0;
		virtual void destroy_texture(TextureHandle handle) = 0;
		virtual void queue_bindless_fallback(uint32_t slot, TextureHandle tex) = 0;
		virtual void update_bindless_texture(uint32_t index, TextureHandle texture) = 0;
		virtual void update_bindless_texture_current_frame(uint32_t index, TextureHandle texture) { update_bindless_texture(index, texture); }
		virtual void update_bindless_image(uint32_t index, TextureHandle texture, uint32_t mip_level = 0, bool is_storage = false) = 0;
		virtual TextureHandle get_fallback_texture() = 0;
		virtual void update_global_shadow_map(TextureHandle texture) = 0;
		virtual void update_global_instance_data(bud::graphics::BufferHandle buffer) = 0;
		virtual void update_global_csm_instance_data(bud::graphics::BufferHandle buffer) = 0;
		virtual void update_global_page_table(bud::graphics::BufferHandle buffer) = 0;
		virtual void update_global_page_pool(bud::graphics::BufferHandle buffer) = 0;
		virtual void update_global_materials_buffer(bud::graphics::BufferHandle buffer) = 0;
		virtual void cmd_copy_image(CommandHandle cmd, TextureHandle src, TextureHandle dst) = 0; // Shadow Caching
		virtual void cmd_blit_image(CommandHandle cmd, TextureHandle src, TextureHandle dst) = 0;
		virtual void cmd_set_scissor(CommandHandle cmd, uint32_t width, uint32_t height) = 0;

		virtual Texture* get_texture(TextureHandle handle) = 0;
		virtual TextureDesc get_texture_desc(TextureHandle handle) const = 0;
		virtual Buffer* get_buffer(BufferHandle handle) = 0;
		virtual BufferDesc get_buffer_desc(BufferHandle handle) const = 0;

		virtual void set_render_config(const RenderConfig& new_render_config) = 0;
		virtual void reload_shaders_async() = 0;
		virtual void load_model_async(const std::string& filepath) = 0;

		virtual ResourcePool* get_resource_pool() = 0;

		virtual void cmd_set_depth_bias(CommandHandle cmd, float constant, float clamp, float slope) = 0;

		virtual void cmd_begin_debug_label(CommandHandle cmd, const std::string& name, float r, float g, float b) = 0;
		virtual void cmd_end_debug_label(CommandHandle cmd) = 0;

		virtual void set_debug_name(TextureHandle texture, ObjectType object_type, const std::string& name) = 0;
		virtual void set_debug_name(const BufferHandle& buffer, ObjectType object_type, const std::string& name) = 0;
		virtual void set_debug_name(CommandHandle cmd, ObjectType object_type, const std::string& name) = 0;

		virtual RenderStats get_stats() const = 0;
		virtual RenderStats& get_render_stats() = 0;
		virtual void add_culling_stats(uint32_t total, uint32_t visible, uint32_t casters) = 0;

		virtual void cmd_copy_buffer(CommandHandle cmd, bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size) = 0;
		virtual void cmd_copy_to_buffer(CommandHandle cmd, bud::graphics::BufferHandle dst, uint64_t offset, uint64_t size, const void* data) = 0;
		virtual void cmd_fill_buffer(CommandHandle cmd, bud::graphics::BufferHandle dst, uint64_t offset, uint64_t size, uint32_t data) = 0;
		virtual void cmd_copy_image_to_buffer(CommandHandle cmd, TextureHandle src, bud::graphics::BufferHandle dst) = 0;
	};
}
