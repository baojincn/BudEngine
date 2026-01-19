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

module bud.graphics.vulkan;

import bud.io;
import bud.math;
import bud.platform;
import bud.threading;
import bud.graphics;

using namespace bud::graphics;
using namespace bud::graphics::vulkan;

VkVertexInputBindingDescription Vertex::get_binding_description() {
	VkVertexInputBindingDescription binding_description{};
	binding_description.binding = 0;
	binding_description.stride = sizeof(Vertex);
	binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	return binding_description;
}


std::vector<VkVertexInputAttributeDescription> Vertex::get_attribute_descriptions() {
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


VulkanLayoutTransition bud::graphics::vulkan::get_vk_transition(bud::graphics::ResourceState state) {
	switch (state) {
	case bud::graphics::ResourceState::Undefined:
		return { VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };

	case bud::graphics::ResourceState::RenderTarget:
		return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

	case bud::graphics::ResourceState::ShaderResource:
		return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				 VK_ACCESS_SHADER_READ_BIT,
				 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };

	case bud::graphics::ResourceState::DepthWrite:
		return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };

	case bud::graphics::ResourceState::Present:
		return { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				 0,
				 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT };

	default:
		return { VK_IMAGE_LAYOUT_GENERAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
	}
}



void VulkanRHI::init(bud::platform::Window* plat_window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation) {
	this->task_scheduler = task_scheduler;
	auto window = plat_window->get_sdl_window();

	// Core Vulkan Setup
	create_instance(window, enable_validation);
	setup_debug_messenger(enable_validation);
	create_surface(window);
	pick_physical_device();
	create_logical_device(enable_validation);

	// Presentation Setup
	create_swapchain(window);
	create_image_views();

	create_depth_resources();

	// Pipeline Setup
	create_main_render_pass();

	// Shadow map
	create_shadow_resources();
	create_shadow_render_pass();
	create_shadow_framebuffer();

	create_descriptor_set_layout(); // Layout 必须在 Pipeline 之前

	create_shadow_pipeline();
	create_graphics_pipeline();

	create_framebuffers();

	// Resources Setup
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

	// Command & Sync
	create_command_buffer();
	create_sync_objects();
}


void VulkanRHI::cmd_resource_barrier(CommandHandle cmd, RHITexture* texture, bud::graphics::ResourceState old_state, bud::graphics::ResourceState new_state) {
	auto vk_cmd = static_cast<VkCommandBuffer>(cmd);
	auto vk_tex = static_cast<VulkanTexture*>(texture);

	auto src = get_vk_transition(old_state);
	auto dst = get_vk_transition(new_state);

	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = src.layout;
	barrier.newLayout = dst.layout;
	barrier.srcAccessMask = src.access;
	barrier.dstAccessMask = dst.access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = vk_tex->image;

	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(
		vk_cmd,
		src.stage,
		dst.stage,
		0,
		0, nullptr,
		0, nullptr,
		1, &barrier
	);
}


void VulkanRHI::set_config(const RenderConfig& new_settings) {
	settings = new_settings;
}

