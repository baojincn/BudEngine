#include <vector>
#include <string>
#include <iostream>
#include <print>
#include <optional>
#include <set>
#include <algorithm>
#include <limits>
#include <mutex>

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "src/graphics/vulkan/bud.graphics.vulkan.hpp"

#include "src/core/bud.math.hpp"
#include "src/platform/bud.platform.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.utils.hpp"

using namespace bud::graphics;
using namespace bud::graphics::vulkan;


PFN_vkCmdBeginDebugUtilsLabelEXT fpCmdBeginDebugUtilsLabelEXT = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT fpCmdEndDebugUtilsLabelEXT = nullptr;
PFN_vkSetDebugUtilsObjectNameEXT fpSetDebugUtilsObjectNameEXT = nullptr;


struct VulkanPipelineObject {
	VkPipeline pipeline;
	VkPipelineLayout layout;
};


VulkanLayoutTransition bud::graphics::vulkan::get_vk_transition(ResourceState state) {
	switch (state) {
	case ResourceState::Undefined:
		return { VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
	case ResourceState::RenderTarget:
		return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	case ResourceState::ShaderResource:
		return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };
	case ResourceState::DepthWrite:
		return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };
	case ResourceState::DepthRead:
		return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };
	case ResourceState::Present:
		return { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT };
	case ResourceState::TransferDst:
		return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
	case ResourceState::TransferSrc:
		return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
	default:
		return { VK_IMAGE_LAYOUT_GENERAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
	}
}


