#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <mutex>
#include <memory>
#include <variant>
#include <unordered_map>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/platform/bud.platform.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"

#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"
#include "src/graphics/vulkan/bud.vulkan.pool.hpp"
#include "src/graphics/vulkan/bud.vulkan.pipeline.hpp"
#include "src/graphics/vulkan/bud.vulkan.descriptors.hpp"


#ifdef BUD_ENABLE_AFTERMATH
#include <GFSDK_Aftermath.h>
#endif

namespace bud::graphics::vulkan {

	struct ImageBinding {
		TextureHandle texture;
		uint32_t mip_level;
		bool is_storage;
		bool is_general = false; // Use VK_IMAGE_LAYOUT_GENERAL when image is a storage image read as sampler
	};

	struct UBOBinding {};

	using ComputeResource = std::variant<bud::graphics::BufferHandle, ImageBinding, UBOBinding>;

	struct VulkanPipelineObject {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
		ComputePipelineDesc::LayoutKind compute_layout_kind = ComputePipelineDesc::LayoutKind::HiZCulling;
	};

	 using VkInstance = struct VkInstance_T*;
	 using VkPhysicalDevice = struct VkPhysicalDevice_T*;
	 using VkDevice = struct VkDevice_T*;
	 using VkQueue = struct VkQueue_T*;
	 using VkSurfaceKHR = struct VkSurfaceKHR_T*;

    // 工具函数声明 - 旧的 transition helper 已迁移到 synchronization2 helpers (sync2::get_transition2)

	 class VulkanRHI : public bud::graphics::RHI {
	public:
		~VulkanRHI() = default;

		void init(bud::platform::Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation, uint32_t inflight_frame_count, bool is_headless = false) override;
		void cleanup() override;
		void wait_idle() override;
		uint32_t get_inflight_frame_count() const override { return max_frames_in_flight; }

		void resize_swapchain(uint32_t width, uint32_t height) override;
		bool is_swapchain_out_of_date() const override { return swapchain_out_of_date.load(std::memory_order_acquire); }
		bool is_headless() const override { return this->headless_mode; }

		uint32_t get_width() const override;
		uint32_t get_height() const override;

		bud::graphics::BufferHandle create_gpu_buffer(uint64_t size, bud::graphics::ResourceState usage_state) override;
		bud::graphics::BufferHandle create_upload_buffer(uint64_t size) override;
		bud::graphics::BufferHandle create_readback_buffer(uint64_t size) override;
		void copy_buffer_immediate(bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size) override;
		void copy_buffer_immediate_offset(bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) override;
		void destroy_buffer(bud::graphics::BufferHandle block) override;

		// 帧控制
		CommandHandle begin_frame() override;
		void end_frame(CommandHandle cmd) override;
		TextureHandle get_current_swapchain_texture() override;
		uint32_t get_current_image_index() override;

		// 命令录制 
		void resource_barrier(CommandHandle cmd, TextureHandle texture, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) override;
		void resource_barrier(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) override;

		// 动态渲染通道
		void cmd_begin_render_pass(CommandHandle cmd, const bud::graphics::RenderPassBeginInfo& info) override;
		void cmd_end_render_pass(CommandHandle cmd) override;

