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
		bud::graphics::Texture* texture;
		uint32_t mip_level;
		bool is_storage;
		bool is_general = false; // Use VK_IMAGE_LAYOUT_GENERAL when image is a storage image read as sampler
	};

	struct UBOBinding {};

	using ComputeResource = std::variant<bud::graphics::BufferHandle, ImageBinding, UBOBinding>;

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
		void copy_buffer_immediate(bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size) override;
		void copy_buffer_immediate_offset(bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) override;
		void destroy_buffer(bud::graphics::BufferHandle block) override;

		// 帧控制
		CommandHandle begin_frame() override;
		void end_frame(CommandHandle cmd) override;
		Texture* get_current_swapchain_texture() override;
		uint32_t get_current_image_index() override;

		// 命令录制 
		void resource_barrier(CommandHandle cmd, bud::graphics::Texture* texture, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) override;
		void resource_barrier(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) override;
		void cmd_bind_pipeline(CommandHandle cmd, void* pipeline) override;
		void cmd_push_constants(CommandHandle cmd, void* pipeline_layout, uint32_t size, const void* data) override;

		// 动态渲染通道
		void cmd_begin_render_pass(CommandHandle cmd, const bud::graphics::RenderPassBeginInfo& info) override;
		void cmd_end_render_pass(CommandHandle cmd) override;

		void cmd_bind_vertex_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) override;
		void cmd_bind_index_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) override;
		void cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) override;
		void cmd_draw_indexed(CommandHandle cmd, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) override;
		void cmd_draw_indexed_indirect(CommandHandle cmd, bud::graphics::BufferHandle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) override;

		void cmd_set_viewport(CommandHandle cmd, float width, float height) override;
		void cmd_set_scissor(CommandHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) override;
		void cmd_set_scissor(CommandHandle cmd, uint32_t width, uint32_t height) override;
		void cmd_set_depth_bias(CommandHandle cmd, float constant, float clamp, float slope) override;
		void update_global_shadow_map(Texture* texture) override;
		void update_global_instance_data(bud::graphics::BufferHandle buffer) override;
		void cmd_copy_image(CommandHandle cmd, Texture* src, Texture* dst) override;
		void cmd_blit_image(CommandHandle cmd, Texture* src, Texture* dst) override;

		bud::graphics::Texture* create_texture(const bud::graphics::TextureDesc& desc, const void* initial_data, uint64_t size) override;
		void update_bindless_texture(uint32_t index, bud::graphics::Texture* texture) override;
		void update_bindless_image(uint32_t index, bud::graphics::Texture* texture, uint32_t mip_level = 0, bool is_storage = false) override;
		bud::graphics::Texture* get_fallback_texture() override;

		// 杂项 / 待重构
		void set_render_config(const bud::graphics::RenderConfig& new_render_config) override;
		void update_global_uniforms(uint32_t image_index, const bud::graphics::SceneView& scene_view) override;
		void reload_shaders_async() override;
		void load_model_async(const std::string& filepath) override;


		void* create_graphics_pipeline(const bud::graphics::GraphicsPipelineDesc& desc) override;
		void* create_compute_pipeline(const bud::graphics::ComputePipelineDesc& desc) override;
		void destroy_pipeline(void* pipeline) override;


		void cmd_bind_descriptor_set(CommandHandle cmd, void* pipeline, uint32_t set_index) override;
		void cmd_bind_storage_buffer(CommandHandle cmd, void* pipeline, uint32_t binding, bud::graphics::BufferHandle buffer) override;
		void cmd_bind_compute_texture(CommandHandle cmd, void* pipeline, uint32_t binding, bud::graphics::Texture* texture, uint32_t mip_level = 0, bool is_storage = false, bool is_general = false) override;
		void cmd_bind_compute_ubo(CommandHandle cmd, void* pipeline, uint32_t binding) override;
		void cmd_dispatch(CommandHandle cmd, uint32_t group_x, uint32_t group_y, uint32_t group_z) override;

		VulkanMemoryAllocator* get_memory_allocator() { return memory_allocator.get(); }
		bud::graphics::ResourcePool* get_resource_pool() override { return resource_pool.get(); }

		// Debug
		void cmd_begin_debug_label(CommandHandle cmd, const std::string& name, float r, float g, float b) override;
		void cmd_end_debug_label(CommandHandle cmd) override;

		void set_debug_name(Texture* texture, ObjectType object_type, const std::string& name) override;
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
		void cmd_copy_image_to_buffer(CommandHandle cmd, bud::graphics::Texture* src, bud::graphics::BufferHandle dst) override;

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
		VkSurfaceKHR surface = nullptr;
		VkQueue graphics_queue = nullptr;
		VkQueue present_queue = nullptr;
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

		// UBO
		VkDescriptorSetLayout global_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout compute_set_layout = VK_NULL_HANDLE; // Used for per-dispatch storage buffer binding
		VkDescriptorPool global_descriptor_pool = VK_NULL_HANDLE;
		VkSampler default_sampler = VK_NULL_HANDLE;
		VkSampler shadow_sampler = VK_NULL_HANDLE;
		VulkanTexture dummy_depth_texture; // Placeholder for shadow map binding

		std::unordered_map<bud::graphics::Texture*, VulkanTexture> textures;
		std::vector<std::unique_ptr<bud::graphics::Texture>> texture_objects;


	

		// Compute Binding state
		std::unordered_map<uint32_t, ComputeResource> current_compute_bindings;
		void* current_compute_pipeline = nullptr;

        std::shared_ptr<bud::graphics::Texture> fallback_texture_ptr;
		std::atomic<bool> swapchain_out_of_date{false};

		RenderStats current_stats;
		
		PFN_vkCmdPushDescriptorSetKHR fpCmdPushDescriptorSetKHR = nullptr;

		std::vector<VkPipelineLayout> created_layouts;
	};
}
