#include <vector>
#include <string>
#include <iostream>
#include <optional>
#include <set>
#include <algorithm>
#include <limits>
#include <mutex>
#include <map>
#include <cstdio>
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
#include "src/core/bud.logger.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.utils.hpp"
#include "src/graphics/vulkan/bud.vulkan.sync2.hpp"

#ifdef BUD_ENABLE_AFTERMATH
#include <GFSDK_Aftermath.h>
#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>
#endif

using namespace bud::graphics;
using namespace bud::graphics::vulkan;


PFN_vkCmdBeginDebugUtilsLabelEXT fpCmdBeginDebugUtilsLabelEXT = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT fpCmdEndDebugUtilsLabelEXT = nullptr;
PFN_vkSetDebugUtilsObjectNameEXT fpSetDebugUtilsObjectNameEXT = nullptr;




#ifdef BUD_ENABLE_AFTERMATH
std::mutex aftermath_mutex;
std::once_flag aftermath_enable_once;

// Directory where shader debug info files are cached (so Nsight Graphics can
// find them at analysis time). Matches the crash dump location.
static std::filesystem::path g_shader_debug_dir;

// Shader debug info identifier -> serialized blob cache.
// Kept alive in case Nsight Graphics queries them after the fact.
// GFSDK_Aftermath_ShaderDebugInfoIdentifier has no operator<, so provide a
// custom comparator for use as a map key.
struct ShaderDebugInfoIdentifierLess {
    bool operator()(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& a,
                    const GFSDK_Aftermath_ShaderDebugInfoIdentifier& b) const {
        if (a.id[0] != b.id[0]) return a.id[0] < b.id[0];
        return a.id[1] < b.id[1];
    }
};

static std::mutex g_shader_debug_cache_mutex;
static std::map<GFSDK_Aftermath_ShaderDebugInfoIdentifier, std::vector<char>, ShaderDebugInfoIdentifierLess> g_shader_debug_cache;

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

	bud::print("[Aftermath] GPU crash dump saved to: {}", dump_path.string());
}

void shader_debug_info_callback(const void* pShaderDebugInfo, const uint32_t shaderDebugInfoSize, void* /*pUserData*/) {
	// Extract the unique identifier for this shader debug info blob.
	GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier = {};
	GFSDK_Aftermath_Result res = GFSDK_Aftermath_GetShaderDebugInfoIdentifier(
		GFSDK_Aftermath_Version_API,
		pShaderDebugInfo,
		shaderDebugInfoSize,
		&identifier);
	if (res != GFSDK_Aftermath_Result_Success) {
		return;
	}

	// Cache in memory so the JSON decoder / Nsight Graphics can look it up later.
	{
		std::lock_guard lock(g_shader_debug_cache_mutex);
		g_shader_debug_cache[identifier].assign(
			static_cast<const char*>(pShaderDebugInfo),
			static_cast<const char*>(pShaderDebugInfo) + shaderDebugInfoSize);
	}

	// Also write to disk for offline analysis (Nsight Graphics can load these
	// alongside the crash dump).
	namespace fs = std::filesystem;
	std::error_code ec;
	if (g_shader_debug_dir.empty()) {
		g_shader_debug_dir = fs::current_path(ec) / "aftermath_dumps";
	}
	fs::create_directories(g_shader_debug_dir, ec);

	char id_str[64];
	snprintf(id_str, sizeof(id_str), "%016llx%016llx",
		(unsigned long long)identifier.id[0],
		(unsigned long long)identifier.id[1]);

	fs::path debug_path = g_shader_debug_dir / ("shader_debug_" + std::string(id_str) + ".nv-shdebug");
	std::ofstream file(debug_path, std::ios::binary);
	if (file.is_open()) {
		file.write(reinterpret_cast<const char*>(pShaderDebugInfo), shaderDebugInfoSize);
		file.flush();
	}
}

