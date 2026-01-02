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

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>
#endif

#define GLM_FORCE_RADIANS           // Force use of radians
#define GLM_FORCE_DEPTH_ZERO_TO_ONE // Force Vulkan depth range (0.0 to 1.0)
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

    struct UniformBufferObject {
        alignas(16) glm::mat4 model;
        alignas(16) glm::mat4 view;
        alignas(16) glm::mat4 proj;
    };

    struct Vertex {
        float pos[3];   // x, y, z
        float color[3]; // r, g, b

        static VkVertexInputBindingDescription get_binding_description() {
            VkVertexInputBindingDescription binding_description{};
            binding_description.binding = 0;
            binding_description.stride = sizeof(Vertex);
            binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            return binding_description;
        }

        static std::vector<VkVertexInputAttributeDescription> get_attribute_descriptions() {
            std::vector<VkVertexInputAttributeDescription> attribute_descriptions(2);

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
        virtual void init(SDL_Window* window, bool enable_validation) = 0;
        virtual void draw_frame() = 0;
        virtual void wait_idle() = 0;
        virtual void cleanup() = 0;
    };

    // ==========================================
    // Vulkan Implementation
    // ==========================================

    export class VulkanRHI : public RHI {
    public:
        void init(SDL_Window* window, bool enable_validation) override {
            // 1. Core Vulkan Setup
            create_instance(window, enable_validation);
            setup_debug_messenger(enable_validation);
            create_surface(window);
            pick_physical_device();
            create_logical_device(enable_validation);

            // 2. Presentation Setup
            create_swapchain(window);
            create_image_views();

            // 3. Pipeline Setup
            create_render_pass();
            create_descriptor_set_layout(); // Layout 必须在 Pipeline 之前
            create_graphics_pipeline();
            create_framebuffers();

            // 4. Resources Setup
            create_command_pool();
            create_vertex_buffer();
            create_uniform_buffers();       // Buffer 必须在 Set 之前
            create_descriptor_pool();       // Pool 必须在 Set 之前
            create_descriptor_sets();

            // 5. Command & Sync
            create_command_buffer();
            create_sync_objects();
        }

        void draw_frame() override {
            vkWaitForFences(device, 1, &in_flight_fence, VK_TRUE, UINT64_MAX);

            uint32_t image_index;
            VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, image_available_semaphore, VK_NULL_HANDLE, &image_index);

            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                throw std::runtime_error("Failed to acquire swap chain image!");
            }

            vkResetFences(device, 1, &in_flight_fence);

            // 更新当前帧的 UBO
            update_uniform_buffer(image_index);

            vkResetCommandBuffer(command_buffer, 0);
            record_command_buffer(command_buffer, image_index);

            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            VkSemaphore wait_semaphores[] = { image_available_semaphore };
            VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
            submit_info.waitSemaphoreCount = 1;
            submit_info.pWaitSemaphores = wait_semaphores;
            submit_info.pWaitDstStageMask = wait_stages;

            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &command_buffer;

            VkSemaphore signal_semaphores[] = { render_finished_semaphore };
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = signal_semaphores;

            if (vkQueueSubmit(graphics_queue, 1, &submit_info, in_flight_fence) != VK_SUCCESS) {
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
        }

        void wait_idle() override {
            if (device) vkDeviceWaitIdle(device);
        }

        void cleanup() override {
            wait_idle();

            // Cleanup Sync Objects
            vkDestroySemaphore(device, render_finished_semaphore, nullptr);
            vkDestroySemaphore(device, image_available_semaphore, nullptr);
            vkDestroyFence(device, in_flight_fence, nullptr);

            // Cleanup Command
            vkDestroyCommandPool(device, command_pool, nullptr);

            // Cleanup Buffers
            vkDestroyBuffer(device, vertex_buffer, nullptr);
            vkFreeMemory(device, vertex_buffer_memory, nullptr);

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

        // ==========================================
        // Private Members
        // ==========================================
    private:
        // Core
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

        // Resources (Command, Buffers, Descriptors)
        VkCommandPool command_pool = nullptr;
        VkCommandBuffer command_buffer = nullptr;

        VkBuffer vertex_buffer = nullptr;
        VkDeviceMemory vertex_buffer_memory = nullptr;

        VkDescriptorPool descriptor_pool = nullptr;
        std::vector<VkDescriptorSet> descriptor_sets;
        std::vector<VkBuffer> uniform_buffers;
        std::vector<VkDeviceMemory> uniform_buffers_memory;
        std::vector<void*> uniform_buffers_mapped;

        // Synchronization
        VkSemaphore image_available_semaphore = nullptr;
        VkSemaphore render_finished_semaphore = nullptr;
        VkFence in_flight_fence = nullptr;

        // Test Data
        const std::vector<Vertex> vertices = {
            {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
            {{0.5f, 0.5f},  {0.0f, 1.0f, 0.0f}},
            {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}
        };

        // ==========================================
        // Initialization Helpers
        // ==========================================
    private:
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
            VkDeviceCreateInfo create_info{};
            create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
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

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_attachment_ref;

            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            VkRenderPassCreateInfo render_pass_info{};
            render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            render_pass_info.attachmentCount = 1;
            render_pass_info.pAttachments = &color_attachment;
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

            VkDescriptorSetLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = 1;
            layout_info.pBindings = &ubo_layout_binding;

            if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_set_layout) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create descriptor set layout!");
            }
            std::println("[Vulkan] Descriptor Set Layout created.");
        }

        void create_graphics_pipeline() {
            auto vert_shader_code = read_file("src/shaders/vert.spv");
            auto frag_shader_code = read_file("src/shaders/frag.spv");

            VkShaderModule vert_module = create_shader_module(vert_shader_code);
            VkShaderModule frag_module = create_shader_module(frag_shader_code);

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
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
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
            pipeline_info.pDepthStencilState = nullptr;
            pipeline_info.pColorBlendState = &color_blending;
            pipeline_info.pDynamicState = &dynamic_state_info;
            pipeline_info.layout = pipeline_layout;
            pipeline_info.renderPass = render_pass;
            pipeline_info.subpass = 0;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &graphics_pipeline) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create graphics pipeline!");
            }

            vkDestroyShaderModule(device, frag_module, nullptr);
            vkDestroyShaderModule(device, vert_module, nullptr);
            std::println("[Vulkan] Graphics pipeline created successfully");
        }

        void create_framebuffers() {
            swapchain_framebuffers.resize(swapchain_image_views.size());
            for (size_t i = 0; i < swapchain_image_views.size(); i++) {
                VkImageView attachments[] = { swapchain_image_views[i] };

                VkFramebufferCreateInfo framebuffer_info{};
                framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebuffer_info.renderPass = render_pass;
                framebuffer_info.attachmentCount = 1;
                framebuffer_info.pAttachments = attachments;
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
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = queue_family_indices.graphics_family.value();

            if (vkCreateCommandPool(device, &pool_info, nullptr, &command_pool) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create command pool!");
            }
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

        void create_descriptor_pool() {
            VkDescriptorPoolSize pool_size{};
            pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            pool_size.descriptorCount = static_cast<uint32_t>(swapchain_images.size());

            VkDescriptorPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &pool_size;
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
                VkDescriptorBufferInfo buffer_info{};
                buffer_info.buffer = uniform_buffers[i];
                buffer_info.offset = 0;
                buffer_info.range = sizeof(UniformBufferObject);

                VkWriteDescriptorSet descriptor_write{};
                descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptor_write.dstSet = descriptor_sets[i];
                descriptor_write.dstBinding = 0;
                descriptor_write.dstArrayElement = 0;
                descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                descriptor_write.descriptorCount = 1;
                descriptor_write.pBufferInfo = &buffer_info;

                vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);
            }
        }

        void create_command_buffer() {
            VkCommandBufferAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc_info.commandPool = command_pool;
            alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc_info.commandBufferCount = 1;

            if (vkAllocateCommandBuffers(device, &alloc_info, &command_buffer) != VK_SUCCESS) {
                throw std::runtime_error("Failed to allocate command buffers!");
            }
        }

        void create_sync_objects() {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            if (vkCreateSemaphore(device, &semaphore_info, nullptr, &image_available_semaphore) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished_semaphore) != VK_SUCCESS ||
                vkCreateFence(device, &fence_info, nullptr, &in_flight_fence) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create synchronization objects!");
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
            VkClearValue clear_color = { {{0.1f, 0.1f, 0.1f, 1.0f}} };
            render_pass_info.clearValueCount = 1;
            render_pass_info.pClearValues = &clear_color;

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

        void update_uniform_buffer(uint32_t current_image) {
            static auto start_time = std::chrono::high_resolution_clock::now();
            auto current_time = std::chrono::high_resolution_clock::now();
            float time = std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();

            UniformBufferObject ubo{};
            ubo.model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            ubo.proj = glm::perspective(glm::radians(45.0f), swapchain_extent.width / (float)swapchain_extent.height, 0.1f, 10.0f);
            ubo.proj[1][1] *= -1; // Fix GLM clip space

            memcpy(uniform_buffers_mapped[current_image], &ubo, sizeof(ubo));
        }

        // ==========================================
        // Low-Level Helpers
        // ==========================================
    private:
        static std::vector<char> read_file(const std::string& filename) {
            std::ifstream file(filename, std::ios::ate | std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open file: " + filename);
            }
            size_t file_size = (size_t)file.tellg();
            std::vector<char> buffer(file_size);
            file.seekg(0);
            file.read(buffer.data(), file_size);
            file.close();
            return buffer;
        }

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
