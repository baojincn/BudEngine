#include <vector>
#include <string>
#include <iostream>
#include <optional>
#include <set>
#include <algorithm>
#include <limits>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <format>

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "src/graphics/vulkan/bud.graphics.vulkan.hpp"

#include "src/core/bud.math.hpp"
#include "src/platform/bud.platform.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.utils.hpp"
#include "src/graphics/vulkan/bud.vulkan.sync2.hpp"

#ifdef BUD_ENABLE_AFTERMATH
#include <GFSDK_Aftermath.h>
#include <GFSDK_Aftermath_GpuCrashDump.h>
#endif

using namespace bud::graphics;
using namespace bud::graphics::vulkan;


PFN_vkCmdBeginDebugUtilsLabelEXT fpCmdBeginDebugUtilsLabelEXT = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT fpCmdEndDebugUtilsLabelEXT = nullptr;
PFN_vkSetDebugUtilsObjectNameEXT fpSetDebugUtilsObjectNameEXT = nullptr;


struct VulkanPipelineObject {
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkPipelineBindPoint bind_point;
};

#ifdef BUD_ENABLE_AFTERMATH
std::mutex aftermath_mutex;
std::once_flag aftermath_enable_once;

void gpu_crash_dump_callback(const void* pGpuCrashDump, const uint32_t gpuCrashDumpSize, void* /*pUserData*/) {
	std::lock_guard lock(aftermath_mutex);

	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dump_dir = fs::current_path(ec) / "aftermath_dumps";
	fs::create_directories(dump_dir, ec);

	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
	fs::path dump_path = dump_dir / ("gpu_crash_" + std::to_string(ns) + ".nv-gpudmp");

	std::ofstream file(dump_path, std::ios::binary);
	if (file.is_open()) {
		file.write(reinterpret_cast<const char*>(pGpuCrashDump), gpuCrashDumpSize);
		file.flush();
	}
}

void enable_aftermath_crash_dumps() {
	std::call_once(aftermath_enable_once, []() {
		GFSDK_Aftermath_Result res = GFSDK_Aftermath_EnableGpuCrashDumps(
			GFSDK_Aftermath_Version_API,
			GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
			GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
			gpu_crash_dump_callback,
			nullptr,
			nullptr,
			nullptr,
			nullptr);

		if (res != GFSDK_Aftermath_Result_Success) {
			bud::eprint("[Aftermath] EnableGpuCrashDumps failed: {}", (int)res);
		}
	});
}

bool VulkanRHI::init_aftermath() {
	bud::print("[Aftermath] Initialized (GPU crash dumps enabled).");
	return true;
}
#endif


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
	case ResourceState::UnorderedAccess:
		return { VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };
	case ResourceState::IndirectArgument:
		return { VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT };
	default:
		return { VK_IMAGE_LAYOUT_GENERAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
	}
}


void VulkanRHI::init(bud::platform::Window* plat_window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation, uint32_t inflight_frame_count) {
	this->task_scheduler = task_scheduler;
	platform_window = plat_window;
	max_frames_in_flight = inflight_frame_count;

	// store runtime validation flag
	enable_validation_layers = enable_validation;

#ifdef BUD_ENABLE_AFTERMATH
	enable_aftermath_crash_dumps();
#endif

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
	memory_allocator = std::make_unique<VulkanMemoryAllocator>(instance, device, physical_device, max_frames_in_flight);
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
	layout_builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 1, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);

	global_set_layout = layout_builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);

	// Compute Pipeline Layout (Unified for Cull & Mip)
	DescriptorLayoutBuilder compute_builder;
	compute_builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT); // Instance / Input
	compute_builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT); // Indirect / Output Buffer
	compute_builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT); // Stats
	compute_builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT); // HiZ / Mip In
	compute_builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT); // UBO
	compute_builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT); // HiZ Out
	compute_set_layout = compute_builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

	// 创建 Per-Frame UBO Buffers (Binding 0)
	VkDeviceSize ubo_size = sizeof(UniformBufferObject);
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
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)frames.size() * 1001 }, // 1000 bindless + 1 shadow
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)frames.size() }
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
	shadow_sampler_info.compareOp = render_config.reversed_z ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
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

		VmaAllocationCreateInfo alloc_info = {};
		alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		if (vmaCreateImage(memory_allocator->get_vma_allocator(), &image_info, &alloc_info, &dummy_depth_texture.image, &dummy_depth_texture.allocation, nullptr) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create dummy depth");
		}

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
		writer.write_image(2, 0, dummy_depth_texture.view, shadow_sampler, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

		writer.update_set(device, frame.global_descriptor_set);
	}

	// 创建 Fallback 纹理 (Index 0)
	bud::print("[Vulkan] Creating Fallback Texture...");
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

	bud::print("[Vulkan] RHI Initialized successfully (Clean Architecture).");
}