		void cmd_bind_vertex_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) override;
		void cmd_bind_index_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer, bool is_u16 = false) override;
		void cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) override;
		void cmd_draw_indexed(CommandHandle cmd, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) override;
		void cmd_draw_indexed_indirect(CommandHandle cmd, bud::graphics::BufferHandle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) override;

		void cmd_set_viewport(CommandHandle cmd, float width, float height) override;
		void cmd_set_scissor(CommandHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) override;
		void cmd_set_scissor(CommandHandle cmd, uint32_t width, uint32_t height) override;
		void cmd_set_depth_bias(CommandHandle cmd, float constant, float clamp, float slope) override;
		void update_global_shadow_map(TextureHandle texture) override;
		void update_global_instance_data(bud::graphics::BufferHandle buffer) override;
		void update_global_csm_instance_data(bud::graphics::BufferHandle buffer) override;
		void update_global_page_table(bud::graphics::BufferHandle buffer) override;
		void update_global_page_pool(bud::graphics::BufferHandle buffer) override;
		void update_global_materials_buffer(bud::graphics::BufferHandle buffer) override;
		void cmd_copy_image(CommandHandle cmd, TextureHandle src, TextureHandle dst) override;
		void cmd_blit_image(CommandHandle cmd, TextureHandle src, TextureHandle dst) override;

		TextureHandle create_texture(const bud::graphics::TextureDesc& desc, const void* initial_data, uint64_t size) override;
		TextureHandle create_texture_async(const bud::graphics::TextureDesc& desc, const void* initial_data, uint64_t size, uint32_t bindless_slot) override;
		void destroy_texture(TextureHandle handle) override;
		void queue_bindless_fallback(uint32_t slot, TextureHandle tex) override;
		void update_bindless_texture(uint32_t index, TextureHandle texture) override;
		void update_bindless_texture_current_frame(uint32_t index, TextureHandle texture) override;
		void update_bindless_image(uint32_t index, TextureHandle texture, uint32_t mip_level = 0, bool is_storage = false) override;
		TextureHandle get_fallback_texture() override;

		Texture* get_texture(TextureHandle handle) override;
		TextureDesc get_texture_desc(TextureHandle handle) const override;
		class VulkanTexture* get_vulkan_texture(TextureHandle handle);
		Buffer* get_buffer(BufferHandle handle) override;
		BufferDesc get_buffer_desc(BufferHandle handle) const override;
		class VulkanBuffer* get_vulkan_buffer(BufferHandle handle);
		struct VulkanPipelineObject* get_pipeline_obj(PipelineHandle handle);

		// Returns the current render-frame slot (0..max_frames_in_flight-1).
		uint32_t get_current_frame_index() const override { return current_frame; }

		// 杂项 / 待重构
		void set_render_config(const bud::graphics::RenderConfig& new_render_config) override;
		void update_global_uniforms(uint32_t image_index, const bud::graphics::SceneView& scene_view) override;
		void reload_shaders_async() override;
		void load_model_async(const std::string& filepath) override;

		PipelineHandle create_graphics_pipeline(const bud::graphics::GraphicsPipelineDesc& desc) override;
		PipelineHandle create_compute_pipeline(const bud::graphics::ComputePipelineDesc& desc) override;
		void destroy_pipeline(PipelineHandle pipeline) override;

		void cmd_bind_pipeline(CommandHandle cmd, PipelineHandle pipeline) override;
		void cmd_push_constants(CommandHandle cmd, PipelineHandle pipeline, uint32_t size, const void* data) override;
		void cmd_bind_descriptor_set(CommandHandle cmd, PipelineHandle pipeline, uint32_t set_index) override;
		void cmd_bind_storage_buffer(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding, bud::graphics::BufferHandle buffer) override;
		void cmd_bind_compute_texture(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding, TextureHandle texture, uint32_t mip_level = 0, bool is_storage = false, bool is_general = false) override;
		void cmd_bind_compute_ubo(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding) override;
		void cmd_dispatch(CommandHandle cmd, uint32_t group_x, uint32_t group_y, uint32_t group_z) override;
		CommandHandle begin_async_compute() override;
		void end_async_compute() override;
		void wait_compute_timeline(uint64_t value) override;
		uint64_t get_compute_timeline_value() const override { return compute_timeline_value; }
		uint64_t get_graphics_timeline_value() const override { return graphics_timeline_value; }
		uint64_t get_graphics_timeline_completed_value() const override;
		bool has_dedicated_compute_queue() const override { return has_dedicated_compute_queue_; }

		VulkanMemoryAllocator* get_memory_allocator() { return memory_allocator.get(); }
		bud::graphics::ResourcePool* get_resource_pool() override { return resource_pool.get(); }

		// Debug
		void cmd_begin_debug_label(CommandHandle cmd, const std::string& name, float r, float g, float b) override;
		void cmd_end_debug_label(CommandHandle cmd) override;

		void set_debug_name(TextureHandle texture, ObjectType object_type, const std::string& name) override;
		void set_debug_name(const bud::graphics::BufferHandle& buffer, ObjectType object_type, const std::string& name) override;
		void set_debug_name(CommandHandle cmd, ObjectType object_type, const std::string& name) override;

		RenderStats get_stats() const override { return current_stats; }
		RenderStats& get_render_stats() override { return current_stats; }
		void add_culling_stats(uint32_t total, uint32_t visible, uint32_t casters) override {
			current_stats.gpu_total_objects += total;
			current_stats.gpu_visible_objects += visible;
			current_stats.shadow_casters += casters;
		}

		void cmd_copy_buffer(CommandHandle cmd, bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size) override;
		void cmd_copy_to_buffer(CommandHandle cmd, bud::graphics::BufferHandle dst, uint64_t offset, uint64_t size, const void* data) override;
		void cmd_copy_image_to_buffer(CommandHandle cmd, TextureHandle src, bud::graphics::BufferHandle dst) override;

		bud::graphics::BufferHandle create_dedicated_upload_buffer(uint64_t size);

	private:
		void create_instance(VkInstance& vk_instance, bool enable_validation);
		void create_surface(bud::platform::Window* window);
		void pick_physical_device();
		void create_logical_device(bool enable_validation);
		void create_swapchain(bud::platform::Window* window);
		void create_image_views();
		void create_command_pool();
		void create_command_buffer();
		void create_sync_objects();

		VkCommandBuffer begin_single_time_commands();
		void end_single_time_commands(VkCommandBuffer command_buffer);
		// Records staging->image copy + mipmap generation + final transition
		// into the given command buffer (shared by sync/async texture uploads).
		void record_texture_upload(VkCommandBuffer cb, class VulkanTexture* tex, VkBuffer staging_buf, uint64_t staging_offset, const TextureDesc& desc);
		void queue_bindless_update(uint32_t slot, TextureHandle tex, VkFence fence, VkCommandBuffer cb, bud::graphics::BufferHandle staging);
		void apply_pending_bindless();
		void transition_image_layout_immediate(VkImage image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout);
		void copy_buffer_to_image(VkImage image, VkBuffer buffer, uint64_t buffer_offset, uint32_t width, uint32_t height);
		void generate_mipmaps(VkImage image, VkFormat format, int32_t texWidth, int32_t texHeight, uint32_t mipLevels); 
		
		QueueFamilyIndices find_queue_families(VkPhysicalDevice device);
		SwapChainSupportDetails query_swapchain_support(VkPhysicalDevice device);
		VkSurfaceFormatKHR choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats);
		VkPresentModeKHR choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes);
		VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, bud::platform::Window* window);

		// Debug
		VkResult create_debug_utils_messenger_ext(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		void destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator);
		void setup_debug_messenger(bool enable);
		static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);

		void set_object_debug_name(uint64_t object_handle, ObjectType object_type, const std::string& name);