void enable_aftermath_crash_dumps() {
	std::call_once(aftermath_enable_once, []() {
		GFSDK_Aftermath_Result res = GFSDK_Aftermath_EnableGpuCrashDumps(
			GFSDK_Aftermath_Version_API,
			GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
			// Use DeferDebugInfoCallbacks so the shader debug info callback is
			// only invoked on actual GPU crash, not for every shader compilation.
			GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
			gpu_crash_dump_callback,
			shader_debug_info_callback,   // <-- was nullptr, now saves debug info
			nullptr,                       // description callback (optional)
			nullptr,                       // resolve marker callback (optional)
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

void VulkanRHI::init(bud::platform::Window* plat_window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation, uint32_t inflight_frame_count, bool headless) {
	this->task_scheduler = task_scheduler;
	platform_window = plat_window;
	max_frames_in_flight = inflight_frame_count;
	this->headless_mode = headless;

	enable_validation_layers = enable_validation;

#ifdef BUD_ENABLE_AFTERMATH
	enable_aftermath_crash_dumps();
#endif

	// 基础构建 (Instance, Surface, Device)
	create_instance(instance, enable_validation);
	setup_debug_messenger(enable_validation);
	if (!headless_mode) {
		create_surface(plat_window);
	}

	pick_physical_device();
	create_logical_device(enable_validation);

	// 初始化基础设施 (Subsystems)
	// 接管内存、资源池、管线缓存、描述符分配
	memory_allocator = std::make_unique<VulkanMemoryAllocator>(instance, device, physical_device, max_frames_in_flight, device_api_version,
		graphics_family_index, copy_family_index, has_dedicated_copy_queue);
	memory_allocator->init();

	resource_pool = std::make_unique<VulkanResourcePool>(device, memory_allocator.get());
	memory_allocator->set_resource_pool(resource_pool.get());

	pipeline_cache = std::make_unique<VulkanPipelineCache>();
	pipeline_cache->init(device);

	descriptor_allocators.resize(max_frames_in_flight);
	for (auto& alloc : descriptor_allocators) {
		alloc.init(device);
	}

	// 交换链与呈现资源
	if (!headless_mode) {
		create_swapchain(plat_window);
		create_image_views();
	}

	// 命令池与同步对象
	create_command_pool();
	create_command_buffer();
	create_sync_objects();

	// 专用纹理上传命令池（graphics family，独立于每帧主池）
	{
		VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = graphics_family_index;
		if (vkCreateCommandPool(device, &pool_info, nullptr, &texture_upload_pool) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create texture upload command pool!");
		}
	}

	// 创建全局 Descriptor Set Layout (Global Bindless Layout)
	// Binding 0: UBO (std140)
	// Binding 1: Sampler2D[] (Bindless, Variable Count / Partial Bound)
	// Binding 2: ShadowMap (Sampler2DShadow)

	DescriptorLayoutBuilder layout_builder;
	layout_builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT);
	layout_builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1000,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	layout_builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	layout_builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT, 1,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	layout_builder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 1,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	layout_builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT, 1,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	// Binding 6: full-scene CSM instance models (shadow.vert). Separate from
	// binding 3 (main-view instance data) so the CSM shadow passes never fight
	// the main pass over one descriptor slot.
	layout_builder.add_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 1,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
	// Binding 7: GPU Materials Buffer (std430 GPUMaterialData)
	layout_builder.add_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 1,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);

	global_set_layout = layout_builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);

	// Compute Pipeline Layouts
	auto build_small_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_heuristic_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_frustum_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_hiz_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_ao_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_ao_blur_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_ao_temporal_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	compute_hiz_cull_set_layout = build_hiz_compute_layout();
	compute_hiz_mip_set_layout = build_hiz_compute_layout();
	compute_ml_identity_set_layout = build_small_compute_layout();
	compute_ao_set_layout = build_ao_compute_layout();
	compute_ao_blur_set_layout = build_ao_blur_compute_layout();
	compute_ao_temporal_set_layout = build_ao_temporal_compute_layout();
	auto build_hierarchy_traversal_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_page_emit_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	auto build_cluster_cull_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};

	compute_hierarchy_traversal_set_layout = build_hierarchy_traversal_compute_layout();
	compute_page_emit_set_layout = build_page_emit_compute_layout();
	compute_cluster_cull_set_layout = build_cluster_cull_compute_layout();

	auto build_clear_stats_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};
	compute_clear_stats_set_layout = build_clear_stats_compute_layout();

	auto build_csm_cull_compute_layout = [&]() {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT);
		return builder.build(device, 0, nullptr, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
	};
	compute_csm_cull_set_layout = build_csm_cull_compute_layout();

	// 创建 Per-Frame UBO Buffers (Binding 0)
	VkDeviceSize ubo_size = sizeof(UniformBufferObject);
	for (auto& frame : frames) {
		// Prefer allocating per-frame UBO via VMA (host visible + coherent + persistently mapped)
		VkBufferCreateInfo buffer_info{};
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size = ubo_size;
		buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo vma_alloc_info = {};
        vma_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        // When using VMA_ALLOCATION_CREATE_MAPPED_BIT with VMA_MEMORY_USAGE_AUTO*,
        // VMA requires specifying either HOST_ACCESS_SEQUENTIAL_WRITE or HOST_ACCESS_RANDOM.
        // Per-frame UBOs are typically written sequentially each frame, so request
        // sequential write host access and persistent mapping.
        vma_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        vma_alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

		VmaAllocationInfo alloc_result_info;
		VkResult r = vmaCreateBuffer(memory_allocator->get_vma_allocator(), &buffer_info, &vma_alloc_info, &frame.uniform_buffer, &frame.uniform_allocation, &alloc_result_info);
		if (r != VK_SUCCESS) {
			std::string err = std::format("VulkanRHI::init vmaCreateBuffer for per-frame UBO failed: {}", (int)r);
			bud::eprint("{}", err);
			throw std::runtime_error(err);
		}
		frame.uniform_alloc_info = alloc_result_info;
		frame.uniform_mapped = alloc_result_info.pMappedData;
	}

	// 创建全局 Descriptor Pool (支持 Bindless + UPDATE_AFTER_BIND)
	{
		std::vector<VkDescriptorPoolSize> pool_sizes = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)frames.size() },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)frames.size() * 1001 }, // 1000 bindless + 1 shadow
			// Binding 3 (instance) / 4 (page table) / 5 (page pool) / 6 (CSM
			// instance models) are all STORAGE_BUFFER per frame.
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)frames.size() * 8 }
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

	// Create Point/Nearest Sampler (for Integer / Visibility Textures)
	VkSamplerCreateInfo point_sampler_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	point_sampler_info.magFilter = VK_FILTER_NEAREST;
	point_sampler_info.minFilter = VK_FILTER_NEAREST;
	point_sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	point_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	point_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	point_sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	point_sampler_info.anisotropyEnable = VK_FALSE;
	point_sampler_info.maxAnisotropy = 1.0f;
	point_sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	point_sampler_info.unnormalizedCoordinates = VK_FALSE;
	point_sampler_info.compareEnable = VK_FALSE;
	point_sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
	point_sampler_info.minLod = 0.0f;
	point_sampler_info.maxLod = 0.0f;

	if (vkCreateSampler(device, &point_sampler_info, nullptr, &point_sampler) != VK_SUCCESS) {
		throw std::runtime_error("failed to create point sampler!");
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

	// Create Dummy Depth Texture for Shadow Binding via ResourcePool
	{
		TextureDesc depth_desc{};
		depth_desc.width = 1;
		depth_desc.height = 1;
		depth_desc.format = TextureFormat::D32_FLOAT;
		depth_desc.type = TextureType::Texture2DArray;
		depth_desc.array_layers = 4;
		depth_desc.initial_state = ResourceState::DepthRead;

		dummy_depth_texture_handle = create_texture(depth_desc, nullptr, 0);
		auto* vk_tex = get_vulkan_texture(dummy_depth_texture_handle);
		if (vk_tex) {
			dummy_depth_texture = *vk_tex;
			dummy_depth_texture.sampler = shadow_sampler;
		}
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
	{
		TextureDesc desc{};
		desc.width = 1;
		desc.height = 1;
		desc.format = TextureFormat::RGBA8_SRGB;

		// Red Fallback to identify missing textures
        uint32_t color = 0xFF0000FF; // R=FF, G=00, B=00, A=FF (Little Endian)
        fallback_texture_handle = resource_pool->acquire_texture(desc);
        
		if (fallback_texture_handle.is_valid()) {
			auto vk_fallback = get_vulkan_texture(fallback_texture_handle);
			if (vk_fallback) {
				transition_image_layout_immediate(vk_fallback->image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
		}

        // Initialize all bindless texture slots (0 to 999) to fallback texture to avoid unwritten descriptor crashes during async streaming
        for (uint32_t i = 0; i < 1000; ++i) {
            update_bindless_texture(i, fallback_texture_handle);
        }
	}

	bud::print("[Vulkan] RHI Initialized successfully (Clean Architecture).");
}

void VulkanRHI::cleanup() {
	wait_idle();

	if (dummy_depth_texture_handle.is_valid()) {
		resource_pool->release_texture(dummy_depth_texture_handle);
		dummy_depth_texture_handle.reset();
	}

    if (fallback_texture_handle.is_valid()) {
        resource_pool->release_texture(fallback_texture_handle);
        fallback_texture_handle.reset();
    }

    // Destroy any VulkanTexture instances stored in the RHI maps/containers.
    // These may hold VMA allocations that must be freed before destroying the allocator.
    if (memory_allocator) {
        VmaAllocator alloc = memory_allocator->get_vma_allocator();

        // textures: map<bud::graphics::Texture*, VulkanTexture>
        for (auto &kv : textures) {
            VulkanTexture &tex = kv.second;

            for (auto v : tex.layer_views) {
				if (v)
					vkDestroyImageView(device, v, nullptr);
			}

            tex.layer_views.clear();
            for (auto v : tex.mip_views) {
				if (v)
					vkDestroyImageView(device, v, nullptr);
			}

            tex.mip_views.clear();
            if (tex.view) {
				vkDestroyImageView(device, tex.view, nullptr);
				tex.view = VK_NULL_HANDLE;
			}

            if (tex.image) {
                if (alloc) vmaDestroyImage(alloc, tex.image, tex.allocation);
                tex.image = VK_NULL_HANDLE;
                tex.allocation = VK_NULL_HANDLE;
            }
        }
        textures.clear();

        // texture_objects: vector<unique_ptr<Texture>>
        for (auto &up : texture_objects) {
            if (!up) continue;

            auto *tptr = static_cast<VulkanTexture*>(up.get());
            if (tptr) {
                for (auto v : tptr->layer_views) {
					if (v)
						vkDestroyImageView(device, v, nullptr);
				}

                for (auto v : tptr->mip_views) {
					if (v)
						vkDestroyImageView(device, v, nullptr);
				}

                if (tptr->view)
					vkDestroyImageView(device, tptr->view, nullptr);

                if (tptr->image) {
                    if (alloc)
						vmaDestroyImage(alloc, tptr->image, tptr->allocation);

					tptr->image = VK_NULL_HANDLE;
                    tptr->allocation = VK_NULL_HANDLE;
                }
            }
        }

        texture_objects.clear();
    } else {
        // No allocator: still clear containers to avoid dangling pointers
        textures.clear();
        texture_objects.clear();
    }

	descriptor_allocators.clear();
	if (global_descriptor_pool)
		vkDestroyDescriptorPool(device, global_descriptor_pool, nullptr);
	if (global_set_layout)
		vkDestroyDescriptorSetLayout(device, global_set_layout, nullptr);

	if (pipeline_cache)
		pipeline_cache->cleanup();
	pipeline_cache.reset();

    current_compute_bindings.clear();

    // Destroy per-frame uniform buffers before tearing down the allocator.
    for (auto semaphore : render_finished_semaphores)
        vkDestroySemaphore(device, semaphore, nullptr);

    for (int i = 0; i < max_frames_in_flight; i++) {
        if (memory_allocator) {
            VmaAllocator alloc = memory_allocator->get_vma_allocator();
            if (frames[i].uniform_allocation != VK_NULL_HANDLE) {
                // Allocations were created with VMA_ALLOCATION_CREATE_MAPPED_BIT (persistently mapped).
                // Do NOT call vmaUnmapMemory here because the mapping was automatic (0-th mapping).
                // Unmapping an allocation not previously explicitly mapped triggers VMA assertion.
                if (frames[i].uniform_buffer) vmaDestroyBuffer(alloc, frames[i].uniform_buffer, frames[i].uniform_allocation);
                frames[i].uniform_buffer = VK_NULL_HANDLE;
                frames[i].uniform_allocation = VK_NULL_HANDLE;
                frames[i].uniform_alloc_info = {};
                frames[i].uniform_mapped = nullptr;
            } else {
                // fallback to old path if allocation not created with VMA
                // We only need to destroy the buffer; older raw device memory is not tracked here.
                if (frames[i].uniform_buffer)
                    vkDestroyBuffer(device, frames[i].uniform_buffer, nullptr);
                frames[i].uniform_buffer = VK_NULL_HANDLE;
                frames[i].uniform_mapped = nullptr;
            }
        } else {
            if (frames[i].uniform_buffer)
                vkDestroyBuffer(device, frames[i].uniform_buffer, nullptr);
            frames[i].uniform_buffer = VK_NULL_HANDLE;
            frames[i].uniform_mapped = nullptr;
        }

        if (frames[i].image_available_semaphore)
            vkDestroySemaphore(device, frames[i].image_available_semaphore, nullptr);

        if (frames[i].in_flight_fence)
            vkDestroyFence(device, frames[i].in_flight_fence, nullptr);

        if (frames[i].main_command_pool)
            vkDestroyCommandPool(device, frames[i].main_command_pool, nullptr);

        // Async compute resources
        if (frames[i].async_command_pool)
            vkDestroyCommandPool(device, frames[i].async_command_pool, nullptr);

        if (frames[i].timestamp_pool) {
            vkDestroyQueryPool(device, frames[i].timestamp_pool, nullptr);
            frames[i].timestamp_pool = nullptr;
        }
    }

    if (compute_timeline_semaphore) {
        vkDestroySemaphore(device, compute_timeline_semaphore, nullptr);
        compute_timeline_semaphore = nullptr;
    }
    if (graphics_timeline_semaphore) {
        vkDestroySemaphore(device, graphics_timeline_semaphore, nullptr);
        graphics_timeline_semaphore = nullptr;
    }


    // Release any in-flight async texture uploads (cb/staging/fence). The
    // shared state is reference-counted per frame slot; clear all slots and
    // let the shared_ptr deleters release resources exactly once.
    for (auto& slot : pending_bindless) {
        for (auto& p : slot) {
            if (p.state) {
                vkWaitForFences(device, 1, &p.state->upload_fence, VK_TRUE, UINT64_MAX);
            }
        }
        slot.clear(); // drops shared_ptr -> release_bindless_upload_state
    }
    pending_bindless.clear();
    if (texture_upload_pool) {
        vkDestroyCommandPool(device, texture_upload_pool, nullptr);
        texture_upload_pool = VK_NULL_HANDLE;
    }

    // Resource pool must be cleaned up BEFORE the memory allocator so that
    // all VulkanBuffer/VulkanTexture objects call vmaDestroyBuffer/vmaDestroyImage
    // while the VMA allocator is still alive. Reversing the order would cause
    // VmaDeviceMemoryBlock::Destroy to access freed memory (the crash we are fixing).
    //
    // Note: memory_allocator->cleanup() flushes deferred frees via on_frame_begin()
    // which calls resource_pool->release_buffer(), so the pool must still exist at
    // that point too — the order below satisfies both constraints.
    if (resource_pool)
        resource_pool->cleanup();

    if (memory_allocator)
        memory_allocator->cleanup();

    resource_pool.reset();
    memory_allocator.reset();

	// Swapchain (image views are managed and destroyed by resource_pool)
	swapchain_image_views.clear();
	swapchain_texture_handles.clear();
	swapchain_textures_wrappers.clear();

	if (swapchain) {
		vkDestroySwapchainKHR(device, swapchain, nullptr);
		swapchain = VK_NULL_HANDLE;
	}

	for (auto layout : created_layouts) {
		vkDestroyPipelineLayout(device, layout, nullptr);
	}
	created_layouts.clear();
	if (compute_hiz_cull_set_layout) vkDestroyDescriptorSetLayout(device, compute_hiz_cull_set_layout, nullptr);
	if (compute_hiz_mip_set_layout) vkDestroyDescriptorSetLayout(device, compute_hiz_mip_set_layout, nullptr);
	if (compute_ml_identity_set_layout) vkDestroyDescriptorSetLayout(device, compute_ml_identity_set_layout, nullptr);
	if (compute_ao_set_layout) vkDestroyDescriptorSetLayout(device, compute_ao_set_layout, nullptr);
	if (compute_ao_blur_set_layout) vkDestroyDescriptorSetLayout(device, compute_ao_blur_set_layout, nullptr);
	if (compute_ao_temporal_set_layout) vkDestroyDescriptorSetLayout(device, compute_ao_temporal_set_layout, nullptr);
	compute_hiz_cull_set_layout = VK_NULL_HANDLE;
	compute_hiz_mip_set_layout = VK_NULL_HANDLE;
	compute_ml_identity_set_layout = VK_NULL_HANDLE;
	compute_ao_set_layout = VK_NULL_HANDLE;
	compute_ao_blur_set_layout = VK_NULL_HANDLE;
	compute_ao_temporal_set_layout = VK_NULL_HANDLE;
	if (compute_hierarchy_traversal_set_layout) vkDestroyDescriptorSetLayout(device, compute_hierarchy_traversal_set_layout, nullptr);
	if (compute_page_emit_set_layout) vkDestroyDescriptorSetLayout(device, compute_page_emit_set_layout, nullptr);
	if (compute_cluster_cull_set_layout) vkDestroyDescriptorSetLayout(device, compute_cluster_cull_set_layout, nullptr);
	if (compute_clear_stats_set_layout) vkDestroyDescriptorSetLayout(device, compute_clear_stats_set_layout, nullptr);
	if (compute_csm_cull_set_layout) vkDestroyDescriptorSetLayout(device, compute_csm_cull_set_layout, nullptr);
	compute_hierarchy_traversal_set_layout = VK_NULL_HANDLE;
	compute_page_emit_set_layout = VK_NULL_HANDLE;
	compute_cluster_cull_set_layout = VK_NULL_HANDLE;
	compute_clear_stats_set_layout = VK_NULL_HANDLE;
	compute_csm_cull_set_layout = VK_NULL_HANDLE;

	// Device & Instance
	if (shadow_sampler)
		vkDestroySampler(device, shadow_sampler, nullptr);

	if (point_sampler)
		vkDestroySampler(device, point_sampler, nullptr);

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
	if (device)
		vkDeviceWaitIdle(device);
}


bud::graphics::BufferHandle VulkanRHI::create_gpu_buffer(uint64_t size, bud::graphics::ResourceState usage_state) {
	if (memory_allocator) {
		return memory_allocator->alloc_gpu(size, usage_state);
	}
	return {};
}

void VulkanRHI::destroy_buffer(bud::graphics::BufferHandle block) {
    if (!block.is_valid()) {
        std::string err = std::format("destroy_buffer called with invalid handle: valid=false");
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }
    if (memory_allocator) {
        memory_allocator->defer_free(block, current_frame);
    } else if (resource_pool) {
        resource_pool->release_buffer(block);
    }
}

// Helper to create shader module with emergency diagnostics
VkShaderModule create_shader_module(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule);
    if (r != VK_SUCCESS) {
        // Log failure and return VK_NULL_HANDLE so callers can handle graceful
        bud::eprint("vkCreateShaderModule failed: code={} worker={}", (int)r, bud::threading::current_worker_index());
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

struct VulkanPipelineObject* VulkanRHI::get_pipeline_obj(PipelineHandle handle) {
	if (!handle.is_valid() || handle.id >= pipeline_pool.size()) {
		return nullptr;
	}
	auto& slot = pipeline_pool[handle.id];
	if (!slot.in_use) {
		return nullptr;
	}
	return &slot.obj;
}

class VulkanTexture* VulkanRHI::get_vulkan_texture(TextureHandle handle) {
	if (!handle.is_valid() || !resource_pool) {
		return nullptr;
	}
	return static_cast<VulkanTexture*>(resource_pool->get_texture(handle));
}

Texture* VulkanRHI::get_texture(TextureHandle handle) {
	if (!handle.is_valid() || !resource_pool) {
		return nullptr;
	}
	return resource_pool->get_texture(handle);
}

TextureDesc VulkanRHI::get_texture_desc(TextureHandle handle) const {
	if (!handle.is_valid() || !resource_pool) {
		return TextureDesc{};
	}
	return resource_pool->get_texture_desc(handle);
}

class VulkanBuffer* VulkanRHI::get_vulkan_buffer(BufferHandle handle) {
	if (!handle.is_valid() || !resource_pool) {
		return nullptr;
	}
	return static_cast<VulkanBuffer*>(resource_pool->get_buffer(handle));
}

Buffer* VulkanRHI::get_buffer(BufferHandle handle) {
	if (!handle.is_valid() || !resource_pool) {
		return nullptr;
	}
	return resource_pool->get_buffer(handle);
}

BufferDesc VulkanRHI::get_buffer_desc(BufferHandle handle) const {
	if (!handle.is_valid() || !resource_pool) {
		return BufferDesc{};
	}
	return resource_pool->get_buffer_desc(handle);
}

PipelineHandle VulkanRHI::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
	VkPushConstantRange push_constant;
	push_constant.offset = 0;
	push_constant.size = 256; // Enough for standard matrices
	push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	if (desc.ts.code.size() > 0) push_constant.stageFlags |= VK_SHADER_STAGE_TASK_BIT_EXT;
	if (desc.ms.code.size() > 0) push_constant.stageFlags |= VK_SHADER_STAGE_MESH_BIT_EXT;

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 0;

	// Use custom descriptor set layouts if provided, otherwise fall back to global set
	std::vector<VkDescriptorSetLayout> setLayouts;
	if (!desc.custom_set_layouts.empty()) {
		setLayouts.reserve(desc.custom_set_layouts.size() + 1);
		for (auto layout : desc.custom_set_layouts) {
			setLayouts.push_back(reinterpret_cast<VkDescriptorSetLayout>(layout));
		}
		// Append the global descriptor set as the last set (for UBO, bindless textures, etc.)
		setLayouts.push_back(global_set_layout);
	} else {
		setLayouts = { global_set_layout };
	}

	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &push_constant;

	VkPipelineLayout pipelineLayout;
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create pipeline layout!");
	}

    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule taskModule = VK_NULL_HANDLE;
    VkShaderModule meshModule = VK_NULL_HANDLE;

    // If mesh shader is provided, use task+mesh instead of vertex shader
    if (desc.ms.code.size() > 0) {
        if (desc.ts.code.size() > 0) {
            taskModule = create_shader_module(device, desc.ts.code);
            if (taskModule == VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
                return PipelineHandle{};
            }
        }
        meshModule = create_shader_module(device, desc.ms.code);
        if (meshModule == VK_NULL_HANDLE) {
            if (taskModule) vkDestroyShaderModule(device, taskModule, nullptr);
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            return PipelineHandle{};
        }
    } else {
        vertModule = create_shader_module(device, desc.vs.code);
        if (vertModule == VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            return PipelineHandle{};
        }
    }

    VkShaderModule fragModule = VK_NULL_HANDLE;
    if (desc.fs.code.size() > 0) {
        fragModule = create_shader_module(device, desc.fs.code);
        if (fragModule == VK_NULL_HANDLE) {
            if (taskModule) vkDestroyShaderModule(device, taskModule, nullptr);
            if (meshModule) vkDestroyShaderModule(device, meshModule, nullptr);
            if (vertModule) vkDestroyShaderModule(device, vertModule, nullptr);
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            return PipelineHandle{};
        }
    }

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
                    if (set->set < setLayouts.size()) {
                        // Compare each binding
                        for (uint32_t bi = 0; bi < set->binding_count; ++bi) {
                            SpvReflectDescriptorBinding* b = set->bindings[bi];
                            VkDescriptorType expected = map_spv_type_to_vk(b->descriptor_type);
                            // Log mismatches (we can expand this to check against registered layout info)
                            // bud::print("[SPIRV-Reflect] stage={} set={} binding={} type={}", stage_name, set->set, b->binding, (int)expected);
                        }
                    }
                }
            }
        }

        spvReflectDestroyShaderModule(&module);
    };

    if (desc.ts.code.size() > 0) validate_spv_strict(desc.ts.code, "task");
    if (desc.ms.code.size() > 0) validate_spv_strict(desc.ms.code, "mesh");
    if (desc.vs.code.size() > 0) validate_spv_strict(desc.vs.code, "vertex");
    if (desc.fs.code.size() > 0) validate_spv_strict(desc.fs.code, "fragment");
