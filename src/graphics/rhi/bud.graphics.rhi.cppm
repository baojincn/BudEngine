module;

#include <vector>
#include <string>
#include <iostream>
#include <print>
#include <optional>
#include <set>
#include <algorithm>
#include <limits>
#include <fstream>
#include <chrono>
#include <mutex>

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#endif

import bud.io;
import bud.math;
import bud.threading;

export module bud.graphics.rhi;

export using VkInstance = struct VkInstance_T*;
export using VkPhysicalDevice = struct VkPhysicalDevice_T*;
export using VkDevice = struct VkDevice_T*;
export using VkQueue = struct VkQueue_T*;
export using VkSurfaceKHR = struct VkSurfaceKHR_T*;

export namespace bud::graphics {

	// ==========================================
	// Data Structures
	// ==========================================

	// Strict alignment for UBO
	struct UniformBufferObject {
		alignas(16) bud::math::mat4 model;
		alignas(16) bud::math::mat4 view;
		alignas(16) bud::math::mat4 proj;
		alignas(16) bud::math::vec3 camPos;   // 相机位置 (用于高光)
		alignas(16) bud::math::vec3 lightDir; // 太阳方向
	};

	// Non strict alignment for Vertex
	struct Vertex {
		float pos[3];		
		float color[3];		
		float normal[3];		
		float texCoord[2];  
		float texIndex;	

		static VkVertexInputBindingDescription get_binding_description() {
			VkVertexInputBindingDescription binding_description{};
			binding_description.binding = 0;
			binding_description.stride = sizeof(Vertex);
			binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
			return binding_description;
		}

		static std::vector<VkVertexInputAttributeDescription> get_attribute_descriptions() {
			std::vector<VkVertexInputAttributeDescription> attribute_descriptions(5);

			// Attribute 0: Position
			attribute_descriptions[0].binding = 0;
			attribute_descriptions[0].location = 0;
			attribute_descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
			attribute_descriptions[0].offset = offsetof(Vertex, pos);

			// Attribute 1: Color
			attribute_descriptions[1].binding = 0;
			attribute_descriptions[1].location = 1;
			attribute_descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
			attribute_descriptions[1].offset = offsetof(Vertex, color);

			// Attribute 2: Normal
			attribute_descriptions[2].binding = 0;
			attribute_descriptions[2].location = 2;
			attribute_descriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
			attribute_descriptions[2].offset = offsetof(Vertex, normal);

			// Attribute 3: TexCoord 
			attribute_descriptions[3].binding = 0;
			attribute_descriptions[3].location = 3;
			attribute_descriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
			attribute_descriptions[3].offset = offsetof(Vertex, texCoord);

			// Attribute 4: TexIndex
			attribute_descriptions[4].binding = 0;
			attribute_descriptions[4].location = 4;
			attribute_descriptions[4].format = VK_FORMAT_R32_SFLOAT;
			attribute_descriptions[4].offset = offsetof(Vertex, texIndex);

			return attribute_descriptions;
		}
	};

	struct QueueFamilyIndices {
		std::optional<uint32_t> graphics_family;
		std::optional<uint32_t> present_family;

		bool is_complete() const {
			return graphics_family.has_value() && present_family.has_value();
		}
	};

	// ==========================================
	// RHI Interface
	// ==========================================

	export class RHI {
	public:
		virtual ~RHI() = default;
		virtual void init(SDL_Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation) = 0;
		virtual void draw_frame(const bud::math::mat4& view, const bud::math::mat4& proj) = 0;
		virtual void wait_idle() = 0;
		virtual void cleanup() = 0;
		virtual void reload_shaders_async() = 0;
		virtual void load_model_async(const std::string& filepath) = 0;
	};

	// ==========================================
	// Vulkan Implementation
	// ==========================================

	export class VulkanRHI : public RHI {
	public:
		void init(SDL_Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation) override {
			this->task_scheduler = task_scheduler;

			// 1. Core Vulkan Setup
			create_instance(window, enable_validation);
			setup_debug_messenger(enable_validation);
			create_surface(window);
			pick_physical_device();
			create_logical_device(enable_validation);

			// 2. Presentation Setup
			create_swapchain(window);
			create_image_views();

			create_depth_resources();

			// 3. Pipeline Setup
			create_render_pass();
			create_descriptor_set_layout(); // Layout 必须在 Pipeline 之前
			create_graphics_pipeline();
			create_framebuffers();

			// 4. Resources Setup
			create_command_pool();

			//create_vertex_buffer();
			create_uniform_buffers();       // Buffer 必须在 Set 之前

			// Texture placeholders
			create_texture_image();
			create_texture_image_view();
			create_texture_sampler();

			create_descriptor_pool();       // Pool 必须在 Set 之前
			create_descriptor_sets();

			// Hot swap a texture
			load_texture_async("data/textures/default.png");

			// 5. Command & Sync
			create_command_buffer();
			create_sync_objects();
		}

		void draw_frame(const bud::math::mat4& view, const bud::math::mat4& proj) override {
			FrameData& frame = frames[current_frame]; // 获取当前帧的资源包

			// 1. 等待上一轮使用该帧资源的操作完成
			vkWaitForFences(device, 1, &frame.in_flight_fence, VK_TRUE, UINT64_MAX);

			// 2. 获取 swapchain image
			uint32_t image_index;
			VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frame.image_available_semaphore, VK_NULL_HANDLE, &image_index);

			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
				// recreate swapchain...
				return;
			}

			// 只有确定要绘制了，才 Reset Fence
			vkResetFences(device, 1, &frame.in_flight_fence);

			update_uniform_buffer(image_index, view, proj); // 更新 UBO (注意：UBO 最好也做多帧缓冲，这里暂且复用)

			// 3. 【关键】重置当前帧的所有 Command Pools
			// 这会释放该帧之前分配的所有 command buffers 内存
			vkResetCommandPool(device, frame.main_command_pool, 0);
			for (auto pool : frame.worker_pools) {
				vkResetCommandPool(device, pool, 0);
			}

			// 重置命令计数器, 不释放内存，只把游标指回 0，下一帧覆盖使用旧的 Handles
			std::fill(frame.worker_cmd_counters.begin(), frame.worker_cmd_counters.end(), 0);

			// 4. 录制主命令缓冲
			VkCommandBuffer cmd = frame.main_command_buffer;
			VkCommandBufferBeginInfo begin_info{};
			begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			vkBeginCommandBuffer(cmd, &begin_info);

			VkRenderPassBeginInfo render_pass_info{};
			render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			render_pass_info.renderPass = render_pass;
			render_pass_info.framebuffer = swapchain_framebuffers[image_index];
			render_pass_info.renderArea.extent = swapchain_extent;

			std::array<VkClearValue, 2> clear_values{};
			clear_values[0].color = { {0.1f, 0.1f, 0.1f, 1.0f} };
			clear_values[1].depthStencil = { 1.0f, 0 };

			render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
			render_pass_info.pClearValues = clear_values.data();