void VulkanRHI::cleanup() {
	wait_idle();

	if (dummy_depth_texture.view)
		vkDestroyImageView(device, dummy_depth_texture.view, nullptr);
	if (dummy_depth_texture.image)
		vmaDestroyImage(memory_allocator->get_vma_allocator(), dummy_depth_texture.image, dummy_depth_texture.allocation);

	if (fallback_texture_ptr) {
		resource_pool->release_texture(fallback_texture_ptr);
		fallback_texture_ptr = nullptr;
	}

	textures.clear();
	texture_objects.clear();

	descriptor_allocators.clear();
	if (global_descriptor_pool)
		vkDestroyDescriptorPool(device, global_descriptor_pool, nullptr);
	if (global_set_layout)
		vkDestroyDescriptorSetLayout(device, global_set_layout, nullptr);
	if (compute_set_layout)
		vkDestroyDescriptorSetLayout(device, compute_set_layout, nullptr);

	if (pipeline_cache)
		pipeline_cache->cleanup();
	if (resource_pool)
		resource_pool->cleanup();

	pipeline_cache.reset();
	resource_pool.reset();

	if (memory_allocator)
		memory_allocator->cleanup();

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


bud::graphics::BufferHandle VulkanRHI::create_gpu_buffer(uint64_t size, bud::graphics::ResourceState usage_state) {
	VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	buffer_info.size = size;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	if (usage_state == bud::graphics::ResourceState::VertexBuffer) {
		buffer_info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	}
	else if (usage_state == bud::graphics::ResourceState::IndexBuffer) {
		buffer_info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	}
	else if (usage_state == bud::graphics::ResourceState::IndirectArgument) {
		buffer_info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; 
	}
	else if (usage_state == bud::graphics::ResourceState::UnorderedAccess) {
		buffer_info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	}
	else if (usage_state == bud::graphics::ResourceState::ShaderResource) {
		buffer_info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	}

	VmaAllocationCreateInfo alloc_info = {};
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
	
	// For UAV/TransferSrc buffers (like our stats counter), ensure host access so we can map it for readback.
	if (usage_state == bud::graphics::ResourceState::UnorderedAccess || (buffer_info.usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
		alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		alloc_info.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	}

	bud::print("[Vulkan][create_gpu_buffer] request size={}, usage_state={}, vk_usage={}, vma_flags={}",
		size, static_cast<uint32_t>(usage_state), (uint32_t)buffer_info.usage, (uint32_t)alloc_info.flags);

	auto* vk_buf = new bud::graphics::vulkan::VulkanBuffer();
	auto vma_allocator = get_memory_allocator()->get_vma_allocator();

	VmaAllocationInfo alloc_result_info;
	VkResult create_result = vmaCreateBuffer(vma_allocator, &buffer_info, &alloc_info, &vk_buf->buffer, &vk_buf->allocation, &alloc_result_info);
	if (create_result != VK_SUCCESS) {
		delete vk_buf;
		return {};
	}
	
	vk_buf->mapped_ptr = alloc_result_info.pMappedData;
	vk_buf->size = size;
	vk_buf->owns_allocation = true;
	bud::graphics::BufferHandle handle;
	handle.internal_state = vk_buf;
	handle.size = size;
	handle.mapped_ptr = vk_buf->mapped_ptr;
    bud::print("[Debug] create_gpu_buffer size={} vk_buf={} mapped_ptr={}", size, (void*)vk_buf, handle.mapped_ptr);
	return handle;
}

void VulkanRHI::destroy_buffer(bud::graphics::BufferHandle block) {
	if (!block.is_valid()) return;
	auto* vk_buf = static_cast<bud::graphics::vulkan::VulkanBuffer*>(block.internal_state);
	if (!vk_buf) return;

	if (vk_buf->owns_allocation) {
		auto vma_allocator = get_memory_allocator()->get_vma_allocator();
		if (vma_allocator && vk_buf->buffer) {
			vmaDestroyBuffer(vma_allocator, vk_buf->buffer, vk_buf->allocation);
		}
	}
	delete vk_buf;
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

#if defined(BUD_HAVE_SPIRV_REFLECT)
    // SPIR-V reflection validation: strict compare reflected descriptor sets against registered layouts
    auto map_spv_type_to_vk = [](SpvReflectDescriptorType t) -> VkDescriptorType {
        switch (t) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
        }
    };

    auto validate_spv_strict = [&](const std::vector<char>& code, const char* stage_name) {
        SpvReflectShaderModule module;
        SpvReflectResult res = spvReflectCreateShaderModule(code.size(), code.data(), &module);
        if (res != SPV_REFLECT_RESULT_SUCCESS) {
            bud::eprint("SPIRV-Reflect: failed to create module for {}", stage_name);
            return;
        }

        uint32_t set_count = 0;
        res = spvReflectEnumerateDescriptorSets(&module, &set_count, nullptr);
        if (res == SPV_REFLECT_RESULT_SUCCESS && set_count > 0) {
            std::vector<SpvReflectDescriptorSet*> sets(set_count);
            res = spvReflectEnumerateDescriptorSets(&module, &set_count, sets.data());
            if (res == SPV_REFLECT_RESULT_SUCCESS) {
                for (uint32_t si = 0; si < set_count; ++si) {
                    SpvReflectDescriptorSet* set = sets[si];
                    // If pipeline uses setLayouts, check only those sets
                    if (si >= setLayouts.size()) {
                        bud::eprint("Shader {} declares descriptor set {} but pipeline only provides {} sets", stage_name, si, setLayouts.size());
                        continue;
                    }
                    VkDescriptorSetLayout layout = setLayouts[si];
                    const auto* layout_info = get_descriptor_set_layout_info(layout);
                    if (!layout_info) {
                        bud::eprint("No layout metadata for VkDescriptorSetLayout {} (set {})", (uintptr_t)layout, si);
                        continue;
                    }

                    for (uint32_t bi = 0; bi < set->binding_count; ++bi) {
                        const SpvReflectDescriptorBinding* binding = set->bindings[bi];
                        // Find binding in layout_info
                        bool found = false;
                        for (const auto& lb : layout_info->bindings) {
                            if (lb.binding == binding->binding) {
                                found = true;
                                VkDescriptorType vk_expected = map_spv_type_to_vk(binding->descriptor_type);
                                if (vk_expected == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
                                    bud::eprint("Shader {} set {} binding {} has unknown SPIR-V descriptor type {}", stage_name, si, binding->binding, (int)binding->descriptor_type);
                                } else if (vk_expected != lb.type) {
                                    bud::eprint("Type mismatch: shader {} set {} binding {} type {} vs layout type {}", stage_name, si, binding->binding, (int)vk_expected, (int)lb.type);
                                }
                                // Check count
                                if (binding->array.dims_count > 0) {
                                    uint32_t arr_count = binding->array.dims[0];
                                    if (arr_count != lb.count && lb.count != VK_DESCRIPTOR_TYPE_MAX_ENUM) {
                                        // Allow layout with large array (e.g., bindless) to be >= reflected count
                                        if (lb.count < arr_count) {
                                            bud::eprint("Count mismatch: shader {} set {} binding {} array count {} vs layout count {}", stage_name, si, binding->binding, arr_count, lb.count);
                                        }
                                    }
                                }
                                // Note: SPIRV-Reflect's SpvReflectDescriptorBinding does not expose stage mask directly;
                                // stage validation is skipped here.
                                break;
                            }
                        }
                        if (!found) {
                            bud::eprint("Shader {} set {} binding {} not present in pipeline layout", stage_name, si, binding->binding);
                        }
                    }
                }
            }
        }

        spvReflectDestroyShaderModule(&module);
    };

    if (enable_validation_layers) {
        validate_spv_strict(desc.vs.code, "vertex");
        validate_spv_strict(desc.fs.code, "fragment");
    }
#endif

	PipelineKey key{};
	key.vert_shader = vertModule;
	key.frag_shader = fragModule;
	key.render_pass = VK_NULL_HANDLE;
	key.depth_test = desc.depth_test;
	key.depth_write = desc.depth_write;
	key.depth_bias_enable = desc.enable_depth_bias;
	key.blending_enable = desc.blending_enable;
	key.vertex_layout = desc.vertex_layout;

	switch (desc.depth_compare_op) {
	case CompareOp::Less: key.depth_compare_op = VK_COMPARE_OP_LESS; break;
	case CompareOp::LessEqual: key.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; break;
	case CompareOp::Greater: key.depth_compare_op = VK_COMPARE_OP_GREATER; break;
	case CompareOp::GreaterEqual: key.depth_compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
	case CompareOp::Always: key.depth_compare_op = VK_COMPARE_OP_ALWAYS; break;
	default: key.depth_compare_op = VK_COMPARE_OP_LESS; break;
	}

	switch (desc.cull_mode) {
	case bud::graphics::CullMode::None: key.cull_mode = VK_CULL_MODE_NONE; break;
	case bud::graphics::CullMode::Front: key.cull_mode = VK_CULL_MODE_FRONT_BIT; break;
	case bud::graphics::CullMode::Back: key.cull_mode = VK_CULL_MODE_BACK_BIT; break;
	default: key.cull_mode = VK_CULL_MODE_BACK_BIT; break;
	}

	key.color_format = to_vk_format(desc.color_attachment_format);
	key.depth_format = to_vk_format(desc.depth_attachment_format);

	bool is_depth_only = (desc.color_attachment_format == bud::graphics::TextureFormat::Undefined);

	VkPipeline pipeline = pipeline_cache->get_pipeline(key, pipelineLayout, is_depth_only);

	vkDestroyShaderModule(device, vertModule, nullptr);
	vkDestroyShaderModule(device, fragModule, nullptr);

	VulkanPipelineObject* pipeObj = new VulkanPipelineObject{ pipeline, pipelineLayout, VK_PIPELINE_BIND_POINT_GRAPHICS };

	created_layouts.push_back(pipelineLayout);

	return pipeObj;
}

void VulkanRHI::destroy_pipeline(void* pipeline) {
	if (!pipeline) return;
	auto* pipeObj = static_cast<VulkanPipelineObject*>(pipeline);
    // Release pipeline from cache; pipeline will be destroyed when no longer referenced
    if (pipeline_cache) {
        pipeline_cache->release_pipeline(pipeObj->pipeline);
    }
    // Destroy wrapper (layout will be cleaned up in VulkanRHI::cleanup via created_layouts)
    delete pipeObj;
}

void* VulkanRHI::create_compute_pipeline(const ComputePipelineDesc& desc) {
	VkPushConstantRange push_constant{};
	push_constant.offset = 0;
	push_constant.size = 256; 
	push_constant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

	std::vector<VkDescriptorSetLayout> setLayouts = { compute_set_layout };
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &push_constant;

	VkPipelineLayout pipelineLayout;
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create compute pipeline layout!");
	}

	VkShaderModule computeModule = create_shader_module(device, desc.cs.code);
	VkPipeline pipeline = pipeline_cache->create_compute_pipeline(computeModule, pipelineLayout);

	vkDestroyShaderModule(device, computeModule, nullptr);

	VulkanPipelineObject* pipeObj = new VulkanPipelineObject{ pipeline, pipelineLayout, VK_PIPELINE_BIND_POINT_COMPUTE };
	created_layouts.push_back(pipelineLayout);

	return pipeObj;
}