void VulkanRHI::draw_frame(const bud::math::mat4& view, const bud::math::mat4& proj) {
	FrameData& frame = frames[current_frame];

	// 等上一帧资源的操作完成
	vkWaitForFences(device, 1, &frame.in_flight_fence, VK_TRUE, UINT64_MAX);

	uint32_t image_index;
	VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frame.image_available_semaphore, VK_NULL_HANDLE, &image_index);

	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		return;
	}

	vkResetFences(device, 1, &frame.in_flight_fence);

	auto lightProj = bud::math::ortho_vk(
		-settings.shadow_ortho_size, settings.shadow_ortho_size,
		-settings.shadow_ortho_size, settings.shadow_ortho_size,
		settings.shadow_near_plane, settings.shadow_far_plane
	);

	auto lightView = bud::math::lookAt(settings.directional_light_position, bud::math::vec3(0.0f), bud::math::vec3(1.0f, 0.0f, 0.0f));

	bud::math::mat4 lightSpaceMatrix = lightProj * lightView;
	bud::math::mat4 modelMatrix = bud::math::mat4(1.0f);

	// 更新 UBO, 最好也做多帧缓冲，这里暂且复用
	update_uniform_buffer(image_index, view, proj, lightSpaceMatrix);

	// 重置当前帧的所有 Command Pools
	vkResetCommandPool(device, frame.main_command_pool, 0);
	for (auto pool : frame.worker_pools) {
		vkResetCommandPool(device, pool, 0);
	}

	// 重置命令计数器, 不释放内存，只把游标指回 0，下一帧覆盖使用旧的 Handles
	std::fill(frame.worker_cmd_counters.begin(), frame.worker_cmd_counters.end(), 0);

	VkCommandBuffer cmd = frame.main_command_buffer;
	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &begin_info);


	// Begin, Pass 1: Shadow Map
	{
		VkRenderPassBeginInfo shadow_pass_info{};
		shadow_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		shadow_pass_info.renderPass = shadow_render_pass;
		shadow_pass_info.framebuffer = shadow_framebuffer;
		shadow_pass_info.renderArea.extent.width = settings.shadow_map_size;
		shadow_pass_info.renderArea.extent.height = settings.shadow_map_size;

		VkClearValue clear_values[1] = {};
		clear_values[0].depthStencil = { 1.0f, 0 };
		shadow_pass_info.clearValueCount = 1;
		shadow_pass_info.pClearValues = clear_values;

		// Record Shadow Pass on main thread
		vkCmdBeginRenderPass(cmd, &shadow_pass_info, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);

		VkViewport viewport{};
		viewport.width = (float)settings.shadow_map_size;
		viewport.height = (float)settings.shadow_map_size;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		VkRect2D scissor{};
		scissor.extent.width = settings.shadow_map_size;
		scissor.extent.height = settings.shadow_map_size;
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		vkCmdSetDepthBias(cmd, settings.shadow_bias_constant, 0.0f, settings.shadow_bias_slope);

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_layout, 0, 1, &descriptor_sets[image_index], 0, nullptr);

		ShadowConstantData shadow_constant_data{};
		shadow_constant_data.lightMVP = lightSpaceMatrix * modelMatrix;
		auto dir = bud::math::normalize(bud::math::vec3(0.0f) - settings.directional_light_position);
		shadow_constant_data.lightDir = bud::math::vec4(dir, 0.0f);


		vkCmdPushConstants(cmd, shadow_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowConstantData), &shadow_constant_data);

		if (vertex_buffer && index_buffer && !indices.empty()) {
			VkBuffer vbs[] = { vertex_buffer };
			VkDeviceSize offs[] = { 0 };
			vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
			vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);

			vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
		}

		vkCmdEndRenderPass(cmd);
	}
	// End, Pass 1


	// Begin, Pass 2: Main Scene Rendering
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

	// 并行录制任务
	bud::threading::Counter recording_dependency;
	std::mutex recorded_cmds_mutex;
	std::vector<VkCommandBuffer> secondary_cmds;

	task_scheduler->spawn("DrawTask", [&, current_frame_idx = current_frame, img_idx = image_index]() {
		size_t worker_id = bud::threading::t_worker_index;

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
		viewport.width = (float)swapchain_extent.width;
		viewport.height = (float)swapchain_extent.height;
		viewport.maxDepth = 1.0f;
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


	// End, Pass 2: Main Scene Rendering

	task_scheduler->wait_for_counter(recording_dependency);

	if (!secondary_cmds.empty()) {
		vkCmdExecuteCommands(cmd, (uint32_t)secondary_cmds.size(), secondary_cmds.data());
	}

	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);



	// 提交
	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkSemaphore wait_semaphores[] = { frame.image_available_semaphore };
	VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = wait_semaphores;
	submit_info.pWaitDstStageMask = wait_stages;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &cmd; // 使用当前帧的 cmd
	VkSemaphore signal_semaphores[] = { render_finished_semaphores[image_index]};
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

	// 切换到下一帧
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRHI::wait_idle() {
	if (device) vkDeviceWaitIdle(device);
}

void VulkanRHI::cleanup() {
	wait_idle();

	if (shadow_pipeline) {
		vkDestroyPipeline(device, shadow_pipeline, nullptr);
	}

	if (shadow_pipeline_layout) {
		vkDestroyPipelineLayout(device, shadow_pipeline_layout, nullptr);
	}

	for (auto view : texture_views) {
		vkDestroyImageView(device, view, nullptr);
	}

	for (auto image : texture_images) {
		vkDestroyImage(device, image, nullptr);
	}

	for (auto memory : texture_images_memories) {
		vkFreeMemory(device, memory, nullptr);
	}

	texture_views.clear();
	texture_images.clear();
	texture_images_memories.clear();


	if (shadow_framebuffer)
		vkDestroyFramebuffer(device, shadow_framebuffer, nullptr);

	if (shadow_render_pass)
		vkDestroyRenderPass(device, shadow_render_pass, nullptr);

	if (shadow_sampler)
		vkDestroySampler(device, shadow_sampler, nullptr);

	if (shadow_image_view)
		vkDestroyImageView(device, shadow_image_view, nullptr);

	if (shadow_image)
		vkDestroyImage(device, shadow_image, nullptr);

	if (shadow_image_memory)
		vkFreeMemory(device, shadow_image_memory, nullptr);

	vkDestroyImageView(device, depth_image_view, nullptr);
	vkDestroyImage(device, depth_image, nullptr);
	vkFreeMemory(device, depth_image_memory, nullptr);

	for (auto semaphore : render_finished_semaphores) {
		vkDestroySemaphore(device, semaphore, nullptr);
	}

	render_finished_semaphores.clear();

	for (int i = 0; i < max_frames_in_flight; i++) {
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
void VulkanRHI::reload_shaders_async() {
	task_scheduler->spawn("AsyncShaderLoad", [this]() {
		auto vert_opt = bud::io::FileSystem::read_binary("src/shaders/main.vert.spv");
		auto frag_opt = bud::io::FileSystem::read_binary("src/shaders/main.frag.spv");

		if (!vert_opt || !frag_opt) return;

		// 移动所有权给主线程
		task_scheduler->submit_main_thread_task([this,
			vert = std::move(*vert_opt),
			frag = std::move(*frag_opt)]() {

			this->recreate_graphics_pipeline(vert, frag);
		});
	});
}

void VulkanRHI::load_model_async(const std::string& filepath) {
	task_scheduler->spawn("AsyncModelLoad", [this, filepath]() {
		// IO 线程解析 OBJ
		auto mesh_opt = bud::io::ModelLoader::load_obj(filepath);
		if (!mesh_opt) return;

		// 主线程上传 GPU
		task_scheduler->submit_main_thread_task([this, mesh = std::move(*mesh_opt)]() {
			this->upload_mesh(mesh);
		});
	});
}


CommandHandle VulkanRHI::begin_frame() {
	vkWaitForFences(device, 1, &frames[current_frame].in_flight_fence, VK_TRUE, UINT64_MAX);

	VkResult result = vkAcquireNextImageKHR(
		device,
		swapchain,
		UINT64_MAX,
		frames[current_frame].image_available_semaphore,
		VK_NULL_HANDLE,
		&current_image_index
	);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		return nullptr;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	vkResetFences(device, 1, &frames[current_frame].in_flight_fence);

	vkResetCommandBuffer(frames[current_frame].main_command_buffer, 0);

	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(frames[current_frame].main_command_buffer, &begin_info) != VK_SUCCESS) {
		throw std::runtime_error("failed to begin recording command buffer!");
	}

	return frames[current_frame].main_command_buffer;
}

void VulkanRHI::end_frame(CommandHandle cmd) {
	VkCommandBuffer command_buffer = static_cast<VkCommandBuffer>(cmd);

	if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to record command buffer!");
	}

	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore wait_semaphores[] = { frames[current_frame].image_available_semaphore };
	VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = wait_semaphores;
	submit_info.pWaitDstStageMask = wait_stages;

	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffer;

	VkSemaphore signal_semaphores[] = { render_finished_semaphores[current_image_index]};
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = signal_semaphores;

	if (vkQueueSubmit(graphics_queue, 1, &submit_info, frames[current_frame].in_flight_fence) != VK_SUCCESS) {
		throw std::runtime_error("failed to submit draw command buffer!");
	}

	VkPresentInfoKHR present_info{};
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = signal_semaphores;

	VkSwapchainKHR swap_chains[] = { swapchain };
	present_info.swapchainCount = 1;
	present_info.pSwapchains = swap_chains;
	present_info.pImageIndices = &current_image_index;

	vkQueuePresentKHR(present_queue, &present_info);

	current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRHI::cmd_bind_pipeline(CommandHandle cmd, void* pipeline) {
	vkCmdBindPipeline(
		static_cast<VkCommandBuffer>(cmd),
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		static_cast<VkPipeline>(pipeline)
	);
}

void VulkanRHI::cmd_draw(CommandHandle cmd, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
	vkCmdDraw(
		static_cast<VkCommandBuffer>(cmd),
		vertex_count,
		instance_count,
		first_vertex,
		first_instance
	);
}

RHITexture* VulkanRHI::get_current_swapchain_texture() {
	if (current_image_index >= swapchain_textures_wrappers.size())
		return nullptr;

	return &swapchain_textures_wrappers[current_image_index];
}

uint32_t VulkanRHI::get_current_image_index() {
	return current_image_index;
}



void VulkanRHI::create_shadow_pipeline() {
	auto vert_code = bud::io::FileSystem::read_binary("src/shaders/shadow.vert.spv");
	auto frag_code = bud::io::FileSystem::read_binary("src/shaders/shadow.frag.spv");

	if (!vert_code)
		throw std::runtime_error("Failed to load shadow.vert.spv!");

	if (!frag_code)
		throw std::runtime_error("Failed to load shadow.frag.spv!");

	VkShaderModule vert_module = create_shader_module(*vert_code);
	VkShaderModule frag_module = create_shader_module(*frag_code);

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

	// 复用现有的 Vertex 结构
	auto bindingDescription = Vertex::get_binding_description();
	auto attributeDescriptions = Vertex::get_attribute_descriptions();

	VkPipelineVertexInputStateCreateInfo vertex_input_info{};
	vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_info.vertexBindingDescriptionCount = 1;
	vertex_input_info.pVertexBindingDescriptions = &bindingDescription;
	vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
	vertex_input_info.pVertexAttributeDescriptions = attributeDescriptions.data();

	// Input Assembly
	VkPipelineInputAssemblyStateCreateInfo input_assembly{};
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	input_assembly.primitiveRestartEnable = VK_FALSE;

	// Viewport & Scissor (设为 Dynamic，因为阴影图尺寸可能变)
	VkPipelineViewportStateCreateInfo viewport_state{};
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;

	// Rasterizer
	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;

	// Shadow Trick 1, 剔除正面
	// 修复彼得潘现象-漏光，让阴影根部更实
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	// Shadow Trick 2, 深度偏移
	// 修复阴影条纹 - Shadow Acne
	rasterizer.depthBiasEnable = VK_TRUE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Depth Stencil
	VkPipelineDepthStencilStateCreateInfo depth_stencil{};
	depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil.depthTestEnable = VK_TRUE;
	depth_stencil.depthWriteEnable = VK_TRUE;
	depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depth_stencil.depthBoundsTestEnable = VK_FALSE;
	depth_stencil.stencilTestEnable = VK_FALSE;

	// Color Blend, 全部禁用
	VkPipelineColorBlendStateCreateInfo color_blending{};
	color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blending.logicOpEnable = VK_FALSE;
	color_blending.attachmentCount = 0;

	// Dynamic States
	std::vector<VkDynamicState> dynamic_states = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_DEPTH_BIAS
	};

	VkPipelineDynamicStateCreateInfo dynamic_state_info{};
	dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic_state_info.pDynamicStates = dynamic_states.data();

	// Pipeline Layout, configs Push Constant
	VkPushConstantRange push_constant_range{};
	push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push_constant_range.offset = 0;
	push_constant_range.size = sizeof(ShadowConstantData);

	VkPipelineLayoutCreateInfo pipeline_layout_info{};
	pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts = &descriptor_set_layout; // 复用
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges = &push_constant_range;

	if (vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &shadow_pipeline_layout) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create shadow pipeline layout!");
	}

	// Create Pipeline
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
	pipeline_info.layout = shadow_pipeline_layout;
	pipeline_info.renderPass = shadow_render_pass;
	pipeline_info.subpass = 0;

	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &shadow_pipeline) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create shadow pipeline!");
	}

	vkDestroyShaderModule(device, vert_module, nullptr);
	vkDestroyShaderModule(device, frag_module, nullptr);
}

