module;

#include <vulkan/vulkan.h>
#include <vector>
#include <optional>
#include <mutex>
#include <SDL3/SDL.h>

export module bud.graphics.vulkan;

import bud.graphics; 
import bud.platform; 
import bud.threading;
import bud.math;     
import bud.io;       

namespace bud::graphics::vulkan {

	export using VkInstance = struct VkInstance_T*;
	export using VkPhysicalDevice = struct VkPhysicalDevice_T*;
	export using VkDevice = struct VkDevice_T*;
	export using VkQueue = struct VkQueue_T*;
	export using VkSurfaceKHR = struct VkSurfaceKHR_T*;


	// Strict alignment for UBO
	struct UniformBufferObject {
		alignas(16) bud::math::mat4 model;
		alignas(16) bud::math::mat4 view;
		alignas(16) bud::math::mat4 proj;
		alignas(16) bud::math::mat4 lightSpaceMatrix;
		alignas(16) bud::math::vec3 camPos;
		alignas(16) bud::math::vec3 lightDir;
		alignas(16) bud::math::vec3 lightColor;
		float lightIntensity;
		float ambientStrength;
		float _padding[2];
	};

	// Non strict alignment for Vertex
	struct Vertex {
		float pos[3];
		float color[3];
		float normal[3];
		float texCoord[2];
		float texIndex;

		static VkVertexInputBindingDescription get_binding_description();

		static std::vector<VkVertexInputAttributeDescription> get_attribute_descriptions();
	};


	struct QueueFamilyIndices {
		std::optional<uint32_t> graphics_family;
		std::optional<uint32_t> present_family;

		bool is_complete() const {
			return graphics_family.has_value() && present_family.has_value();
		}
	};

	struct SwapChainSupportDetails {
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> present_modes;
	};

	struct ShadowConstantData {
		bud::math::mat4 lightMVP;  // 64 bytes (offset 0)
		bud::math::vec4 lightDir;  // 16 bytes (offset 64)
	};


	export class VulkanRHI : public bud::graphics::RHI {
	public:
		~VulkanRHI() = default;

		void init(bud::platform::Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation) override;
		void set_config(const RenderConfig& new_settings) override;
		void draw_frame(const bud::math::mat4& view, const bud::math::mat4& proj) override;
		void wait_idle() override;
		void cleanup() override;
		void reload_shaders_async() override;
		void load_model_async(const std::string& filepath) override;

	private:
		void create_shadow_pipeline();
		void create_shadow_framebuffer();
		void create_shadow_render_pass();
		void create_shadow_resources();

		void create_depth_resources();