void VulkanRHI::cmd_dispatch(CommandHandle cmd, uint32_t group_x, uint32_t group_y, uint32_t group_z) {
	if (current_compute_pipeline) {
		auto pipeObj = static_cast<VulkanPipelineObject*>(current_compute_pipeline);
		
		if (fpCmdPushDescriptorSetKHR && !current_compute_bindings.empty()) {
			std::vector<VkWriteDescriptorSet> writes;
			writes.reserve(current_compute_bindings.size());

			// We need to keep the info structs alive until the push command
			std::vector<VkDescriptorBufferInfo> buffer_infos;
			std::vector<VkDescriptorImageInfo> image_infos;
			buffer_infos.reserve(current_compute_bindings.size());
			image_infos.reserve(current_compute_bindings.size());

			for (const auto& [binding, res] : current_compute_bindings) {
				VkWriteDescriptorSet write{};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstBinding = binding;
				write.descriptorCount = 1;

				if (std::holds_alternative<BufferHandle>(res)) {
					auto& buffer_handle = std::get<BufferHandle>(res);
					if (!buffer_handle.is_valid()) continue;
					auto* vk_buf = static_cast<bud::graphics::vulkan::VulkanBuffer*>(buffer_handle.internal_state);
					
					VkDescriptorBufferInfo info{};
					info.buffer = vk_buf->buffer;
					info.offset = buffer_handle.offset;
					info.range = buffer_handle.size;
					buffer_infos.push_back(info);
					
					write.descriptorType = (binding == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					write.pBufferInfo = &buffer_infos.back();
				} else if (std::holds_alternative<ImageBinding>(res)) {
					auto& image_binding = std::get<ImageBinding>(res);
					if (!image_binding.texture) continue;
					auto* vk_tex = static_cast<VulkanTexture*>(image_binding.texture);

					VkDescriptorImageInfo info{};
					VkImageLayout layout;
					bool tex_is_depth = (image_binding.texture->format == TextureFormat::D32_FLOAT ||
					                     image_binding.texture->format == TextureFormat::D24_UNORM_S8_UINT);
					if (image_binding.is_storage) {
						layout = VK_IMAGE_LAYOUT_GENERAL;
					} else if (image_binding.is_general) {
						layout = VK_IMAGE_LAYOUT_GENERAL;
					} else if (tex_is_depth) {
						layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
					} else {
						layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					}
					info.imageLayout = layout;
					info.imageView = (image_binding.mip_level == bud::graphics::ALL_MIPS) ? vk_tex->view : 
					                 ((image_binding.mip_level < vk_tex->mip_views.size()) ? vk_tex->mip_views[image_binding.mip_level] : vk_tex->view);
					info.sampler = vk_tex->sampler ? vk_tex->sampler : default_sampler;
					image_infos.push_back(info);

					write.descriptorType = image_binding.is_storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					write.pImageInfo = &image_infos.back();
				} else if (std::holds_alternative<UBOBinding>(res)) {
					auto& frame = frames[current_frame];
					
					VkDescriptorBufferInfo info{};
					info.buffer = frame.uniform_buffer;
					info.offset = 0;
					info.range = VK_WHOLE_SIZE;
					buffer_infos.push_back(info);

					write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
					write.pBufferInfo = &buffer_infos.back();
				}
				writes.push_back(write);
			}

			fpCmdPushDescriptorSetKHR(static_cast<VkCommandBuffer>(cmd), VK_PIPELINE_BIND_POINT_COMPUTE, pipeObj->layout, 0, static_cast<uint32_t>(writes.size()), writes.data());
		}
	}

	vkCmdDispatch(static_cast<VkCommandBuffer>(cmd), group_x, group_y, group_z);
}

// Helpers are implemented at the end of the file.

// Frame Control

CommandHandle VulkanRHI::begin_frame() {
	vkWaitForFences(device, 1, &frames[current_frame].in_flight_fence, VK_TRUE, UINT64_MAX);

	VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frames[current_frame].image_available_semaphore, VK_NULL_HANDLE, &current_image_index);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		swapchain_out_of_date.store(true, std::memory_order_release);
		return nullptr;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	vkResetFences(device, 1, &frames[current_frame].in_flight_fence);

	current_stats.reset();

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
	VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
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

	VkResult present_result = vkQueuePresentKHR(present_queue, &present_info);
	if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
		swapchain_out_of_date.store(true, std::memory_order_release);
	} else if (present_result != VK_SUCCESS) {
		throw std::runtime_error("failed to present swap chain image!");
	}

	current_frame = (current_frame + 1) % max_frames_in_flight;
}