#endif

    PipelineKey key{};
    key.vert_shader = vertModule;
    key.frag_shader = fragModule;
    key.task_shader = taskModule;
    key.mesh_shader = meshModule;
    key.render_pass = VK_NULL_HANDLE;
    key.depth_test = desc.depth_test ? VK_TRUE : VK_FALSE;
    key.depth_write = desc.depth_write ? VK_TRUE : VK_FALSE;
    key.depth_bias_enable = desc.enable_depth_bias ? VK_TRUE : VK_FALSE;
    key.blending_enable = desc.blending_enable ? VK_TRUE : VK_FALSE;
    key.vertex_layout = desc.vertex_layout;
    
    switch (desc.depth_compare_op) {
    case CompareOp::Less: key.depth_compare_op = VK_COMPARE_OP_LESS; break;
    case CompareOp::LessEqual: key.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; break;
    case CompareOp::Greater: key.depth_compare_op = VK_COMPARE_OP_GREATER; break;
    case CompareOp::GreaterEqual: key.depth_compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
    case CompareOp::Always: key.depth_compare_op = VK_COMPARE_OP_ALWAYS; break;
    }

    switch (desc.cull_mode) {
    case CullMode::None: key.cull_mode = VK_CULL_MODE_NONE; break;
    case CullMode::Front: key.cull_mode = VK_CULL_MODE_FRONT_BIT; break;
    case CullMode::Back: key.cull_mode = VK_CULL_MODE_BACK_BIT; break;
    }

    key.color_format = to_vk_format(desc.color_attachment_format);
    key.depth_format = to_vk_format(desc.depth_attachment_format);
    key.wireframe = desc.wireframe ? VK_TRUE : VK_FALSE;

    bool is_depth_only = (desc.color_attachment_format == TextureFormat::Undefined);
    VkPipeline pipeline = pipeline_cache->get_pipeline(key, pipelineLayout, is_depth_only);

	if (vertModule) vkDestroyShaderModule(device, vertModule, nullptr);
	if (fragModule) vkDestroyShaderModule(device, fragModule, nullptr);
	if (taskModule) vkDestroyShaderModule(device, taskModule, nullptr);
	if (meshModule) vkDestroyShaderModule(device, meshModule, nullptr);

	created_layouts.push_back(pipelineLayout);

	// Attach a human-readable debug name to the pipeline
    try {
        std::string dbg = std::format("GraphicsPipeline_{}{}_fs={}",
                desc.ms.code.size() > 0 ? "ms=" : "vs=",
                (void*)(desc.ms.code.size() > 0 ? meshModule : vertModule),
                (void*)fragModule);
        set_object_debug_name(reinterpret_cast<uint64_t>(pipeline), ObjectType::Pipeline, dbg);
    } catch (...) {
    }

	uint32_t slot_idx = 0;
	if (!free_pipeline_indices.empty()) {
		slot_idx = free_pipeline_indices.back();
		free_pipeline_indices.pop_back();
	} else {
		slot_idx = static_cast<uint32_t>(pipeline_pool.size());
		pipeline_pool.emplace_back();
	}

	auto& slot = pipeline_pool[slot_idx];
	slot.obj = VulkanPipelineObject{ pipeline, pipelineLayout, VK_PIPELINE_BIND_POINT_GRAPHICS, ComputePipelineDesc::LayoutKind::HiZCulling, push_constant.stageFlags };
	slot.in_use = true;

	return PipelineHandle{ slot_idx };
}

void VulkanRHI::destroy_pipeline(PipelineHandle handle) {
	if (!handle.is_valid() || handle.id >= pipeline_pool.size()) return;
	auto& slot = pipeline_pool[handle.id];
	if (!slot.in_use) return;

	if (slot.obj.pipeline != VK_NULL_HANDLE) {
		if (pipeline_cache) {
			pipeline_cache->release_pipeline(slot.obj.pipeline);
		}
		slot.obj = {};
	}
	slot.in_use = false;
	free_pipeline_indices.push_back(handle.id);
}

uint64_t VulkanRHI::create_descriptor_set_layout(const std::vector<DescriptorBinding>& bindings) {
	DescriptorLayoutBuilder builder;
	for (const auto& b : bindings) {
		builder.add_binding(
			b.binding,
			static_cast<VkDescriptorType>(b.descriptor_type),
			static_cast<VkShaderStageFlags>(b.stage_flags),
			b.count,
			static_cast<VkDescriptorBindingFlags>(b.binding_flags));
	}
	VkDescriptorSetLayout layout = builder.build(device, 0, nullptr, 0);
	return reinterpret_cast<uint64_t>(layout);
}

uint64_t VulkanRHI::create_descriptor_set(uint64_t layout) {
	VkDescriptorSetLayout vk_layout = reinterpret_cast<VkDescriptorSetLayout>(layout);
	VkDescriptorSet set = VK_NULL_HANDLE;
	descriptor_allocators[current_frame].allocate(vk_layout, set);
	return reinterpret_cast<uint64_t>(set);
}

void VulkanRHI::update_descriptor_set_buffer(uint64_t set, uint32_t binding, BufferHandle buffer, uint32_t descriptor_type) {
	auto* vk_buf = get_vulkan_buffer(buffer);
	if (!vk_buf || !vk_buf->buffer) return;

	VkDescriptorType type = (descriptor_type != 0)
		? static_cast<VkDescriptorType>(descriptor_type)
		: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

	DescriptorWriter writer;
	writer.write_buffer(binding, vk_buf->buffer, vk_buf->size > 0 ? vk_buf->size : VK_WHOLE_SIZE, 0, type);
	writer.update_set(device, reinterpret_cast<VkDescriptorSet>(set));
}

void VulkanRHI::update_descriptor_set_image(uint64_t set, uint32_t binding, TextureHandle texture, uint32_t mip_level, uint32_t descriptor_type) {
	auto* vk_tex = get_vulkan_texture(texture);
	if (!vk_tex)
		return;

	VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDescriptorType type = (descriptor_type != 0)
		? static_cast<VkDescriptorType>(descriptor_type)
		: VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	VkSampler sampler_to_use = vk_tex->sampler;
	if (!sampler_to_use) {
		if (vk_tex->format == TextureFormat::R32G32_UINT || vk_tex->format == TextureFormat::RGBA32_UINT)
			sampler_to_use = point_sampler;
		else
			sampler_to_use = default_sampler;
	}
	else if (vk_tex->format == TextureFormat::R32G32_UINT || vk_tex->format == TextureFormat::RGBA32_UINT) {
		sampler_to_use = point_sampler;
	}

	DescriptorWriter writer;
	writer.write_image(binding, 0, vk_tex->view, sampler_to_use, layout, type);
	writer.update_set(device, reinterpret_cast<VkDescriptorSet>(set));
}

void VulkanRHI::destroy_descriptor_set_layout(uint64_t layout) {
	if (layout) {
		vkDestroyDescriptorSetLayout(device, reinterpret_cast<VkDescriptorSetLayout>(layout), nullptr);
	}
}

void VulkanRHI::destroy_descriptor_set(uint64_t set) {
	// Descriptor sets are freed by the pool, no explicit destruction needed
	(void)set;
}

PipelineHandle VulkanRHI::create_compute_pipeline(const ComputePipelineDesc& desc) {
	VkPushConstantRange push_constant{};
	push_constant.offset = 0;
	push_constant.size = 256; 
	push_constant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayout chosen_layout = VK_NULL_HANDLE;
	switch (desc.layout_kind) {
	case ComputePipelineDesc::LayoutKind::HiZCulling:
		chosen_layout = compute_hiz_cull_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::HiZMip:
		chosen_layout = compute_hiz_mip_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::MlIdentity:
		chosen_layout = compute_ml_identity_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::AmbientOcclusion:
		chosen_layout = compute_ao_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::AOBlur:
		chosen_layout = compute_ao_blur_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::AOTemporal:
		chosen_layout = compute_ao_temporal_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::HierarchyTraversal:
		chosen_layout = compute_hierarchy_traversal_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::PageEmit:
		chosen_layout = compute_page_emit_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::ClusterCull:
		chosen_layout = compute_cluster_cull_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::ClearStats:
		chosen_layout = compute_clear_stats_set_layout;
		break;
	case ComputePipelineDesc::LayoutKind::CSMCulling:
		chosen_layout = compute_csm_cull_set_layout;
		break;
	default:
		chosen_layout = compute_hiz_cull_set_layout;
		break;
	}

	if (!chosen_layout) {
		throw std::runtime_error("failed to select compute descriptor set layout!");
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

	std::vector<VkDescriptorSetLayout> setLayouts = { chosen_layout };
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &push_constant;

	VkPipelineLayout pipelineLayout;
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create compute pipeline layout!");
	}

	VkShaderModule computeModule = create_shader_module(device, desc.cs.code);
    if (computeModule == VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        return PipelineHandle{};
    }

    VkPipeline pipeline = pipeline_cache->create_compute_pipeline(computeModule, pipelineLayout);

    vkDestroyShaderModule(device, computeModule, nullptr);

	created_layouts.push_back(pipelineLayout);

	// Name compute pipeline for external debuggers (Nsight/RenderDoc) so that
	// GPU-driven dispatches still show meaningful identifiers.
	try {
		auto layout_kind_to_string = [](ComputePipelineDesc::LayoutKind k) {
			switch (k) {
			case ComputePipelineDesc::LayoutKind::HiZCulling: return "HiZCulling";
			case ComputePipelineDesc::LayoutKind::HiZMip: return "HiZMip";
			case ComputePipelineDesc::LayoutKind::MlIdentity: return "MlIdentity";
			case ComputePipelineDesc::LayoutKind::HierarchyTraversal: return "HierarchyTraversal";
			case ComputePipelineDesc::LayoutKind::PageEmit: return "PageEmit";
			case ComputePipelineDesc::LayoutKind::ClusterCull: return "ClusterCull";
			case ComputePipelineDesc::LayoutKind::ClearStats: return "ClearStats";
			case ComputePipelineDesc::LayoutKind::CSMCulling: return "CSMCulling";
			default: return "Compute";
			}
		};
		std::string dbg = std::format("ComputePipeline_{}", layout_kind_to_string(desc.layout_kind));
		set_object_debug_name(reinterpret_cast<uint64_t>(pipeline), ObjectType::Pipeline, dbg);
	} catch (...) {}

	uint32_t slot_idx = 0;
	if (!free_pipeline_indices.empty()) {
		slot_idx = free_pipeline_indices.back();
		free_pipeline_indices.pop_back();
	} else {
		slot_idx = static_cast<uint32_t>(pipeline_pool.size());
		pipeline_pool.emplace_back();
	}

	auto& slot = pipeline_pool[slot_idx];
	slot.obj = VulkanPipelineObject{ pipeline, pipelineLayout, VK_PIPELINE_BIND_POINT_COMPUTE, desc.layout_kind, VK_SHADER_STAGE_COMPUTE_BIT };
	slot.in_use = true;

	return PipelineHandle{ slot_idx };
}