			vkCmdBeginRenderPass(cmd, &render_pass_info, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

			// 5. 并行录制任务
			bud::threading::Counter recording_dependency;
			std::mutex recorded_cmds_mutex;
			std::vector<VkCommandBuffer> secondary_cmds;

			task_scheduler->spawn("DrawTask", [&, current_frame_idx = current_frame, img_idx = image_index]() {
				size_t worker_id = bud::threading::t_worker_index;

				// 从当前帧的 FrameData 里拿 Pool
				// 此时 Pool 已经被主线程 Reset 过了，Worker 可以直接 Alloc，不用 Reset
				auto& frame_data = frames[current_frame_idx];
				VkCommandPool worker_pool = frame_data.worker_pools[worker_id];

				// 分配或复用 Secondary Command Buffer
				VkCommandBuffer sec_cmd = nullptr;
				auto& cmd_counter = frame_data.worker_cmd_counters[worker_id];
				auto& cmd_buffer = frame_data.worker_cmd_buffers[worker_id];

				if (cmd_counter < cmd_buffer.size()) {
					sec_cmd = cmd_buffer[cmd_counter];
				}
				else {
					sec_cmd = allocate_secondary_command_buffer(worker_pool);
					cmd_buffer.push_back(sec_cmd);
				}
				cmd_counter++;


				VkCommandBufferInheritanceInfo inheritance_info{};
				inheritance_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
				inheritance_info.renderPass = render_pass;
				inheritance_info.framebuffer = swapchain_framebuffers[img_idx];

				VkCommandBufferBeginInfo sec_begin_info{};
				sec_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				sec_begin_info.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
				sec_begin_info.pInheritanceInfo = &inheritance_info;

				vkBeginCommandBuffer(sec_cmd, &sec_begin_info);

				vkCmdBindPipeline(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);

				VkViewport viewport{};
				viewport.width = (float)swapchain_extent.width; viewport.height = (float)swapchain_extent.height; viewport.maxDepth = 1.0f;
				vkCmdSetViewport(sec_cmd, 0, 1, &viewport);


				VkRect2D scissor{}; scissor.extent = swapchain_extent;
				vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

				// Don't draw if didn't load model yet
				if (!indices.empty() && vertex_buffer && index_buffer) {
					VkBuffer vbs[] = { vertex_buffer }; VkDeviceSize offs[] = { 0 };
					vkCmdBindVertexBuffers(sec_cmd, 0, 1, vbs, offs);

					vkCmdBindIndexBuffer(sec_cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);

					vkCmdBindDescriptorSets(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor_sets[img_idx], 0, nullptr);

					vkCmdDrawIndexed(sec_cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
				}

				vkEndCommandBuffer(sec_cmd);

				{
					std::lock_guard lock(recorded_cmds_mutex);
					secondary_cmds.push_back(sec_cmd);
				}

			}, &recording_dependency);

			task_scheduler->wait_for_counter(recording_dependency);

			if (!secondary_cmds.empty()) {
				vkCmdExecuteCommands(cmd, (uint32_t)secondary_cmds.size(), secondary_cmds.data());
			}

			vkCmdEndRenderPass(cmd);
			vkEndCommandBuffer(cmd);

			// 6. 提交
			VkSubmitInfo submit_info{};
			submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			VkSemaphore wait_semaphores[] = { frame.image_available_semaphore };
			VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
			submit_info.waitSemaphoreCount = 1;
			submit_info.pWaitSemaphores = wait_semaphores;
			submit_info.pWaitDstStageMask = wait_stages;
			submit_info.commandBufferCount = 1;
			submit_info.pCommandBuffers = &cmd; // 使用当前帧的 cmd
			VkSemaphore signal_semaphores[] = { frame.render_finished_semaphore };
			submit_info.signalSemaphoreCount = 1;
			submit_info.pSignalSemaphores = signal_semaphores;

			if (vkQueueSubmit(graphics_queue, 1, &submit_info, frame.in_flight_fence) != VK_SUCCESS) {
				throw std::runtime_error("Failed to submit draw command buffer!");
			}

			VkPresentInfoKHR present_info{};
			present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			present_info.waitSemaphoreCount = 1;
			present_info.pWaitSemaphores = signal_semaphores;
			VkSwapchainKHR swapchains[] = { swapchain };
			present_info.swapchainCount = 1;
			present_info.pSwapchains = swapchains;
			present_info.pImageIndices = &image_index;

			vkQueuePresentKHR(present_queue, &present_info);

			// 7. 切换到下一帧
			current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
		}

		void wait_idle() override {
			if (device) vkDeviceWaitIdle(device);
		}

		void cleanup() override {
			wait_idle();

			vkDestroyImageView(device, depth_image_view, nullptr);
			vkDestroyImage(device, depth_image, nullptr);
			vkFreeMemory(device, depth_image_memory, nullptr);

			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
				vkDestroySemaphore(device, frames[i].render_finished_semaphore, nullptr);
				vkDestroySemaphore(device, frames[i].image_available_semaphore, nullptr);
				vkDestroyFence(device, frames[i].in_flight_fence, nullptr);

				vkDestroyCommandPool(device, frames[i].main_command_pool, nullptr);
				for (auto pool : frames[i].worker_pools) {
					vkDestroyCommandPool(device, pool, nullptr);
				}
			}

			// Cleanup Texture
			vkDestroySampler(device, texture_sampler, nullptr);
			vkDestroyImageView(device, texture_image_view, nullptr);
			vkDestroyImage(device, texture_image, nullptr);
			vkFreeMemory(device, texture_image_memory, nullptr);

			// Cleanup Buffers
			vkDestroyBuffer(device, vertex_buffer, nullptr);
			vkFreeMemory(device, vertex_buffer_memory, nullptr);
			vkDestroyBuffer(device, index_buffer, nullptr);
			vkFreeMemory(device, index_buffer_memory, nullptr);

			for (size_t i = 0; i < swapchain_images.size(); i++) {
				vkDestroyBuffer(device, uniform_buffers[i], nullptr);
				vkFreeMemory(device, uniform_buffers_memory[i], nullptr);
			}

			// Cleanup Descriptors
			if (descriptor_pool)
				vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
			if (descriptor_set_layout)
				vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);

			// Cleanup Pipeline
			if (graphics_pipeline)
				vkDestroyPipeline(device, graphics_pipeline, nullptr);
			if (pipeline_layout)
				vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
			if (render_pass)
				vkDestroyRenderPass(device, render_pass, nullptr);

			// Cleanup Framebuffers
			for (auto framebuffer : swapchain_framebuffers) {
				vkDestroyFramebuffer(device, framebuffer, nullptr);
			}

			// Cleanup Swapchain
			for (auto imageView : swapchain_image_views) {
				vkDestroyImageView(device, imageView, nullptr);
			}
			if (swapchain) {
				vkDestroySwapchainKHR(device, swapchain, nullptr);
			}