void VulkanRHI::resize_swapchain(uint32_t width, uint32_t height) {
	if (!device || !platform_window)
		return;

	if (width == 0 || height == 0)
		return;

	vkDeviceWaitIdle(device);

	swapchain_out_of_date.store(false, std::memory_order_release);

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


void VulkanRHI::cmd_begin_render_pass(CommandHandle cmd, const RenderPassBeginInfo& info) {
	VkCommandBuffer vk_cmd = static_cast<VkCommandBuffer>(cmd);
	VkRenderingInfo rendering_info{ VK_STRUCTURE_TYPE_RENDERING_INFO };

	if (!info.color_attachments.empty()) {
		rendering_info.renderArea = { {0, 0}, {info.color_attachments[0]->width, info.color_attachments[0]->height} };
	}
	else if (info.depth_attachment) {
		rendering_info.renderArea = { {0, 0}, {info.depth_attachment->width, info.depth_attachment->height} };
	}

	rendering_info.layerCount = info.layer_count;

	std::vector<VkRenderingAttachmentInfo> color_attachments;
	for (auto* tex : info.color_attachments) {
		auto vk_tex = static_cast<VulkanTexture*>(tex);
		VkRenderingAttachmentInfo attach{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };

		if (!vk_tex->layer_views.empty()) {
			attach.imageView = vk_tex->layer_views[info.base_array_layer];
		}
		else {
			attach.imageView = vk_tex->view;
		}

		attach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attach.loadOp = info.clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attach.clearValue.color = { info.clear_color_value.r, info.clear_color_value.g, info.clear_color_value.b, info.clear_color_value.a };
		color_attachments.push_back(attach);
	}

	rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
	rendering_info.pColorAttachments = color_attachments.data();

	VkRenderingAttachmentInfo depth_attach{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	if (info.depth_attachment) {
		auto vk_depth = static_cast<VulkanTexture*>(info.depth_attachment);

		if (!vk_depth->layer_views.empty()) {
			depth_attach.imageView = vk_depth->layer_views[info.base_array_layer];
		}
		else {
			depth_attach.imageView = vk_depth->view;
		}

		depth_attach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depth_attach.loadOp = info.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		depth_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depth_attach.clearValue.depthStencil = { info.clear_depth_value, 0 };
		rendering_info.pDepthAttachment = &depth_attach;
	}

	vkCmdBeginRendering(vk_cmd, &rendering_info);
}

void VulkanRHI::cmd_end_render_pass(CommandHandle cmd) {
	vkCmdEndRendering(static_cast<VkCommandBuffer>(cmd));
}

void VulkanRHI::cmd_copy_buffer(CommandHandle cmd, bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size) {
	if (!src.is_valid() || !dst.is_valid()) {
		bud::eprint("[Vulkan][cmd_copy_buffer] invalid handle: src_valid={}, dst_valid={}, size={}", src.is_valid(), dst.is_valid(), size);
		return;
	}

	auto* vk_src = static_cast<bud::graphics::vulkan::VulkanBuffer*>(src.internal_state);
	auto* vk_dst = static_cast<bud::graphics::vulkan::VulkanBuffer*>(dst.internal_state);
	if (!vk_src || !vk_dst) {
		bud::eprint("[Vulkan][cmd_copy_buffer] null internal_state: vk_src={}, vk_dst={}, size={}", (void*)vk_src, (void*)vk_dst, size);
		return;
	}
	if (!vk_src->buffer || !vk_dst->buffer) {
		bud::eprint("[Vulkan][cmd_copy_buffer] null VkBuffer: src_buf={}, dst_buf={}, src_size={}, dst_size={}, src_offset={}, dst_offset={}",
			(void*)vk_src->buffer, (void*)vk_dst->buffer, src.size, dst.size, src.offset, dst.offset);
		return;
	}
	VkBufferCopy copy_region{};
	copy_region.size = size;
	vkCmdCopyBuffer(static_cast<VkCommandBuffer>(cmd), vk_src->buffer, vk_dst->buffer, 1, &copy_region);
}

void VulkanRHI::resource_barrier(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) {
	// Helper: map ResourceState -> (stage, access) for BUFFER barriers
	auto buf_transition = [](bud::graphics::ResourceState state) -> std::pair<VkPipelineStageFlags2, VkAccessFlags2> {
		using RS = bud::graphics::ResourceState;
		switch (state) {
		case RS::UnorderedAccess:
			return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
		case RS::IndirectArgument:
			return { VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT };
		case RS::ShaderResource:
			return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
		case RS::VertexBuffer:
			return { VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT };
		case RS::IndexBuffer:
			return { VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT };
		case RS::TransferSrc:
			return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
		case RS::TransferDst:
			return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
		default:
			return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
		}
	};

	auto [src_stage, src_access] = buf_transition(old_state);
	auto [dst_stage, dst_access] = buf_transition(new_state);

	VkBufferMemoryBarrier2 barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	barrier.srcStageMask = src_stage;
	barrier.srcAccessMask = src_access;
	barrier.dstStageMask = dst_stage;
	barrier.dstAccessMask = dst_access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	auto* vk_buf = static_cast<bud::graphics::vulkan::VulkanBuffer*>(buffer.internal_state);
	barrier.buffer = vk_buf->buffer;
	barrier.offset = 0;
	barrier.size = VK_WHOLE_SIZE;

	VkDependencyInfo depInfo{};
	depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	depInfo.bufferMemoryBarrierCount = 1;
	depInfo.pBufferMemoryBarriers = &barrier;

	vkCmdPipelineBarrier2(static_cast<VkCommandBuffer>(cmd), &depInfo);
}

void VulkanRHI::cmd_bind_pipeline(CommandHandle cmd, void* pipeline) {
	auto pipeObj = static_cast<VulkanPipelineObject*>(pipeline);
	if (pipeObj->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
		current_compute_pipeline = pipeObj;
		current_compute_bindings.clear();
	}
	vkCmdBindPipeline(static_cast<VkCommandBuffer>(cmd), pipeObj->bind_point, pipeObj->pipeline);
	current_stats.pipeline_binds++;
}

void VulkanRHI::cmd_bind_vertex_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) {
	auto* vk_buf = static_cast<VulkanBuffer*>(buffer.internal_state);
	VkDeviceSize offsets[] = { 0 };
	vkCmdBindVertexBuffers(static_cast<VkCommandBuffer>(cmd), 0, 1, &vk_buf->buffer, offsets);
}

void VulkanRHI::cmd_bind_index_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) {
	auto* vk_buf = static_cast<VulkanBuffer*>(buffer.internal_state);
	vkCmdBindIndexBuffer(static_cast<VkCommandBuffer>(cmd), vk_buf->buffer, 0, VK_INDEX_TYPE_UINT32);
}

