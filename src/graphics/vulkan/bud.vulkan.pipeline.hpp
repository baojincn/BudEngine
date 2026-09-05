#pragma once

#include <vulkan/vulkan.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional> // for std::hash
#include <stdexcept>

#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics::vulkan {

	// 创建管线所需的所有状态
	 struct PipelineKey {
		VkShaderModule vert_shader;
		VkShaderModule frag_shader;
		VkShaderModule task_shader;   // Task shader (mesh shader pipeline)
		VkShaderModule mesh_shader;   // Mesh shader (mesh shader pipeline)
		VkRenderPass render_pass;
		VkBool32 depth_test;
		VkBool32 depth_write;
		VkBool32 depth_bias_enable;
		VkBool32 blending_enable;
		BlendMode blend_mode;
		VertexLayoutType vertex_layout;
		VkPrimitiveTopology topology;
		VkCompareOp depth_compare_op;
		VkCullModeFlags cull_mode;
		VkFormat color_format;
		VkFormat depth_format;
		VkBool32 wireframe;

		bool operator==(const PipelineKey& other) const {
			return vert_shader == other.vert_shader &&
				frag_shader == other.frag_shader &&
				task_shader == other.task_shader &&
				mesh_shader == other.mesh_shader &&
				render_pass == other.render_pass &&
				depth_test == other.depth_test &&
				depth_write == other.depth_write &&
				depth_bias_enable == other.depth_bias_enable &&
				blending_enable == other.blending_enable &&
				blend_mode == other.blend_mode &&
				vertex_layout == other.vertex_layout &&
				topology == other.topology &&
				depth_compare_op == other.depth_compare_op &&
				cull_mode == other.cull_mode &&
				color_format == other.color_format &&
				depth_format == other.depth_format &&
				wireframe == other.wireframe;
		}
	};

	 struct PipelineKeyHash {
		std::size_t operator()(const PipelineKey& k) const {
			return std::hash<void*>()(k.vert_shader) ^
				(std::hash<void*>()(k.frag_shader) << 1) ^
				(std::hash<void*>()(k.task_shader) << 2) ^
				(std::hash<void*>()(k.mesh_shader) << 3) ^
				(std::hash<uint32_t>()(k.cull_mode) << 4) ^
				(std::hash<uint32_t>()(k.color_format) << 5) ^
				(std::hash<uint32_t>()(k.depth_compare_op) << 6) ^
				(std::hash<uint32_t>()(k.depth_write) << 7) ^
				(std::hash<uint32_t>()(k.blending_enable) << 8) ^
				(std::hash<uint32_t>()((uint32_t)k.blend_mode) << 9) ^
				(std::hash<uint32_t>()((uint32_t)k.vertex_layout) << 10) ^
				(std::hash<uint32_t>()(k.topology) << 11) ^
				(std::hash<uint32_t>()(k.depth_bias_enable) << 12) ^
				(std::hash<uint32_t>()(k.depth_format) << 13) ^
				(std::hash<uint32_t>()(k.wireframe) << 14);
		}
	};


	 class VulkanPipelineCache {
	public:
		void init(VkDevice device);
		void cleanup();
		VkPipeline get_pipeline(const PipelineKey& key, VkPipelineLayout layout, bool is_depth_only = false);
		VkPipeline create_compute_pipeline(VkShaderModule compute_shader, VkPipelineLayout layout);

		// Release a pipeline reference; destroy underlying VkPipeline when refcount reaches zero
		void release_pipeline(VkPipeline pipeline);

	private:
    VkDevice device;
    // Store pipeline and a reference count so pipelines can be released when no longer used
    struct PipelineEntry { VkPipeline pipeline; uint32_t refcount; };
    std::unordered_map<PipelineKey, PipelineEntry, PipelineKeyHash> cache;
    // Reverse map for O(1) lookup from VkPipeline -> PipelineKey
    std::unordered_map<VkPipeline, PipelineKey> pipeline_to_key;
    // Compute pipelines tracking
    std::vector<VkPipeline> compute_pipelines;
    std::unordered_set<VkPipeline> compute_pipeline_set;


		VkPipeline create_pipeline_internal(const PipelineKey& key, VkPipelineLayout layout, bool is_depth_only);
	};
}
