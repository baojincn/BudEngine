module;

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <mutex>
#include <memory>
#include <unordered_map> // [FIX] Include
#include <SDL3/SDL.h>

export module bud.graphics.vulkan;

import bud.io;
import bud.math;     
import bud.platform; 
import bud.threading;
import bud.graphics;
import bud.graphics.defs;
import bud.graphics.types;

import bud.vulkan.types;      
import bud.vulkan.memory;     
import bud.vulkan.pool;       
import bud.vulkan.pipeline;   
import bud.vulkan.descriptors;

namespace bud::graphics::vulkan {

	export using VkInstance = struct VkInstance_T*;
	export using VkPhysicalDevice = struct VkPhysicalDevice_T*;
	export using VkDevice = struct VkDevice_T*;
	export using VkQueue = struct VkQueue_T*;
	export using VkSurfaceKHR = struct VkSurfaceKHR_T*;

	// 工具函数声明
	VulkanLayoutTransition get_vk_transition(bud::graphics::ResourceState state);

	export class VulkanRHI : public bud::graphics::RHI {
	public:
		~VulkanRHI() = default;

		// --- 生命周期 ---
		void init(bud::platform::Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation) override;
		void cleanup() override;
		void wait_idle() override;

		bud::graphics::MemoryBlock create_gpu_buffer(uint64_t size, bud::graphics::ResourceState usage_state) override;
		bud::graphics::MemoryBlock create_upload_buffer(uint64_t size) override;
		void copy_buffer_immediate(bud::graphics::MemoryBlock src, bud::graphics::MemoryBlock dst, uint64_t size) override;
		void destroy_buffer(bud::graphics::MemoryBlock block) override; // [FIX] Implement

		// --- 帧控制 ---
		CommandHandle begin_frame() override;
		void end_frame(CommandHandle cmd) override;
		Texture* get_current_swapchain_texture() override;
		uint32_t get_current_image_index() override;

		// --- 命令录制 (原子操作) ---
		void resource_barrier(CommandHandle cmd, bud::graphics::Texture* texture, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) override;
		void cmd_bind_pipeline(CommandHandle cmd, void* pipeline) override;
		void cmd_push_constants(CommandHandle cmd, void* pipeline_layout, uint32_t size, const void* data) override;

		// [核心] 动态渲染通道
		void cmd_begin_render_pass(CommandHandle cmd, const bud::graphics::RenderPassBeginInfo& info) override;
		void cmd_end_render_pass(CommandHandle cmd) override;

		void cmd_bind_vertex_buffer(CommandHandle cmd, void* buffer) override;
		void cmd_bind_index_buffer(CommandHandle cmd, void* buffer) override;
		void cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) override;
		void cmd_draw_indexed(CommandHandle cmd, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) override;

		void cmd_set_viewport(CommandHandle cmd, float width, float height) override;
		void cmd_set_scissor(CommandHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) override;
		void cmd_set_scissor(CommandHandle cmd, uint32_t width, uint32_t height) override;
		void cmd_set_depth_bias(CommandHandle cmd, float constant, float clamp, float slope) override;
		void update_global_shadow_map(Texture* texture) override;
		void cmd_copy_image(CommandHandle cmd, Texture* src, Texture* dst) override;

		bud::graphics::Texture* create_texture(const bud::graphics::TextureDesc& desc, const void* initial_data, uint64_t size) override;
		void update_bindless_texture(uint32_t index, bud::graphics::Texture* texture) override;
		bud::graphics::Texture* get_fallback_texture() override;

		// --- 杂项 / 待重构 ---
		void set_render_config(const bud::graphics::RenderConfig& new_render_config) override;
		void update_global_uniforms(uint32_t image_index, const bud::graphics::SceneView& scene_view) override;
		void reload_shaders_async() override;
		void load_model_async(const std::string& filepath) override;

		// [Pipeline System]
		void* create_graphics_pipeline(const bud::graphics::GraphicsPipelineDesc& desc) override;
		void cmd_bind_descriptor_set(CommandHandle cmd, void* pipeline, uint32_t set_index) override;

		VulkanMemoryAllocator* get_memory_allocator() { return memory_allocator.get(); }
		//VulkanResourcePool* get_resource_pool() { return resource_pool.get(); }
		bud::graphics::ResourcePool* get_resource_pool() override { return resource_pool.get(); }