void VulkanRHI::init(bud::platform::Window* plat_window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation, uint32_t inflight_frame_count) {
	this->task_scheduler = task_scheduler;
	platform_window = plat_window;
	max_frames_in_flight = inflight_frame_count;

	// 基础构建 (Instance, Surface, Device)
	create_instance(instance, enable_validation);
	setup_debug_messenger(enable_validation);
	create_surface(plat_window);
	pick_physical_device();
	create_logical_device(enable_validation);

	// 交换链与呈现资源
	create_swapchain(plat_window);
	create_image_views();

	// 命令池与同步对象
	create_command_pool();
	create_command_buffer();
	create_sync_objects();

	// 初始化基础设施 (Subsystems)
	// 接管内存、资源池、管线缓存、描述符分配
	memory_allocator = std::make_unique<VulkanMemoryAllocator>(device, physical_device, max_frames_in_flight);
	memory_allocator->init();

	resource_pool = std::make_unique<VulkanResourcePool>(device, memory_allocator.get());

	pipeline_cache = std::make_unique<VulkanPipelineCache>();
	pipeline_cache->init(device);

	descriptor_allocators.resize(max_frames_in_flight);
	for (auto& alloc : descriptor_allocators) {
		alloc.init(device);
	}

	// 创建全局 Descriptor Set Layout (Global Bindless Layout)
	// Binding 0: UBO (std140)
	// Binding 1: Sampler2D[] (Bindless, Variable Count / Partial Bound)
	// Binding 2: ShadowMap (Sampler2DShadow)

	DescriptorLayoutBuilder layout_builder;
	layout_builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	layout_builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1000, 
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	layout_builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);

	global_set_layout = layout_builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);

	// 创建 Per-Frame UBO Buffers (Binding 0)
	VkDeviceSize ubo_size = 512; // Enough for matrices + light data (was 256)
	for (auto& frame : frames) {
		VkBufferCreateInfo buffer_info{};
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size = ubo_size;
		buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateBuffer(device, &buffer_info, nullptr, &frame.uniform_buffer) != VK_SUCCESS) {
			throw std::runtime_error("failed to create uniform buffer!");
		}

		VkMemoryRequirements mem_reqs;
		vkGetBufferMemoryRequirements(device, frame.uniform_buffer, &mem_reqs);

		VkPhysicalDeviceMemoryProperties mem_props;
		vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

		uint32_t memory_type = 0;
		for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
			if ((mem_reqs.memoryTypeBits & (1 << i)) &&
				(mem_props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
				memory_type = i;
				break;
			}
		}

		VkMemoryAllocateInfo alloc_info{};
		alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc_info.allocationSize = mem_reqs.size;
		alloc_info.memoryTypeIndex = memory_type;

		if (vkAllocateMemory(device, &alloc_info, nullptr, &frame.uniform_memory) != VK_SUCCESS) {
			throw std::runtime_error("failed to allocate uniform buffer memory!");
		}

		vkBindBufferMemory(device, frame.uniform_buffer, frame.uniform_memory, 0);
		vkMapMemory(device, frame.uniform_memory, 0, ubo_size, 0, &frame.uniform_mapped);
	}

	// 创建全局 Descriptor Pool (支持 Bindless + UPDATE_AFTER_BIND)
	{
		std::vector<VkDescriptorPoolSize> pool_sizes = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)frames.size() },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)frames.size() * 1001 } // 1000 bindless + 1 shadow
		};

		VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
		pool_info.maxSets = (uint32_t)frames.size();
		pool_info.poolSizeCount = (uint32_t)pool_sizes.size();
		pool_info.pPoolSizes = pool_sizes.data();

		if (vkCreateDescriptorPool(device, &pool_info, nullptr, &global_descriptor_pool) != VK_SUCCESS) {
			throw std::runtime_error("failed to create global descriptor pool!");
		}
	}

	// 创建默认采样器 (用于 Bindless Textures)
	VkSamplerCreateInfo sampler_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
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
	sampler_info.minLod = 0.0f;
	sampler_info.maxLod = VK_LOD_CLAMP_NONE; // Allow all mip levels

	if (vkCreateSampler(device, &sampler_info, nullptr, &default_sampler) != VK_SUCCESS) {
		throw std::runtime_error("failed to create default sampler!");
	}

	// Create Shadow Sampler (Compare Enable)
	VkSamplerCreateInfo shadow_sampler_info = sampler_info;
	shadow_sampler_info.magFilter = VK_FILTER_LINEAR;
	shadow_sampler_info.minFilter = VK_FILTER_LINEAR;
	shadow_sampler_info.compareEnable = VK_TRUE;
	shadow_sampler_info.compareOp = VK_COMPARE_OP_LESS; // or LESS_OR_EQUAL
	shadow_sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // Depths outside [0,1]?
	shadow_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	shadow_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

	if (vkCreateSampler(device, &shadow_sampler_info, nullptr, &shadow_sampler) != VK_SUCCESS) {
		throw std::runtime_error("failed to create shadow sampler!");
	}

	// Create Dummy Depth Texture for Shadow Binding
	{
		VkImageCreateInfo image_info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.extent.width = 1;
		image_info.extent.height = 1;
		image_info.extent.depth = 1;
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.format = VK_FORMAT_D32_SFLOAT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		
		if (vkCreateImage(device, &image_info, nullptr, &dummy_depth_texture.image) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create dummy depth");
		}
		
		VkMemoryRequirements mem_reqs;
		vkGetImageMemoryRequirements(device, dummy_depth_texture.image, &mem_reqs);
		
		MemoryBlock block = memory_allocator->alloc_static(mem_reqs.size, mem_reqs.alignment, mem_reqs.memoryTypeBits, MemoryUsage::GpuOnly);
		vkBindImageMemory(device, dummy_depth_texture.image, (VkDeviceMemory)block.internal_handle, block.offset);
		dummy_depth_texture.memory = (VkDeviceMemory)block.internal_handle; // [FIX] Store for cleanup
		
		VkImageViewCreateInfo view_info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view_info.image = dummy_depth_texture.image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = VK_FORMAT_D32_SFLOAT;
		view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		view_info.subresourceRange.baseMipLevel = 0;
		view_info.subresourceRange.levelCount = 1;
		view_info.subresourceRange.baseArrayLayer = 0;
		view_info.subresourceRange.layerCount = 1;
		
		if (vkCreateImageView(device, &view_info, nullptr, &dummy_depth_texture.view) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create dummy depth view");
		}
		
		transition_image_layout_immediate(dummy_depth_texture.image, VK_FORMAT_D32_SFLOAT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

		transition_image_layout_immediate(dummy_depth_texture.image, VK_FORMAT_D32_SFLOAT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

		dummy_depth_texture.sampler = shadow_sampler;


	}

	// 分配并初始化全局 Descriptor Sets
	for (auto& frame : frames) {
		VkDescriptorSetAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		alloc_info.descriptorPool = global_descriptor_pool;
		alloc_info.descriptorSetCount = 1;
		alloc_info.pSetLayouts = &global_set_layout;

		if (vkAllocateDescriptorSets(device, &alloc_info, &frame.global_descriptor_set) != VK_SUCCESS) {
			throw std::runtime_error("failed to allocate global descriptor set!");
		}

		DescriptorWriter writer;
		writer.write_buffer(0, frame.uniform_buffer, ubo_size, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		writer.write_image(2, 0, dummy_depth_texture.view, default_sampler, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		
		writer.update_set(device, frame.global_descriptor_set);
	}

	// 创建 Fallback 纹理 (Index 0)
	std::println("[Vulkan] Creating Fallback Texture...");
	{
		TextureDesc desc{};
		desc.width = 1;
		desc.height = 1;
		desc.format = TextureFormat::RGBA8_UNORM;

		// Red Fallback to identify missing textures
		uint32_t color = 0xFF0000FF; // R=FF, G=00, B=00, A=FF (Little Endian)
		fallback_texture_ptr = static_cast<VulkanTexture*>(create_texture(desc, &color, 4));
		update_bindless_texture(0, fallback_texture_ptr);
	}

	std::println("[Vulkan] RHI Initialized successfully (Clean Architecture).");
}

void VulkanRHI::cleanup() {
	wait_idle();

	if (dummy_depth_texture.view)
		vkDestroyImageView(device, dummy_depth_texture.view, nullptr);
	if (dummy_depth_texture.image)
		vkDestroyImage(device, dummy_depth_texture.image, nullptr);
	if (dummy_depth_texture.memory)
		vkFreeMemory(device, dummy_depth_texture.memory, nullptr);

	descriptor_allocators.clear();
	if (global_descriptor_pool)
		vkDestroyDescriptorPool(device, global_descriptor_pool, nullptr);
	if (global_set_layout)
		vkDestroyDescriptorSetLayout(device, global_set_layout, nullptr);

	if (pipeline_cache)
		pipeline_cache->cleanup();
	if (resource_pool)
		resource_pool->cleanup();
	if (memory_allocator)
		memory_allocator->cleanup();

	pipeline_cache.reset();
	resource_pool.reset();
	
	if (!buffer_memory_map.empty()) {
		std::println("[Vulkan] Warning: {} buffers were not explicitly destroyed, cleaning up now...", buffer_memory_map.size());
		for (auto& [buffer, memory] : buffer_memory_map) {
			vkDestroyBuffer(device, buffer, nullptr);
			if (memory)
				vkFreeMemory(device, memory, nullptr);
		}
		buffer_memory_map.clear();
	}
	
	memory_allocator.reset();

	for (auto semaphore : render_finished_semaphores)
		vkDestroySemaphore(device, semaphore, nullptr);

	for (int i = 0; i < max_frames_in_flight; i++) {
		if (frames[i].uniform_mapped)
			vkUnmapMemory(device, frames[i].uniform_memory);
		if (frames[i].uniform_buffer)
			vkDestroyBuffer(device, frames[i].uniform_buffer, nullptr);
		if (frames[i].uniform_memory)
			vkFreeMemory(device, frames[i].uniform_memory, nullptr);
		if (frames[i].image_available_semaphore)
			vkDestroySemaphore(device, frames[i].image_available_semaphore, nullptr);
		if (frames[i].in_flight_fence)
			vkDestroyFence(device, frames[i].in_flight_fence, nullptr);
		if (frames[i].main_command_pool)
			vkDestroyCommandPool(device, frames[i].main_command_pool, nullptr);
	}

	// Swapchain
	for (auto imageView : swapchain_image_views)
		vkDestroyImageView(device, imageView, nullptr);

	if (swapchain)
		vkDestroySwapchainKHR(device, swapchain, nullptr);

	for (auto layout : created_layouts) {
		vkDestroyPipelineLayout(device, layout, nullptr);
	}
	created_layouts.clear();

	// Device & Instance
	if (shadow_sampler)
		vkDestroySampler(device, shadow_sampler, nullptr);
	if (default_sampler)
		vkDestroySampler(device, default_sampler, nullptr);
	if (device)
		vkDestroyDevice(device, nullptr);
	if (enable_validation_layers && debug_messenger)
		destroy_debug_utils_messenger_ext(instance, debug_messenger, nullptr);
	if (surface)
		vkDestroySurfaceKHR(instance, surface, nullptr);
	if (instance)
		vkDestroyInstance(instance, nullptr);
}

void VulkanRHI::wait_idle() {
	if (device) vkDeviceWaitIdle(device);
}


bud::graphics::MemoryBlock VulkanRHI::create_gpu_buffer(uint64_t size, bud::graphics::ResourceState usage_state) {
	VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	buffer_info.size = size;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	// 总是包含 TRANSFER_DST，因为我们需要从 Upload Buffer 拷贝数据进来
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	if (usage_state == ResourceState::VertexBuffer) {
		buffer_info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	}
	else if (usage_state == ResourceState::IndexBuffer) {
		buffer_info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	}

	VkBuffer buffer;
	if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create GPU buffer!");
	}

	// 获取内存需求
	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(device, buffer, &mem_reqs);

	// 使用 Allocator 分配 Device Local 显存
	// 注意：这里 alloc_static 返回的 internal_handle 是 VkDeviceMemory
	MemoryBlock mem_block = memory_allocator->alloc_static(
		mem_reqs.size,
		mem_reqs.alignment,
		mem_reqs.memoryTypeBits,
		MemoryUsage::GpuOnly
	);

	// 绑定
	vkBindBufferMemory(device, buffer, static_cast<VkDeviceMemory>(mem_block.internal_handle), mem_block.offset);

	// [关键] 我们返回的 MemoryBlock 需要携带 VkBuffer 句柄给 Renderer 使用
	// 所以我们把 internal_handle 替换为 VkBuffer
	MemoryBlock result = mem_block;
	result.internal_handle = buffer; // 偷梁换柱：现在 handle 是 VkBuffer

	// [FIX] Record Memory Mapping for Destruction
	buffer_memory_map[buffer] = static_cast<VkDeviceMemory>(mem_block.internal_handle);

	return result;
}

void VulkanRHI::destroy_buffer(bud::graphics::MemoryBlock block) {
	if (!block.is_valid()) return;
	VkBuffer buffer = static_cast<VkBuffer>(block.internal_handle);
	if (!buffer) return;

	// Free Memory
	if (buffer_memory_map.contains(buffer)) {
		VkDeviceMemory mem = buffer_memory_map[buffer];
		// Reconstruct Original Block for Allocator (needs internal_handle = VkDeviceMemory)
		bud::graphics::MemoryBlock mem_block = block;
		mem_block.internal_handle = mem;

		memory_allocator->free(mem_block);
		buffer_memory_map.erase(buffer);
	}

	// Destroy Buffer
	vkDestroyBuffer(device, buffer, nullptr);
}

// Helper to create shader module
VkShaderModule create_shader_module(VkDevice device, const std::vector<char>& code) {
	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

	VkShaderModule shaderModule;
	if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
		throw std::runtime_error("failed to create shader module!");
	}
	return shaderModule;
}

void* VulkanRHI::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
	VkPushConstantRange push_constant;
	push_constant.offset = 0;
	push_constant.size = 256; // Enough for standard matrices
	push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 0; 
	
	// 使用全局 Descriptor Set Layout
	std::vector<VkDescriptorSetLayout> setLayouts = { global_set_layout };
	
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &push_constant;

	VkPipelineLayout pipelineLayout;
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create pipeline layout!");
	}

	VkShaderModule vertModule = create_shader_module(device, desc.vs.code);
	VkShaderModule fragModule = create_shader_module(device, desc.fs.code);

	PipelineKey key{};
	key.vert_shader = vertModule;
	key.frag_shader = fragModule;
	key.render_pass = VK_NULL_HANDLE;
	key.depth_test = desc.depth_test;
	key.depth_write = desc.depth_write;

	switch (desc.cull_mode) {
	case bud::graphics::CullMode::None: key.cull_mode = VK_CULL_MODE_NONE; break;
	case bud::graphics::CullMode::Front: key.cull_mode = VK_CULL_MODE_FRONT_BIT; break;
	case bud::graphics::CullMode::Back: key.cull_mode = VK_CULL_MODE_BACK_BIT; break;
	default: key.cull_mode = VK_CULL_MODE_BACK_BIT; break;
	}

	key.color_format = to_vk_format(desc.color_attachment_format);

	bool is_depth_only = (desc.color_attachment_format == bud::graphics::TextureFormat::Undefined);

	VkPipeline pipeline = pipeline_cache->get_pipeline(key, pipelineLayout, is_depth_only);

	vkDestroyShaderModule(device, vertModule, nullptr);
	vkDestroyShaderModule(device, fragModule, nullptr);

	VulkanPipelineObject* pipeObj = new VulkanPipelineObject{ pipeline, pipelineLayout };
	
	created_layouts.push_back(pipelineLayout);
	
	return pipeObj;
}