			// Cleanup Device & Instance
			if (device) {
				vkDestroyDevice(device, nullptr);
			}
			if (enable_validation_layers && debug_messenger) {
				destroy_debug_utils_messenger_ext(instance, debug_messenger, nullptr);
			}
			if (surface) {
				vkDestroySurfaceKHR(instance, surface, nullptr);
			}
			if (instance) {
				vkDestroyInstance(instance, nullptr);
			}
		}

		// [Worker Thread] 异步加载 Shader
		void reload_shaders_async() override {
			task_scheduler->spawn("AsyncShaderLoad", [this]() {
				auto vert_opt = bud::io::FileSystem::read_binary("src/shaders/triangle.vert.spv");
				auto frag_opt = bud::io::FileSystem::read_binary("src/shaders/triangle.frag.spv");

				if (!vert_opt || !frag_opt) return;

				// 移动所有权给主线程
				task_scheduler->submit_main_thread_task([this,
					vert = std::move(*vert_opt),
					frag = std::move(*frag_opt)]() {

					this->recreate_graphics_pipeline(vert, frag);
				});
			});
		}

		void load_model_async(const std::string& filepath) override {
			task_scheduler->spawn("AsyncModelLoad", [this, filepath]() {
				// 1. IO 线程解析 OBJ
				auto mesh_opt = bud::io::ModelLoader::load_obj(filepath);
				if (!mesh_opt) return;

				// 2. 主线程上传 GPU
				task_scheduler->submit_main_thread_task([this, mesh = std::move(*mesh_opt)]() {
					this->upload_mesh(mesh);
				});
			});
		}

		// ==========================================
		// Private Members
		// ==========================================
	private:
		// Core
		static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

		struct FrameData {
			// 同步原语
			VkSemaphore image_available_semaphore = nullptr;
			VkSemaphore render_finished_semaphore = nullptr;
			VkFence in_flight_fence = nullptr;

			// 命令资源
			VkCommandPool main_command_pool = nullptr;
			VkCommandBuffer main_command_buffer = nullptr;

			// Worker 资源 (每个 Worker 一个 Pool)
			std::vector<VkCommandPool> worker_pools;
			std::vector<std::vector<VkCommandBuffer>> worker_cmd_buffers;
			std::vector<uint32_t> worker_cmd_counters;
		};

		std::array<FrameData, MAX_FRAMES_IN_FLIGHT> frames;
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
		std::vector<VkSampler> texture_samplers;

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


		bud::threading::TaskScheduler* task_scheduler = nullptr;

		// ==========================================
		// Initialization Helpers
		// ==========================================
	private:
		void create_depth_resources() {
			VkFormat depth_format = find_depth_format();
			create_image(swapchain_extent.width, swapchain_extent.height, 1, depth_format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depth_image, depth_image_memory);

			// 创建 View (注意 aspectMask 是 DEPTH)
			VkImageViewCreateInfo view_info{};
			view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			view_info.image = depth_image;
			view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view_info.format = depth_format;
			view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			view_info.subresourceRange.baseMipLevel = 0;
			view_info.subresourceRange.levelCount = 1;
			view_info.subresourceRange.baseArrayLayer = 0;
			view_info.subresourceRange.layerCount = 1;

			if (vkCreateImageView(device, &view_info, nullptr, &depth_image_view) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create depth image view!");
			}
		}

		VkFormat find_supported_format(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
			for (VkFormat format : candidates) {
				VkFormatProperties props;
				vkGetPhysicalDeviceFormatProperties(physical_device, format, &props);

				if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
					return format;
				}
				else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
					return format;
				}
			}
			throw std::runtime_error("Failed to find supported format!");
		}

		VkFormat find_depth_format() {
			return find_supported_format(
				{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
				VK_IMAGE_TILING_OPTIMAL,
				VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
			);
		}

		void create_instance(SDL_Window* window, bool enable_validation) {
			enable_validation_layers = enable_validation;
			VkApplicationInfo app_info{};
			app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			app_info.pApplicationName = "Bud Engine";
			app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
			app_info.pEngineName = "Bud";
			app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
			app_info.apiVersion = VK_API_VERSION_1_3;

			VkInstanceCreateInfo create_info{};
			create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			create_info.pApplicationInfo = &app_info;

			uint32_t sdl_ext_count = 0;
			auto sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
			std::vector<const char*> extensions(sdl_exts, sdl_exts + sdl_ext_count);

			if (enable_validation) {
				extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			}

			create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
			create_info.ppEnabledExtensionNames = extensions.data();

			if (enable_validation) {
				create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
				create_info.ppEnabledLayerNames = validation_layers.data();
			}
			else {
				create_info.enabledLayerCount = 0;
			}

			if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create Vulkan instance!");
			}
		}

		void create_surface(SDL_Window* window) {
			if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
				throw std::runtime_error("Failed to create Window Surface!");
			}
		}

		void pick_physical_device() {
			uint32_t device_count = 0;
			vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
			if (device_count == 0) throw std::runtime_error("No GPUs with Vulkan support!");

			std::vector<VkPhysicalDevice> devices(device_count);
			vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

			for (const auto& dev : devices) {
				VkPhysicalDeviceProperties props;
				vkGetPhysicalDeviceProperties(dev, &props);
				if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
					physical_device = dev;
					std::println("[Vulkan] Selected Discrete GPU: {}", props.deviceName);
					break;
				}
			}

			if (physical_device == nullptr) {
				physical_device = devices[0];
				std::println("[Vulkan] Warning: Using Integrated/Fallback GPU.");
			}
		}

		void create_logical_device(bool enable_validation) {
			QueueFamilyIndices indices = find_queue_families(physical_device);
			std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
			std::set<uint32_t> unique_queue_families = { indices.graphics_family.value(), indices.present_family.value() };

			float queue_priority = 1.0f;
			for (uint32_t queue_family : unique_queue_families) {
				VkDeviceQueueCreateInfo queue_create_info{};
				queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queue_create_info.queueFamilyIndex = queue_family;
				queue_create_info.queueCount = 1;
				queue_create_info.pQueuePriorities = &queue_priority;
				queue_create_infos.push_back(queue_create_info);
			}

			VkPhysicalDeviceFeatures device_features{};
			device_features.samplerAnisotropy = VK_TRUE;

			// 开启 Descriptor Indexing 特性链
			VkPhysicalDeviceDescriptorIndexingFeatures indexing_features{};
			indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
			indexing_features.descriptorBindingPartiallyBound = VK_TRUE; // 允许数组不填满
			indexing_features.runtimeDescriptorArray = VK_TRUE;          // 允许 Shader 使用非固定大小数组
			indexing_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

			VkDeviceCreateInfo create_info{};
			create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			create_info.pNext = &indexing_features;	// Link
			create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
			create_info.pQueueCreateInfos = queue_create_infos.data();
			create_info.pEnabledFeatures = &device_features;
			create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
			create_info.ppEnabledExtensionNames = device_extensions.data();

			if (vkCreateDevice(physical_device, &create_info, nullptr, &device) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create logical device!");
			}

			vkGetDeviceQueue(device, indices.graphics_family.value(), 0, &graphics_queue);
			vkGetDeviceQueue(device, indices.present_family.value(), 0, &present_queue);
		}

		void create_swapchain(SDL_Window* window) {
			SwapChainSupportDetails swapchain_support = query_swapchain_support(physical_device);
			VkSurfaceFormatKHR surface_format = choose_swap_surface_format(swapchain_support.formats);
			VkPresentModeKHR present_mode = choose_swap_present_mode(swapchain_support.present_modes);
			VkExtent2D extent = choose_swap_extent(swapchain_support.capabilities, window);

			uint32_t image_count = swapchain_support.capabilities.minImageCount + 1;
			if (swapchain_support.capabilities.maxImageCount > 0 && image_count > swapchain_support.capabilities.maxImageCount) {
				image_count = swapchain_support.capabilities.maxImageCount;
			}

			VkSwapchainCreateInfoKHR create_info{};
			create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			create_info.surface = surface;
			create_info.minImageCount = image_count;
			create_info.imageFormat = surface_format.format;
			create_info.imageColorSpace = surface_format.colorSpace;
			create_info.imageExtent = extent;
			create_info.imageArrayLayers = 1;
			create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

			QueueFamilyIndices indices = find_queue_families(physical_device);
			uint32_t queue_family_indices[] = { indices.graphics_family.value(), indices.present_family.value() };

			if (indices.graphics_family != indices.present_family) {
				create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
				create_info.queueFamilyIndexCount = 2;
				create_info.pQueueFamilyIndices = queue_family_indices;
			}
			else {
				create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			}

			create_info.preTransform = swapchain_support.capabilities.currentTransform;
			create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			create_info.presentMode = present_mode;
			create_info.clipped = VK_TRUE;
			create_info.oldSwapchain = VK_NULL_HANDLE;

			if (vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create swapchain!");
			}

			vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
			swapchain_images.resize(image_count);
			vkGetSwapchainImagesKHR(device, swapchain, &image_count, swapchain_images.data());

			swapchain_image_format = surface_format.format;
			swapchain_extent = extent;

			std::println("[Vulkan] Swapchain created: {}x{} (Images: {})", extent.width, extent.height, image_count);
		}

		void create_image_views() {
			swapchain_image_views.resize(swapchain_images.size());
			for (size_t i = 0; i < swapchain_images.size(); i++) {
				VkImageViewCreateInfo create_info{};
				create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				create_info.image = swapchain_images[i];
				create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
				create_info.format = swapchain_image_format;
				create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
				create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
				create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
				create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
				create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				create_info.subresourceRange.baseMipLevel = 0;
				create_info.subresourceRange.levelCount = 1;
				create_info.subresourceRange.baseArrayLayer = 0;
				create_info.subresourceRange.layerCount = 1;

				if (vkCreateImageView(device, &create_info, nullptr, &swapchain_image_views[i]) != VK_SUCCESS) {
					throw std::runtime_error("Failed to create image views!");
				}
			}
		}

		void create_render_pass() {
			VkAttachmentDescription color_attachment{};
			color_attachment.format = swapchain_image_format;
			color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

			VkAttachmentReference color_attachment_ref{};
			color_attachment_ref.attachment = 0;
			color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depth_attachment{};
			depth_attachment.format = find_depth_format();
			depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // 每一帧开始时清空深度
			depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depth_attachment_ref{};
			depth_attachment_ref.attachment = 1; // 这里的 1 对应下面 array 的下标
			depth_attachment_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &color_attachment_ref;
			subpass.pDepthStencilAttachment = &depth_attachment_ref;

			VkSubpassDependency dependency{};
			dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
			dependency.dstSubpass = 0;
			dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependency.srcAccessMask = 0;
			dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			VkRenderPassCreateInfo render_pass_info{};
			std::array<VkAttachmentDescription, 2> attachments = { color_attachment, depth_attachment }; // 数组包含两个
			render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
			render_pass_info.pAttachments = attachments.data();
			render_pass_info.subpassCount = 1;
			render_pass_info.pSubpasses = &subpass;
			render_pass_info.dependencyCount = 1;
			render_pass_info.pDependencies = &dependency;

			if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create render pass!");
			}
		}

		void create_descriptor_set_layout() {
			VkDescriptorSetLayoutBinding ubo_layout_binding{};
			ubo_layout_binding.binding = 0;
			ubo_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			ubo_layout_binding.descriptorCount = 1;
			ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
			ubo_layout_binding.pImmutableSamplers = nullptr;

			VkDescriptorSetLayoutBinding sampler_layout_binding{};
			sampler_layout_binding.binding = 1;
			sampler_layout_binding.descriptorCount = 100;
			sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			sampler_layout_binding.pImmutableSamplers = nullptr;
			sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

			std::array<VkDescriptorSetLayoutBinding, 2> bindings = { ubo_layout_binding, sampler_layout_binding };

			VkDescriptorSetLayoutCreateInfo layout_info{};
			layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

			VkDescriptorBindingFlags binding_flags[] = {
			  0,
			  VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
			};

			VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
			flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
			flags_info.bindingCount = 2;
			flags_info.pBindingFlags = binding_flags;

			layout_info.pNext = &flags_info; // Link in
			layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
			layout_info.pBindings = bindings.data();

			if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_set_layout) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create descriptor set layout!");
			}
			std::println("[Vulkan] Descriptor Set Layout created.");
		}

		// Called by Init and Hot-Reload
		void setup_pipeline_state(VkShaderModule vert_module, VkShaderModule frag_module) {
			VkPipelineShaderStageCreateInfo vert_stage_info{};
			vert_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			vert_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
			vert_stage_info.module = vert_module;
			vert_stage_info.pName = "main";

			VkPipelineShaderStageCreateInfo frag_stage_info{};
			frag_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			frag_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			frag_stage_info.module = frag_module;
			frag_stage_info.pName = "main";

			VkPipelineShaderStageCreateInfo shader_stages[] = { vert_stage_info, frag_stage_info };

			auto binding_description = Vertex::get_binding_description();
			auto attribute_descriptions = Vertex::get_attribute_descriptions();

			VkPipelineVertexInputStateCreateInfo vertex_input_info{};
			vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertex_input_info.vertexBindingDescriptionCount = 1;
			vertex_input_info.pVertexBindingDescriptions = &binding_description;
			vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descriptions.size());
			vertex_input_info.pVertexAttributeDescriptions = attribute_descriptions.data();

			VkPipelineInputAssemblyStateCreateInfo input_assembly{};
			input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			input_assembly.primitiveRestartEnable = VK_FALSE;

			// Viewport & Scissor (Dynamic)
			VkViewport viewport{}; // Placeholder
			VkRect2D scissor{};    // Placeholder

			VkPipelineViewportStateCreateInfo viewport_state{};
			viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			viewport_state.viewportCount = 1;
			viewport_state.pViewports = &viewport;
			viewport_state.scissorCount = 1;
			viewport_state.pScissors = &scissor;

			VkPipelineRasterizationStateCreateInfo rasterizer{};
			rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
			rasterizer.depthClampEnable = VK_FALSE;
			rasterizer.rasterizerDiscardEnable = VK_FALSE;
			rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
			rasterizer.lineWidth = 1.0f;
			rasterizer.cullMode = VK_CULL_MODE_NONE;
			rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // GLM 投影矩阵翻转了 Y，这里顺应调整
			rasterizer.depthBiasEnable = VK_FALSE;

			VkPipelineMultisampleStateCreateInfo multisampling{};
			multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			multisampling.sampleShadingEnable = VK_FALSE;
			multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			VkPipelineColorBlendAttachmentState color_blend_attachment{};
			color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			color_blend_attachment.blendEnable = VK_FALSE;

			VkPipelineColorBlendStateCreateInfo color_blending{};
			color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			color_blending.logicOpEnable = VK_FALSE;
			color_blending.attachmentCount = 1;
			color_blending.pAttachments = &color_blend_attachment;

			std::vector<VkDynamicState> dynamic_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamic_state_info{};
			dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
			dynamic_state_info.pDynamicStates = dynamic_states.data();

			VkPipelineDepthStencilStateCreateInfo depth_stencil{};
			depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depth_stencil.depthTestEnable = VK_TRUE;  // 开启测试
			depth_stencil.depthWriteEnable = VK_TRUE; // 开启写入
			depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS; // 近的覆盖远的
			depth_stencil.depthBoundsTestEnable = VK_FALSE;
			depth_stencil.stencilTestEnable = VK_FALSE;

			VkPipelineLayoutCreateInfo pipeline_layout_info{};
			pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipeline_layout_info.setLayoutCount = 1;
			pipeline_layout_info.pSetLayouts = &descriptor_set_layout;

			if (vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create pipeline layout!");
			}

			VkGraphicsPipelineCreateInfo pipeline_info{};
			pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipeline_info.stageCount = 2;
			pipeline_info.pStages = shader_stages;
			pipeline_info.pVertexInputState = &vertex_input_info;
			pipeline_info.pInputAssemblyState = &input_assembly;
			pipeline_info.pViewportState = &viewport_state;
			pipeline_info.pRasterizationState = &rasterizer;
			pipeline_info.pMultisampleState = &multisampling;
			pipeline_info.pDepthStencilState = &depth_stencil;
			pipeline_info.pColorBlendState = &color_blending;
			pipeline_info.pDynamicState = &dynamic_state_info;
			pipeline_info.layout = pipeline_layout;
			pipeline_info.renderPass = render_pass;
			pipeline_info.subpass = 0;

			// 创建新管线
			if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &graphics_pipeline) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create graphics pipeline!");
			}

			std::println("[Vulkan] Pipeline State Created.");
		}

		void create_graphics_pipeline() {
			auto vert_shader_code = bud::io::FileSystem::read_binary("src/shaders/triangle.vert.spv");
			auto frag_shader_code = bud::io::FileSystem::read_binary("src/shaders/triangle.frag.spv");

			if (!vert_shader_code || !frag_shader_code) {
				throw std::runtime_error("Failed to load shader SPIR-V files!");
			}

			VkShaderModule vert_module = create_shader_module(*vert_shader_code);
			VkShaderModule frag_module = create_shader_module(*frag_shader_code);

			setup_pipeline_state(vert_module, frag_module);

			vkDestroyShaderModule(device, frag_module, nullptr);
			vkDestroyShaderModule(device, vert_module, nullptr);
			std::println("[Vulkan] Graphics pipeline created successfully");
		}

		// [新增] 仅重建管线的内部函数
		void recreate_graphics_pipeline(const std::vector<char>& vert_code, const std::vector<char>& frag_code) {
			std::println("[Main] Recreating Pipeline for Hot-Reload...");

			// 1. 等待 GPU 空闲 (因为我们要销毁正在用的管线)
			vkDeviceWaitIdle(device);

			// 2. 销毁旧管线
			if (graphics_pipeline) {
				vkDestroyPipeline(device, graphics_pipeline, nullptr);
				graphics_pipeline = nullptr;
			}

			// 3. 创建新 Modules
			VkShaderModule vert_module = create_shader_module(vert_code);
			VkShaderModule frag_module = create_shader_module(frag_code);

			// 4. [复用] 创建新管线
			try {
				setup_pipeline_state(vert_module, frag_module);
				std::println("[Main] Hot-Reload Success!");
			}
			catch (const std::exception& e) {
				std::println(stderr, "[Main] Hot-Reload Failed: {}", e.what());
			}

			// 5. 清理 Modules
			vkDestroyShaderModule(device, frag_module, nullptr);
			vkDestroyShaderModule(device, vert_module, nullptr);
		}

		void create_framebuffers() {
			swapchain_framebuffers.resize(swapchain_image_views.size());
			for (size_t i = 0; i < swapchain_image_views.size(); i++) {
				std::array<VkImageView, 2> attachments = {
					swapchain_image_views[i],
					depth_image_view
				};

				VkFramebufferCreateInfo framebuffer_info{};
				framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
				framebuffer_info.renderPass = render_pass;
				framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
				framebuffer_info.pAttachments = attachments.data();
				framebuffer_info.width = swapchain_extent.width;
				framebuffer_info.height = swapchain_extent.height;
				framebuffer_info.layers = 1;

				if (vkCreateFramebuffer(device, &framebuffer_info, nullptr, &swapchain_framebuffers[i]) != VK_SUCCESS) {
					throw std::runtime_error("Failed to create framebuffer!");
				}
			}
		}

		void create_command_pool() {
			QueueFamilyIndices queue_family_indices = find_queue_families(physical_device);
			size_t thread_count = task_scheduler ? task_scheduler->get_thread_count() : 1;

			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
				// 1. 创建主线程 Pool
				VkCommandPoolCreateInfo pool_info{};
				pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
				pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
				pool_info.queueFamilyIndex = queue_family_indices.graphics_family.value();

				if (vkCreateCommandPool(device, &pool_info, nullptr, &frames[i].main_command_pool) != VK_SUCCESS) {
					throw std::runtime_error("Failed to create main command pool!");
				}

				// 2. 创建 Worker Pools
				frames[i].worker_pools.resize(thread_count);
				frames[i].worker_cmd_buffers.resize(thread_count);
				frames[i].worker_cmd_counters.resize(thread_count);

				for (size_t t = 0; t < thread_count; t++) {
					VkCommandPoolCreateInfo worker_pool_info{};
					worker_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
					worker_pool_info.queueFamilyIndex = queue_family_indices.graphics_family.value();
					// TRANSIENT: 告诉驱动这个 Pool 的 Buffer 活不过一帧，请优化内存分配
					worker_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

					if (vkCreateCommandPool(device, &worker_pool_info, nullptr, &frames[i].worker_pools[t]) != VK_SUCCESS) {
						throw std::runtime_error("Failed to create worker command pool!");
					}
				}
			}
		}


		void copy_buffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
			VkCommandBuffer command_buffer = begin_single_time_commands();

			VkBufferCopy copyRegion{};
			copyRegion.srcOffset = 0;
			copyRegion.dstOffset = 0;
			copyRegion.size = size;

			vkCmdCopyBuffer(command_buffer, srcBuffer, dstBuffer, 1, &copyRegion);

			end_single_time_commands(command_buffer);
		}

		void create_vertex_buffer() {
			VkDeviceSize buffer_size = sizeof(vertices[0]) * vertices.size();
			create_buffer(buffer_size,
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				vertex_buffer,
				vertex_buffer_memory);

			void* data;
			vkMapMemory(device, vertex_buffer_memory, 0, buffer_size, 0, &data);
			memcpy(data, vertices.data(), (size_t)buffer_size);
			vkUnmapMemory(device, vertex_buffer_memory);
		}

		void create_index_buffer() {
			VkDeviceSize buffer_size = sizeof(indices[0]) * indices.size();

			VkBuffer staging_buffer;
			VkDeviceMemory staging_buffer_memory;
			create_buffer(buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_buffer, staging_buffer_memory);

			void* data;
			vkMapMemory(device, staging_buffer_memory, 0, buffer_size, 0, &data);
			memcpy(data, indices.data(), (size_t)buffer_size);
			vkUnmapMemory(device, staging_buffer_memory);

			create_buffer(buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, index_buffer, index_buffer_memory);

			copy_buffer(staging_buffer, index_buffer, buffer_size);

			vkDestroyBuffer(device, staging_buffer, nullptr);
			vkFreeMemory(device, staging_buffer_memory, nullptr);
		}

		void create_uniform_buffers() {
			VkDeviceSize buffer_size = sizeof(UniformBufferObject);

			uniform_buffers.resize(swapchain_images.size());
			uniform_buffers_memory.resize(swapchain_images.size());
			uniform_buffers_mapped.resize(swapchain_images.size());

			for (size_t i = 0; i < swapchain_images.size(); i++) {
				create_buffer(buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					uniform_buffers[i], uniform_buffers_memory[i]);

				vkMapMemory(device, uniform_buffers_memory[i], 0, buffer_size, 0, &uniform_buffers_mapped[i]);
			}
		}


		void upload_mesh(const bud::io::MeshData& mesh) {
			vkDeviceWaitIdle(device); // 等待 GPU 空闲

			// 1. 清理旧缓冲
			if (vertex_buffer) {
				vkDestroyBuffer(device, vertex_buffer, nullptr);
				vkFreeMemory(device, vertex_buffer_memory, nullptr);
			}

			if (index_buffer) {
				vkDestroyBuffer(device, index_buffer, nullptr);
				vkFreeMemory(device, index_buffer_memory, nullptr);
			}

			// 2. 转换数据格式 (bud::io::Vertex -> bud::graphics::Vertex)
			this->vertices.clear();
			this->indices = mesh.indices;

			for (const auto& v : mesh.vertices) {
				Vertex rhi_v{};
				rhi_v.pos[0] = v.pos.x;
				rhi_v.pos[1] = v.pos.y;
				rhi_v.pos[2] = v.pos.z;

				rhi_v.color[0] = v.color.x;
				rhi_v.color[1] = v.color.y;
				rhi_v.color[2] = v.color.z;

				rhi_v.normal[0] = v.normal.x;
				rhi_v.normal[1] = v.normal.y;
				rhi_v.normal[2] = v.normal.z;

				rhi_v.texCoord[0] = v.texture_uv.x;
				rhi_v.texCoord[1] = v.texture_uv.y;

				rhi_v.texIndex = v.texture_index;

				this->vertices.push_back(rhi_v);
			}

			// 3. 创建新缓冲
			create_vertex_buffer();
			create_index_buffer();


			// 1. 清理旧的 Sponza 纹理 (保留 Slot 0 的 default texture)
			for (auto v : texture_views) vkDestroyImageView(device, v, nullptr);
			for (auto i : texture_images) vkDestroyImage(device, i, nullptr);
			for (auto m : texture_images_memories) vkFreeMemory(device, m, nullptr);
			// Sampler 通常可以复用，不需要每个纹理建一个。如果需要清理：
			// for(auto s : texture_samplers) vkDestroySampler(device, s, nullptr);

			texture_views.clear();
			texture_images.clear();
			texture_images_memories.clear();
			// texture_samplers.clear(); 

			// 2. 预分配空间
			size_t count = mesh.texture_paths.size();
			texture_views.resize(count);
			texture_images.resize(count);
			texture_images_memories.resize(count);
			// texture_samplers.resize(count); 

			std::println("[RHI] Loading {} textures for mesh...", count);

			// 3. 循环加载并更新 Descriptor
			for (size_t index = 0; index < count; ++index) {
				std::string path = mesh.texture_paths[index];

				// [新增] 打印正在加载哪张图，方便定位是谁的问题
				std::println("[RHI] Loading texture [{}/{}]: {}", index + 1, count, path);

				try {
					// 尝试加载
					create_texture_from_file(path, texture_images[index], texture_images_memories[index], texture_views[index]);
				}
				catch (const std::exception& e) {
					// [关键] 捕获异常！不要让线程死掉！
					std::println("❌ Error loading texture: {} | Reason: {}", path, e.what());

					// [补救] 加载失败时，给一个 fallback (比如复用第一张图，或者创建一个 placeholder)
					// 这里为了简单，我们假设 index=0 的图（通常是占位图或第一张）是好的，复用它
					// 或者你可以再创建一个 1x1 的粉色图
					if (index > 0 && texture_views[0]) {
						std::println("   -> Using fallback texture (Slot 0)");
						// 注意：这里只是复用 View，不要 Destroy 它
						// 这种简单的复用在清理时可能会有问题（重复 destroy），
						// 最稳妥的是生成一个专用的 1x1 错误贴图。
						// 但为了现在不崩，我们先暂时创建一个新的 1x1 占位图给它：
						create_texture_from_file("data/textures/default.png", texture_images[index], texture_images_memories[index], texture_views[index]);
					}
				}

				// B. 准备 Descriptor 信息
				VkDescriptorImageInfo image_info{};
				image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				image_info.imageView = texture_views[index];
				// 优化：所有纹理复用同一个全局采样器 texture_sampler
				// 除非你需要不同的过滤方式
				image_info.sampler = texture_sampler;

				VkWriteDescriptorSet descriptor_write{};
				descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptor_write.dstBinding = 1; // 绑定点 1
				// 【关键】偏移量 = index + 1 (因为 0 号被占用了)
				descriptor_write.dstArrayElement = static_cast<uint32_t>(index + 1);
				descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				descriptor_write.descriptorCount = 1;
				descriptor_write.pImageInfo = &image_info;

				// C. 更新所有 Frame 的 Set
				for (size_t i = 0; i < swapchain_images.size(); i++) {
					descriptor_write.dstSet = descriptor_sets[i];
					vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);
				}

				// 打印进度 (可选，防止以为卡死)
				if ((index + 1) % 5 == 0) std::println("[RHI] Loaded {}/{} textures...", index + 1, count);
			}
		}

		// ==========================================
		// Texture Implementation
		// ==========================================

		// [新增] 加载单张纹理并返回资源
		void create_texture_from_file(const std::string& path, VkImage& out_image, VkDeviceMemory& out_mem, VkImageView& out_view) {
			// 1. 使用 IO 模块加载像素数据
			auto img_opt = bud::io::ImageLoader::load(path);

			// 如果加载失败（比如路径不对），就生成一个粉色 1x1 像素作为占位，防止崩
			if (!img_opt) {
				std::println("[RHI] Failed to load texture: {}. Using fallback.", path);
				// 这里可以写个生成 1x1 纯色像素的逻辑，或者简单抛出异常
				// 为了代码简短，这里先抛出异常，实际项目建议用 fallback
				throw std::runtime_error("Texture load failed");
			}
			auto& img = *img_opt;

			// 2. 创建 Staging Buffer
			VkDeviceSize image_size = img.width * img.height * 4;
			VkBuffer staging_buffer;
			VkDeviceMemory staging_buffer_memory;
			create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_buffer, staging_buffer_memory);

			void* data;
			vkMapMemory(device, staging_buffer_memory, 0, image_size, 0, &data);
			memcpy(data, img.pixels, static_cast<size_t>(image_size));
			vkUnmapMemory(device, staging_buffer_memory);

			// 3. 计算 Mipmaps
			uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(std::max(img.width, img.height)))) + 1;

			// 4. 创建 Image
			create_image(img.width, img.height, mips, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out_image, out_mem);

			// 5. 拷贝数据 + 生成 Mipmaps
			transition_image_layout(out_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			copy_buffer_to_image(staging_buffer, out_image, static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height));
			generate_mipmaps(out_image, VK_FORMAT_R8G8B8A8_SRGB, img.width, img.height, mips);

			// 6. 清理 Staging
			vkDestroyBuffer(device, staging_buffer, nullptr);
			vkFreeMemory(device, staging_buffer_memory, nullptr);

			// 7. 创建 View
			// 注意：这里我们需要稍微修改 create_image_view 让它支持传入 mips 参数，或者直接在这里写
			VkImageViewCreateInfo view_info{};
			view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			view_info.image = out_image;
			view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
			view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			view_info.subresourceRange.baseMipLevel = 0;
			view_info.subresourceRange.levelCount = mips; // 使用计算出的 mips
			view_info.subresourceRange.baseArrayLayer = 0;
			view_info.subresourceRange.layerCount = 1;

			if (vkCreateImageView(device, &view_info, nullptr, &out_view) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create texture view!");
			}
		}

		void create_texture_image() {
			// 1. 手动造一个 1x1 的白色像素 (R=255, G=255, B=255, A=255)
			uint32_t pixel = 0xFFFFFFFF;
			int tex_width = 1;
			int tex_height = 1;
			VkDeviceSize image_size = 4;

			// 2. 创建 Staging Buffer
			VkBuffer staging_buffer;
			VkDeviceMemory staging_buffer_memory;
			create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				staging_buffer, staging_buffer_memory);

			// 3. 拷贝数据 (直接拷贝那个 uint32_t)
			void* data;
			vkMapMemory(device, staging_buffer_memory, 0, image_size, 0, &data);
			memcpy(data, &pixel, static_cast<size_t>(image_size));
			vkUnmapMemory(device, staging_buffer_memory);

			mip_levels = 1; // 占位图只有 1 层

			// 4. 创建 GPU Image
			create_image(tex_width, tex_height, mip_levels, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture_image, texture_image_memory);

			// 5. 布局转换 + 拷贝
			transition_image_layout(texture_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			copy_buffer_to_image(staging_buffer, texture_image, static_cast<uint32_t>(tex_width), static_cast<uint32_t>(tex_height));
			transition_image_layout(texture_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			// 6. 清理
			vkDestroyBuffer(device, staging_buffer, nullptr);
			vkFreeMemory(device, staging_buffer_memory, nullptr);

			std::println("[Vulkan] Placeholder texture created (1x1).");
		}

		// [Worker Thread] 异步加载图片
		void load_texture_async(const std::string& filename) {
			std::println("[RHI] Dispatching async texture load: {}", filename);

			task_scheduler->spawn("AsyncTextureLoad", [this, filename]() {
				// 1. 调用 IO 模块 (自动处理内存，RAII)
				// 使用 std::optional 处理可能的失败
				auto img_opt = bud::io::ImageLoader::load(filename);

				if (!img_opt) {
					return; // IO 模块内部已经打印了错误
				}

				// Move 语义转移所有权，img_opt 里的内存现在属于这个 lambda
				auto img = std::move(*img_opt);

				// 2. 提交给主线程
				task_scheduler->submit_main_thread_task([this, img = std::move(img)]() mutable {
					// 3. 主线程拿到了 img (包含 width, height, pixels)
					// 注意：这里传的是 img 对象，lambda 结束时 img 析构，自动调用 stbi_image_free
					// 所以要在 update_texture_resources 里面拷贝数据，或者把 img 所有权传进去
					this->update_texture_resources(img.pixels, img.width, img.height);
				});
			});
		}

		void update_texture_resources(unsigned char* pixels, int width, int height) {
			std::println("[Main] Texture data received. Uploading with Mipmaps...");
			vkDeviceWaitIdle(device);

			// 清理旧资源
			vkDestroyImageView(device, texture_image_view, nullptr);
			vkDestroyImage(device, texture_image, nullptr);
			vkFreeMemory(device, texture_image_memory, nullptr);

			// 1. 计算 Mipmap 层级数
			// 公式：log2(max(w, h)) + 1
			mip_levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

			VkDeviceSize image_size = width * height * 4;
			VkBuffer staging_buffer;
			VkDeviceMemory staging_buffer_memory;
			create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_buffer, staging_buffer_memory);

			void* data;
			vkMapMemory(device, staging_buffer_memory, 0, image_size, 0, &data);
			memcpy(data, pixels, static_cast<size_t>(image_size));
			vkUnmapMemory(device, staging_buffer_memory);

			// 2. 创建 Image
			// 注意 usage：除了 TRANSFER_DST (接收 buffer)，还需要 TRANSFER_SRC (作为下一级 mip 的源)
			create_image(width, height, mip_levels, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture_image, texture_image_memory);

			// 3. 拷贝 Base Level (Level 0)
			transition_image_layout(texture_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			copy_buffer_to_image(staging_buffer, texture_image, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

			// 4. 【关键】生成 Mipmaps (这会自动把 layout 转为 SHADER_READ_ONLY)
			generate_mipmaps(texture_image, VK_FORMAT_R8G8B8A8_SRGB, width, height, mip_levels);

			vkDestroyBuffer(device, staging_buffer, nullptr);
			vkFreeMemory(device, staging_buffer_memory, nullptr);

			// 5. 重建 View 和 Sampler
			texture_image_view = create_image_view(texture_image, VK_FORMAT_R8G8B8A8_SRGB);
			// 采样器可能需要重建以支持 maxLod，或者我们直接修改 create_texture_sampler
			// 简单起见，这里我们假设 sampler 在 init 里创建一次就够了，只要 update Descriptor 就行
			// 但为了严谨，最好更新 Sampler 的 maxLod
			vkDestroySampler(device, texture_sampler, nullptr);
			create_texture_sampler(); // 使用新的 mip_levels 创建采样器

			update_descriptor_sets_texture();
			std::println("[Vulkan] Texture loaded with {} mip levels!", mip_levels);
		}

		// [新增] 辅助函数：只更新纹理的 Descriptor Write
		void update_descriptor_sets_texture() {
			for (size_t i = 0; i < swapchain_images.size(); i++) {
				VkDescriptorImageInfo image_info{};
				image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				image_info.imageView = texture_image_view; // 新的 View
				image_info.sampler = texture_sampler;      // 采样器通常不用变

				VkWriteDescriptorSet descriptor_write{};
				descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptor_write.dstSet = descriptor_sets[i];
				descriptor_write.dstBinding = 1; // Binding 1 是纹理
				descriptor_write.dstArrayElement = 0;
				descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				descriptor_write.descriptorCount = 1;
				descriptor_write.pImageInfo = &image_info;

				vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);
			}
		}

		void create_texture_image_view() {
			texture_image_view = create_image_view(texture_image, VK_FORMAT_R8G8B8A8_SRGB);
		}

		void create_texture_sampler() {
			VkSamplerCreateInfo sampler_info{};
			sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			sampler_info.magFilter = VK_FILTER_LINEAR;
			sampler_info.minFilter = VK_FILTER_LINEAR;
			sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			sampler_info.anisotropyEnable = VK_TRUE;
			sampler_info.maxAnisotropy = 16.0f;
			sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
			sampler_info.unnormalizedCoordinates = VK_FALSE;
			sampler_info.compareEnable = VK_FALSE;
			sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
			sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			sampler_info.mipLodBias = 0.0f;
			sampler_info.minLod = 0.0f;
			sampler_info.maxLod = static_cast<float>(mip_levels);


			if (vkCreateSampler(device, &sampler_info, nullptr, &texture_sampler) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create texture sampler!");
			}
		}

		// --- 辅助函数 ---

		VkImageView create_image_view(VkImage image, VkFormat format) {
			VkImageViewCreateInfo view_info{};
			view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			view_info.image = image;
			view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view_info.format = format;
			view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			view_info.subresourceRange.baseMipLevel = 0;
			view_info.subresourceRange.levelCount = mip_levels;
			view_info.subresourceRange.baseArrayLayer = 0;
			view_info.subresourceRange.layerCount = 1;

			VkImageView image_view;
			if (vkCreateImageView(device, &view_info, nullptr, &image_view) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create texture image view!");
			}
			return image_view;
		}

		void create_image(uint32_t width, uint32_t height, uint32_t mip_levels, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& image_memory) {
			VkImageCreateInfo image_info{};
			image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			image_info.imageType = VK_IMAGE_TYPE_2D;
			image_info.extent.width = width;
			image_info.extent.height = height;
			image_info.extent.depth = 1;
			image_info.mipLevels = mip_levels;
			image_info.arrayLayers = 1;
			image_info.format = format;
			image_info.tiling = tiling;
			image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			image_info.usage = usage;
			image_info.samples = VK_SAMPLE_COUNT_1_BIT;
			image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create image!");
			}

			VkMemoryRequirements mem_requirements;
			vkGetImageMemoryRequirements(device, image, &mem_requirements);

			VkMemoryAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc_info.allocationSize = mem_requirements.size;
			alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, properties);

			if (vkAllocateMemory(device, &alloc_info, nullptr, &image_memory) != VK_SUCCESS) {
				throw std::runtime_error("Failed to allocate image memory!");
			}

			vkBindImageMemory(device, image, image_memory, 0);
		}

		// 一次性命令缓冲辅助函数
		VkCommandBuffer begin_single_time_commands() {
			VkCommandBufferAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			alloc_info.commandPool = frames[0].main_command_pool;
			alloc_info.commandBufferCount = 1;

			VkCommandBuffer command_buffer;
			vkAllocateCommandBuffers(device, &alloc_info, &command_buffer);

			VkCommandBufferBeginInfo begin_info{};
			begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

			vkBeginCommandBuffer(command_buffer, &begin_info);
			return command_buffer;
		}

		void end_single_time_commands(VkCommandBuffer command_buffer) {
			vkEndCommandBuffer(command_buffer);

			VkSubmitInfo submit_info{};
			submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit_info.commandBufferCount = 1;
			submit_info.pCommandBuffers = &command_buffer;

			vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
			vkQueueWaitIdle(graphics_queue);

			vkFreeCommandBuffers(device, frames[0].main_command_pool, 1, &command_buffer);
		}

		void transition_image_layout(VkImage image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout) {
			VkCommandBuffer command_buffer = begin_single_time_commands();

			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = old_layout;
			barrier.newLayout = new_layout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;

			VkPipelineStageFlags source_stage;
			VkPipelineStageFlags destination_stage;

			if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
				barrier.srcAccessMask = 0;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			}
			else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			}
			else {
				throw std::invalid_argument("Unsupported layout transition!");
			}

			vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

			end_single_time_commands(command_buffer);
		}

		void copy_buffer_to_image(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
			VkCommandBuffer command_buffer = begin_single_time_commands();

			VkBufferImageCopy region{};
			region.bufferOffset = 0;
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = 0;
			region.imageSubresource.baseArrayLayer = 0;
			region.imageSubresource.layerCount = 1;
			region.imageOffset = { 0, 0, 0 };
			region.imageExtent = { width, height, 1 };

			vkCmdCopyBufferToImage(command_buffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			end_single_time_commands(command_buffer);
		}

		void create_descriptor_pool() {
			std::array<VkDescriptorPoolSize, 2> pool_sizes{};
			pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			pool_sizes[0].descriptorCount = static_cast<uint32_t>(swapchain_images.size());
			// 为 Sampler 预留池子空间
			pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			pool_sizes[1].descriptorCount = static_cast<uint32_t>(swapchain_images.size() * 1000);

			VkDescriptorPoolCreateInfo pool_info{};
			pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
			pool_info.pPoolSizes = pool_sizes.data();
			pool_info.maxSets = static_cast<uint32_t>(swapchain_images.size());

			if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create descriptor pool!");
			}
		}

		void create_descriptor_sets() {
			std::vector<VkDescriptorSetLayout> layouts(swapchain_images.size(), descriptor_set_layout);
			VkDescriptorSetAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			alloc_info.descriptorPool = descriptor_pool;
			alloc_info.descriptorSetCount = static_cast<uint32_t>(swapchain_images.size());
			alloc_info.pSetLayouts = layouts.data();

			descriptor_sets.resize(swapchain_images.size());
			if (vkAllocateDescriptorSets(device, &alloc_info, descriptor_sets.data()) != VK_SUCCESS) {
				throw std::runtime_error("Failed to allocate descriptor sets!");
			}

			for (size_t i = 0; i < swapchain_images.size(); i++) {
				// Info 0: UBO
				VkDescriptorBufferInfo buffer_info{};
				buffer_info.buffer = uniform_buffers[i];
				buffer_info.offset = 0;
				buffer_info.range = sizeof(UniformBufferObject);

				// [新增] Info 1: Image Sampler
				VkDescriptorImageInfo image_info{};
				image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				image_info.imageView = texture_image_view;
				image_info.sampler = texture_sampler;

				std::array<VkWriteDescriptorSet, 2> descriptor_writes{};

				// Write 0: UBO
				descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptor_writes[0].dstSet = descriptor_sets[i];
				descriptor_writes[0].dstBinding = 0;
				descriptor_writes[0].dstArrayElement = 0;
				descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				descriptor_writes[0].descriptorCount = 1;
				descriptor_writes[0].pBufferInfo = &buffer_info;

				// [新增] Write 1: Sampler
				descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptor_writes[1].dstSet = descriptor_sets[i];
				descriptor_writes[1].dstBinding = 1;
				descriptor_writes[1].dstArrayElement = 0;
				descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				descriptor_writes[1].descriptorCount = 1;
				descriptor_writes[1].pImageInfo = &image_info;

				vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptor_writes.size()), descriptor_writes.data(), 0, nullptr);
			}
		}

		void create_command_buffer() {
			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
				VkCommandBufferAllocateInfo alloc_info{};
				alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
				alloc_info.commandPool = frames[i].main_command_pool;
				alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				alloc_info.commandBufferCount = 1;

				if (vkAllocateCommandBuffers(device, &alloc_info, &frames[i].main_command_buffer) != VK_SUCCESS) {
					throw std::runtime_error("Failed to allocate command buffers!");
				}
			}
		}


		// [新增] 辅助函数：从 Pool 分配 Secondary Buffer
		VkCommandBuffer allocate_secondary_command_buffer(VkCommandPool pool) {
			VkCommandBufferAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			alloc_info.commandPool = pool;
			alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY; // [关键] 二级缓冲
			alloc_info.commandBufferCount = 1;

			VkCommandBuffer cmdBuffer;
			if (vkAllocateCommandBuffers(device, &alloc_info, &cmdBuffer) != VK_SUCCESS) {
				throw std::runtime_error("Failed to allocate worker command buffer!");
			}
			return cmdBuffer;
		}

		void create_sync_objects() {
			VkSemaphoreCreateInfo semaphore_info{};
			semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			VkFenceCreateInfo fence_info{};
			fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT; // 初始状态要是 Signaled，否则第一帧会卡死

			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
				if (vkCreateSemaphore(device, &semaphore_info, nullptr, &frames[i].image_available_semaphore) != VK_SUCCESS ||
					vkCreateSemaphore(device, &semaphore_info, nullptr, &frames[i].render_finished_semaphore) != VK_SUCCESS ||
					vkCreateFence(device, &fence_info, nullptr, &frames[i].in_flight_fence) != VK_SUCCESS) {
					throw std::runtime_error("Failed to create synchronization objects for a frame!");
				}
			}
		}

		// ==========================================
		// Runtime Helpers
		// ==========================================
	private:
		void record_command_buffer(VkCommandBuffer buffer, uint32_t image_index) {
			VkCommandBufferBeginInfo begin_info{};
			begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

			if (vkBeginCommandBuffer(buffer, &begin_info) != VK_SUCCESS) {
				throw std::runtime_error("Failed to begin recording command buffer!");
			}

			VkRenderPassBeginInfo render_pass_info{};
			render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			render_pass_info.renderPass = render_pass;
			render_pass_info.framebuffer = swapchain_framebuffers[image_index];
			render_pass_info.renderArea.offset = { 0, 0 };
			render_pass_info.renderArea.extent = swapchain_extent;

			std::array<VkClearValue, 2> clear_values{};
			clear_values[0].color = { {0.1f, 0.1f, 0.1f, 1.0f} };
			clear_values[1].depthStencil = { 1.0f, 0 };

			render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
			render_pass_info.pClearValues = clear_values.data();

			vkCmdBeginRenderPass(buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);

			VkViewport viewport{};
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = static_cast<float>(swapchain_extent.width);
			viewport.height = static_cast<float>(swapchain_extent.height);
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vkCmdSetViewport(buffer, 0, 1, &viewport);

			VkRect2D scissor{};
			scissor.offset = { 0, 0 };
			scissor.extent = swapchain_extent;
			vkCmdSetScissor(buffer, 0, 1, &scissor);

			VkBuffer vertex_buffers[] = { vertex_buffer };
			VkDeviceSize offsets[] = { 0 };
			vkCmdBindVertexBuffers(buffer, 0, 1, vertex_buffers, offsets);

			vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor_sets[image_index], 0, nullptr);

			vkCmdDraw(buffer, static_cast<uint32_t>(vertices.size()), 1, 0, 0);

			vkCmdEndRenderPass(buffer);
			if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
				throw std::runtime_error("Failed to record command buffer!");
			}
		}

		void update_uniform_buffer(uint32_t current_image, const bud::math::mat4& view, const bud::math::mat4& proj) {
			static auto start_time = std::chrono::high_resolution_clock::now();
			auto current_time = std::chrono::high_resolution_clock::now();
			float time = std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();

			UniformBufferObject ubo{};
			ubo.model = bud::math::mat4(1.0f);
			ubo.view = view;
			ubo.proj = proj;

			// 假设相机位置从 view 矩阵逆推，或者从 camera 类传进来更好
			// 直接从 View Matrix 的逆矩阵取位移
			auto invView = bud::math::inverse(view);
			ubo.camPos = bud::math::vec3(invView[3]);

			// 从侧后方打过来法线细节更明显
			ubo.lightDir = bud::math::normalize(bud::math::vec3(-1.0f, 2.0f, 1.5f));

			memcpy(uniform_buffers_mapped[current_image], &ubo, sizeof(ubo));
		}

		// ==========================================
		// Low-Level Helpers
		// ==========================================
	private:

		VkShaderModule create_shader_module(const std::vector<char>& code) {
			VkShaderModuleCreateInfo create_info{};
			create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			create_info.codeSize = code.size();
			create_info.pCode = reinterpret_cast<const uint32_t*>(code.data());

			VkShaderModule shader_module;
			if (vkCreateShaderModule(device, &create_info, nullptr, &shader_module) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create shader module!");
			}
			return shader_module;
		}

		uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
			VkPhysicalDeviceMemoryProperties mem_properties;
			vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);
			for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
				if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
					return i;
				}
			}
			throw std::runtime_error("Failed to find suitable memory type!");
		}

		void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& buffer_memory) {
			VkBufferCreateInfo buffer_info{};
			buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			buffer_info.size = size;
			buffer_info.usage = usage;
			buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create buffer!");
			}

			VkMemoryRequirements mem_requirements;
			vkGetBufferMemoryRequirements(device, buffer, &mem_requirements);

			VkMemoryAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc_info.allocationSize = mem_requirements.size;
			alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, properties);

			if (vkAllocateMemory(device, &alloc_info, nullptr, &buffer_memory) != VK_SUCCESS) {
				throw std::runtime_error("Failed to allocate buffer memory!");
			}
			vkBindBufferMemory(device, buffer, buffer_memory, 0);
		}

		// Swapchain Helpers
		struct SwapChainSupportDetails {
			VkSurfaceCapabilitiesKHR capabilities;
			std::vector<VkSurfaceFormatKHR> formats;
			std::vector<VkPresentModeKHR> present_modes;
		};

		SwapChainSupportDetails query_swapchain_support(VkPhysicalDevice device) {
			SwapChainSupportDetails details;
			vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
			uint32_t format_count;
			vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
			if (format_count != 0) {
				details.formats.resize(format_count);
				vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, details.formats.data());
			}
			uint32_t present_mode_count;
			vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, nullptr);
			if (present_mode_count != 0) {
				details.present_modes.resize(present_mode_count);
				vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, details.present_modes.data());
			}
			return details;
		}

		VkSurfaceFormatKHR choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats) {
			for (const auto& available_format : available_formats) {
				if (available_format.format == VK_FORMAT_B8G8R8A8_SRGB && available_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
					return available_format;
				}
			}
			return available_formats[0];
		}

		VkPresentModeKHR choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes) {
			for (const auto& available_present_mode : available_present_modes) {
				if (available_present_mode == VK_PRESENT_MODE_MAILBOX_KHR) return available_present_mode;
			}
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, SDL_Window* window) {
			if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
				return capabilities.currentExtent;
			}
			else {
				int width, height;
				SDL_GetWindowSizeInPixels(window, &width, &height);
				VkExtent2D actual_extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
				actual_extent.width = std::clamp(actual_extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
				actual_extent.height = std::clamp(actual_extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
				return actual_extent;
			}
		}

		QueueFamilyIndices find_queue_families(VkPhysicalDevice device) {
			QueueFamilyIndices indices;
			uint32_t queue_family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
			std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

			int i = 0;
			for (const auto& queue_family : queue_families) {
				if (queue_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphics_family = i;
				VkBool32 present_support = false;
				vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
				if (present_support) indices.present_family = i;
				if (indices.is_complete()) break;
				i++;
			}
			return indices;
		}

		void generate_mipmaps(VkImage image, VkFormat format, int32_t tex_width, int32_t tex_height, uint32_t mip_levels) {
			// Blit 需要纹理格式是否支持线性过滤
			VkFormatProperties format_properties;
			vkGetPhysicalDeviceFormatProperties(physical_device, format, &format_properties);

			if (!(format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
				throw std::runtime_error("Texture image format does not support linear blitting!");
			}

			VkCommandBuffer command_buffer = begin_single_time_commands();

			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.image = image;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;
			barrier.subresourceRange.levelCount = 1;

			int32_t mip_width = tex_width;
			int32_t mip_height = tex_height;

			for (uint32_t i = 1; i < mip_levels; i++) {
				// 准备目标层 (Level i)
				// 此时 Level i 是 UNDEFINED，需要把它转为 TRANSFER_DST_OPTIMAL 以便 Blit 写入
				VkImageMemoryBarrier barrier_dst{};
				barrier_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier_dst.image = image;
				barrier_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				barrier_dst.subresourceRange.baseArrayLayer = 0;
				barrier_dst.subresourceRange.layerCount = 1;
				barrier_dst.subresourceRange.levelCount = 1;
				barrier_dst.subresourceRange.baseMipLevel = i; // 目标是当前层 i

				barrier_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				barrier_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier_dst.srcAccessMask = 0;
				barrier_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

				vkCmdPipelineBarrier(command_buffer,
					VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					0,
					0, nullptr,
					0, nullptr,
					1, &barrier_dst);


				// 1. 转换 Level i-1: TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL
				// 因为它要作为下一次 Blit 的源
				barrier.subresourceRange.baseMipLevel = i - 1;
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

				vkCmdPipelineBarrier(command_buffer,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					0,
					0, nullptr,
					0, nullptr,
					1, &barrier);

				// 2. 执行 Blit (缩放)
				VkImageBlit blit{};
				blit.srcOffsets[0] = { 0, 0, 0 };
				blit.srcOffsets[1] = { mip_width, mip_height, 1 };
				blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blit.srcSubresource.mipLevel = i - 1;
				blit.srcSubresource.baseArrayLayer = 0;
				blit.srcSubresource.layerCount = 1;

				blit.dstOffsets[0] = { 0, 0, 0 };
				blit.dstOffsets[1] = { mip_width > 1 ? mip_width / 2 : 1, mip_height > 1 ? mip_height / 2 : 1, 1 };
				blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blit.dstSubresource.mipLevel = i;
				blit.dstSubresource.baseArrayLayer = 0;
				blit.dstSubresource.layerCount = 1;

				vkCmdBlitImage(command_buffer,
					image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1, &blit,
					VK_FILTER_LINEAR);

				// 3. 转换 Level i-1: TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
				// 这层做完了，可以给 Shader 读了
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

				vkCmdPipelineBarrier(command_buffer,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					0,
					0, nullptr,
					0, nullptr,
					1, &barrier);

				if (mip_width > 1) mip_width /= 2;
				if (mip_height > 1) mip_height /= 2;
			}

			// 4. 处理最后一层 (Level n-1)
			// 它一直是 DST，从未变成 SRC，所以最后还需要手动转一次
			barrier.subresourceRange.baseMipLevel = mip_levels - 1;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			vkCmdPipelineBarrier(command_buffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &barrier);

			end_single_time_commands(command_buffer);
		}

		// Debug Helpers
		VkResult create_debug_utils_messenger_ext(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
			auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
			if (func != nullptr) return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}

		void destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
			auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
			if (func != nullptr) func(instance, debugMessenger, pAllocator);
		}

		void setup_debug_messenger(bool enable) {
			if (!enable) return;
			VkDebugUtilsMessengerCreateInfoEXT create_info{};
			create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			create_info.pfnUserCallback = debug_callback;
			if (create_debug_utils_messenger_ext(instance, &create_info, nullptr, &debug_messenger) != VK_SUCCESS) {
				throw std::runtime_error("Failed to set up debug messenger!");
			}
		}

		static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
			if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
				std::println(stderr, "[Validation Layer]: {}", pCallbackData->pMessage);
			}
			return VK_FALSE;
		}
	};
}
