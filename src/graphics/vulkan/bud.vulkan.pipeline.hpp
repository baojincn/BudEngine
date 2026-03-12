#pragma once

#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <functional> // for std::hash
#include <stdexcept>

#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics::vulkan {

	// 创建管线所需的所有状态
	 struct PipelineKey {
		VkShaderModule vert_shader;
		VkShaderModule frag_shader;
		VkRenderPass render_pass;
		VkBool32 depth_test;
		VkBool32 depth_write;
		VkBool32 depth_bias_enable;
		VkBool32 blending_enable;
		VkBool32 is_ui_layout;
		VkCompareOp depth_compare_op;
		VkCullModeFlags cull_mode;
		VkFormat color_format;
		VkFormat depth_format;

		bool operator==(const PipelineKey& other) const {
			return vert_shader == other.vert_shader &&
				frag_shader == other.frag_shader &&
				render_pass == other.render_pass &&
				depth_test == other.depth_test &&
				depth_write == other.depth_write &&
				depth_bias_enable == other.depth_bias_enable &&
				blending_enable == other.blending_enable &&
				is_ui_layout == other.is_ui_layout &&
				depth_compare_op == other.depth_compare_op &&
				cull_mode == other.cull_mode &&
				color_format == other.color_format &&
				depth_format == other.depth_format;
		}
	};

	 struct PipelineKeyHash {
		std::size_t operator()(const PipelineKey& k) const {
			return std::hash<void*>()(k.vert_shader) ^
				(std::hash<void*>()(k.frag_shader) << 1) ^
				(std::hash<uint32_t>()(k.cull_mode) << 2) ^
				(std::hash<uint32_t>()(k.color_format) << 3) ^
				(std::hash<uint32_t>()(k.depth_compare_op) << 4) ^
				(std::hash<uint32_t>()(k.depth_write) << 5) ^
				(std::hash<uint32_t>()(k.blending_enable) << 6) ^
				(std::hash<uint32_t>()(k.is_ui_layout) << 7) ^
				(std::hash<uint32_t>()(k.depth_bias_enable) << 8) ^
				(std::hash<uint32_t>()(k.depth_format) << 9);
		}
	};


	 class VulkanPipelineCache {
	public:
		void init(VkDevice device);
		void cleanup();
		VkPipeline get_pipeline(const PipelineKey& key, VkPipelineLayout layout, bool is_depth_only = false);

	private:
		VkDevice device;
		std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> cache;

		VkPipeline create_pipeline_internal(const PipelineKey& key, VkPipelineLayout layout, bool is_depth_only);
	};
}