// Helpers are implemented at the end of the file.

// --- Frame Control ---

CommandHandle VulkanRHI::begin_frame() {
	vkWaitForFences(device, 1, &frames[current_frame].in_flight_fence, VK_TRUE, UINT64_MAX);

	VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frames[current_frame].image_available_semaphore, VK_NULL_HANDLE, &current_image_index);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		int w = 0;
		int h = 0;
		if (platform_window) {
			platform_window->get_size_in_pixels(w, h);
		}
		resize_swapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
		return nullptr;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	vkResetFences(device, 1, &frames[current_frame].in_flight_fence);

	// 通知分配器新的一帧开始了 (重置 Linear Allocator)
	memory_allocator->on_frame_begin(current_frame);
	descriptor_allocators[current_frame].reset_frame();

	// 使用 init 时分配的持久化 Global Descriptor Set，仅更新 UBO 绑定
	// Bindless Textures 是持久化的，不需要每一帧重绑一次，否则会丢失状态导致 GPU Hang

	// 更新 Descriptor Set 指向当前帧的 UBO
	DescriptorWriter writer;
	writer.write_buffer(0, frames[current_frame].uniform_buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);

	vkResetCommandBuffer(frames[current_frame].main_command_buffer, 0);

	VkCommandBufferBeginInfo begin_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	if (vkBeginCommandBuffer(frames[current_frame].main_command_buffer, &begin_info) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	return frames[current_frame].main_command_buffer;
}

void VulkanRHI::end_frame(CommandHandle cmd) {
	VkCommandBuffer command_buffer = static_cast<VkCommandBuffer>(cmd);

	if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS)
		throw std::runtime_error("failed to record command buffer!");

	VkSubmitInfo submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkSemaphore wait_semaphores[] = { frames[current_frame].image_available_semaphore };
	VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT };
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = wait_semaphores;
	submit_info.pWaitDstStageMask = wait_stages;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffer;
	VkSemaphore signal_semaphores[] = { render_finished_semaphores[current_image_index] };
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = signal_semaphores;

	VkResult submit_result = vkQueueSubmit(graphics_queue, 1, &submit_info, frames[current_frame].in_flight_fence);
	if (submit_result != VK_SUCCESS) {
		throw std::runtime_error(std::format("failed to submit draw command buffer! Error: {}", (int)submit_result));
	}

	VkPresentInfoKHR present_info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = signal_semaphores;
	VkSwapchainKHR swap_chains[] = { swapchain };
	present_info.swapchainCount = 1;
	present_info.pSwapchains = swap_chains;
	present_info.pImageIndices = &current_image_index;

	vkQueuePresentKHR(present_queue, &present_info);
	current_frame = (current_frame + 1) % max_frames_in_flight;
}