		VkFormat find_supported_format(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
		VkFormat find_depth_format();

		void create_instance(SDL_Window* window, bool enable_validation);
		void create_surface(SDL_Window* window);

		void pick_physical_device();
		void create_logical_device(bool enable_validation);
		void create_swapchain(SDL_Window* window);
		void create_image_views();

		void create_main_render_pass();
		void create_descriptor_set_layout();
		void setup_pipeline_state(VkShaderModule vert_module, VkShaderModule frag_module);
		void create_graphics_pipeline();
		void recreate_graphics_pipeline(const std::vector<char>& vert_code, const std::vector<char>& frag_code);
		void create_framebuffers();
		void create_command_pool();
		void copy_buffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
		void create_vertex_buffer();
		void create_index_buffer();
		void create_uniform_buffers();
		void upload_mesh(const bud::io::MeshData& mesh);
		void create_texture_from_file(const std::string& path, VkImage& out_image, VkDeviceMemory& out_mem, VkImageView& out_view);
		void create_texture_image();
		void load_texture_async(const std::string& filename);
		void update_texture_resources(unsigned char* pixels, int width, int height);
		void update_descriptor_sets_texture();
		void create_texture_image_view();
		void create_texture_sampler();
		VkImageView create_image_view(VkImage image, VkFormat format);
		void create_image(uint32_t width, uint32_t height, uint32_t mip_levels, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& image_memory);
		VkCommandBuffer begin_single_time_commands();
		void end_single_time_commands(VkCommandBuffer command_buffer);
		void transition_image_layout(VkImage image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout);
		void copy_buffer_to_image(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
		void create_descriptor_pool();
		void create_descriptor_sets();
		void create_command_buffer();
		VkCommandBuffer allocate_secondary_command_buffer(VkCommandPool pool);
		void create_sync_objects();
		void record_command_buffer(VkCommandBuffer buffer, uint32_t image_index);
		void update_uniform_buffer(uint32_t current_image, const bud::math::mat4& view, const bud::math::mat4& proj, const bud::math::mat4& lightSpaceMatrix);
		VkShaderModule create_shader_module(const std::vector<char>& code);
		uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
		void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& buffer_memory);
		SwapChainSupportDetails query_swapchain_support(VkPhysicalDevice device);
		VkSurfaceFormatKHR choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats);
		VkPresentModeKHR choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes);
		VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, SDL_Window* window);
		QueueFamilyIndices find_queue_families(VkPhysicalDevice device);
		void generate_mipmaps(VkImage image, VkFormat format, int32_t tex_width, int32_t tex_height, uint32_t mip_levels);
		VkResult create_debug_utils_messenger_ext(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
		void destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator);
		void setup_debug_messenger(bool enable);

		static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);

	private:
		static constexpr int MAX_FRAMES_IN_FLIGHT = 3;
		struct FrameData {
			// 同步原语
			VkSemaphore image_available_semaphore = nullptr;
			VkSemaphore render_finished_semaphore = nullptr;
			VkFence in_flight_fence = nullptr;

			// 命令资源
			VkCommandPool main_command_pool = nullptr;
			VkCommandBuffer main_command_buffer = nullptr;

			// Worker 资源
			std::vector<VkCommandPool> worker_pools;
			std::vector<std::vector<VkCommandBuffer>> worker_cmd_buffers;
			std::vector<uint32_t> worker_cmd_counters;
		};

		std::array<FrameData, MAX_FRAMES_IN_FLIGHT> frames;

		bud::graphics::RenderConfig settings;

		uint32_t current_frame = 0;

		VkInstance instance = nullptr;
		VkPhysicalDevice physical_device = nullptr;
		VkDevice device = nullptr;
		VkSurfaceKHR surface = nullptr;
		VkQueue graphics_queue = nullptr;
		VkQueue present_queue = nullptr;
		VkDebugUtilsMessengerEXT debug_messenger = nullptr;
		bool enable_validation_layers = false;
		const std::vector<const char*> validation_layers = { "VK_LAYER_KHRONOS_validation" };
		const std::vector<const char*> device_extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		// Swapchain
		VkSwapchainKHR swapchain = nullptr;
		std::vector<VkImage> swapchain_images;
		std::vector<VkImageView> swapchain_image_views;
		std::vector<VkFramebuffer> swapchain_framebuffers;
		VkFormat swapchain_image_format;
		VkExtent2D swapchain_extent;

		// Pipeline
		VkRenderPass render_pass = nullptr;
		VkDescriptorSetLayout descriptor_set_layout = nullptr;
		VkPipelineLayout pipeline_layout = nullptr;
		VkPipeline graphics_pipeline = nullptr;


		VkImage texture_image = nullptr;
		VkDeviceMemory texture_image_memory = nullptr;

		uint32_t mip_levels = 1;

		VkImageView texture_image_view = nullptr;
		VkSampler texture_sampler = nullptr;

		std::vector<VkImage> texture_images;
		std::vector<VkDeviceMemory> texture_images_memories;
		std::vector<VkImageView> texture_views;

		VkImage depth_image = nullptr;
		VkDeviceMemory depth_image_memory = nullptr;
		VkImageView depth_image_view = nullptr;

		VkDescriptorPool descriptor_pool = nullptr;
		std::vector<VkDescriptorSet> descriptor_sets;
		std::vector<VkBuffer> uniform_buffers;
		std::vector<VkDeviceMemory> uniform_buffers_memory;
		std::vector<void*> uniform_buffers_mapped;

		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;

		VkBuffer vertex_buffer = nullptr;
		VkDeviceMemory vertex_buffer_memory = nullptr;
		VkBuffer index_buffer = nullptr;
		VkDeviceMemory index_buffer_memory = nullptr;

		// Begin, Shadow Mapping Resources
		VkImage shadow_image = nullptr;
		VkDeviceMemory shadow_image_memory = nullptr;
		VkImageView shadow_image_view = nullptr;
		VkSampler shadow_sampler = nullptr;

		VkRenderPass shadow_render_pass = nullptr;
		VkFramebuffer shadow_framebuffer = nullptr;

		VkPipeline shadow_pipeline = nullptr;
		VkPipelineLayout shadow_pipeline_layout = nullptr;
		// End, Shadow Mapping Resources


		bud::threading::TaskScheduler* task_scheduler = nullptr;
	};

}