void VulkanRHI::cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
	vkCmdDraw(static_cast<VkCommandBuffer>(cmd), vertex_count, instance_count, first_vertex, first_instance);
	current_stats.draw_calls++;
	current_stats.drawn_triangles += (vertex_count / 3) * instance_count;
}

void VulkanRHI::cmd_draw_indexed(CommandHandle cmd, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
	vkCmdDrawIndexed(static_cast<VkCommandBuffer>(cmd), index_count, instance_count, first_index, vertex_offset, first_instance);
	current_stats.draw_calls++;
	current_stats.drawn_triangles += (index_count / 3) * instance_count;
}

void VulkanRHI::cmd_draw_indexed_indirect(CommandHandle cmd, bud::graphics::BufferHandle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) {
	if (!buffer.is_valid()) return;
	auto* vk_buf = static_cast<bud::graphics::vulkan::VulkanBuffer*>(buffer.internal_state);
	vkCmdDrawIndexedIndirect(static_cast<VkCommandBuffer>(cmd), vk_buf->buffer, offset, draw_count, stride);
	current_stats.draw_calls += draw_count; 
}

void VulkanRHI::cmd_push_constants(CommandHandle cmd, void* pipeline_layout, uint32_t size, const void* data) {
	auto pipeObj = static_cast<VulkanPipelineObject*>(pipeline_layout);
	VkShaderStageFlags stage = (pipeObj->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) 
		? VK_SHADER_STAGE_COMPUTE_BIT 
		: (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	vkCmdPushConstants(static_cast<VkCommandBuffer>(cmd), pipeObj->layout, stage, 0, size, data);
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
		pipeObj->bind_point,
		pipeObj->layout,
		set_index,  // first set
		1,          // descriptor set count
		&frame.global_descriptor_set,
		0,
		nullptr  // dynamic offsets
	);
}

void VulkanRHI::cmd_bind_storage_buffer(CommandHandle cmd, void* pipeline, uint32_t binding, bud::graphics::BufferHandle buffer) {
	current_compute_bindings[binding] = buffer;
}

void VulkanRHI::cmd_bind_compute_texture(CommandHandle cmd, void* pipeline, uint32_t binding, bud::graphics::Texture* texture, uint32_t mip_level, bool is_storage, bool is_general) {
	current_compute_bindings[binding] = ImageBinding{ texture, mip_level, is_storage, is_general };
}

void VulkanRHI::cmd_bind_compute_ubo(CommandHandle cmd, void* pipeline, uint32_t binding) {
	current_compute_bindings[binding] = UBOBinding{};
}

void VulkanRHI::resource_barrier(CommandHandle cmd, bud::graphics::Texture* texture, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) {
    if (!texture) return;
    auto vk_tex = static_cast<VulkanTexture*>(texture);
    auto src = sync2::get_transition2(old_state);
    auto dst = sync2::get_transition2(new_state);

    bool is_depth = (texture->format == TextureFormat::D32_FLOAT || texture->format == TextureFormat::D24_UNORM_S8_UINT);
    VkImageLayout newLayout = dst.layout;
    VkImageLayout oldLayout = src.layout;
    // Depth textures must use DEPTH_STENCIL_READ_ONLY_OPTIMAL instead of SHADER_READ_ONLY_OPTIMAL
    if (is_depth && new_state == ResourceState::ShaderResource) {
        newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    if (is_depth && old_state == ResourceState::ShaderResource) {
        oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }

    VkImageAspectFlags aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (is_depth && texture->format == TextureFormat::D24_UNORM_S8_UINT) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    sync2::cmd_image_barrier2(static_cast<VkCommandBuffer>(cmd), vk_tex->image, aspect,
                              0, (texture->mips > 0 ? texture->mips : 1), 0, (texture->array_layers > 0 ? texture->array_layers : 1),
                              oldLayout, newLayout,
                              src.stage, src.access,
                              dst.stage, dst.access);
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


// Boilerplate (Device Creation & Utils)

void VulkanRHI::create_instance(VkInstance& vk_instance, bool enable_validation) {
	enable_validation_layers = enable_validation;
	VkApplicationInfo app_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName = "Bud Engine";
	app_info.apiVersion = VK_API_VERSION_1_3;

	VkInstanceCreateInfo create_info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	create_info.pApplicationInfo = &app_info;

    uint32_t count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    std::vector<const char*> exts;
    if (extensions && count > 0) {
        exts.assign(extensions, extensions + count);
    }
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
			bud::print("[Vulkan] Selected Discrete GPU: {}", props.deviceName);
			break;
		}
	}
	if (physical_device == nullptr) {
		physical_device = devices[0];
		bud::print("[Vulkan] Warning: Using Integrated/Fallback GPU.");
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
	features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
	features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

	VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
	features11.pNext = &features12;

	// 使用 VkPhysicalDeviceFeatures2 整合所有 Features
	VkPhysicalDeviceFeatures2 device_features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	device_features2.pNext = &features11;
	device_features2.features.samplerAnisotropy = VK_TRUE;
	device_features2.features.multiDrawIndirect = VK_TRUE;

#ifdef BUD_ENABLE_AFTERMATH
	// Enable NV_device_diagnostic_checkpoints extension to be able to
	// use Aftermath event markers.
	device_extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);

	// Enable NV_device_diagnostics_config extension to configure Aftermath
	// features.
	device_extensions.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);

	// Set up device creation info for Aftermath feature flag configuration.
	VkDeviceDiagnosticsConfigFlagsNV aftermathFlags =
		VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV |  // Enable automatic call stack checkpoints.
		VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV |      // Enable tracking of resources.
		VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV |      // Generate debug information for shaders.
		VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;  // Enable additional runtime shader error reporting.

	VkDeviceDiagnosticsConfigCreateInfoNV aftermathInfo = {};
	aftermathInfo.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
	aftermathInfo.flags = aftermathFlags;
	aftermathInfo.pNext = &device_features2;  // Chain to the main device features struct