		// Debug
		void cmd_begin_debug_label(CommandHandle cmd, const std::string& name, float r, float g, float b) override;
		void cmd_end_debug_label(CommandHandle cmd) override;

		void set_debug_name(Texture* texture, ObjectType object_type, const std::string& name) override;
		void set_debug_name(const MemoryBlock& buffer, ObjectType object_type, const std::string& name) override;
		void set_debug_name(CommandHandle cmd, ObjectType object_type, const std::string& name) override;

	private:
		// --- 初始化辅助 ---
		void create_instance(SDL_Window* window, bool enable_validation);
		void create_surface(SDL_Window* window);
		void pick_physical_device();
		void create_logical_device(bool enable_validation);
		void create_swapchain(SDL_Window* window);
		void create_image_views();
		void create_command_pool();
		void create_command_buffer();
		void create_sync_objects();

		VkCommandBuffer begin_single_time_commands();
		void end_single_time_commands(VkCommandBuffer command_buffer);
		void transition_image_layout_immediate(VkImage image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout);
		void copy_buffer_to_image(VkImage image, VkBuffer buffer, uint32_t width, uint32_t height);
		void generate_mipmaps(VkImage image, VkFormat format, int32_t texWidth, int32_t texHeight, uint32_t mipLevels); // [FIX] helper
		
		QueueFamilyIndices find_queue_families(VkPhysicalDevice device);
		SwapChainSupportDetails query_swapchain_support(VkPhysicalDevice device);
		VkSurfaceFormatKHR choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats);
		VkPresentModeKHR choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes);
		VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, SDL_Window* window);

		// Debug
		VkResult create_debug_utils_messenger_ext(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		void destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator);
		void setup_debug_messenger(bool enable);
		static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);

		void set_object_debug_name(uint64_t object_handle, ObjectType object_type, const std::string& name);

	private:
		struct FrameData {
			VkSemaphore image_available_semaphore = nullptr;
			VkFence in_flight_fence = nullptr;
			VkCommandPool main_command_pool = nullptr;
			VkCommandBuffer main_command_buffer = nullptr;
			VkBuffer uniform_buffer = nullptr;       // Per-frame UBO
			VkDeviceMemory uniform_memory = nullptr;
			void* uniform_mapped = nullptr;          // Persistently mapped
			VkDescriptorSet global_descriptor_set = VK_NULL_HANDLE;
		};

		// 核心 Vulkan 对象
		VkInstance instance = nullptr;
		VkPhysicalDevice physical_device = nullptr;
		VkDevice device = nullptr;
		VkSurfaceKHR surface = nullptr;
		VkQueue graphics_queue = nullptr;
		VkQueue present_queue = nullptr;
		VkDebugUtilsMessengerEXT debug_messenger = nullptr;
		bool enable_validation_layers = false;

		const std::vector<const char*> validation_layers = { "VK_LAYER_KHRONOS_validation" };
		std::vector<const char*> device_extensions = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
			VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME
		};

		// Swapchain
		VkSwapchainKHR swapchain = nullptr;
		std::vector<VkImage> swapchain_images;
		std::vector<VkImageView> swapchain_image_views;
		std::vector<VulkanTexture> swapchain_textures_wrappers; // 给 Graph 用的 Handle
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

		// [基础设施] 接管所有资源管理
		std::unique_ptr<VulkanMemoryAllocator> memory_allocator;
		std::unique_ptr<VulkanResourcePool>    resource_pool;
		std::unique_ptr<VulkanPipelineCache>   pipeline_cache;
		std::vector<VulkanDescriptorAllocator> descriptor_allocators;

		// [UBO 系统]
		VkDescriptorSetLayout global_set_layout = VK_NULL_HANDLE;
		VkDescriptorPool global_descriptor_pool = VK_NULL_HANDLE;
		VkSampler default_sampler = VK_NULL_HANDLE;
		VkSampler shadow_sampler = VK_NULL_HANDLE; // [FIX] Shadow Sampler with Compare
		VulkanTexture dummy_depth_texture; // Placeholder for shadow map binding

		std::unordered_map<VkBuffer, VkDeviceMemory> buffer_memory_map; // [FIX] Track memory
	
		// [FIX] Track created pipeline layouts for cleanup
		std::vector<VkPipelineLayout> created_layouts;

		VulkanTexture* fallback_texture_ptr = nullptr;
	};
}