void VulkanRHI::create_shadow_framebuffer() {
	VkFramebufferCreateInfo framebuffer_info{};
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = shadow_render_pass;
	framebuffer_info.attachmentCount = 1;
	framebuffer_info.pAttachments = &shadow_image_view;
	framebuffer_info.width = settings.shadow_map_size;
	framebuffer_info.height = settings.shadow_map_size;
	framebuffer_info.layers = 1;

	if (vkCreateFramebuffer(device, &framebuffer_info, nullptr, &shadow_framebuffer) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create shadow framebuffer!");
	}
}

void VulkanRHI::create_shadow_render_pass() {
	VkAttachmentDescription depth_attachment{};
	depth_attachment.format = find_depth_format();
	depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;

	// STORE_OP_STORE: 保留渲染数据
	depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	// 渲染完后，转为 SHADER_READ_ONLY，方便主 Pass 读取
	depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

	VkAttachmentReference depth_attachment_ref{};
	depth_attachment_ref.attachment = 0;
	depth_attachment_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 0;
	subpass.pDepthStencilAttachment = &depth_attachment_ref;

	// 处理依赖关系
	std::array<VkSubpassDependency, 2> dependencies{};
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	// 进入 Pass 前：确保之前的读取结束
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	// 离开 Pass 后：确保写入结束
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	VkRenderPassCreateInfo render_pass_info{};
	render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.attachmentCount = 1;
	render_pass_info.pAttachments = &depth_attachment;
	render_pass_info.subpassCount = 1;
	render_pass_info.pSubpasses = &subpass;
	render_pass_info.dependencyCount = static_cast<uint32_t>(dependencies.size());
	render_pass_info.pDependencies = dependencies.data();

	if (vkCreateRenderPass(device, &render_pass_info, nullptr, &shadow_render_pass) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create shadow render pass!");
	}
}