#endif

	VkDeviceCreateInfo create_info{};
	create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
#ifdef BUD_ENABLE_AFTERMATH
	create_info.pNext = &aftermathInfo;  // Chain Aftermath config into device creation
#else
	create_info.pNext = &device_features2;
#endif
	create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
	create_info.pQueueCreateInfos = queue_infos.data();
	create_info.pEnabledFeatures = nullptr;

	create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
	create_info.ppEnabledExtensionNames = device_extensions.data();

	if (physical_device == nullptr) throw std::runtime_error("Physical device is NULL!");

	VkResult res = vkCreateDevice(physical_device, &create_info, nullptr, &device);
	if (res != VK_SUCCESS) {
		bud::eprint("[Vulkan] vkCreateDevice failed with code: {}", (int)res);
		throw std::runtime_error("Device creation failed");
	}
	bud::print("[Vulkan] Logical Device created successfully.");

#ifdef BUD_ENABLE_AFTERMATH
	aftermath_initialized = init_aftermath();
#endif

	vkGetDeviceQueue(device, indices.graphics_family.value(), 0, &graphics_queue);
	vkGetDeviceQueue(device, indices.present_family.value(), 0, &present_queue);

	fpCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
	if (!fpCmdPushDescriptorSetKHR) {
		bud::eprint("[Vulkan] Warning: vkCmdPushDescriptorSetKHR not found, compute bindings may fail!");
	}
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

	if (vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain) != VK_SUCCESS)
		throw std::runtime_error("Failed to create swapchain!");

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

		if (vkCreateImageView(device, &create_info, nullptr, &swapchain_image_views[i]) != VK_SUCCESS)
			throw std::runtime_error("Failed to create image views!");
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
		swapchain_textures_wrappers[i].allocation = VK_NULL_HANDLE; // Swapchain image memory is managed by driver
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
	// 使用第0帧的命令池（这在多线程渲染时可能不安全，但在初始化阶段是安全的）	
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
		bud::eprint("[Vulkan] Failed to submit single time command!");
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
		VkExtent2D extent = capabilities.currentExtent;
		if (extent.width == 0 || extent.height == 0) {
			extent.width = std::max(1u, extent.width);
			extent.height = std::max(1u, extent.height);
		}
		return extent;
	}

	int width = 0;
	int height = 0;
	if (window) {
		window->get_size_in_pixels(width, height);
	}

	if (width == 0 || height == 0) {
		width = static_cast<int>(std::max(1u, capabilities.minImageExtent.width));
		height = static_cast<int>(std::max(1u, capabilities.minImageExtent.height));
	}

	VkExtent2D actual_extent = { static_cast<uint32_t>(std::max(1, width)), static_cast<uint32_t>(std::max(1, height)) };
	actual_extent.width = std::clamp(actual_extent.width, std::max(1u, capabilities.minImageExtent.width), capabilities.maxImageExtent.width);
	actual_extent.height = std::clamp(actual_extent.height, std::max(1u, capabilities.minImageExtent.height), capabilities.maxImageExtent.height);
	return actual_extent;
}

// Debug Utils

VkResult VulkanRHI::create_debug_utils_messenger_ext(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if (func != nullptr)
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);

	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void VulkanRHI::destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if (func != nullptr)
		func(instance, debugMessenger, pAllocator);
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
		bud::eprint("[Validation Layer]: {}", pCallbackData->pMessage);
	}
	return VK_FALSE;
}
void VulkanRHI::transition_image_layout_immediate(VkImage image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout) {
    VkCommandBuffer commandBuffer = this->begin_single_time_commands();

    VkImageAspectFlags aspect = get_aspect_flags(format);

    VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 srcAccess = 0;
    VkAccessFlags2 dstAccess = 0;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        srcAccess = 0;
        dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        srcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        dstAccess = VK_ACCESS_2_SHADER_READ_BIT;
    }

    sync2::cmd_image_barrier2(commandBuffer, image, aspect, 0, 1, 0, 1, old_layout, new_layout, srcStage, srcAccess, dstStage, dstAccess);

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

void VulkanRHI::copy_buffer_immediate(bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size) {
	VkCommandBuffer cmd = this->begin_single_time_commands();
	if (!src.is_valid() || !dst.is_valid()) {
		bud::eprint("[Vulkan][copy_buffer_immediate] invalid handle: src_valid={}, dst_valid={}, size={}", src.is_valid(), dst.is_valid(), size);
		this->end_single_time_commands(cmd);
		return;
	}

	VkBufferCopy copy_region{};
	copy_region.srcOffset = 0;
	copy_region.dstOffset = 0;
	copy_region.size = size;

	auto* vk_src = static_cast<bud::graphics::vulkan::VulkanBuffer*>(src.internal_state);
	auto* vk_dst = static_cast<bud::graphics::vulkan::VulkanBuffer*>(dst.internal_state);
	if (!vk_src || !vk_dst) {
		bud::eprint("[Vulkan][copy_buffer_immediate] null internal_state: vk_src={}, vk_dst={}, size={}", (void*)vk_src, (void*)vk_dst, size);
		this->end_single_time_commands(cmd);
		return;
	}
	if (!vk_src->buffer || !vk_dst->buffer) {
		bud::eprint("[Vulkan][copy_buffer_immediate] null VkBuffer: src_buf={}, dst_buf={}, src_size={}, dst_size={}, src_offset={}, dst_offset={}",
			(void*)vk_src->buffer, (void*)vk_dst->buffer, src.size, dst.size, src.offset, dst.offset);
		this->end_single_time_commands(cmd);
		return;
	}

	vkCmdCopyBuffer(cmd, vk_src->buffer, vk_dst->buffer, 1, &copy_region);

	this->end_single_time_commands(cmd);
}