void VulkanRHI::resize_swapchain(uint32_t width, uint32_t height) {
	if (!device || !platform_window) {
		return;
	}

	if (width == 0 || height == 0) {
		return;
	}

	vkDeviceWaitIdle(device);

	for (auto imageView : swapchain_image_views) {
		vkDestroyImageView(device, imageView, nullptr);
	}
	swapchain_image_views.clear();
	swapchain_textures_wrappers.clear();

	for (auto semaphore : render_finished_semaphores) {
		vkDestroySemaphore(device, semaphore, nullptr);
	}
	render_finished_semaphores.clear();

	if (swapchain) {
		vkDestroySwapchainKHR(device, swapchain, nullptr);
		swapchain = VK_NULL_HANDLE;
	}

	create_swapchain(platform_window);
	create_image_views();

	VkSemaphoreCreateInfo semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	render_finished_semaphores.resize(swapchain_images.size());
	for (size_t i = 0; i < swapchain_images.size(); ++i) {
		if (vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished_semaphores[i]) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create render finished semaphores!");
		}
	}

	current_image_index = 0;
}


// --- Atomic Commands (核心指令集) ---

void VulkanRHI::cmd_begin_render_pass(CommandHandle cmd, const RenderPassBeginInfo& info) {
	VkCommandBuffer vk_cmd = static_cast<VkCommandBuffer>(cmd);

	// 使用 Vulkan 1.3 Dynamic Rendering
	// 不需要 VkRenderPass 和 VkFramebuffer 对象
	VkRenderingInfo rendering_info{ VK_STRUCTURE_TYPE_RENDERING_INFO };

	// 默认取第一个附件的大小
	if (!info.color_attachments.empty()) {
		rendering_info.renderArea = { {0, 0}, {info.color_attachments[0]->width, info.color_attachments[0]->height} };
	}
	else if (info.depth_attachment) {
		rendering_info.renderArea = { {0, 0}, {info.depth_attachment->width, info.depth_attachment->height} };
	}
	rendering_info.layerCount = info.layer_count;

	// 准备 Color Attachments
	std::vector<VkRenderingAttachmentInfo> color_attachments;
	for (auto* tex : info.color_attachments) {
		auto vk_tex = static_cast<VulkanTexture*>(tex);
		VkRenderingAttachmentInfo attach{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		// [CSM] Support rendering to specific layer. Always use layer_views if available for array textures.
		if (!vk_tex->layer_views.empty()) {
			attach.imageView = vk_tex->layer_views[info.base_array_layer];
		} else {
			attach.imageView = vk_tex->view;
		}
		attach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // Graph 必须保证这一点
		attach.loadOp = info.clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attach.clearValue.color = { info.clear_color_value.r, info.clear_color_value.g, info.clear_color_value.b, info.clear_color_value.a };
		color_attachments.push_back(attach);
	}
	rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
	rendering_info.pColorAttachments = color_attachments.data();

	// 准备 Depth Attachment
	VkRenderingAttachmentInfo depth_attach{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	if (info.depth_attachment) {
		auto vk_depth = static_cast<VulkanTexture*>(info.depth_attachment);
		// [CSM] Support rendering to specific layer. Always use layer_views if available for array textures.
		if (!vk_depth->layer_views.empty()) {
			depth_attach.imageView = vk_depth->layer_views[info.base_array_layer];
		} else {
			depth_attach.imageView = vk_depth->view;
		}
		depth_attach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth_attach.loadOp = info.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		depth_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depth_attach.clearValue.depthStencil = { 1.0f, 0 };
		rendering_info.pDepthAttachment = &depth_attach;
	}

	vkCmdBeginRendering(vk_cmd, &rendering_info);
}

void VulkanRHI::cmd_end_render_pass(CommandHandle cmd) {
	vkCmdEndRendering(static_cast<VkCommandBuffer>(cmd));
}

void VulkanRHI::cmd_bind_pipeline(CommandHandle cmd, void* pipeline) {
	auto pipeObj = static_cast<VulkanPipelineObject*>(pipeline);
	vkCmdBindPipeline(static_cast<VkCommandBuffer>(cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeObj->pipeline);
}

void VulkanRHI::cmd_bind_vertex_buffer(CommandHandle cmd, void* buffer) {
	VkBuffer vk_buf = static_cast<VkBuffer>(buffer);
	VkDeviceSize offsets[] = { 0 };
	vkCmdBindVertexBuffers(static_cast<VkCommandBuffer>(cmd), 0, 1, &vk_buf, offsets);
}

void VulkanRHI::cmd_bind_index_buffer(CommandHandle cmd, void* buffer) {
	vkCmdBindIndexBuffer(static_cast<VkCommandBuffer>(cmd), static_cast<VkBuffer>(buffer), 0, VK_INDEX_TYPE_UINT32);
}

void VulkanRHI::cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
	vkCmdDraw(static_cast<VkCommandBuffer>(cmd), vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanRHI::cmd_draw_indexed(CommandHandle cmd, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
	vkCmdDrawIndexed(static_cast<VkCommandBuffer>(cmd), index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VulkanRHI::cmd_push_constants(CommandHandle cmd, void* pipeline_layout, uint32_t size, const void* data) {
	auto pipeObj = static_cast<VulkanPipelineObject*>(pipeline_layout);
	vkCmdPushConstants(static_cast<VkCommandBuffer>(cmd), pipeObj->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void VulkanRHI::cmd_set_viewport(CommandHandle cmd, float width, float height) {
	VkViewport viewport{ 0, 0, width, height, 0.0f, 1.0f };
	vkCmdSetViewport(static_cast<VkCommandBuffer>(cmd), 0, 1, &viewport);
}

void VulkanRHI::cmd_set_scissor(CommandHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) {
	VkRect2D scissor{ {x, y}, {width, height} };
	vkCmdSetScissor(static_cast<VkCommandBuffer>(cmd), 0, 1, &scissor);
}

void VulkanRHI::cmd_set_scissor(CommandHandle cmd, uint32_t width, uint32_t height) {
	cmd_set_scissor(cmd, 0, 0, width, height);
}

void VulkanRHI::cmd_set_depth_bias(CommandHandle cmd, float constant, float clamp, float slope) {
	vkCmdSetDepthBias(static_cast<VkCommandBuffer>(cmd), constant, clamp, slope);
}

void VulkanRHI::cmd_bind_descriptor_set(CommandHandle cmd, void* pipeline, uint32_t set_index) {
	auto pipeObj = static_cast<VulkanPipelineObject*>(pipeline);
	auto& frame = frames[current_frame];
	
	vkCmdBindDescriptorSets(
		static_cast<VkCommandBuffer>(cmd),
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeObj->layout,
		set_index,  // first set
		1,          // descriptor set count
		&frame.global_descriptor_set,
		0, nullptr  // dynamic offsets
	);
}

void VulkanRHI::resource_barrier(CommandHandle cmd, bud::graphics::Texture* texture, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) {
	auto vk_tex = static_cast<VulkanTexture*>(texture);
	auto src = get_vk_transition(old_state);
	auto dst = get_vk_transition(new_state);

	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.oldLayout = src.layout;
	barrier.newLayout = dst.layout;
	barrier.srcAccessMask = src.access;
	barrier.dstAccessMask = dst.access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = vk_tex->image;
	bool is_depth = (texture->format == TextureFormat::D32_FLOAT || 
					 texture->format == TextureFormat::D24_UNORM_S8_UINT);

	barrier.subresourceRange.aspectMask = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	if (is_depth && texture->format == TextureFormat::D24_UNORM_S8_UINT) barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = texture->mips > 0 ? texture->mips : 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = texture->array_layers > 0 ? texture->array_layers : 1;

	// std::println("[Barrier] Image {} ({}x{} @ {}L): {} -> {}", (void*)vk_tex->image, texture->width, texture->height, barrier.subresourceRange.layerCount, (int)src.layout, (int)dst.layout);

	vkCmdPipelineBarrier(static_cast<VkCommandBuffer>(cmd), src.stage, dst.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

Texture* VulkanRHI::get_current_swapchain_texture() {
	if (current_image_index >= swapchain_textures_wrappers.size())
		return nullptr;

	return &swapchain_textures_wrappers[current_image_index];
}

uint32_t VulkanRHI::get_current_image_index() {
	return current_image_index;
}


void VulkanRHI::set_render_config(const RenderConfig& new_render_config) {
	render_config = new_render_config;
}

void VulkanRHI::reload_shaders_async() {}


void VulkanRHI::load_model_async(const std::string& filepath) {}


// --- Boilerplate (Device Creation & Utils) ---

void VulkanRHI::create_instance(VkInstance& vk_instance, bool enable_validation) {
	enable_validation_layers = enable_validation;
	VkApplicationInfo app_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName = "Bud Engine";
	app_info.apiVersion = VK_API_VERSION_1_3;

	VkInstanceCreateInfo create_info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	create_info.pApplicationInfo = &app_info;

	uint32_t count = 0;
	auto extensions = SDL_Vulkan_GetInstanceExtensions(&count);
	std::vector<const char*> exts(extensions, extensions + count);
	if (enable_validation) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

	create_info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
	create_info.ppEnabledExtensionNames = exts.data();
	if (enable_validation) {
		create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
		create_info.ppEnabledLayerNames = validation_layers.data();
	}

	if (vkCreateInstance(&create_info, nullptr, &vk_instance) != VK_SUCCESS) throw std::runtime_error("Instance creation failed");
}

void VulkanRHI::create_surface(bud::platform::Window* window) {

	window->create_surface(instance, surface);
	//if (!SDL_Vulkan_CreateSurface(window->get_sdl_window(), instance, nullptr, &surface)) {
	//	throw std::runtime_error("Failed to create Window Surface!");
	//}
}

void VulkanRHI::pick_physical_device() {
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

void VulkanRHI::create_logical_device(bool enable_validation) {
	QueueFamilyIndices indices = find_queue_families(physical_device);
	std::vector<VkDeviceQueueCreateInfo> queue_infos;
	std::set<uint32_t> unique_families = { indices.graphics_family.value(), indices.present_family.value() };
	float priority = 1.0f;
	for (uint32_t family : unique_families) {
		VkDeviceQueueCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		info.queueFamilyIndex = family;
		info.queueCount = 1;
		info.pQueuePriorities = &priority;
		queue_infos.push_back(info);
	}

	VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	features13.pNext = nullptr; 
	features13.dynamicRendering = VK_TRUE;
	features13.synchronization2 = VK_TRUE;

	VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features12.pNext = &features13;
	features12.descriptorBindingPartiallyBound = VK_TRUE;
	features12.runtimeDescriptorArray = VK_TRUE;
	features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
	features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

	VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
	features11.pNext = &features12;

	// 使用 VkPhysicalDeviceFeatures2 整合所有 Features
	VkPhysicalDeviceFeatures2 device_features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	device_features2.pNext = &features11;
	device_features2.features.samplerAnisotropy = VK_TRUE;

	VkDeviceCreateInfo create_info{};
	create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create_info.pNext = &device_features2;
	create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
	create_info.pQueueCreateInfos = queue_infos.data();
	create_info.pEnabledFeatures = nullptr;

	create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
	create_info.ppEnabledExtensionNames = device_extensions.data();

	if (physical_device == nullptr) throw std::runtime_error("Physical device is NULL!");

	VkResult res = vkCreateDevice(physical_device, &create_info, nullptr, &device);
	if (res != VK_SUCCESS) {
		std::println(stderr, "[Vulkan] vkCreateDevice failed with code: {}", (int)res);
		throw std::runtime_error("Device creation failed");
	}
	std::println("[Vulkan] Logical Device created successfully.");

	vkGetDeviceQueue(device, indices.graphics_family.value(), 0, &graphics_queue);
	vkGetDeviceQueue(device, indices.present_family.value(), 0, &present_queue);
}

void VulkanRHI::create_swapchain(bud::platform::Window* window) {
	SwapChainSupportDetails swapchain_support = query_swapchain_support(physical_device);
	VkSurfaceFormatKHR surface_format = choose_swap_surface_format(swapchain_support.formats);
	VkPresentModeKHR present_mode = choose_swap_present_mode(swapchain_support.present_modes);
	VkExtent2D extent = choose_swap_extent(swapchain_support.capabilities, window);

	auto image_count = swapchain_support.capabilities.minImageCount + 1;
	if (swapchain_support.capabilities.maxImageCount > 0 && image_count > swapchain_support.capabilities.maxImageCount) {
		image_count = swapchain_support.capabilities.maxImageCount;
	}

	frames.resize(max_frames_in_flight);

	VkSwapchainCreateInfoKHR create_info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	create_info.surface = surface;
	create_info.minImageCount = image_count;
	create_info.imageFormat = surface_format.format;
	create_info.imageColorSpace = surface_format.colorSpace;
	create_info.imageExtent = extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; // 允许作为 Blit 目标

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

	if (vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain) != VK_SUCCESS) throw std::runtime_error("Failed to create swapchain!");

	vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
	swapchain_images.resize(image_count);
	vkGetSwapchainImagesKHR(device, swapchain, &image_count, swapchain_images.data());

	swapchain_image_format = surface_format.format;
	swapchain_extent = extent;
}

void VulkanRHI::create_image_views() {
	swapchain_image_views.resize(swapchain_images.size());
	for (size_t i = 0; i < swapchain_images.size(); i++) {
		VkImageViewCreateInfo create_info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		create_info.image = swapchain_images[i];
		create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		create_info.format = swapchain_image_format;
		create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		create_info.subresourceRange.baseMipLevel = 0;
		create_info.subresourceRange.levelCount = 1;
		create_info.subresourceRange.baseArrayLayer = 0;
		create_info.subresourceRange.layerCount = 1;

		if (vkCreateImageView(device, &create_info, nullptr, &swapchain_image_views[i]) != VK_SUCCESS) throw std::runtime_error("Failed to create image views!");
	}

	// 包装为 Engine Texture Handle
	swapchain_textures_wrappers.resize(swapchain_images.size());
	for (size_t i = 0; i < swapchain_images.size(); i++) {
		swapchain_textures_wrappers[i].image = swapchain_images[i];
		swapchain_textures_wrappers[i].view = swapchain_image_views[i];
		swapchain_textures_wrappers[i].width = swapchain_extent.width;
		swapchain_textures_wrappers[i].height = swapchain_extent.height;
		swapchain_textures_wrappers[i].mips = 1;
		swapchain_textures_wrappers[i].array_layers = 1;
		swapchain_textures_wrappers[i].format = TextureFormat::BGRA8_UNORM;
		swapchain_textures_wrappers[i].memory = VK_NULL_HANDLE; // Swapchain image memory is managed by driver
	}
}

void VulkanRHI::create_command_pool() {
	QueueFamilyIndices queue_family_indices = find_queue_families(physical_device);

	for (int i = 0; i < max_frames_in_flight; i++) {
		VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = queue_family_indices.graphics_family.value();

		if (vkCreateCommandPool(device, &pool_info, nullptr, &frames[i].main_command_pool) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create main command pool!");
		}
	}
}

void VulkanRHI::create_command_buffer() {
	for (int i = 0; i < max_frames_in_flight; i++) {
		VkCommandBufferAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		alloc_info.commandPool = frames[i].main_command_pool;
		alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc_info.commandBufferCount = 1;

		if (vkAllocateCommandBuffers(device, &alloc_info, &frames[i].main_command_buffer) != VK_SUCCESS) {
			throw std::runtime_error("Failed to allocate command buffers!");
		}
	}
}

void VulkanRHI::create_sync_objects() {
	VkSemaphoreCreateInfo semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (int i = 0; i < max_frames_in_flight; i++) {
		if (vkCreateSemaphore(device, &semaphore_info, nullptr, &frames[i].image_available_semaphore) != VK_SUCCESS ||
			vkCreateFence(device, &fence_info, nullptr, &frames[i].in_flight_fence) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create synchronization objects for a frame!");
		}
	}

	render_finished_semaphores.resize(swapchain_images.size());
	for (size_t i = 0; i < swapchain_images.size(); ++i) {
		if (vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished_semaphores[i]) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create render finished semaphores!");
		}
	}
}


VkCommandBuffer VulkanRHI::begin_single_time_commands() {
	VkCommandBufferAllocateInfo alloc_info{};
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	// 使用第0帧的命令池（注意：这在多线程渲染时可能不安全，但在初始化阶段是安全的）	
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

void VulkanRHI::end_single_time_commands(VkCommandBuffer command_buffer) {
	vkEndCommandBuffer(command_buffer);

	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffer;

	// 提交并等待队列空闲（同步阻塞）
	if (vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
		std::println(stderr, "[Vulkan] Failed to submit single time command!");
	}
	vkQueueWaitIdle(graphics_queue);

	vkFreeCommandBuffers(device, frames[0].main_command_pool, 1, &command_buffer);
}


QueueFamilyIndices VulkanRHI::find_queue_families(VkPhysicalDevice device) {
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

SwapChainSupportDetails VulkanRHI::query_swapchain_support(VkPhysicalDevice device) {
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

VkSurfaceFormatKHR VulkanRHI::choose_swap_surface_format(const std::vector<VkSurfaceFormatKHR>& available_formats) {
	for (const auto& available_format : available_formats) {
		if (available_format.format == VK_FORMAT_B8G8R8A8_SRGB && available_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return available_format;
		}
	}
	return available_formats[0];
}

VkPresentModeKHR VulkanRHI::choose_swap_present_mode(const std::vector<VkPresentModeKHR>& available_present_modes) {
	for (const auto& available_present_mode : available_present_modes) {
		if (available_present_mode == VK_PRESENT_MODE_MAILBOX_KHR) return available_present_mode;
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRHI::choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, bud::platform::Window* window) {
	if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
		return capabilities.currentExtent;
	}

	int width = 0;
	int height = 0;
	if (window) {
		window->get_size_in_pixels(width, height);
	}

	VkExtent2D actual_extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
	actual_extent.width = std::clamp(actual_extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
	actual_extent.height = std::clamp(actual_extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
	return actual_extent;
}

// --- Debug Utils ---

VkResult VulkanRHI::create_debug_utils_messenger_ext(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if (func != nullptr) return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void VulkanRHI::destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if (func != nullptr) func(instance, debugMessenger, pAllocator);
}

void VulkanRHI::setup_debug_messenger(bool enable) {
	if (!enable) return;
	VkDebugUtilsMessengerCreateInfoEXT create_info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
	create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	create_info.pfnUserCallback = debug_callback;

	if (create_debug_utils_messenger_ext(instance, &create_info, nullptr, &debug_messenger) != VK_SUCCESS) {
		throw std::runtime_error("Failed to set up debug messenger!");
	}

	if (enable) {
		fpCmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
		fpCmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
		fpSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");
	}
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRHI::debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		std::println(stderr, "[Validation Layer]: {}", pCallbackData->pMessage);
	}
	return VK_FALSE;
}
void VulkanRHI::transition_image_layout_immediate(VkImage image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout) {
	VkCommandBuffer commandBuffer = this->begin_single_time_commands();

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = get_aspect_flags(format);
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = 0;
		sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}

	vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	this->end_single_time_commands(commandBuffer);
}

void VulkanRHI::copy_buffer_to_image(VkImage image, VkBuffer buffer, uint32_t width, uint32_t height) {
	VkCommandBuffer commandBuffer = this->begin_single_time_commands();

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

	vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	this->end_single_time_commands(commandBuffer);
}

void VulkanRHI::copy_buffer_immediate(bud::graphics::MemoryBlock src, bud::graphics::MemoryBlock dst, uint64_t size) {
	VkCommandBuffer cmd = this->begin_single_time_commands();

	VkBufferCopy copy_region{};
	copy_region.srcOffset = 0; 
	copy_region.dstOffset = 0;
	copy_region.size = size;

	vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(src.internal_handle), static_cast<VkBuffer>(dst.internal_handle), 1, &copy_region);

	this->end_single_time_commands(cmd);
}

	// [CSM] Shadow Caching Copy
	void VulkanRHI::cmd_copy_image(CommandHandle cmd, Texture* src, Texture* dst) {
		VulkanTexture* vk_src = static_cast<VulkanTexture*>(src);
		VulkanTexture* vk_dst = static_cast<VulkanTexture*>(dst);

		VkImageCopy region{};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; // Assuming Depth for ShadowMap
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount = src->array_layers;
		region.srcSubresource.mipLevel = 0;
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount = dst->array_layers;
		region.dstSubresource.mipLevel = 0;
		region.extent.width = src->width;
		region.extent.height = src->height;
		region.extent.depth = 1;

		// Note: We assume caller handles barriers (TRANSFER_SRC / TRANSFER_DST) via RenderGraph
		// But for safety, we could inject them here. RenderGraph logic is preferred.
		
		vkCmdCopyImage(static_cast<VkCommandBuffer>(cmd), 
			vk_src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			vk_dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &region);
	}

bud::graphics::MemoryBlock VulkanRHI::create_upload_buffer(uint64_t size) {
	VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	buffer_info.size = size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer buffer;
	if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create Upload buffer!");
	}

	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(device, buffer, &mem_reqs);

	// [FIX] Align to nonCoherentAtomSize (typically 64-256) to ensure vkFlushMappedMemoryRanges is valid
	// Using 256 as a safe conservative default.
	VkDeviceSize align = std::max(mem_reqs.alignment, (VkDeviceSize)256); 

	bud::graphics::MemoryBlock mem_block = memory_allocator->alloc_staging(mem_reqs.size, align);
	vkBindBufferMemory(device, buffer, static_cast<VkDeviceMemory>(mem_block.internal_handle), mem_block.offset);

	bud::graphics::MemoryBlock result = mem_block;
	result.internal_handle = buffer;
	
	// [FIX] Track staging buffers for proper cleanup
	buffer_memory_map[buffer] = static_cast<VkDeviceMemory>(mem_block.internal_handle);
	
	return result;
}

void VulkanRHI::generate_mipmaps(VkImage image, VkFormat format, int32_t texWidth, int32_t texHeight, uint32_t mipLevels) {
	// Check if image format supports linear blitting
	VkFormatProperties formatProperties;
	vkGetPhysicalDeviceFormatProperties(physical_device, format, &formatProperties);

	if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
		throw std::runtime_error("texture image format does not support linear blitting!");
	}

	VkCommandBuffer commandBuffer = begin_single_time_commands();

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.image = image;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	int32_t mipWidth = texWidth;
	int32_t mipHeight = texHeight;

	for (uint32_t i = 1; i < mipLevels; i++) {
		// 1. Transition Level i-1 to TRANSFER_SRC_OPTIMAL
		barrier.subresourceRange.baseMipLevel = i - 1;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		vkCmdPipelineBarrier(commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, nullptr,
			0, nullptr,
			1, &barrier);

		// 1.5 Transition Level i to TRANSFER_DST_OPTIMAL (from UNDEFINED)
		// We can reuse the barrier struct but be careful
		VkImageMemoryBarrier dstBarrier = barrier;
		dstBarrier.subresourceRange.baseMipLevel = i;
		dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		dstBarrier.srcAccessMask = 0;
		dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		
		vkCmdPipelineBarrier(commandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, nullptr,
			0, nullptr,
			1, &dstBarrier);

		// 2. Blit
		VkImageBlit blit{};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = i - 1;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount = 1;

		blit.dstOffsets[0] = { 0, 0, 0 };
		blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount = 1;

		vkCmdBlitImage(commandBuffer,
			image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit,
			VK_FILTER_LINEAR);

		// 3. Transition Level i-1 to SHADER_READ_ONLY_OPTIMAL
		barrier.subresourceRange.baseMipLevel = i - 1;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
			0, nullptr,
			0, nullptr,
			1, &barrier);

		if (mipWidth > 1) mipWidth /= 2;
		if (mipHeight > 1) mipHeight /= 2;
	}

	// 4. Transition Final Level to SHADER_READ_ONLY_OPTIMAL
	barrier.subresourceRange.baseMipLevel = mipLevels - 1;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	vkCmdPipelineBarrier(commandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		0, nullptr,
		0, nullptr,
		1, &barrier);

	end_single_time_commands(commandBuffer);
}

bud::graphics::Texture* VulkanRHI::create_texture(const bud::graphics::TextureDesc& desc, const void* initial_data, uint64_t size) {
	auto tex = dynamic_cast<VulkanTexture*>(resource_pool->acquire_texture(desc));
	
	tex->width = desc.width;
	tex->height = desc.height;
	tex->format = desc.format;
	tex->mips = desc.mips;
	tex->array_layers = desc.array_layers;

	if (initial_data && size > 0) {
		bud::graphics::MemoryBlock staging = this->create_upload_buffer(size);
		std::memcpy(staging.mapped_ptr, initial_data, size);

		this->transition_image_layout_immediate(tex->image, to_vk_format(desc.format), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		
		// [FIX] Explicitly flush memory to guarantee GPU visibility (fixes corruption on some vendors/drivers if Coherent bit is missing/ignored)
		if (buffer_memory_map.count(static_cast<VkBuffer>(staging.internal_handle))) {
			VkMappedMemoryRange range{};
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = buffer_memory_map[static_cast<VkBuffer>(staging.internal_handle)];
			range.offset = staging.offset;
			range.size = VK_WHOLE_SIZE; // Flush everything from this offset to end (safe for linear allocator)
			
			// Handle alignment for non-coherent atom size (simplification: assume offset is aligned, length is whole)
			// Ideally we query physicalDeviceProperties.limits.nonCoherentAtomSize
			vkFlushMappedMemoryRanges(device, 1, &range); 
		}

		this->copy_buffer_to_image(tex->image, static_cast<VkBuffer>(staging.internal_handle), desc.width, desc.height);
		
		if (desc.mips > 1) {
			//std::println("[Texture] Generating {} mip levels for {}x{} texture", desc.mips, desc.width, desc.height);
			this->generate_mipmaps(tex->image, to_vk_format(desc.format), desc.width, desc.height, desc.mips);
		} else {
			this->transition_image_layout_immediate(tex->image, to_vk_format(desc.format), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		
		this->destroy_buffer(staging);
	}

	tex->sampler = default_sampler;
	return tex;
}

void VulkanRHI::update_bindless_texture(uint32_t index, bud::graphics::Texture* texture) {
	if (!texture) return;
	auto vk_tex = static_cast<VulkanTexture*>(texture);

	for (int i = 0; i < max_frames_in_flight; i++) {
		if (frames[i].global_descriptor_set == VK_NULL_HANDLE) { 
			std::println(stderr, "[Vulkan] ERROR: global_descriptor_set at frame {} is NULL!", i);
			continue; 
		}

		DescriptorWriter writer;
		writer.write_image(1, index, vk_tex->view, vk_tex->sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		writer.update_set(device, frames[i].global_descriptor_set);
	}
}

void VulkanRHI::update_global_uniforms(uint32_t image_index, const SceneView& scene_view) {
	UniformBufferObject ubo{};
	ubo.view = scene_view.view_matrix;
	ubo.proj = scene_view.proj_matrix;

	// [CSM] Populate cascade matrices and depths
	for (uint32_t i = 0; i < MAX_CASCADES; ++i) {
		ubo.cascade_view_proj[i] = scene_view.cascade_view_proj_matrices[i];
	}
	ubo.cascade_split_depths = bud::math::vec4(
		scene_view.cascade_split_depths[0],
		scene_view.cascade_split_depths[1],
		scene_view.cascade_split_depths[2],
		scene_view.cascade_split_depths[3]
	);
	ubo.cascade_count = 4; // Or from config if dynamic

	ubo.cam_pos = scene_view.camera_position;
	ubo.light_dir = scene_view.light_dir;
	ubo.light_color = scene_view.light_color;
	ubo.light_intensity = scene_view.light_intensity;
	ubo.ambient_strength = scene_view.ambient_strength;
	ubo.debug_cascades = render_config.debug_cascades ? 1 : 0;

	if (frames[current_frame].uniform_mapped) {
		std::memcpy(frames[current_frame].uniform_mapped, &ubo, sizeof(UniformBufferObject));
	}
}

void VulkanRHI::update_global_shadow_map(bud::graphics::Texture* texture) {
	if (!texture) return;
	VulkanTexture* vk_tex = static_cast<VulkanTexture*>(texture);

	for (int i = 0; i < max_frames_in_flight; i++) {
		DescriptorWriter writer;
		writer.write_image(2, 0, vk_tex->view, shadow_sampler, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		writer.update_set(device, frames[i].global_descriptor_set);
	}
}

bud::graphics::Texture* VulkanRHI::get_fallback_texture() {
	return fallback_texture_ptr;
}


VkVertexInputBindingDescription Vertex::get_binding_description() {
	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.binding = 0;
	bindingDescription.stride = sizeof(Vertex);
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	return bindingDescription;
}

std::vector<VkVertexInputAttributeDescription> Vertex::get_attribute_descriptions() {
	std::vector<VkVertexInputAttributeDescription> attributeDescriptions(5);

	// Position (Location 0) -> Offset 0
	attributeDescriptions[0].binding = 0;
	attributeDescriptions[0].location = 0;
	attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[0].offset = offsetof(Vertex, pos);

	// Color (Location 1) -> Offset 12 
	attributeDescriptions[1].binding = 0;
	attributeDescriptions[1].location = 1;
	attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[1].offset = offsetof(Vertex, color);

	// Normal (Location 2) -> Offset 24
	attributeDescriptions[2].binding = 0;
	attributeDescriptions[2].location = 2;
	attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[2].offset = offsetof(Vertex, normal);

	// UV (Location 3) -> Offset 36
	attributeDescriptions[3].binding = 0;
	attributeDescriptions[3].location = 3;
	attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
	attributeDescriptions[3].offset = offsetof(Vertex, uv);

	// TexIndex (Location 4) -> Offset 44
	//attributeDescriptions[4].binding = 0;
	//attributeDescriptions[4].location = 4;
	//attributeDescriptions[4].format = VK_FORMAT_R32_SFLOAT;
	//attributeDescriptions[4].offset = offsetof(Vertex, tex_index);

	return attributeDescriptions;
}


void VulkanRHI::cmd_begin_debug_label(CommandHandle cmd, const std::string& name, float r, float g, float b) {
	if (fpCmdBeginDebugUtilsLabelEXT) {
		VkDebugUtilsLabelEXT labelInfo{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
		labelInfo.pLabelName = name.c_str();
		labelInfo.color[0] = r;
		labelInfo.color[1] = g;
		labelInfo.color[2] = b;
		labelInfo.color[3] = 1.0f;

		fpCmdBeginDebugUtilsLabelEXT(static_cast<VkCommandBuffer>(cmd), &labelInfo);
	}
}

void VulkanRHI::cmd_end_debug_label(CommandHandle cmd) {
	if (fpCmdEndDebugUtilsLabelEXT) {
		fpCmdEndDebugUtilsLabelEXT(static_cast<VkCommandBuffer>(cmd));
	}
}

void VulkanRHI::set_object_debug_name(uint64_t object_handle, ObjectType object_type, const std::string& name) {
	if (fpSetDebugUtilsObjectNameEXT) {
		VkDebugUtilsObjectNameInfoEXT nameInfo{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
		nameInfo.objectType = to_vk_object_type(object_type);
		nameInfo.objectHandle = object_handle;
		nameInfo.pObjectName = name.c_str();
		fpSetDebugUtilsObjectNameEXT(device, &nameInfo);
	}
}

void VulkanRHI::set_debug_name(Texture* texture, ObjectType object_type, const std::string& name) {
	if (!texture) return;

	auto vk_tex = dynamic_cast<VulkanTexture*>(texture);

	// 给 Image 命名
	set_object_debug_name(reinterpret_cast<uint64_t>(vk_tex->image), object_type, name);

	if (vk_tex->view) {
		set_object_debug_name((uint64_t)vk_tex->view, ObjectType::ImageView, name + "_View");
	}
}

void VulkanRHI::set_debug_name(const MemoryBlock& buffer, ObjectType object_type, const std::string& name) {
	if (!buffer.is_valid()) return;

	auto vk_buf = static_cast<VkBuffer>(buffer.internal_handle);

	set_object_debug_name(reinterpret_cast<uint64_t>(vk_buf), object_type, name);
}

void VulkanRHI::set_debug_name(CommandHandle cmd, ObjectType object_type, const std::string& name) {
	auto vk_cmd = static_cast<VkCommandBuffer>(cmd);
	set_object_debug_name(reinterpret_cast<uint64_t>(vk_cmd), object_type, name);
}