void VulkanRHI::create_shadow_resources() {
	VkFormat depth_format = find_depth_format();

	// 创建 Image
	// DEPTH_STENCIL_ATTACHMENT (作为渲染目标), SAMPLED (作为贴图被 Shader 读取)
	create_image(settings.shadow_map_size, settings.shadow_map_size, 1, depth_format, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		shadow_image, shadow_image_memory);

	// 创建 View
	VkImageViewCreateInfo view_info{};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = shadow_image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = depth_format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	view_info.subresourceRange.baseMipLevel = 0;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.baseArrayLayer = 0;
	view_info.subresourceRange.layerCount = 1;

	if (vkCreateImageView(device, &view_info, nullptr, &shadow_image_view) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create shadow image view!");
	}

	// 创建 Sampler
	VkSamplerCreateInfo sampler_info{};
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_LINEAR;
	sampler_info.minFilter = VK_FILTER_LINEAR;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

	// 边界颜色设为白色 = 1.0, depth > shadow_map_value 才会有阴影。
	sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

	sampler_info.mipLodBias = 0.0f;
	sampler_info.minLod = 0.0f;
	sampler_info.maxLod = 1.0f;
	sampler_info.anisotropyEnable = VK_FALSE;

	sampler_info.compareEnable = VK_TRUE;
	sampler_info.compareOp = VK_COMPARE_OP_LESS;

	if (vkCreateSampler(device, &sampler_info, nullptr, &shadow_sampler) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create shadow sampler!");
	}
}