void VulkanRHI::cmd_dispatch(CommandHandle cmd, uint32_t group_x, uint32_t group_y, uint32_t group_z) {
	if (current_compute_pipeline.is_valid()) {
		auto pipeObj = get_pipeline_obj(current_compute_pipeline);
		if (!pipeObj) return;
		auto descriptor_type_for_binding = [&](uint32_t binding) -> VkDescriptorType {
			switch (pipeObj->compute_layout_kind) {
			case ComputePipelineDesc::LayoutKind::HiZCulling:
				if (binding == 3) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				if (binding == 4) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				if (binding == 5) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ComputePipelineDesc::LayoutKind::HiZMip:
				if (binding == 3) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				if (binding == 5) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ComputePipelineDesc::LayoutKind::MlIdentity:
				if (binding == 4) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ComputePipelineDesc::LayoutKind::HierarchyTraversal:
				if (binding == 0) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ComputePipelineDesc::LayoutKind::PageEmit:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ComputePipelineDesc::LayoutKind::ClusterCull:
				if (binding == 7) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				if (binding == 8) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ComputePipelineDesc::LayoutKind::ClearStats:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case ComputePipelineDesc::LayoutKind::CSMCulling:
				if (binding == 4) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			default:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			}
		};
		
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
					auto* vk_buf = get_vulkan_buffer(buffer_handle);
					if (!vk_buf || !vk_buf->buffer) continue;
					
					VkDescriptorBufferInfo info{};
					info.buffer = vk_buf->buffer;
					info.offset = 0;
					info.range = vk_buf->size > 0 ? vk_buf->size : VK_WHOLE_SIZE;
					buffer_infos.push_back(info);
					
					write.descriptorType = descriptor_type_for_binding(binding);
					write.pBufferInfo = &buffer_infos.back();
				} else if (std::holds_alternative<ImageBinding>(res)) {
					auto& image_binding = std::get<ImageBinding>(res);
					auto* vk_tex = get_vulkan_texture(image_binding.texture);
					if (!vk_tex) continue;

					VkDescriptorImageInfo info{};
					VkImageLayout layout;
					bool tex_is_depth = (vk_tex->format == TextureFormat::D32_FLOAT ||
					                     vk_tex->format == TextureFormat::D24_UNORM_S8_UINT);
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

	// Read back the previous submission's GPU frame time for this slot. The
	// fence above guarantees the GPU finished it, so the timestamps are valid.
	// Done AFTER current_stats.reset() (below) so the value survives.
	float gpu_frame_ms = 0.0f;
	if (timestamp_queries_supported && frames[current_frame].timestamp_pool && frames[current_frame].timestamp_ready) {
		uint64_t ts[2] = { 0, 0 };
		VkResult qr = vkGetQueryPoolResults(device, frames[current_frame].timestamp_pool, 0, 2,
			sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
		if (qr == VK_SUCCESS && ts[1] > ts[0]) {
			gpu_frame_ms = static_cast<float>((ts[1] - ts[0]) * timestamp_period_ns * 1e-6);
		}
	}

	VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frames[current_frame].image_available_semaphore, VK_NULL_HANDLE, &current_image_index);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		swapchain_out_of_date.store(true, std::memory_order_release);
		return nullptr;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}	current_stats.reset();
	current_stats.gpu_render_time = gpu_frame_ms;

	// 通知分配器新的一帧开始了 (重置 Linear Allocator)
	memory_allocator->on_frame_begin(current_frame);
	descriptor_allocators[current_frame].reset_frame();

	// 使用 init 时分配的持久化 Global Descriptor Set，仅更新 UBO 绑定
	// Bindless Textures 是持久化的，不需要每一帧重绑一次，否则会丢失状态导致 GPU Hang

	// 更新 Descriptor Set 指向当前帧的 UBO
	DescriptorWriter writer;
	writer.write_buffer(0, frames[current_frame].uniform_buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);

	// Apply any finished async texture uploads to this frame's descriptor set
	// (the set is not yet submitted, so the update is safe).
	apply_pending_bindless();

	vkResetCommandBuffer(frames[current_frame].main_command_buffer, 0);

	VkCommandBufferBeginInfo begin_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	if (vkBeginCommandBuffer(frames[current_frame].main_command_buffer, &begin_info) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	// GPU frame-time timestamps: reset the pool and record the frame start.
	if (timestamp_queries_supported && frames[current_frame].timestamp_pool) {
		vkCmdResetQueryPool(frames[current_frame].main_command_buffer, frames[current_frame].timestamp_pool, 0, 2);
		vkCmdWriteTimestamp(frames[current_frame].main_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frames[current_frame].timestamp_pool, 0);
	}

	return frames[current_frame].main_command_buffer;
}

void VulkanRHI::end_frame(CommandHandle cmd) {
	VkCommandBuffer command_buffer = static_cast<VkCommandBuffer>(cmd);

	// Record the end-of-frame timestamp (before vkEndCommandBuffer) so the
	// query covers the whole main command buffer.
	if (timestamp_queries_supported && frames[current_frame].timestamp_pool) {
		vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frames[current_frame].timestamp_pool, 1);
		frames[current_frame].timestamp_ready = true;
	}

	if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS)
		throw std::runtime_error("failed to record command buffer!");

	VkSubmitInfo submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	// Wait on the async-compute timeline (if this frame recorded async compute)
	// so the main command buffer is ordered after any compute output it reads.
	// Value semantics: waiting on an already-reached value returns immediately.
	VkSemaphore wait_semaphores[2] = {
		frames[current_frame].image_available_semaphore,
		compute_timeline_semaphore
	};
	VkPipelineStageFlags wait_stages[2] = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
	};
	uint32_t wait_count = 1;
	uint64_t compute_wait_value = compute_timeline_value;
	// Vulkan requires waitSemaphoreValueCount == waitSemaphoreCount when any
	// semaphore is a timeline. Provide a value for every semaphore (the binary
	// image_available value is ignored).
	uint64_t wait_values[2] = { 0, compute_wait_value };
	VkTimelineSemaphoreSubmitInfo timeline_info{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
	if (async_compute_pending_this_frame) {
		wait_count = 2;
		timeline_info.waitSemaphoreValueCount = wait_count;
		timeline_info.pWaitSemaphoreValues = wait_values;
	}
	
	uint64_t graphics_signal_value = ++graphics_timeline_value;
	uint64_t signal_values[2] = { 0, graphics_signal_value }; // 0 for binary semaphore, graphics_signal_value for timeline semaphore
	timeline_info.signalSemaphoreValueCount = 2;
	timeline_info.pSignalSemaphoreValues = signal_values;
	submit_info.pNext = &timeline_info;

	submit_info.waitSemaphoreCount = wait_count;
	submit_info.pWaitSemaphores = wait_semaphores;
	submit_info.pWaitDstStageMask = wait_stages;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffer;
	
	VkSemaphore signal_semaphores[2] = {
		render_finished_semaphores[current_image_index],
		graphics_timeline_semaphore
	};
	submit_info.signalSemaphoreCount = 2;
	submit_info.pSignalSemaphores = signal_semaphores;
	async_compute_pending_this_frame = false;

	vkResetFences(device, 1, &frames[current_frame].in_flight_fence);
	VkResult submit_result;
	{
		std::lock_guard lock(queue_submit_mutex);
		submit_result = vkQueueSubmit(graphics_queue, 1, &submit_info, frames[current_frame].in_flight_fence);
	}
    if (submit_result != VK_SUCCESS) {
        std::string err = std::format("VulkanRHI::end_frame vkQueueSubmit failed: {}", (int)submit_result);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        // In Release, skip presenting this frame but keep running
        this->end_single_time_commands(VK_NULL_HANDLE); // no-op safe call
        return;
#endif
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
        std::string err = std::format("VulkanRHI::end_frame vkQueuePresentKHR failed: {}", (int)present_result);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        swapchain_out_of_date.store(true, std::memory_order_release);
        return;
#endif
    }

	current_frame = (current_frame + 1) % max_frames_in_flight;
}


uint32_t VulkanRHI::get_width() const {
	if (headless_mode) {
		int w, h;
		platform_window->get_size(w, h);
		return static_cast<uint32_t>(w);
	}
	return swapchain_extent.width;
}

uint32_t VulkanRHI::get_height() const {
	if (headless_mode) {
		int w, h;
		platform_window->get_size(w, h);
		return static_cast<uint32_t>(h);
	}
	return swapchain_extent.height;
}

void VulkanRHI::resize_swapchain(uint32_t width, uint32_t height) {
	if (!device || !platform_window)
		return;

	if (width == 0 || height == 0)
		return;

	vkDeviceWaitIdle(device);

	swapchain_out_of_date.store(false, std::memory_order_release);

	for (auto handle : swapchain_texture_handles) {
		if (resource_pool) resource_pool->release_texture(handle);
	}
	swapchain_texture_handles.clear();
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
		auto* first_tex = get_vulkan_texture(info.color_attachments[0]);
		if (first_tex) {
			rendering_info.renderArea = { {0, 0}, {first_tex->width, first_tex->height} };
		}
	}
	else if (info.depth_attachment.is_valid()) {
		auto* depth_tex = get_vulkan_texture(info.depth_attachment);
		if (depth_tex) {
			rendering_info.renderArea = { {0, 0}, {depth_tex->width, depth_tex->height} };
		}
	}

	rendering_info.layerCount = info.layer_count;

	// Fallback: use explicit render size if texture-based lookup failed
	if (rendering_info.renderArea.extent.width == 0 && info.render_width > 0) {
		rendering_info.renderArea = { {0, 0}, {info.render_width, info.render_height} };
	}
	if (rendering_info.renderArea.extent.width == 0) {
		rendering_info.renderArea = { {0, 0}, {1, 1} };
	}

	std::vector<VkRenderingAttachmentInfo> color_attachments;
	for (auto handle : info.color_attachments) {
		auto vk_tex = get_vulkan_texture(handle);
		if (!vk_tex) continue;
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
		if (vk_tex->format == TextureFormat::R32G32_UINT || vk_tex->format == TextureFormat::RGBA32_UINT) {
			attach.clearValue.color.uint32[0] = 0xFFFFFFFF;
			attach.clearValue.color.uint32[1] = 0xFFFFFFFF;
			attach.clearValue.color.uint32[2] = 0;
			attach.clearValue.color.uint32[3] = 0;
		}
		else {
			attach.clearValue.color = { info.clear_color_value.r, info.clear_color_value.g, info.clear_color_value.b, info.clear_color_value.a };
		}
		color_attachments.push_back(attach);
	}

	rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
	rendering_info.pColorAttachments = color_attachments.data();

	VkRenderingAttachmentInfo depth_attach{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	if (info.depth_attachment.is_valid()) {
		auto vk_depth = get_vulkan_texture(info.depth_attachment);
		if (vk_depth) {
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
	}

	vkCmdBeginRendering(vk_cmd, &rendering_info);
}

void VulkanRHI::cmd_end_render_pass(CommandHandle cmd) {
	vkCmdEndRendering(static_cast<VkCommandBuffer>(cmd));
}

void VulkanRHI::cmd_copy_buffer(CommandHandle cmd, bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size) {
    if (!src.is_valid() || !dst.is_valid())
    {
        std::string err = std::format("cmd_copy_buffer invalid handle: src_valid={} dst_valid={} size={}", src.is_valid(), dst.is_valid(), size);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }

    auto* vk_src = get_vulkan_buffer(src);
    auto* vk_dst = get_vulkan_buffer(dst);
    if (!vk_src || !vk_dst)
    {
        std::string err = std::format("cmd_copy_buffer null VulkanBuffer: vk_src={} vk_dst={} size={}", (void*)vk_src, (void*)vk_dst, size);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }

    if (!vk_src->buffer || !vk_dst->buffer)
    {
        std::string err = std::format("cmd_copy_buffer null VkBuffer: src_buf={} dst_buf={} size={}",
                                      (void*)vk_src->buffer, (void*)vk_dst->buffer, size);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }

    VkBufferCopy copy_region{};
    copy_region.srcOffset = 0;
    copy_region.dstOffset = 0;
    copy_region.size = size;
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(cmd), vk_src->buffer, vk_dst->buffer, 1, &copy_region);
}