#ifdef BUD_ENABLE_AFTERMATH
		bool init_aftermath();
#endif

	private:
		struct FrameData {
			VkSemaphore image_available_semaphore = nullptr;
			VkFence in_flight_fence = nullptr;
			VkCommandPool main_command_pool = nullptr;
			VkCommandBuffer main_command_buffer = nullptr;
			// 2 timestamp queries (frame start / frame end) for GPU frame time.
			// Read back after the in-flight fence is reached (next begin_frame).
			VkQueryPool timestamp_pool = nullptr;
			bool timestamp_ready = false; // true once timestamps were written once
			// Per-frame async compute command pool/buffer (compute family).
			VkCommandPool async_command_pool = nullptr;
			VkCommandBuffer async_command_buffer = nullptr;
			VkBuffer uniform_buffer = nullptr;       // Per-frame UBO (allocated via VMA when available)
			VmaAllocation uniform_allocation = VK_NULL_HANDLE; // VMA allocation for the UBO (if used)
			VmaAllocationInfo uniform_alloc_info = {}; // allocation info containing mapped ptr
			void* uniform_mapped = nullptr;          // Persistently mapped (points to alloc_info.pMappedData when VMA is used)
			VkDescriptorSet global_descriptor_set = VK_NULL_HANDLE;
		};

		
		bud::platform::Window* platform_window = nullptr;

		VkInstance instance = nullptr;
		VkPhysicalDevice physical_device = nullptr;
		VkDevice device = nullptr;
		uint32_t instance_api_version = VK_API_VERSION_1_1;
		uint32_t device_api_version = VK_API_VERSION_1_1;
		VkSurfaceKHR surface = nullptr;
		VkQueue graphics_queue = nullptr;
		VkQueue present_queue = nullptr;
		// Device timestamp period in nanoseconds per tick (Vulkan timestamps).
		// Guarded by timestamp_queries_supported.
		float timestamp_period_ns = 1.0f;
		bool timestamp_queries_supported = false;
		// Dedicated transfer/copy queue for async uploads (may alias the
		// graphics queue when no dedicated transfer family exists).
		VkQueue copy_queue = nullptr;
		bool has_dedicated_copy_queue = false;
		uint32_t graphics_family_index = 0;
		uint32_t copy_family_index = 0;
		bool upload_pending_this_frame = false;
		// Async compute queue (dedicated compute family) + per-frame async
		// command pool/buffer + compute timeline semaphore.
		VkQueue compute_queue = nullptr;
		bool has_dedicated_compute_queue_ = false;
		uint32_t compute_family_index = 0;
		VkSemaphore compute_timeline_semaphore = nullptr; // shared, value-based
		uint64_t compute_timeline_value = 0;
		VkSemaphore graphics_timeline_semaphore = nullptr; // main graphics queue
		uint64_t graphics_timeline_value = 0;
		bool async_recording = false;
		bool async_compute_pending_this_frame = false;

		// Asynchronous texture upload: one command buffer per in-flight frame,
		// submitted to the graphics queue without blocking the main render.
		// The upload fence signals completion; bindless binding is applied at
		// frame begin once the fence is reached.
		// Shared upload resources (fence/cb/staging) released exactly once when
		// the LAST frame slot applies the pending update (reference counted via
		// shared_ptr). The same PendingBindless is replicated per frame slot so
		// every frame's descriptor set gets bound over consecutive frames, but
		// the underlying fence/cb/staging must not be freed more than once.
		struct BindlessUploadState {
			VkFence upload_fence = VK_NULL_HANDLE;
			VkCommandBuffer cb = VK_NULL_HANDLE;
			bud::graphics::BufferHandle staging;
		};
		struct PendingBindless {
			uint32_t slot = 0;
			TextureHandle tex;
			std::shared_ptr<BindlessUploadState> state; // null for fallback (already ready)
		};
		// Releases a BindlessUploadState's GPU resources (uses RHI members).
		void release_bindless_upload_state(BindlessUploadState* s);
		// Per-frame-slot pending bindless updates (indexed by frame slot).
		std::vector<std::vector<PendingBindless>> pending_bindless;
		std::mutex pending_bindless_mutex;
		std::mutex single_time_mutex;
		std::mutex queue_submit_mutex;
		// Dedicated graphics-family pool for one-time async texture upload cbs
		// (kept separate from the per-frame main command pools).
		VkCommandPool texture_upload_pool = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT debug_messenger = nullptr;
		bool enable_validation_layers = false;
		bool aftermath_initialized = false;
		bool headless_mode = false;

		const std::vector<const char*> validation_layers = { "VK_LAYER_KHRONOS_validation" };
		std::vector<const char*> device_extensions = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
			VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
			VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
		};

		// Swapchain
		VkSwapchainKHR swapchain = nullptr;
		std::vector<VkImage> swapchain_images;
		std::vector<VkImageView> swapchain_image_views;
		std::vector<VulkanTexture> swapchain_textures_wrappers;
		std::vector<TextureHandle> swapchain_texture_handles;
		VkFormat swapchain_image_format;
		VkExtent2D swapchain_extent;

		// Frame Data
		std::vector<FrameData> frames;
		std::vector<VkSemaphore> render_finished_semaphores;
		uint32_t max_frames_in_flight = 2;
		uint32_t current_frame = 0;
		uint32_t current_image_index = 0;

		RenderConfig render_config;
		bud::threading::TaskScheduler* task_scheduler = nullptr;

		// 资源管理
		bud::graphics::Allocator* get_allocator() override { return memory_allocator.get(); }

		std::unique_ptr<VulkanMemoryAllocator> memory_allocator;
		std::unique_ptr<VulkanResourcePool>    resource_pool;
		std::unique_ptr<VulkanPipelineCache>   pipeline_cache;
		std::vector<VulkanDescriptorAllocator> descriptor_allocators;

		struct VulkanPipelineSlot {
			std::unique_ptr<struct VulkanPipelineObject> obj;
			bool in_use = false;
		};
		std::vector<VulkanPipelineSlot> pipeline_pool;
		std::vector<uint32_t> free_pipeline_indices;

		// UBO
		VkDescriptorSetLayout global_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_hiz_cull_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_hiz_mip_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_ml_identity_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_ao_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_ao_blur_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_ao_temporal_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_hierarchy_traversal_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_page_emit_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_cluster_cull_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_clear_stats_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_csm_cull_set_layout = VK_NULL_HANDLE;
		VkDescriptorPool global_descriptor_pool = VK_NULL_HANDLE;
		VkSampler default_sampler = VK_NULL_HANDLE;
		VkSampler shadow_sampler = VK_NULL_HANDLE;
		VulkanTexture dummy_depth_texture; // Placeholder for shadow map binding

		std::unordered_map<bud::graphics::Texture*, VulkanTexture> textures;
		std::vector<std::unique_ptr<bud::graphics::Texture>> texture_objects;

		// Compute Binding state
		std::unordered_map<uint32_t, ComputeResource> current_compute_bindings;
		PipelineHandle current_compute_pipeline;

		TextureHandle fallback_texture_handle;
		TextureHandle dummy_depth_texture_handle;
		std::atomic<bool> swapchain_out_of_date{false};

		RenderStats current_stats;
		
		PFN_vkCmdPushDescriptorSetKHR fpCmdPushDescriptorSetKHR = nullptr;

		std::vector<VkPipelineLayout> created_layouts;
	};
}