void VulkanRHI::copy_buffer_immediate_offset(bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) {
	if (!src.is_valid() || !dst.is_valid()) {
		bud::eprint("[Vulkan][copy_buffer_immediate_offset] invalid handle: src_valid={}, dst_valid={}, size={}", src.is_valid(), dst.is_valid(), size);
		return;
	}

	auto* vk_src = static_cast<bud::graphics::vulkan::VulkanBuffer*>(src.internal_state);
	auto* vk_dst = static_cast<bud::graphics::vulkan::VulkanBuffer*>(dst.internal_state);
	if (!vk_src || !vk_dst || !vk_src->buffer || !vk_dst->buffer) {
		bud::eprint("[Vulkan][copy_buffer_immediate_offset] null VkBuffer handles");
		return;
	}

	VkCommandBuffer cmd = this->begin_single_time_commands();

	VkBufferCopy copy_region{};
	copy_region.srcOffset = src_offset;
	copy_region.dstOffset = dst_offset;
	copy_region.size = size;

	vkCmdCopyBuffer(cmd, vk_src->buffer, vk_dst->buffer, 1, &copy_region);

	this->end_single_time_commands(cmd);
}

void VulkanRHI::cmd_copy_image(CommandHandle cmd, Texture* src, Texture* dst) {
    if (!src || !dst) {
        bud::eprint("[Vulkan][cmd_copy_image] null texture pointer(s)");
        return;
    }
    VulkanTexture* vk_src = static_cast<VulkanTexture*>(src);
    VulkanTexture* vk_dst = static_cast<VulkanTexture*>(dst);
    if (!vk_src || !vk_src->image || !vk_dst || !vk_dst->image) {
        bud::eprint("[Vulkan][cmd_copy_image] invalid VulkanTexture or VkImage");
        return;
    }

    VkImageCopy region{};
    // choose aspect masks based on texture formats (color vs depth/stencil)
    bool src_is_depth = (src->format == TextureFormat::D32_FLOAT || src->format == TextureFormat::D24_UNORM_S8_UINT);
    bool dst_is_depth = (dst->format == TextureFormat::D32_FLOAT || dst->format == TextureFormat::D24_UNORM_S8_UINT);
    VkImageAspectFlags srcAspect = src_is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags dstAspect = dst_is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (src_is_depth && src->format == TextureFormat::D24_UNORM_S8_UINT) srcAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dst_is_depth && dst->format == TextureFormat::D24_UNORM_S8_UINT) dstAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    region.srcSubresource.aspectMask = srcAspect;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = src->array_layers;
    region.srcSubresource.mipLevel = 0;
    region.dstSubresource.aspectMask = dstAspect;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount = dst->array_layers;
    region.dstSubresource.mipLevel = 0;
    region.extent.width = src->width;
    region.extent.height = src->height;
    region.extent.depth = 1;

    vkCmdCopyImage(static_cast<VkCommandBuffer>(cmd),
        vk_src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vk_dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);
}