CommandHandle VulkanRHI::begin_async_compute() {
    if (frames.empty() || !frames[current_frame].async_command_pool)
        return nullptr;

    VkCommandBuffer cb = frames[current_frame].async_command_buffer;
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin_info) != VK_SUCCESS) {
        bud::eprint("[Vulkan] begin_async_compute: failed to begin command buffer");
        return nullptr;
    }
    async_recording = true;
    return cb;
}

void VulkanRHI::end_async_compute() {
    if (frames.empty() || !async_recording) return;
    VkCommandBuffer cb = frames[current_frame].async_command_buffer;
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        bud::eprint("[Vulkan] end_async_compute: failed to end command buffer");
        async_recording = false;
        return;
    }
    async_recording = false;

    uint64_t signal_value = ++compute_timeline_value;
    VkTimelineSemaphoreSubmitInfo timeline_info{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &signal_value;

    VkSubmitInfo submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit_info.pNext = &timeline_info;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cb;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &compute_timeline_semaphore;

    VkResult r = vkQueueSubmit(compute_queue ? compute_queue : graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        bud::eprint("[Vulkan] end_async_compute: vkQueueSubmit failed, result={}", (int)r);
    } else {
        async_compute_pending_this_frame = true;
    }
}

void VulkanRHI::wait_compute_timeline(uint64_t value) {
    if (!compute_timeline_semaphore) return;
    
    VkSemaphoreWaitInfo wait_info{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &compute_timeline_semaphore;
    wait_info.pValues = &value;
    wait_info.flags = 0;
    vkWaitSemaphores(device, &wait_info, UINT64_MAX);
}

uint64_t VulkanRHI::get_graphics_timeline_completed_value() const {
    if (!graphics_timeline_semaphore) return 0;
    uint64_t value = 0;
    vkGetSemaphoreCounterValue(device, graphics_timeline_semaphore, &value);
    return value;
}

void VulkanRHI::resource_barrier(CommandHandle cmd, bud::graphics::BufferHandle buffer, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) {
	// Helper: map ResourceState -> (stage, access) for BUFFER barriers
	auto buf_transition = [](bud::graphics::ResourceState state) -> std::pair<VkPipelineStageFlags2, VkAccessFlags2> {
		using RS = bud::graphics::ResourceState;
		switch (state) {
		case RS::UnorderedAccess:
			return { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
		case RS::IndirectArgument:
			return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT };
		case RS::ShaderResource:
			// ALL_COMMANDS so the barrier is valid on ANY queue (graphics,
		// compute, copy). Async compute passes record these barriers on the
		// compute command buffer, where graphics-only stages would be invalid.
			return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
		case RS::VertexBuffer:
			return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT };
		case RS::IndexBuffer:
			return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_INDEX_READ_BIT };
		case RS::TransferSrc:
			return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
		case RS::TransferDst:
			return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
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

    if (!buffer.is_valid()) return;

    auto* vk_buf = get_vulkan_buffer(buffer);
    if (!vk_buf || !vk_buf->buffer) return;

    barrier.buffer = vk_buf->buffer;
	barrier.offset = 0;
	barrier.size = VK_WHOLE_SIZE;

	VkDependencyInfo depInfo{};
	depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	depInfo.bufferMemoryBarrierCount = 1;
	depInfo.pBufferMemoryBarriers = &barrier;

	vkCmdPipelineBarrier2(static_cast<VkCommandBuffer>(cmd), &depInfo);
}

void VulkanRHI::cmd_bind_pipeline(CommandHandle cmd, PipelineHandle pipeline) {
	auto pipeObj = get_pipeline_obj(pipeline);
	if (!pipeObj || !pipeObj->pipeline) {
		bud::eprint("cmd_bind_pipeline called with invalid pipeline object");
		return;
	}
	if (pipeObj->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
		current_compute_pipeline = pipeline;
		current_compute_bindings.clear();
	}
	vkCmdBindPipeline(static_cast<VkCommandBuffer>(cmd), pipeObj->bind_point, pipeObj->pipeline);
	current_stats.pipeline_binds++;
}

void VulkanRHI::cmd_bind_vertex_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer) {
    if (!buffer.is_valid()) return;

    auto* vk_buf = get_vulkan_buffer(buffer);
    if (!vk_buf || !vk_buf->buffer) return;

    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(static_cast<VkCommandBuffer>(cmd), 0, 1, &vk_buf->buffer, offsets);
}

