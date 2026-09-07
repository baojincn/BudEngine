#pragma once

#include <vector>
#include <deque>
#include <string>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <stack>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.pool.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"


namespace bud::threading { class TaskScheduler; }

namespace bud::graphics {

	struct RGHandle {
		uint32_t id = 0;
		bool is_valid() const {
			if (id != 0)
				return true;
			return false;
		}
		auto operator<=>(const RGHandle&) const = default;
	};

	struct RGResourceNode {
		std::string name;
		TextureHandle physical_texture;
		bud::graphics::BufferHandle physical_buffer;
		bool is_buffer = false;
		TextureDesc desc;
		BufferDesc buffer_desc;
		bool is_transient = true;
		bool is_external = false;

		uint32_t version = 0;
		RGHandle parent_handle = { 0 };
		ResourceState initial_state = ResourceState::Undefined;
		bool is_active = false;
	};


	struct RGPassNode {
		std::string name;
		std::function<void(RHI*, CommandHandle)> execute;

		// Graph Connectivity
		struct Access {
			RGHandle handle;
			ResourceState state;
		};

		std::vector<Access> reads;
		std::vector<Access> writes;
		std::vector<int> dependencies; // Indices of passes this pass waits for

		// Culling info
		uint32_t ref_count = 0;
		bool has_side_effects = false;
		bool is_culled = false;
		QueueType queue_type = QueueType::Graphics;
		bool async_compute = false;

		// Declarative RenderPass Attachments
		struct AttachmentDesc {
			RGHandle handle;
			bool is_depth = false;
			bool clear = false;
			bool read_only = false;
			bud::math::vec4 clear_color{ 0.0f, 0.0f, 0.0f, 1.0f };
			float clear_depth = 1.0f;
			uint32_t base_array_layer = 0;
			uint32_t layer_count = 1;
		};
		std::vector<AttachmentDesc> color_attachments;
		AttachmentDesc depth_attachment;
		bool has_depth_attachment = false;

		// Barrier info calculated during compile()
		struct BarrierInfo { 
			RGHandle handle; 
			ResourceState old_state; 
			ResourceState new_state;
			uint32_t src_queue_family = 0xFFFFFFFF;
			uint32_t dst_queue_family = 0xFFFFFFFF;
			bool is_release = false;
			bool is_acquire = false;
		};
		std::vector<BarrierInfo> before_barriers;
		std::vector<BarrierInfo> after_barriers;
	};


	class RGBuilder {
	public:
		RGBuilder(class RenderGraph& graph, struct RGPassNode& pass)
			: render_graph(graph), pass_node(pass) {
		}

		// Declare read (Input)
		RGHandle read(RGHandle handle, ResourceState state = ResourceState::ShaderResource);

		// Declare write (Output)
		RGHandle write(RGHandle handle, ResourceState state = ResourceState::RenderTarget);

		// Create new transient resource
		RGHandle create(const std::string& name, const TextureDesc& desc);
		RGHandle create(const std::string& name, const BufferDesc& desc);

		// Declarative RenderPass Attachments
		void set_color_attachment(uint32_t slot, RGHandle handle, bool clear = false, const bud::math::vec4& clear_color = { 0.0f, 0.0f, 0.0f, 1.0f });
		void set_depth_attachment(RGHandle handle, bool clear = false, float clear_depth = 1.0f, bool read_only = false);

		// Mark pass as having side effects (cannot be culled)
		void set_side_effect(bool value = true);

		// Mark the pass queue affinity (Graphics, AsyncCompute, Transfer)
		void set_queue(QueueType queue) {
			pass_node.queue_type = queue;
			pass_node.async_compute = (queue == QueueType::AsyncCompute);
		}

		// Mark the pass as an async compute pass (recorded on the dedicated
		// compute command buffer when a dedicated compute queue is available).
		void set_async_compute(bool value = true) {
			pass_node.async_compute = value;
			pass_node.queue_type = value ? QueueType::AsyncCompute : QueueType::Graphics;
		}

	private:
		class RenderGraph& render_graph;
		struct RGPassNode& pass_node;
	};


	class RenderGraph {
		friend class RGBuilder;
	public:
		RenderGraph(RHI* rhi) : rhi(rhi) {}
		~RenderGraph() { reset(); }

		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;
		RenderGraph(RenderGraph&&) = delete;
		RenderGraph& operator=(RenderGraph&&) = delete;

		void reset() {
			if (rhi) {
				auto* pool = rhi->get_resource_pool();
				if (pool) {
					for (auto& node : resources) {
						if (node.is_transient) {
							if (node.physical_texture.is_valid()) {
								pool->release_texture(node.physical_texture);
								node.physical_texture.reset();
							}
							if (node.physical_buffer.is_valid()) {
								pool->release_buffer(node.physical_buffer);
								node.physical_buffer.reset();
							}
						}
					}
				}
			}

			passes.clear();
			resources.clear();
			adjacency_list.clear();
			sorted_passes.clear();
			// Clear resource nodes but keep ID 0 reserved as invalid
			resources.emplace_back(); 
		}

		template <typename SetupFn, typename ExecFn>
		auto add_pass(const std::string& name, SetupFn setup, ExecFn execute) {
			auto& pass = passes.emplace_back();
			pass.name = name;
			pass.execute = [exec = std::move(execute)](RHI* rhi, CommandHandle cmd) {
				exec(rhi, cmd);
			};

			RGBuilder builder(*this, pass);
			return setup(builder);
		}

		RGHandle import_texture(const std::string& name, TextureHandle texture, ResourceState current_state = ResourceState::Undefined);
		RGHandle import_buffer(const std::string& name, bud::graphics::BufferHandle buffer, ResourceState current_state = ResourceState::Undefined);
		TextureHandle get_texture(RGHandle handle) const;
		bud::graphics::BufferHandle get_buffer(RGHandle handle) const;
		
		const TextureDesc& get_texture_desc(RGHandle handle) const {
			if (handle.id == 0 || handle.id >= resources.size()) {
				static TextureDesc empty{};
				return empty;
			}
			return resources[handle.id].desc;
		}

		const BufferDesc& get_buffer_desc(RGHandle handle) const {
			if (handle.id == 0 || handle.id >= resources.size()) {
				static BufferDesc empty{};
				return empty;
			}
			return resources[handle.id].buffer_desc;
		}

		void compile();
		void execute(CommandHandle cmd);
		
		void execute_parallel(CommandHandle cmd, bud::threading::TaskScheduler* task_scheduler);

		void export_graphviz(const std::string& filepath) const;
		size_t get_culled_pass_count() const { return culled_pass_count; }
		size_t get_active_pass_count() const { return sorted_passes.size(); }

	private:
		RHI* rhi;
		std::vector<RGPassNode> passes;
		std::vector<RGResourceNode> resources;
		
		// Compiled Data
		std::vector<std::vector<int>> adjacency_list; // DAG
		std::vector<int> sorted_passes; // Execution Order
		size_t culled_pass_count = 0;
	};

}