void VulkanRHI::cmd_blit_image(CommandHandle cmd, Texture* src, Texture* dst) {
	VulkanTexture* vk_src = static_cast<VulkanTexture*>(src);
	VulkanTexture* vk_dst = static_cast<VulkanTexture*>(dst);

	VkImageBlit blit{};
	blit.srcOffsets[0] = { 0, 0, 0 };
	blit.srcOffsets[1] = { (int32_t)src->width, (int32_t)src->height, 1 };
	blit.srcSubresource.aspectMask = (src->format == TextureFormat::D32_FLOAT || src->format == TextureFormat::D24_UNORM_S8_UINT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.baseArrayLayer = 0;
	blit.srcSubresource.layerCount = src->array_layers;

	blit.dstOffsets[0] = { 0, 0, 0 };
	blit.dstOffsets[1] = { (int32_t)dst->width, (int32_t)dst->height, 1 };
	blit.dstSubresource.aspectMask = (dst->format == TextureFormat::D32_FLOAT || dst->format == TextureFormat::D24_UNORM_S8_UINT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.baseArrayLayer = 0;
	blit.dstSubresource.layerCount = dst->array_layers;

	vkCmdBlitImage(static_cast<VkCommandBuffer>(cmd),
		vk_src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		vk_dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit,
		VK_FILTER_LINEAR);
}

void VulkanRHI::cmd_copy_to_buffer(CommandHandle cmd, bud::graphics::BufferHandle dst, uint64_t offset, uint64_t size, const void* data) {
	// Not used for GPU-to-GPU copy, this seems to be for CPU-to-GPU copy via cmd buffer (vkCmdUpdateBuffer)
	if (!dst.is_valid()) {
		bud::eprint("[Vulkan][cmd_copy_to_buffer] invalid handle: dst_valid=false, offset={}, size={}", offset, size);
		return;
	}
	auto* vk_dst = static_cast<bud::graphics::vulkan::VulkanBuffer*>(dst.internal_state);
	if (!vk_dst || !vk_dst->buffer) {
		bud::eprint("[Vulkan][cmd_copy_to_buffer] null destination buffer: vk_dst={}, dst_buf={}, offset={}, size={}", (void*)vk_dst, vk_dst ? (void*)vk_dst->buffer : nullptr, offset, size);
		return;
	}
	vkCmdUpdateBuffer(static_cast<VkCommandBuffer>(cmd), vk_dst->buffer, offset, size, data);
}

bud::graphics::BufferHandle VulkanRHI::create_upload_buffer(uint64_t size) {
	VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	buffer_info.size = size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT; // Allow using upload buffer directly for dynamic UI
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo alloc_info = {};
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
	alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	auto* vk_buf = new bud::graphics::vulkan::VulkanBuffer();
	auto vma_allocator = get_memory_allocator()->get_vma_allocator();

	VkResult create_result = vmaCreateBuffer(vma_allocator, &buffer_info, &alloc_info, &vk_buf->buffer, &vk_buf->allocation, &vk_buf->alloc_info);
	if (create_result != VK_SUCCESS) {
		delete vk_buf;
		throw std::runtime_error("Failed to create Upload buffer via VMA!");
	}

	bud::graphics::BufferHandle handle;
	vk_buf->owns_allocation = true;
	handle.internal_state = vk_buf;
	handle.offset = 0;
	handle.size = size;
	handle.mapped_ptr = vk_buf->alloc_info.pMappedData;
    //bud::print("[Debug] create_upload_buffer size={} vk_buf={} mapped_ptr={}", size, (void*)vk_buf, handle.mapped_ptr);
	return handle;
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

		sync2::cmd_image_barrier2(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
			(i - 1), 1, 0, 1,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

		// 1.5 Transition Level i to TRANSFER_DST_OPTIMAL (from UNDEFINED)
		// We can reuse the barrier struct but be careful
		VkImageMemoryBarrier dstBarrier = barrier;
		dstBarrier.subresourceRange.baseMipLevel = i;
		dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		dstBarrier.srcAccessMask = 0;
		dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;


		sync2::cmd_image_barrier2(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
			i, 1, 0, 1,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

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

		sync2::cmd_image_barrier2(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
			(i - 1), 1, 0, 1,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

		if (mipWidth > 1) mipWidth /= 2;
		if (mipHeight > 1) mipHeight /= 2;
	}

	// 4. Transition Final Level to SHADER_READ_ONLY_OPTIMAL
	barrier.subresourceRange.baseMipLevel = mipLevels - 1;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    sync2::cmd_image_barrier2(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                              (mipLevels - 1), 1, 0, 1,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

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
		bud::graphics::BufferHandle staging = this->create_upload_buffer(size);
		std::memcpy(staging.mapped_ptr, initial_data, size);

		this->transition_image_layout_immediate(tex->image, to_vk_format(desc.format), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		auto* vk_buf = static_cast<VulkanBuffer*>(staging.internal_state);

		this->copy_buffer_to_image(tex->image, vk_buf->buffer, desc.width, desc.height);

		if (desc.mips > 1) {
			//bud::print("[Texture] Generating {} mip levels for {}x{} texture", desc.mips, desc.width, desc.height);
			this->generate_mipmaps(tex->image, to_vk_format(desc.format), desc.width, desc.height, desc.mips);
		}
		else {
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
			bud::eprint("[Vulkan] ERROR: global_descriptor_set at frame {} is NULL!", i);
			continue;
		}

		DescriptorWriter writer;
		writer.write_image(1, index, vk_tex->view, vk_tex->sampler ? vk_tex->sampler : default_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		writer.update_set(device, frames[i].global_descriptor_set);
	}
}
 
void VulkanRHI::update_bindless_image(uint32_t index, bud::graphics::Texture* texture, uint32_t mip_level, bool is_storage) {
	if (!texture) return;
	auto vk_tex = static_cast<VulkanTexture*>(texture);
	VkImageView view_to_bind = vk_tex->view;
	if (mip_level < vk_tex->mip_views.size()) {
		view_to_bind = vk_tex->mip_views[mip_level];
	}

	for (int i = 0; i < max_frames_in_flight; i++) {
		DescriptorWriter writer;
		if (is_storage) {
			writer.write_image(1, index, view_to_bind, vk_tex->sampler ? vk_tex->sampler : default_sampler, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		} else {
			writer.write_image(1, index, view_to_bind, vk_tex->sampler ? vk_tex->sampler : default_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		}
		writer.update_set(device, frames[i].global_descriptor_set);
	}
}

void VulkanRHI::update_global_uniforms(uint32_t image_index, const SceneView& scene_view) {
	UniformBufferObject ubo{};
	ubo.view = scene_view.view_matrix;
	ubo.proj = scene_view.proj_matrix;

	for (uint32_t i = 0; i < MAX_CASCADES; ++i) {
		ubo.cascade_view_proj[i] = scene_view.cascade_view_proj_matrices[i];
	}

	ubo.cascade_split_depths = bud::math::vec4(
		scene_view.cascade_split_depths[0],
		scene_view.cascade_split_depths[1],
		scene_view.cascade_split_depths[2],
		scene_view.cascade_split_depths[3]
	);

	ubo.cascade_count = render_config.cascade_count;

	ubo.cam_pos = scene_view.camera_position;
	ubo.light_dir = scene_view.light_dir;
	ubo.light_color = scene_view.light_color;
	ubo.light_intensity = scene_view.light_intensity;
	ubo.ambient_strength = scene_view.ambient_strength;
	ubo.debug_cascades = render_config.debug_cascades ? 1 : 0;
	ubo.reversed_z = render_config.reversed_z ? 1 : 0;
	ubo.shadow_bias_constant = render_config.shadow_bias_constant;
	ubo.shadow_bias_slope = render_config.shadow_bias_slope;

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

void VulkanRHI::update_global_instance_data(bud::graphics::BufferHandle buffer) {
	if (!buffer.is_valid()) return;
	auto* vk_buf = static_cast<VulkanBuffer*>(buffer.internal_state);

	for (int i = 0; i < max_frames_in_flight; i++) {
		DescriptorWriter writer;
		writer.write_buffer(3, vk_buf->buffer, buffer.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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
std::vector<VkVertexInputAttributeDescription> attributeDescriptions(4);

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

	set_object_debug_name(reinterpret_cast<uint64_t>(vk_tex->image), object_type, name);

	if (vk_tex->view) {
		set_object_debug_name((uint64_t)vk_tex->view, ObjectType::ImageView, name + "_View");
	}
}

void VulkanRHI::set_debug_name(const bud::graphics::BufferHandle& buffer, ObjectType object_type, const std::string& name) {
	if (!buffer.is_valid()) return;

	auto* vk_buf = static_cast<VulkanBuffer*>(buffer.internal_state);
	set_object_debug_name(reinterpret_cast<uint64_t>(vk_buf->buffer), object_type, name);
}

void VulkanRHI::set_debug_name(CommandHandle cmd, ObjectType object_type, const std::string& name) {
	auto vk_cmd = static_cast<VkCommandBuffer>(cmd);
	set_object_debug_name(reinterpret_cast<uint64_t>(vk_cmd), object_type, name);
}



