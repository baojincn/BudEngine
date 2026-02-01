#pragma once

#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <stack>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.pool.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"

// Forward Decl
namespace bud::threading { class TaskScheduler; }

namespace bud::graphics {

	// 1. Defitions
	struct RGHandle {
		uint32_t id = 0;
		bool is_valid() const { return id != 0; }
		auto operator<=>(const RGHandle&) const = default;
	};

	struct RGResourceNode {
		std::string name;
		Texture* physical_texture = nullptr; // nullptr if transient
		TextureDesc desc;
		bool is_transient = true;
		bool is_external = false; // e.g. imported swapchain image

		// Versioning for future SSA
		uint32_t version = 0;
		RGHandle parent_handle = { 0 }; // if this is an alias or new version
		ResourceState initial_state = ResourceState::Undefined; // [FIX] Store Initial State
	};

	struct RGPassNode {
		std::string name;
		std::function<void(RHI*, CommandHandle)> execute;

		// Graph Connectivity
		struct Access { RGHandle handle; ResourceState state; };
		std::vector<Access> reads;
		std::vector<Access> writes;
		std::vector<int> dependencies; // Indices of passes this pass waits for

		// Culling info
		uint32_t ref_count = 0;
		bool has_side_effects = false; // e.g. Present, Compute Write to buffer

		// Barrier info calculated during compile()
		struct BarrierInfo { 
			RGHandle handle; 
			ResourceState old_state; 
			ResourceState new_state; 
		};
		std::vector<BarrierInfo> before_barriers;
	};

	// 2. Builder
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

	private:
		class RenderGraph& render_graph;
		struct RGPassNode& pass_node;
	};

	// 3. RenderGraph
	class RenderGraph {
		friend class RGBuilder;
	public:
		RenderGraph(RHI* rhi) : rhi(rhi) {}

		void reset() {
			// Phase 2: Release transient resources back to pool
			if (rhi) {
				auto* pool = rhi->get_resource_pool();
				if (pool) {
					for (auto& node : resources) {
						if (node.is_transient && node.physical_texture) {
							pool->release_texture(node.physical_texture);
							node.physical_texture = nullptr;
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

		RGHandle import_texture(const std::string& name, Texture* texture, ResourceState current_state);
		Texture* get_texture(RGHandle handle) const;

		// --- Core Features ---
		void compile();
		void execute(CommandHandle cmd);
		
		// Task-based execution (Phase 4)
		void execute_parallel(bud::threading::TaskScheduler* task_scheduler);

	private:
		RHI* rhi;
		std::vector<RGPassNode> passes;
		std::vector<RGResourceNode> resources;
		
		// Compiled Data
		std::vector<std::vector<int>> adjacency_list; // DAG
		std::vector<int> sorted_passes; // Execution Order
	};

}