void VulkanRHI::create_depth_resources() {
	VkFormat depth_format = find_depth_format();
	create_image(swapchain_extent.width, swapchain_extent.height, 1, depth_format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depth_image, depth_image_memory);

	// 创建 View, aspectMask = DEPTH
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

VkFormat VulkanRHI::find_supported_format(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
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

VkFormat VulkanRHI::find_depth_format() {
	return find_supported_format(
		{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
	);
}

void VulkanRHI::create_instance(SDL_Window* window, bool enable_validation) {
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

void VulkanRHI::create_surface(SDL_Window* window) {
	if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
		throw std::runtime_error("Failed to create Window Surface!");
	}
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

void VulkanRHI::create_swapchain(SDL_Window* window) {
	SwapChainSupportDetails swapchain_support = query_swapchain_support(physical_device);
	VkSurfaceFormatKHR surface_format = choose_swap_surface_format(swapchain_support.formats);
	VkPresentModeKHR present_mode = choose_swap_present_mode(swapchain_support.present_modes);
	VkExtent2D extent = choose_swap_extent(swapchain_support.capabilities, window);

	auto image_count = swapchain_support.capabilities.minImageCount + 1;

	if (swapchain_support.capabilities.maxImageCount > 0 && image_count > swapchain_support.capabilities.maxImageCount) {
		image_count = swapchain_support.capabilities.maxImageCount;
	}

	max_frames_in_flight = image_count + 1;
	frames.resize(max_frames_in_flight);

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

void VulkanRHI::create_image_views() {
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

	swapchain_textures_wrappers.resize(swapchain_images.size());
	for (size_t i = 0; i < swapchain_images.size(); i++) {
		swapchain_textures_wrappers[i].image = swapchain_images[i];
		swapchain_textures_wrappers[i].view = swapchain_image_views[i];
		swapchain_textures_wrappers[i].memory = VK_NULL_HANDLE; // 内存由驱动管理
	}
}

void VulkanRHI::create_main_render_pass() {
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

void VulkanRHI::create_descriptor_set_layout() {
	// UBO
	VkDescriptorSetLayoutBinding ubo_layout_binding{};
	ubo_layout_binding.binding = 0;
	ubo_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	ubo_layout_binding.descriptorCount = 1;
	ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	ubo_layout_binding.pImmutableSamplers = nullptr;

	// Texture array
	VkDescriptorSetLayoutBinding sampler_layout_binding{};
	sampler_layout_binding.binding = 1;
	sampler_layout_binding.descriptorCount = 100;
	sampler_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sampler_layout_binding.pImmutableSamplers = nullptr;
	sampler_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	// Shadow map
	VkDescriptorSetLayoutBinding shadow_layout_binding{};
	shadow_layout_binding.binding = 2;
	shadow_layout_binding.descriptorCount = 1;
	shadow_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	shadow_layout_binding.pImmutableSamplers = nullptr;
	shadow_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	std::array<VkDescriptorSetLayoutBinding, 3> bindings = {
		ubo_layout_binding,
		sampler_layout_binding,
		shadow_layout_binding
	};

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

	VkDescriptorBindingFlags binding_flags[] = {
	  0,	// UBO 
	  VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,	// Texture array
	  0		// Shadow
	};

	VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
	flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	flags_info.bindingCount = 3;
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
void VulkanRHI::setup_pipeline_state(VkShaderModule vert_module, VkShaderModule frag_module) {
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

void VulkanRHI::create_graphics_pipeline() {
	auto vert_shader_code = bud::io::FileSystem::read_binary("src/shaders/main.vert.spv");
	auto frag_shader_code = bud::io::FileSystem::read_binary("src/shaders/main.frag.spv");

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

// 仅重建管线的内部函数
void VulkanRHI::recreate_graphics_pipeline(const std::vector<char>& vert_code, const std::vector<char>& frag_code) {
	std::println("[Main] Recreating Pipeline for Hot-Reload...");

	vkDeviceWaitIdle(device);

	// 销毁旧管线
	if (graphics_pipeline) {
		vkDestroyPipeline(device, graphics_pipeline, nullptr);
		graphics_pipeline = nullptr;
	}

	// 创建新 ShaderModules
	VkShaderModule vert_module = create_shader_module(vert_code);
	VkShaderModule frag_module = create_shader_module(frag_code);

	// 创建新管线
	try {
		setup_pipeline_state(vert_module, frag_module);
		std::println("[Main] Hot-Reload Success!");
	}
	catch (const std::exception& e) {
		std::println(stderr, "[Main] Hot-Reload Failed: {}", e.what());
	}

	vkDestroyShaderModule(device, frag_module, nullptr);
	vkDestroyShaderModule(device, vert_module, nullptr);
}

void VulkanRHI::create_framebuffers() {
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

void VulkanRHI::create_command_pool() {
	QueueFamilyIndices queue_family_indices = find_queue_families(physical_device);
	size_t thread_count = task_scheduler ? task_scheduler->get_thread_count() : 1;

	for (int i = 0; i < max_frames_in_flight; i++) {
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


void VulkanRHI::copy_buffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer command_buffer = begin_single_time_commands();

	VkBufferCopy copyRegion{};
	copyRegion.srcOffset = 0;
	copyRegion.dstOffset = 0;
	copyRegion.size = size;

	vkCmdCopyBuffer(command_buffer, srcBuffer, dstBuffer, 1, &copyRegion);

	end_single_time_commands(command_buffer);
}

void VulkanRHI::create_vertex_buffer() {
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

void VulkanRHI::create_index_buffer() {
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

void VulkanRHI::create_uniform_buffers() {
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


void VulkanRHI::upload_mesh(const bud::io::MeshData& mesh) {
	vkDeviceWaitIdle(device); // 等待 GPU 空闲

	// 清理旧缓冲
	if (vertex_buffer) {
		vkDestroyBuffer(device, vertex_buffer, nullptr);
		vkFreeMemory(device, vertex_buffer_memory, nullptr);
	}

	if (index_buffer) {
		vkDestroyBuffer(device, index_buffer, nullptr);
		vkFreeMemory(device, index_buffer_memory, nullptr);
	}

	// 转换数据格式 (bud::io::Vertex -> bud::graphics::Vertex)
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

	// 创建新缓冲
	create_vertex_buffer();
	create_index_buffer();


	// 清理旧的纹理 (保留 Slot 0 的 default texture)
	for (auto v : texture_views)
		vkDestroyImageView(device, v, nullptr);

	for (auto i : texture_images)
		vkDestroyImage(device, i, nullptr);

	for (auto m : texture_images_memories)
		vkFreeMemory(device, m, nullptr);

	texture_views.clear();
	texture_images.clear();
	texture_images_memories.clear();

	// 预分配空间
	size_t count = mesh.texture_paths.size();
	texture_views.resize(count);
	texture_images.resize(count);
	texture_images_memories.resize(count);

	std::println("[RHI] Loading {} textures for mesh...", count);

	// 循环加载并更新 Descriptor
	for (size_t index = 0; index < count; ++index) {
		std::string path = mesh.texture_paths[index];

		std::println("[RHI] Loading texture [{}/{}]: {}", index + 1, count, path);

		try {
			create_texture_from_file(path, texture_images[index], texture_images_memories[index], texture_views[index]);
		}
		catch (const std::exception& e) {
			std::println("Error loading texture: {} | Reason: {}", path, e.what());

			if (index > 0 && texture_views[0]) {
				std::println("   -> Using fallback texture (Slot 0)");

				// 这种简单的复用在清理时可能会有问题（重复 destroy），
				create_texture_from_file("data/textures/default.png", texture_images[index], texture_images_memories[index], texture_views[index]);
			}
		}

		// 准备 Descriptor 信息
		VkDescriptorImageInfo image_info{};
		image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_info.imageView = texture_views[index];
		image_info.sampler = texture_sampler;

		VkWriteDescriptorSet descriptor_write{};
		descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write.dstBinding = 1;
		descriptor_write.dstArrayElement = static_cast<uint32_t>(index + 1);
		descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptor_write.descriptorCount = 1;
		descriptor_write.pImageInfo = &image_info;

		// 更新所有 Frame 的 descriptor_set
		for (size_t i = 0; i < swapchain_images.size(); i++) {
			descriptor_write.dstSet = descriptor_sets[i];
			vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);
		}

		// 打印进度，防止以为卡死
		if ((index + 1) % 5 == 0)
			std::println("[RHI] Loaded {}/{} textures...", index + 1, count);
	}
}


void VulkanRHI::create_texture_from_file(const std::string& path, VkImage& out_image, VkDeviceMemory& out_mem, VkImageView& out_view) {
	auto img_opt = bud::io::ImageLoader::load(path);

	// 加载失败就生成一个粉色 1x1 像素作为占位，防止崩
	if (!img_opt) {
		std::println("[RHI] Failed to load texture: {}. Using fallback.", path);
		// 这里可以写个生成 1x1 纯色像素的逻辑，或者简单抛出异常
		throw std::runtime_error("Texture load failed");
	}
	auto& img = *img_opt;

	// 创建 Staging Buffer
	VkDeviceSize image_size = img.width * img.height * 4;
	VkBuffer staging_buffer;
	VkDeviceMemory staging_buffer_memory;
	create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_buffer, staging_buffer_memory);

	void* data;
	vkMapMemory(device, staging_buffer_memory, 0, image_size, 0, &data);
	memcpy(data, img.pixels, static_cast<size_t>(image_size));
	vkUnmapMemory(device, staging_buffer_memory);

	// 计算 Mipmaps
	uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(std::max(img.width, img.height)))) + 1;

	// 创建 Image
	create_image(img.width, img.height, mips, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out_image, out_mem);

	// 拷贝数据 + 生成 Mipmaps
	transition_image_layout(out_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	copy_buffer_to_image(staging_buffer, out_image, static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height));
	generate_mipmaps(out_image, VK_FORMAT_R8G8B8A8_SRGB, img.width, img.height, mips);

	// 清理 Staging
	vkDestroyBuffer(device, staging_buffer, nullptr);
	vkFreeMemory(device, staging_buffer_memory, nullptr);

	// 创建 View
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

void VulkanRHI::create_texture_image() {
	// 手动造一个 1x1 的白色像素 (R=255, G=255, B=255, A=255)
	uint32_t pixel = 0xFFFFFFFF;
	int tex_width = 1;
	int tex_height = 1;
	VkDeviceSize image_size = 4;

	// 创建 Staging Buffer
	VkBuffer staging_buffer;
	VkDeviceMemory staging_buffer_memory;
	create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		staging_buffer, staging_buffer_memory);

	// 拷贝数据
	void* data;
	vkMapMemory(device, staging_buffer_memory, 0, image_size, 0, &data);
	memcpy(data, &pixel, static_cast<size_t>(image_size));
	vkUnmapMemory(device, staging_buffer_memory);

	mip_levels = 1; // 占位图只有 1 层

	// 创建 GPU Image
	create_image(tex_width, tex_height, mip_levels, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture_image, texture_image_memory);

	// 布局转换 + 拷贝
	transition_image_layout(texture_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	copy_buffer_to_image(staging_buffer, texture_image, static_cast<uint32_t>(tex_width), static_cast<uint32_t>(tex_height));
	transition_image_layout(texture_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// 清理
	vkDestroyBuffer(device, staging_buffer, nullptr);
	vkFreeMemory(device, staging_buffer_memory, nullptr);

	std::println("[Vulkan] Placeholder texture created (1x1).");
}

// 异步加载图片
void VulkanRHI::load_texture_async(const std::string& filename) {
	std::println("[RHI] Dispatching async texture load: {}", filename);

	task_scheduler->spawn("AsyncTextureLoad", [this, filename]() {
		// 使用 std::optional 处理可能的失败
		auto img_opt = bud::io::ImageLoader::load(filename);

		if (!img_opt)
			return;

		// img_opt 里的内存现在属于这个 lambda
		auto img = std::move(*img_opt);

		// 提交给主线程
		task_scheduler->submit_main_thread_task([this, img = std::move(img)]() mutable {
			this->update_texture_resources(img.pixels, img.width, img.height);
		});
	});
}

void VulkanRHI::update_texture_resources(unsigned char* pixels, int width, int height) {
	std::println("[Main] Texture data received. Uploading with Mipmaps...");
	vkDeviceWaitIdle(device);

	// 清理旧资源
	vkDestroyImageView(device, texture_image_view, nullptr);
	vkDestroyImage(device, texture_image, nullptr);
	vkFreeMemory(device, texture_image_memory, nullptr);

	// 计算 Mipmap 层级数, 公式：log2(max(w, h)) + 1
	mip_levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

	VkDeviceSize image_size = width * height * 4;
	VkBuffer staging_buffer;
	VkDeviceMemory staging_buffer_memory;
	create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_buffer, staging_buffer_memory);

	void* data;
	vkMapMemory(device, staging_buffer_memory, 0, image_size, 0, &data);
	memcpy(data, pixels, static_cast<size_t>(image_size));
	vkUnmapMemory(device, staging_buffer_memory);

	// 创建 Image
	// 注意 usage：除了 TRANSFER_DST (接收 buffer)，还需要 TRANSFER_SRC (作为下一级 mip 的源)
	create_image(width, height, mip_levels, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture_image, texture_image_memory);

	// 拷贝 Base Level (Level 0)
	transition_image_layout(texture_image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	copy_buffer_to_image(staging_buffer, texture_image, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

	// 生成 Mipmaps (这会自动把 layout 转为 SHADER_READ_ONLY)
	generate_mipmaps(texture_image, VK_FORMAT_R8G8B8A8_SRGB, width, height, mip_levels);

	vkDestroyBuffer(device, staging_buffer, nullptr);
	vkFreeMemory(device, staging_buffer_memory, nullptr);

	// 重建 View 和 Sampler
	texture_image_view = create_image_view(texture_image, VK_FORMAT_R8G8B8A8_SRGB);
	// 采样器可能需要重建以支持 maxLod，或者直接修改 create_texture_sampler
	// 简单起见，假设 sampler 在 init 里创建一次就够了，只要 update Descriptor 就行
	vkDestroySampler(device, texture_sampler, nullptr);
	create_texture_sampler(); // 使用新的 mip_levels 创建采样器

	update_descriptor_sets_texture();
	std::println("[Vulkan] Texture loaded with {} mip levels!", mip_levels);
}

// 只更新纹理的 Descriptor Write
void VulkanRHI::update_descriptor_sets_texture() {
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

void VulkanRHI::create_texture_image_view() {
	texture_image_view = create_image_view(texture_image, VK_FORMAT_R8G8B8A8_SRGB);
}

void VulkanRHI::create_texture_sampler() {
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

VkImageView VulkanRHI::create_image_view(VkImage image, VkFormat format) {
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

void VulkanRHI::create_image(uint32_t width, uint32_t height, uint32_t mip_levels, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& image_memory) {
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
VkCommandBuffer VulkanRHI::begin_single_time_commands() {
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

void VulkanRHI::end_single_time_commands(VkCommandBuffer command_buffer) {
	vkEndCommandBuffer(command_buffer);

	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffer;

	vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
	vkQueueWaitIdle(graphics_queue);

	vkFreeCommandBuffers(device, frames[0].main_command_pool, 1, &command_buffer);
}

void VulkanRHI::transition_image_layout(VkImage image, VkFormat format, VkImageLayout old_layout, VkImageLayout new_layout) {
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

void VulkanRHI::copy_buffer_to_image(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
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

void VulkanRHI::create_descriptor_pool() {
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

void VulkanRHI::create_descriptor_sets() {
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
		// UBO
		VkDescriptorBufferInfo buffer_info{};
		buffer_info.buffer = uniform_buffers[i];
		buffer_info.offset = 0;
		buffer_info.range = sizeof(UniformBufferObject);

		// Image Sampler
		VkDescriptorImageInfo image_info{};
		image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_info.imageView = texture_image_view;
		image_info.sampler = texture_sampler;

		// Shadow Map
		VkDescriptorImageInfo shadow_info{};
		shadow_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		shadow_info.imageView = shadow_image_view;
		shadow_info.sampler = shadow_sampler;

		std::array<VkWriteDescriptorSet, 3> descriptor_writes{};

		// Write 0: UBO
		descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_writes[0].dstSet = descriptor_sets[i];
		descriptor_writes[0].dstBinding = 0;
		descriptor_writes[0].dstArrayElement = 0;
		descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptor_writes[0].descriptorCount = 1;
		descriptor_writes[0].pBufferInfo = &buffer_info;

		// Write 1: Sampler
		descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_writes[1].dstSet = descriptor_sets[i];
		descriptor_writes[1].dstBinding = 1;
		descriptor_writes[1].dstArrayElement = 0;
		descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptor_writes[1].descriptorCount = 1;
		descriptor_writes[1].pImageInfo = &image_info;

		// Write 3:	Shadow
		descriptor_writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_writes[2].dstSet = descriptor_sets[i];
		descriptor_writes[2].dstBinding = 2;
		descriptor_writes[2].dstArrayElement = 0;
		descriptor_writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptor_writes[2].descriptorCount = 1;
		descriptor_writes[2].pImageInfo = &shadow_info;

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptor_writes.size()), descriptor_writes.data(), 0, nullptr);
	}
}

void VulkanRHI::create_command_buffer() {
	for (int i = 0; i < max_frames_in_flight; i++) {
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


// 从 Pool 分配 Secondary Buffer
VkCommandBuffer VulkanRHI::allocate_secondary_command_buffer(VkCommandPool pool) {
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

void VulkanRHI::create_sync_objects() {
	VkSemaphoreCreateInfo semaphore_info{};
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fence_info{};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
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


void VulkanRHI::record_command_buffer(VkCommandBuffer buffer, uint32_t image_index) {
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

void VulkanRHI::update_uniform_buffer(uint32_t current_image, const bud::math::mat4& view, const bud::math::mat4& proj, const bud::math::mat4& lightSpaceMatrix) {
	static auto start_time = std::chrono::high_resolution_clock::now();
	auto current_time = std::chrono::high_resolution_clock::now();
	float time = std::chrono::duration<float, std::chrono::seconds::period>(current_time - start_time).count();

	UniformBufferObject ubo{};
	ubo.model = bud::math::mat4(1.0f);
	ubo.view = view;
	ubo.proj = proj;

	// 从 View Matrix 的逆矩阵取位移
	auto invView = bud::math::inverse(view);
	ubo.camPos = bud::math::vec3(invView[3]);
	ubo.lightDir = bud::math::normalize(settings.directional_light_position - bud::math::vec3(0.0f));
	ubo.lightColor = settings.directional_light_color;
	ubo.lightIntensity = settings.directional_light_intensity;
	ubo.ambientStrength = settings.ambient_strength;
	ubo.lightSpaceMatrix = lightSpaceMatrix;

	memcpy(uniform_buffers_mapped[current_image], &ubo, sizeof(ubo));
}


VkShaderModule VulkanRHI::create_shader_module(const std::vector<char>& code) {
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

uint32_t VulkanRHI::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties mem_properties;
	vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);
	for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
		if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	throw std::runtime_error("Failed to find suitable memory type!");
}

void VulkanRHI::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& buffer_memory) {
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

VkExtent2D VulkanRHI::choose_swap_extent(const VkSurfaceCapabilitiesKHR& capabilities, SDL_Window* window) {
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

void VulkanRHI::generate_mipmaps(VkImage image, VkFormat format, int32_t tex_width, int32_t tex_height, uint32_t mip_levels) {
	// Blit 操作需要纹理格式是否支持线性过滤
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


		// 转换 Level i-1: TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL, 因为它要作为下一次 Blit 的源
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

		// 执行 Blit (缩放)
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

		// 转换 Level i-1: TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
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

	// 处理最后一层 (Level n-1)
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
VkResult VulkanRHI::create_debug_utils_messenger_ext(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if (func != nullptr) return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void VulkanRHI::destroy_debug_utils_messenger_ext(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");

	if (func != nullptr)
		func(instance, debugMessenger, pAllocator);
}

void VulkanRHI::setup_debug_messenger(bool enable) {
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

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRHI::debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		std::println(stderr, "[Validation Layer]: {}", pCallbackData->pMessage);
	}
	return VK_FALSE;
}