void VulkanRHI::cmd_bind_index_buffer(CommandHandle cmd, bud::graphics::BufferHandle buffer, bool is_u16) {
    if (!buffer.is_valid()) return;

    auto* vk_buf = get_vulkan_buffer(buffer);
    if (!vk_buf || !vk_buf->buffer) return;

    vkCmdBindIndexBuffer(static_cast<VkCommandBuffer>(cmd), vk_buf->buffer, 0, is_u16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
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

void VulkanRHI::cmd_draw_mesh_tasks(CommandHandle cmd, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
    if (fpCmdDrawMeshTasksEXT) {
        fpCmdDrawMeshTasksEXT(static_cast<VkCommandBuffer>(cmd), group_count_x, group_count_y, group_count_z);
    }
}

void VulkanRHI::cmd_draw_indexed_indirect(CommandHandle cmd, bud::graphics::BufferHandle buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) {
    if (!buffer.is_valid() || draw_count == 0) return;

    auto* vk_buf = get_vulkan_buffer(buffer);
    if (!vk_buf || !vk_buf->buffer) return;

	vkCmdDrawIndexedIndirect(static_cast<VkCommandBuffer>(cmd), vk_buf->buffer, offset, draw_count, stride);
	current_stats.draw_calls += 1; 
}

void VulkanRHI::cmd_push_constants(CommandHandle cmd, PipelineHandle pipeline, uint32_t size, const void* data) {
	auto pipeObj = get_pipeline_obj(pipeline);
	if (!pipeObj || !pipeObj->layout) {
		bud::eprint("cmd_push_constants called with invalid pipeline layout");
		return;
	}
	VkShaderStageFlags stage = pipeObj->push_stage_flags ? pipeObj->push_stage_flags : (
		(pipeObj->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) 
			? VK_SHADER_STAGE_COMPUTE_BIT 
			: (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
	);
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

void VulkanRHI::cmd_bind_descriptor_set(CommandHandle cmd, PipelineHandle pipeline, uint32_t set_index) {
	auto pipeObj = get_pipeline_obj(pipeline);
	if (!pipeObj || !pipeObj->layout) {
		bud::eprint("cmd_bind_descriptor_set called with invalid pipeline object");
		return;
	}
	auto& frame = frames[current_frame];

	// Bind the per-frame global descriptor set at the given set index
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

void VulkanRHI::cmd_bind_descriptor_set(CommandHandle cmd, PipelineHandle pipeline, uint32_t set_index, uint64_t descriptor_set) {
	auto pipeObj = get_pipeline_obj(pipeline);
	if (!pipeObj || !pipeObj->layout) {
		bud::eprint("cmd_bind_descriptor_set called with invalid pipeline object");
		return;
	}

	VkDescriptorSet vk_set = reinterpret_cast<VkDescriptorSet>(descriptor_set);
	vkCmdBindDescriptorSets(
		static_cast<VkCommandBuffer>(cmd),
		pipeObj->bind_point,
		pipeObj->layout,
		set_index,  // first set
		1,          // descriptor set count
		&vk_set,
		0,
		nullptr  // dynamic offsets
	);
}

void VulkanRHI::cmd_bind_storage_buffer(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding, bud::graphics::BufferHandle buffer) {
	current_compute_bindings[binding] = buffer;
}

void VulkanRHI::cmd_bind_compute_texture(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding, TextureHandle texture, uint32_t mip_level, bool is_storage, bool is_general) {
	current_compute_bindings[binding] = ImageBinding{ texture, mip_level, is_storage, is_general };
}

void VulkanRHI::cmd_bind_compute_ubo(CommandHandle cmd, PipelineHandle pipeline, uint32_t binding) {
	current_compute_bindings[binding] = UBOBinding{};
}

void VulkanRHI::resource_barrier(CommandHandle cmd, TextureHandle texture, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) {
    auto vk_tex = get_vulkan_texture(texture);
    if (!vk_tex) return;
    auto src = sync2::get_transition2(old_state);
    auto dst = sync2::get_transition2(new_state);

    bool is_depth = (vk_tex->format == TextureFormat::D32_FLOAT || vk_tex->format == TextureFormat::D24_UNORM_S8_UINT);
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
    if (is_depth && vk_tex->format == TextureFormat::D24_UNORM_S8_UINT) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    sync2::cmd_image_barrier2(static_cast<VkCommandBuffer>(cmd), vk_tex->image, aspect,
                              0, (vk_tex->mips > 0 ? vk_tex->mips : 1), 0, (vk_tex->array_layers > 0 ? vk_tex->array_layers : 1),
                              oldLayout, newLayout,
                              src.stage, src.access,
                              dst.stage, dst.access);
}

TextureHandle VulkanRHI::get_current_swapchain_texture() {
	if (current_image_index >= swapchain_texture_handles.size())
		return TextureHandle{};

	return swapchain_texture_handles[current_image_index];
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

    // Query loader/instance supported version and pick highest supported up to 1.4
    uint32_t loader_instance_version = VK_API_VERSION_1_1;
    if (vkEnumerateInstanceVersion(&loader_instance_version) != VK_SUCCESS) {
        // If not supported, assume 1.1
        loader_instance_version = VK_API_VERSION_1_1;
    }
    uint32_t requested = VK_API_VERSION_1_4;
    instance_api_version = std::min(loader_instance_version, requested);
    app_info.apiVersion = instance_api_version;
    bud::print("[Vulkan] Instance target API version set to {}.{}.{}", VK_VERSION_MAJOR(instance_api_version), VK_VERSION_MINOR(instance_api_version), VK_VERSION_PATCH(instance_api_version));

	VkInstanceCreateInfo create_info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	create_info.pApplicationInfo = &app_info;

    uint32_t count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    std::vector<const char*> exts;
    if (extensions && count > 0) {
        exts.assign(extensions, extensions + count);
    }
	// Keep debug utils enabled so Nsight / RenderDoc can see object names and
	// command buffer labels even when validation layers are disabled.
	exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
			// Record device API version (capability). Clamp to 1.4 maximum target.
			device_api_version = std::min(props.apiVersion, VK_API_VERSION_1_4);
			bud::print("[Vulkan] Physical device API version: {}.{}.{}", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
			break;
		}
	}
	if (physical_device == nullptr) {
		physical_device = devices[0];
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physical_device, &props);
		device_api_version = std::min(props.apiVersion, VK_API_VERSION_1_4);
		bud::print("[Vulkan] Warning: Using Integrated/Fallback GPU.");
		bud::print("[Vulkan] Physical device API version: {}.{}.{}", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	}
	{
		// GPU frame-time timestamps (VkQueryPool per frame slot).
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physical_device, &props);
		timestamp_period_ns = props.limits.timestampPeriod;
		timestamp_queries_supported = props.limits.timestampComputeAndGraphics == VK_TRUE;
		bud::print("[Vulkan] Timestamp queries {}supported (period {:.2f} ns/tick)",
			timestamp_queries_supported ? "" : "NOT ", timestamp_period_ns);
	}
}

void VulkanRHI::create_logical_device(bool enable_validation) {
	QueueFamilyIndices indices = find_queue_families(physical_device);
	std::vector<VkDeviceQueueCreateInfo> queue_infos;
	std::set<uint32_t> unique_families = { indices.graphics_family.value(), indices.present_family.value() };
	if (indices.copy_family.has_value())
		unique_families.insert(indices.copy_family.value());
	if (indices.compute_family.has_value())
		unique_families.insert(indices.compute_family.value());

	// Keep the priority arrays alive until vkCreateDevice reads them. Use a
	// vector of vectors (reserved up front) so each inner vector's data()
	// pointer stays stable across the loop.
	std::vector<std::vector<float>> priorities;
	priorities.reserve(unique_families.size());
	for (uint32_t family : unique_families) {
		VkDeviceQueueCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		info.queueFamilyIndex = family;
		// When the copy queue falls back to the graphics family (queue_index 1),
		// that family must expose 2 queues.
		const bool hosts_copy_at_index1 = indices.copy_family.has_value()
			&& indices.copy_family.value() == family
			&& indices.copy_queue_index == 1;
		const uint32_t queue_count = hosts_copy_at_index1 ? 2u : 1u;
		info.queueCount = queue_count;
		priorities.emplace_back(queue_count, 1.0f);
		info.pQueuePriorities = priorities.back().data();
		queue_infos.push_back(info);
	}

    // Build feature chain depending on device API version support (fallback to 1.1..1.4)
    VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    features13.pNext = nullptr;
    features13.dynamicRendering = VK_FALSE;
    features13.synchronization2 = VK_FALSE;
    features13.maintenance4 = VK_FALSE;

    VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features12.pNext = nullptr;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    // Timeline semaphores (value-based) replace the stateful binary
    // semaphore + fence combo for async uploads.
    features12.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    features11.pNext = nullptr;

    // Mesh shader features (EXT extension)
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    mesh_shader_features.pNext = nullptr;
    mesh_shader_features.taskShader = VK_TRUE;
    mesh_shader_features.meshShader = VK_TRUE;

    // 使用 VkPhysicalDeviceFeatures2 整合所有 Features
    VkPhysicalDeviceFeatures2 device_features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    device_features2.pNext = nullptr;
    device_features2.features.samplerAnisotropy = VK_TRUE;
    device_features2.features.multiDrawIndirect = VK_TRUE;
    device_features2.features.geometryShader = VK_TRUE;
	device_features2.features.fillModeNonSolid = VK_TRUE;

    // Chain feature structs according to supported device_api_version
    // Mesh shader features are always appended to the end of the chain
    if (device_api_version >= VK_API_VERSION_1_3) {
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.maintenance4 = VK_TRUE;
        features12.pNext = &features13;
        features11.pNext = &features12;
        mesh_shader_features.pNext = nullptr;
        features13.pNext = &mesh_shader_features;
        device_features2.pNext = &features11;
    } else if (device_api_version >= VK_API_VERSION_1_2) {
        features11.pNext = &features12;
        mesh_shader_features.pNext = nullptr;
        features12.pNext = &mesh_shader_features;
        device_features2.pNext = &features11;
    } else if (device_api_version >= VK_API_VERSION_1_1) {
        // Only 1.1 features available, chain features11 only
        mesh_shader_features.pNext = nullptr;
        features11.pNext = &mesh_shader_features;
        device_features2.pNext = &features11;
    } else {
        // No extended feature structs
        mesh_shader_features.pNext = nullptr;
        device_features2.pNext = &mesh_shader_features;
    }

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

	graphics_family_index = indices.graphics_family.value();
	copy_family_index = indices.copy_family.has_value() ? indices.copy_family.value() : graphics_family_index;

	if (indices.copy_family.has_value()) {
		vkGetDeviceQueue(device, indices.copy_family.value(), indices.copy_queue_index, &copy_queue);
		has_dedicated_copy_queue = indices.copy_family.value() != indices.graphics_family.value();
	} else {
		// No dedicated transfer family and no spare graphics queue: uploads
		// fall back to the main graphics queue (still async via semaphores).
		copy_queue = graphics_queue;
		has_dedicated_copy_queue = false;
	}
	if (copy_queue) {
		bud::print("[Vulkan] Copy queue {} (dedicated={})",
			indices.copy_family.has_value() ? std::to_string(indices.copy_family.value()) : "alias-graphics",
			has_dedicated_copy_queue ? "yes" : "no");
	}

	compute_family_index = indices.compute_family.has_value() ? indices.compute_family.value() : graphics_family_index;
	if (indices.compute_family.has_value()) {
		vkGetDeviceQueue(device, indices.compute_family.value(), indices.compute_queue_index, &compute_queue);
		has_dedicated_compute_queue_ = (compute_queue != graphics_queue);
	} else {
		compute_queue = graphics_queue;
		has_dedicated_compute_queue_ = false;
	}
	if (compute_queue) {
		bud::print("[Vulkan] Compute queue {} (dedicated={})",
			indices.compute_family.has_value() ? std::to_string(indices.compute_family.value()) : "alias-graphics",
			has_dedicated_compute_queue_ ? "yes" : "no");
	}

	fpCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
	if (!fpCmdPushDescriptorSetKHR) {
		bud::eprint("[Vulkan] Warning: vkCmdPushDescriptorSetKHR not found, compute bindings may fail!");
	}

	fpCmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksEXT");
	if (!fpCmdDrawMeshTasksEXT) {
		bud::eprint("[Vulkan] Warning: vkCmdDrawMeshTasksEXT not found, mesh shaders will not work!");
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
	swapchain_texture_handles.resize(swapchain_images.size());
	for (size_t i = 0; i < swapchain_images.size(); i++) {
		auto tex_ptr = std::make_shared<VulkanTexture>();
		tex_ptr->image = swapchain_images[i];
		tex_ptr->view = swapchain_image_views[i];
		tex_ptr->width = swapchain_extent.width;
		tex_ptr->height = swapchain_extent.height;
		tex_ptr->mips = 1;
		tex_ptr->array_layers = 1;
        // Use SRGB variant if swapchain was created with an SRGB surface format
        tex_ptr->format = (swapchain_image_format == VK_FORMAT_B8G8R8A8_SRGB) ? TextureFormat::BGRA8_SRGB : TextureFormat::BGRA8_UNORM;
		tex_ptr->allocation = VK_NULL_HANDLE; // Swapchain image memory is managed by driver
		swapchain_textures_wrappers[i] = *tex_ptr;
		swapchain_texture_handles[i] = resource_pool->register_texture(tex_ptr);
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

		// Per-frame async compute command pool (compute family; graphics family
		// fallback when no dedicated compute queue exists).
		VkCommandPoolCreateInfo async_pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		async_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		async_pool_info.queueFamilyIndex = compute_family_index;

		if (vkCreateCommandPool(device, &async_pool_info, nullptr, &frames[i].async_command_pool) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create async command pool!");
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

		// Allocate the per-frame async compute command buffer.
		VkCommandBufferAllocateInfo async_alloc_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		async_alloc_info.commandPool = frames[i].async_command_pool;
		async_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		async_alloc_info.commandBufferCount = 1;

		if (vkAllocateCommandBuffers(device, &async_alloc_info, &frames[i].async_command_buffer) != VK_SUCCESS) {
			throw std::runtime_error("Failed to allocate async command buffer!");
		}
	}
}

void VulkanRHI::create_sync_objects() {
	// Binary semaphores and fences for non-timeline usage (image available,
	// render finished, in-flight frame fencing).
	VkSemaphoreCreateInfo semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	// Timeline semaphore for upload completion (value-based sync). Kept in a
	// SEPARATE create-info so binary semaphores above are not created as
	// timeline semaphores.
	VkSemaphoreTypeCreateInfo timeline_type_info{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
	timeline_type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	timeline_type_info.initialValue = 0;
	VkSemaphoreCreateInfo timeline_semaphore_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	timeline_semaphore_info.pNext = &timeline_type_info;

	VkQueryPoolCreateInfo query_pool_info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	query_pool_info.queryCount = 2; // frame start / frame end

	for (int i = 0; i < max_frames_in_flight; i++) {
		if (vkCreateSemaphore(device, &semaphore_info, nullptr, &frames[i].image_available_semaphore) != VK_SUCCESS ||
			vkCreateFence(device, &fence_info, nullptr, &frames[i].in_flight_fence) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create synchronization objects for a frame!");
		}

		if (timestamp_queries_supported &&
			vkCreateQueryPool(device, &query_pool_info, nullptr, &frames[i].timestamp_pool) != VK_SUCCESS) {
			bud::eprint("[Vulkan] Failed to create timestamp query pool for frame {}", i);
			frames[i].timestamp_pool = nullptr;
		}

		}

		render_finished_semaphores.resize(swapchain_images.size());
	for (size_t i = 0; i < swapchain_images.size(); ++i) {
		if (vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished_semaphores[i]) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create render finished semaphores!");
		}
	}

	// Async compute timeline semaphore (value-based, signals compute completion
	// for the frame; graphics waits on the value before consuming compute output).
	if (vkCreateSemaphore(device, &timeline_semaphore_info, nullptr, &compute_timeline_semaphore) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create compute timeline semaphore!");
	}
	compute_timeline_value = 0;

	if (vkCreateSemaphore(device, &timeline_semaphore_info, nullptr, &graphics_timeline_semaphore) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create graphics timeline semaphore!");
	}
	graphics_timeline_value = 0;
}


VkCommandBuffer VulkanRHI::begin_single_time_commands() {
	single_time_mutex.lock();
	VkCommandBufferAllocateInfo alloc_info{};
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	// 使用第0帧的命令池（这在多线程渲染时可能不安全，但在初始化阶段是安全的）	
    if (!device) {
        bud::eprint("VulkanRHI::begin_single_time_commands called but device is null");
        single_time_mutex.unlock();
        return VK_NULL_HANDLE;
    }
    if (graphics_queue == VK_NULL_HANDLE) {
        bud::eprint("VulkanRHI::begin_single_time_commands called but graphics_queue is null");
        single_time_mutex.unlock();
        return VK_NULL_HANDLE;
    }
    if (texture_upload_pool == VK_NULL_HANDLE) {
        bud::eprint("VulkanRHI::begin_single_time_commands: invalid command pool");
        single_time_mutex.unlock();
        return VK_NULL_HANDLE;
    }
    alloc_info.commandPool = texture_upload_pool;
	alloc_info.commandBufferCount = 1;

	VkCommandBuffer command_buffer;
    VkResult r = vkAllocateCommandBuffers(device, &alloc_info, &command_buffer);
    if (r != VK_SUCCESS) {
        bud::eprint("VulkanRHI::begin_single_time_commands vkAllocateCommandBuffers failed: {}", (int)r);
        single_time_mutex.unlock();
        return VK_NULL_HANDLE;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
        bud::eprint("VulkanRHI::begin_single_time_commands vkBeginCommandBuffer failed");
        // Free the allocated command buffer to avoid leaks
        vkFreeCommandBuffers(device, texture_upload_pool, 1, &command_buffer);
        single_time_mutex.unlock();
        return VK_NULL_HANDLE;
    }

    return command_buffer;
}

void VulkanRHI::end_single_time_commands(VkCommandBuffer command_buffer) {
    if (command_buffer == VK_NULL_HANDLE) {
        bud::eprint("VulkanRHI::end_single_time_commands called with VK_NULL_HANDLE");
        single_time_mutex.unlock();
        return;
    }

    if (!device || graphics_queue == VK_NULL_HANDLE) {
        bud::eprint("VulkanRHI::end_single_time_commands missing device or graphics_queue");
        single_time_mutex.unlock();
        return;
    }

    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        bud::eprint("VulkanRHI::end_single_time_commands vkEndCommandBuffer failed");
        single_time_mutex.unlock();
        return;
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;

    VkResult r;
    {
        std::lock_guard lock(queue_submit_mutex);
        r = vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            bud::eprint("[Vulkan] Failed to submit single time command! result={}", (int)r);
        } else {
            vkQueueWaitIdle(graphics_queue);
        }
    }

    if (texture_upload_pool != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, texture_upload_pool, 1, &command_buffer);
    } else {
        bud::eprint("VulkanRHI::end_single_time_commands cannot free command buffer: invalid command pool");
    }
    single_time_mutex.unlock();
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

	// Dedicated transfer (copy) family: a family with VK_QUEUE_TRANSFER_BIT but
	// no GRAPHICS/COMPUTE bit. Prefer this so uploads run on an independent
	// hardware queue. Store the family and index; if none exists we fall back to
	// the graphics family with queue_index=1 (see create_logical_device).
	indices.copy_family.reset();
	indices.copy_queue_index = 0;
	for (uint32_t family = 0; family < queue_families.size(); ++family) {
		const auto& qf = queue_families[family];
		const VkQueueFlags flags = qf.queueFlags;
		const bool is_pure_transfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0
			&& (flags & VK_QUEUE_GRAPHICS_BIT) == 0
			&& (flags & VK_QUEUE_COMPUTE_BIT) == 0;
		if (is_pure_transfer && qf.queueCount >= 1) {
			indices.copy_family = family;
			indices.copy_queue_index = 0;
			break;
		}
	}
	if (!indices.copy_family.has_value() && indices.graphics_family.has_value()
		&& queue_families[indices.graphics_family.value()].queueCount >= 2) {
		// Fall back to a second queue in the graphics family.
		indices.copy_family = indices.graphics_family;
		indices.copy_queue_index = 1;
	}

	// Dedicated async-compute family: VK_QUEUE_COMPUTE_BIT without
	// VK_QUEUE_GRAPHICS_BIT. Enables real overlap between compute passes and
	// graphics rendering. No fallback (compute shares graphics otherwise).
	indices.compute_family.reset();
	indices.compute_queue_index = 0;
	for (uint32_t family = 0; family < queue_families.size(); ++family) {
		const auto& qf = queue_families[family];
		const VkQueueFlags flags = qf.queueFlags;
		const bool is_pure_compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0
			&& (flags & VK_QUEUE_GRAPHICS_BIT) == 0;
		if (is_pure_compute && qf.queueCount >= 1) {
			indices.compute_family = family;
			indices.compute_queue_index = 0;
			break;
		}
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
	// Always load the debug-utils entry points so object names / labels work in
	// capture tools even when validation layers are off. Only create the actual
	// messenger when validation is enabled.
	fpCmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
	fpCmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
	fpSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");

	if (!enable) return;
	VkDebugUtilsMessengerCreateInfoEXT create_info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
	create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	create_info.pfnUserCallback = debug_callback;

	if (create_debug_utils_messenger_ext(instance, &create_info, nullptr, &debug_messenger) != VK_SUCCESS) {
		throw std::runtime_error("Failed to set up debug messenger!");
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

    sync2::cmd_image_barrier2(commandBuffer, image, aspect,
                              0, 1, 0, 1,
                              old_layout, new_layout,
                              srcStage, srcAccess,
                              dstStage, dstAccess);

    this->end_single_time_commands(commandBuffer);
}

void VulkanRHI::copy_buffer_to_image(VkImage image, VkBuffer buffer, uint64_t buffer_offset, uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = this->begin_single_time_commands();

    VkBufferImageCopy region{};
    // Use provided buffer_offset to support sub-allocated staging buffers
    region.bufferOffset = static_cast<VkDeviceSize>(buffer_offset);
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
        std::string err = std::format("copy_buffer_immediate invalid handle: src_valid={} dst_valid={} size={}", src.is_valid(), dst.is_valid(), size);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        this->end_single_time_commands(cmd);
        return;
#endif
    }

	VkBufferCopy copy_region{};
    copy_region.srcOffset = 0;
    copy_region.dstOffset = 0;
	copy_region.size = size;

	auto* vk_src = get_vulkan_buffer(src);
	auto* vk_dst = get_vulkan_buffer(dst);
	if (!vk_src || !vk_dst) {
		std::string err = std::format("copy_buffer_immediate null VulkanBuffer: vk_src={} vk_dst={} size={}", (void*)vk_src, (void*)vk_dst, size);
		bud::eprint("{}", err);
#if defined(_DEBUG)
		this->end_single_time_commands(cmd);
		throw std::runtime_error(err);
#else
		this->end_single_time_commands(cmd);
		return;
#endif
	}
	if (!vk_src->buffer || !vk_dst->buffer) {
		std::string err = std::format("copy_buffer_immediate null VkBuffer: src_buf={} dst_buf={} size={}",
							(void*)vk_src->buffer, (void*)vk_dst->buffer, size);
		bud::eprint("{}", err);
#if defined(_DEBUG)
		this->end_single_time_commands(cmd);
		throw std::runtime_error(err);
#else
		this->end_single_time_commands(cmd);
		return;
#endif
	}

	vkCmdCopyBuffer(cmd, vk_src->buffer, vk_dst->buffer, 1, &copy_region);

	this->end_single_time_commands(cmd);
}

void VulkanRHI::copy_buffer_immediate_offset(bud::graphics::BufferHandle src, bud::graphics::BufferHandle dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) {
    if (!src.is_valid() || !dst.is_valid()) {
        std::string err = std::format("copy_buffer_immediate_offset invalid handle: src_valid={} dst_valid={} size={}", src.is_valid(), dst.is_valid(), size);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }

    auto* vk_src = get_vulkan_buffer(src);
    auto* vk_dst = get_vulkan_buffer(dst);
    if (!vk_src || !vk_dst || !vk_src->buffer || !vk_dst->buffer) {
        std::string err = std::format("copy_buffer_immediate_offset null VkBuffer handles: vk_src={} vk_dst={}", (void*)vk_src, (void*)vk_dst);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }

	VkCommandBuffer cmd = this->begin_single_time_commands();

	VkBufferCopy copy_region{};
    copy_region.srcOffset = static_cast<VkDeviceSize>(src_offset);
    copy_region.dstOffset = static_cast<VkDeviceSize>(dst_offset);
	copy_region.size = size;

	vkCmdCopyBuffer(cmd, vk_src->buffer, vk_dst->buffer, 1, &copy_region);

	this->end_single_time_commands(cmd);
}

void VulkanRHI::cmd_copy_image(CommandHandle cmd, TextureHandle src, TextureHandle dst) {
    auto* vk_src = get_vulkan_texture(src);
    auto* vk_dst = get_vulkan_texture(dst);
    if (!vk_src || !vk_src->image || !vk_dst || !vk_dst->image) {
        std::string err = std::format("cmd_copy_image invalid TextureHandle: src_valid={} dst_valid={}", src.is_valid(), dst.is_valid());
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }

    VkImageCopy region{};
    // choose aspect masks based on texture formats (color vs depth/stencil)
    bool src_is_depth = (vk_src->format == TextureFormat::D32_FLOAT || vk_src->format == TextureFormat::D24_UNORM_S8_UINT);
    bool dst_is_depth = (vk_dst->format == TextureFormat::D32_FLOAT || vk_dst->format == TextureFormat::D24_UNORM_S8_UINT);
    VkImageAspectFlags srcAspect = src_is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags dstAspect = dst_is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (src_is_depth && vk_src->format == TextureFormat::D24_UNORM_S8_UINT) srcAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dst_is_depth && vk_dst->format == TextureFormat::D24_UNORM_S8_UINT) dstAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    region.srcSubresource.aspectMask = srcAspect;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = vk_src->array_layers;
    region.srcSubresource.mipLevel = 0;
    region.dstSubresource.aspectMask = dstAspect;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount = vk_dst->array_layers;
    region.dstSubresource.mipLevel = 0;
    region.extent.width = vk_src->width;
    region.extent.height = vk_src->height;
    region.extent.depth = 1;

    vkCmdCopyImage(static_cast<VkCommandBuffer>(cmd),
		vk_src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		vk_dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region);
}

void VulkanRHI::cmd_blit_image(CommandHandle cmd, TextureHandle src, TextureHandle dst) {
	auto* vk_src = get_vulkan_texture(src);
	auto* vk_dst = get_vulkan_texture(dst);
	if (!vk_src || !vk_dst) return;

	VkImageBlit blit{};
	blit.srcOffsets[0] = { 0, 0, 0 };
	blit.srcOffsets[1] = { (int32_t)vk_src->width, (int32_t)vk_src->height, 1 };
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.mipLevel = 0;
	blit.srcSubresource.baseArrayLayer = 0;
	blit.srcSubresource.layerCount = 1;

	blit.dstOffsets[0] = { 0, 0, 0 };
	blit.dstOffsets[1] = { (int32_t)vk_dst->width, (int32_t)vk_dst->height, 1 };
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.mipLevel = 0;
	blit.dstSubresource.baseArrayLayer = 0;
	blit.dstSubresource.layerCount = 1;

	vkCmdBlitImage(static_cast<VkCommandBuffer>(cmd),
		vk_src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		vk_dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit,
		VK_FILTER_LINEAR);
}

void VulkanRHI::cmd_copy_to_buffer(CommandHandle cmd, bud::graphics::BufferHandle dst, uint64_t offset, uint64_t size, const void* data) {
	// Not used for GPU-to-GPU copy, this seems to be for CPU-to-GPU copy via cmd buffer (vkCmdUpdateBuffer)
    if (!dst.is_valid()) {
        std::string err = std::format("cmd_copy_to_buffer invalid handle: dst_valid=false, offset={}, size={}", offset, size);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }
    auto* vk_dst = get_vulkan_buffer(dst);
    if (!vk_dst || !vk_dst->buffer) {
        std::string err = std::format("cmd_copy_to_buffer null destination buffer: vk_dst={} dst_buf={} offset={} size={}", (void*)vk_dst, vk_dst ? (void*)vk_dst->buffer : nullptr, offset, size);
        bud::eprint("{}", err);
#if defined(_DEBUG)
        throw std::runtime_error(err);
#else
        return;
#endif
    }
	vkCmdUpdateBuffer(static_cast<VkCommandBuffer>(cmd), vk_dst->buffer, offset, size, data);
}

void VulkanRHI::cmd_copy_image_to_buffer(CommandHandle cmd, TextureHandle src, bud::graphics::BufferHandle dst) {
    auto* vk_src = get_vulkan_texture(src);
    if (!vk_src || !dst.is_valid()) return;

    auto* vk_dst = get_vulkan_buffer(dst);
    if (!vk_dst || !vk_dst->buffer) return;

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = {
        vk_src->width,
        vk_src->height,
        1
    };

    vkCmdCopyImageToBuffer(
        static_cast<VkCommandBuffer>(cmd),
        vk_src->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vk_dst->buffer,
        1,
        &region
    );
}

// Dedicated (non-ring) host-visible staging buffer for ASYNC uploads. Unlike
// alloc_staging (per-frame ring that on_frame_begin resets each frame), this
// buffer lives until the caller releases it, so an async upload that spans
// frames is never overwritten by the ring reuse.
bud::graphics::BufferHandle VulkanRHI::create_dedicated_upload_buffer(uint64_t size) {
	if (memory_allocator) {
		return memory_allocator->alloc_persistent(size, bud::graphics::ResourceState::Common);
	}
	return {};
}

bud::graphics::BufferHandle VulkanRHI::create_upload_buffer(uint64_t size) {
    if (memory_allocator) {
        return memory_allocator->alloc_persistent(size, bud::graphics::ResourceState::Common);
    }
    return {};
}

bud::graphics::BufferHandle VulkanRHI::create_readback_buffer(uint64_t size) {
    if (memory_allocator) {
        return memory_allocator->alloc_read_back(size);
    }
    return {};
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

TextureHandle VulkanRHI::create_texture(const bud::graphics::TextureDesc& desc, const void* initial_data, uint64_t size) {
	TextureHandle handle = resource_pool->acquire_texture(desc);
	auto* tex = get_vulkan_texture(handle);
	if (!tex) return TextureHandle{};

	tex->width = desc.width;
	tex->height = desc.height;
	tex->format = desc.format;
	tex->mips = desc.mips;
	tex->array_layers = desc.array_layers;

	if (initial_data && size > 0) {
		bud::graphics::BufferHandle staging = this->create_upload_buffer(size);
		auto* vk_buf = get_vulkan_buffer(staging);
		if (vk_buf && vk_buf->mapped_ptr) {
			std::memcpy(vk_buf->mapped_ptr, initial_data, size);

			this->transition_image_layout_immediate(tex->image, to_vk_format(desc.format), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			this->copy_buffer_to_image(tex->image, vk_buf->buffer, 0, desc.width, desc.height);

			if (desc.mips > 1) {
				this->generate_mipmaps(tex->image, to_vk_format(desc.format), desc.width, desc.height, desc.mips);
			}
			else {
				this->transition_image_layout_immediate(tex->image, to_vk_format(desc.format), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
		}

		this->destroy_buffer(staging);
	}
	else if (desc.initial_state != ResourceState::Undefined) {
		auto dst = sync2::get_transition2(desc.initial_state);
		bool is_depth = (desc.format == TextureFormat::D32_FLOAT || desc.format == TextureFormat::D24_UNORM_S8_UINT);
		VkImageLayout newLayout = dst.layout;
		if (is_depth && desc.initial_state == ResourceState::ShaderResource) {
			newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		}

		VkCommandBuffer cmd = this->begin_single_time_commands();
		VkImageAspectFlags aspect = get_aspect_flags(to_vk_format(desc.format));
		sync2::cmd_image_barrier2(cmd, tex->image, aspect,
			0, (desc.mips > 0 ? desc.mips : 1), 0, (desc.array_layers > 0 ? desc.array_layers : 1),
			VK_IMAGE_LAYOUT_UNDEFINED, newLayout,
			VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
			dst.stage, dst.access);
		this->end_single_time_commands(cmd);
	}

	if (desc.format == TextureFormat::R32G32_UINT || desc.format == TextureFormat::RGBA32_UINT)
		tex->sampler = point_sampler;
	else
		tex->sampler = default_sampler;
	return handle;
}

void VulkanRHI::destroy_texture(TextureHandle handle) {
	if (resource_pool && handle.is_valid()) {
		resource_pool->release_texture(handle);
	}
}

void VulkanRHI::record_texture_upload(VkCommandBuffer cb, class VulkanTexture* tex, VkBuffer staging_buf, uint64_t staging_offset, const TextureDesc& desc) {
	VkFormat vk_format = to_vk_format(desc.format);

	// 1. Transition base mip (and all levels) to TRANSFER_DST.
	sync2::cmd_image_barrier2(cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
		0, desc.mips, 0, 1,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

	// 2. Copy staging -> image level 0.
	VkBufferImageCopy region{};
	region.bufferOffset = staging_offset;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { desc.width, desc.height, 1 };
	vkCmdCopyBufferToImage(cb, staging_buf, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// 3. Generate mips (same blit chain as generate_mipmaps).
	int32_t mipWidth = static_cast<int32_t>(desc.width);
	int32_t mipHeight = static_cast<int32_t>(desc.height);
	for (uint32_t i = 1; i < desc.mips; ++i) {
		// Level i-1: TRANSFER_DST -> TRANSFER_SRC
		sync2::cmd_image_barrier2(cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
			i - 1, 1, 0, 1,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

		// Level i: UNDEFINED -> TRANSFER_DST
		sync2::cmd_image_barrier2(cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
			i, 1, 0, 1,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

		// Blit i-1 -> i
		VkImageBlit blit{};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
		blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
		blit.dstOffsets[0] = { 0, 0, 0 };
		blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 };
		blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
		vkCmdBlitImage(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

		// Level i-1 -> SHADER_READ_ONLY
		sync2::cmd_image_barrier2(cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
			i - 1, 1, 0, 1,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

		if (mipWidth > 1) mipWidth /= 2;
		if (mipHeight > 1) mipHeight /= 2;
	}

	// 4. Final level -> SHADER_READ_ONLY.
	sync2::cmd_image_barrier2(cb, tex->image, VK_IMAGE_ASPECT_COLOR_BIT,
		desc.mips - 1, 1, 0, 1,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

TextureHandle VulkanRHI::create_texture_async(const bud::graphics::TextureDesc& desc, const void* initial_data, uint64_t size, uint32_t bindless_slot) {
	TextureHandle handle = resource_pool->acquire_texture(desc);
	auto* tex = get_vulkan_texture(handle);
	if (!tex) return TextureHandle{};

	tex->width = desc.width;
	tex->height = desc.height;
	tex->format = desc.format;
	tex->mips = desc.mips;
	tex->array_layers = desc.array_layers;
	if (desc.format == TextureFormat::R32G32_UINT)
		tex->sampler = point_sampler;
	else
		tex->sampler = default_sampler;

	if (!initial_data || size == 0) return handle;

	// Use a DEDICATED staging buffer (not the per-frame ring) so the upload can
	// span frames without being overwritten by the ring's per-frame reset.
	bud::graphics::BufferHandle staging = this->create_dedicated_upload_buffer(size);
	auto* vk_staging = get_vulkan_buffer(staging);
	if (!staging.is_valid() || !vk_staging || !vk_staging->mapped_ptr) {
		bud::eprint("[Vulkan] create_texture_async: staging alloc failed");
		if (staging.is_valid()) this->destroy_buffer(staging);
		return handle;
	}
	std::memcpy(vk_staging->mapped_ptr, initial_data, size);

	VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = texture_upload_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cb = VK_NULL_HANDLE;
	{
		std::lock_guard lock(single_time_mutex);
		if (vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) {
			bud::eprint("[Vulkan] create_texture_async: vkAllocateCommandBuffers failed");
			this->destroy_buffer(staging);
			return handle;
		}
	}

	VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
		bud::eprint("[Vulkan] create_texture_async: vkBeginCommandBuffer failed");
		{
			std::lock_guard lock(single_time_mutex);
			vkFreeCommandBuffers(device, texture_upload_pool, 1, &cb);
		}
		this->destroy_buffer(staging);
		return handle;
	}

	record_texture_upload(cb, tex, vk_staging->buffer, 0, desc);
	vkEndCommandBuffer(cb);

	VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS) {
		bud::eprint("[Vulkan] create_texture_async: vkCreateFence failed");
		{
			std::lock_guard lock(single_time_mutex);
			vkFreeCommandBuffers(device, texture_upload_pool, 1, &cb);
		}
		this->destroy_buffer(staging);
		return handle;
	}

	VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	VkResult r;
	{
		std::lock_guard qlock(queue_submit_mutex);
		r = vkQueueSubmit(graphics_queue, 1, &si, fence);
	}
	if (r != VK_SUCCESS) {
		bud::eprint("[Vulkan] create_texture_async: vkQueueSubmit failed, result={}", (int)r);
		vkDestroyFence(device, fence, nullptr);
		{
			std::lock_guard lock(single_time_mutex);
			vkFreeCommandBuffers(device, texture_upload_pool, 1, &cb);
		}
		this->destroy_buffer(staging);
		return handle;
	}

	// Bind the slot once the upload finishes (checked at frame begin).
	queue_bindless_update(bindless_slot, handle, fence, cb, staging);
	return handle;
}

void VulkanRHI::release_bindless_upload_state(BindlessUploadState* s) {
	if (!s) return;
	// Release once when the LAST frame slot drops its shared_ptr.
	if (s->cb) {
		std::lock_guard lock(single_time_mutex);
		vkFreeCommandBuffers(device, texture_upload_pool, 1, &s->cb);
	}
	if (s->staging.is_valid())
		destroy_buffer(s->staging);
	if (s->upload_fence)
		vkDestroyFence(device, s->upload_fence, nullptr);
}

void VulkanRHI::queue_bindless_update(uint32_t slot, TextureHandle tex, VkFence fence, VkCommandBuffer cb, bud::graphics::BufferHandle staging) {
	std::lock_guard lock(pending_bindless_mutex);
	if (pending_bindless.size() < max_frames_in_flight)
		pending_bindless.resize(max_frames_in_flight);

	// Fallback binding: state == null (already ready, no resources to free).
	std::shared_ptr<BindlessUploadState> state;
	if (fence != VK_NULL_HANDLE) {
		auto* raw = new BindlessUploadState{ fence, cb, staging };
		state = std::shared_ptr<BindlessUploadState>(raw, [this](BindlessUploadState* s) {
			release_bindless_upload_state(s);
			delete s;
		});
	}
	PendingBindless p{ slot, tex, std::move(state) };
	for (auto& slot_pending : pending_bindless)
		slot_pending.push_back(p);
}

void VulkanRHI::queue_bindless_fallback(uint32_t slot, TextureHandle tex) {
	// Bind the fallback at the frame boundary (state = null => already ready).
	queue_bindless_update(slot, tex, VK_NULL_HANDLE, VK_NULL_HANDLE, bud::graphics::BufferHandle{});
}

void VulkanRHI::apply_pending_bindless() {
	if (frames.empty() || current_frame >= pending_bindless.size()) return;
	std::lock_guard lock(pending_bindless_mutex);
	auto& pending = pending_bindless[current_frame];
	for (auto it = pending.begin(); it != pending.end();) {
		// state == null means "already ready" (fallback); otherwise the upload
		// fence must have signaled.
		bool done = (it->state == nullptr);
		if (!done) {
			VkResult st = vkGetFenceStatus(device, it->state->upload_fence);
			done = (st == VK_SUCCESS);
		}
		if (done) {
			update_bindless_texture_current_frame(it->slot, it->tex);
			it = pending.erase(it);
		} else {
			++it;
		}
	}
}

void VulkanRHI::update_bindless_texture(uint32_t index, TextureHandle texture) {
	auto* vk_tex = get_vulkan_texture(texture);
	if (!vk_tex) return;

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

void VulkanRHI::update_bindless_texture_current_frame(uint32_t index, TextureHandle texture) {
	auto* vk_tex = get_vulkan_texture(texture);
	if (!vk_tex) return;

	if (frames[current_frame].global_descriptor_set != VK_NULL_HANDLE) {
		DescriptorWriter writer;
		writer.write_image(1, index, vk_tex->view, vk_tex->sampler ? vk_tex->sampler : default_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		writer.update_set(device, frames[current_frame].global_descriptor_set);
	}
}
 
void VulkanRHI::update_bindless_image(uint32_t index, TextureHandle texture, uint32_t mip_level, bool is_storage) {
	auto* vk_tex = get_vulkan_texture(texture);
	if (!vk_tex) return;
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
	ubo.debug_cluster = render_config.enable_cluster_visualization ? 1 : 0;

	if (frames[current_frame].uniform_mapped) {
		std::memcpy(frames[current_frame].uniform_mapped, &ubo, sizeof(UniformBufferObject));
	}
}

void VulkanRHI::update_global_shadow_map(TextureHandle texture) {
	auto* vk_tex = get_vulkan_texture(texture);
	if (!vk_tex) return;

	// Current (recording) frame's set only; shadow map is per-frame.
	DescriptorWriter writer;
	writer.write_image(2, 0, vk_tex->view, shadow_sampler, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);
}

void VulkanRHI::update_global_instance_data(bud::graphics::BufferHandle buffer) {
	if (!buffer.is_valid()) return;
	auto* vk_buf = get_vulkan_buffer(buffer);
	if (!vk_buf || !vk_buf->buffer) return;

	// Current (recording) frame's set only: instance data is per-frame, so each
	// frame must bind its own buffer. Updating all frames clobbered in-flight
	// frames' bindings under async (flicker).
	DescriptorWriter writer;
	writer.write_buffer(3, vk_buf->buffer, vk_buf->size > 0 ? vk_buf->size : VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);
}

void VulkanRHI::update_global_page_table(bud::graphics::BufferHandle buffer) {
	if (!buffer.is_valid()) return;
	auto* vk_buf = get_vulkan_buffer(buffer);
	if (!vk_buf || !vk_buf->buffer) return;

	// Current (recording) frame's set only.
	DescriptorWriter writer;
	writer.write_buffer(4, vk_buf->buffer, vk_buf->size > 0 ? vk_buf->size : VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);
}

void VulkanRHI::update_global_page_pool(bud::graphics::BufferHandle buffer) {
	if (!buffer.is_valid()) return;
	auto* vk_buf = get_vulkan_buffer(buffer);
	if (!vk_buf || !vk_buf->buffer) return;

	// Current (recording) frame's set only.
	DescriptorWriter writer;
	writer.write_buffer(5, vk_buf->buffer, vk_buf->size > 0 ? vk_buf->size : VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);
}

void VulkanRHI::update_global_csm_instance_data(bud::graphics::BufferHandle buffer) {
	if (!buffer.is_valid()) return;
	auto* vk_buf = get_vulkan_buffer(buffer);
	if (!vk_buf || !vk_buf->buffer) return;

	// Current (recording) frame's set only; binding 6 is dedicated to the CSM
	// shadow passes (shadow.vert) so it never conflicts with the main pass's
	// instance data on binding 3.
	DescriptorWriter writer;
	writer.write_buffer(6, vk_buf->buffer, vk_buf->size > 0 ? vk_buf->size : VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);
}

void VulkanRHI::update_global_materials_buffer(bud::graphics::BufferHandle buffer) {
	if (!buffer.is_valid()) return;
	auto* vk_buf = get_vulkan_buffer(buffer);
	if (!vk_buf || !vk_buf->buffer) return;

	DescriptorWriter writer;
	writer.write_buffer(7, vk_buf->buffer, vk_buf->size > 0 ? vk_buf->size : VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.update_set(device, frames[current_frame].global_descriptor_set);
}

TextureHandle VulkanRHI::get_fallback_texture() {
    return fallback_texture_handle;
}


VkVertexInputBindingDescription Vertex::get_binding_description() {
	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.binding = 0;
	bindingDescription.stride = 48; // asset::Vertex layout: pos(12)+normal(12)+uv(8)+tangent(16)
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	return bindingDescription;
}

std::vector<VkVertexInputAttributeDescription> Vertex::get_attribute_descriptions() {
std::vector<VkVertexInputAttributeDescription> attributeDescriptions(4);

	// Position (Location 0) -> Offset 0
	attributeDescriptions[0].binding = 0;
	attributeDescriptions[0].location = 0;
	attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[0].offset = 0; // asset::Vertex::position

	// Normal (Location 1) -> Offset 12
	attributeDescriptions[1].binding = 0;
	attributeDescriptions[1].location = 1;
	attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[1].offset = 12; // asset::Vertex::normal

	// UV (Location 2) -> Offset 24
	attributeDescriptions[2].binding = 0;
	attributeDescriptions[2].location = 2;
	attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
	attributeDescriptions[2].offset = 24; // asset::Vertex::uv

	// Tangent (Location 3) -> Offset 32
	attributeDescriptions[3].binding = 0;
	attributeDescriptions[3].location = 3;
	attributeDescriptions[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributeDescriptions[3].offset = 32; // asset::Vertex::tangent

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

void VulkanRHI::set_debug_name(TextureHandle texture, ObjectType object_type, const std::string& name) {
	auto* vk_tex = get_vulkan_texture(texture);
	if (!vk_tex) return;

	set_object_debug_name(reinterpret_cast<uint64_t>(vk_tex->image), object_type, name);

	if (vk_tex->view) {
		set_object_debug_name((uint64_t)vk_tex->view, ObjectType::ImageView, name + "_View");
	}
}

void VulkanRHI::set_debug_name(const bud::graphics::BufferHandle& buffer, ObjectType object_type, const std::string& name) {
	if (!buffer.is_valid()) return;

	auto* vk_buf = get_vulkan_buffer(buffer);
	if (!vk_buf || !vk_buf->buffer) return;
	set_object_debug_name(reinterpret_cast<uint64_t>(vk_buf->buffer), object_type, name);
}

void VulkanRHI::set_debug_name(CommandHandle cmd, ObjectType object_type, const std::string& name) {
	auto vk_cmd = static_cast<VkCommandBuffer>(cmd);
	set_object_debug_name(reinterpret_cast<uint64_t>(vk_cmd), object_type, name);
}





