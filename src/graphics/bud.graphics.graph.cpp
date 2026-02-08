#include "src/graphics/bud.graphics.graph.hpp"

#include <iostream>

namespace bud::graphics {

	// --- Implementation ---

	RGHandle RGBuilder::read(RGHandle handle, ResourceState state) {
		pass_node.reads.push_back({ handle, state });
		return handle;
	}

	RGHandle RGBuilder::write(RGHandle handle, ResourceState state) {
		pass_node.writes.push_back({ handle, state });
		return handle;
	}

	RGHandle RGBuilder::create(const std::string& name, const TextureDesc& desc) {
		if (render_graph.resources.empty())
			render_graph.resources.emplace_back();

		RGResourceNode node;
		node.name = name;
		node.desc = desc;
		node.is_transient = true;
		node.is_external = false;
		
		render_graph.resources.push_back(node);
		RGHandle handle = RGHandle{ static_cast<uint32_t>(render_graph.resources.size() - 1) };
		
		// Auto declare write if creating? Usually yes, or pass explicit writes.
		// For now we just register existence. Pass needs to call write(handle) to actually write.
		return handle;
	}

	RGHandle RenderGraph::import_texture(const std::string& name, Texture* texture, ResourceState current_state) {
		if (resources.empty())
			resources.emplace_back(); // Ensure index 0 is invalid

		RGResourceNode node;
		node.name = name;
		node.physical_texture = texture;
		node.is_external = (texture != nullptr);
		node.is_transient = !node.is_external;
		node.initial_state = current_state; // [FIX] Capture imported state
		// node.desc = ... get from texture if possible
		
		resources.push_back(node);
		return RGHandle{ static_cast<uint32_t>(resources.size() - 1) };
	}

	Texture* RenderGraph::get_texture(RGHandle handle) const {
		if (handle.id == 0 || handle.id >= resources.size())
			return nullptr;

		return resources[handle.id].physical_texture;
	}

	void RenderGraph::compile() {
		// 1. Build Adjacency List (Naive O(N^2) for prototype)
		// Better: Map<ResourceId, ProducerPassId>
		adjacency_list.assign(passes.size(), {});
		std::vector<int> in_degree(passes.size(), 0);

		// Map: ResourceHandle -> Last Writing Pass Index
		std::unordered_map<uint32_t, int> resource_writers;

		for (int i = 0; i < passes.size(); ++i) {
			auto& pass = passes[i];
			
			// Who writes what?
			for (auto& access : pass.writes) {
				resource_writers[access.handle.id] = i;

				// If this writes to backbuffer, it has side effects (root)
				if (resources[access.handle.id].is_external) {
					pass.has_side_effects = true;
				}
			}

			// Who reads what?
			for (auto& access : pass.reads) {
				if (resource_writers.contains(access.handle.id)) {
					int producer = resource_writers[access.handle.id];
					// Dependency: Producer -> Consumer (i)
					adjacency_list[producer].push_back(i);
					passes[i].dependencies.push_back(producer); // Add this
					in_degree[i]++;
				}
			}
		}

		// 2. Culling (Start from side effects and traverse up)
		// For prototype, skip complex culling, just execute all connected to root
		// Or naive: execute topological sort, all passes are executed
		// TODO: Implement real culling (Phase 1.5)
		// (Skipped for conciseness)

		// 3. Topological Sort (Kahn's Algorithm)
		std::vector<int> queue;
		for (int i = 0; i < passes.size(); ++i) {
			if (in_degree[i] == 0) queue.push_back(i);
		}

		sorted_passes.clear();
		while (!queue.empty()) {
			int u = queue.back();
			queue.pop_back();
			sorted_passes.push_back(u);

			for (int v : adjacency_list[u]) {
				if (--in_degree[v] == 0) {
					queue.push_back(v);
				}
			}
		}

		if (sorted_passes.size() != passes.size()) {
			std::cerr << "[RenderGraph] Cycle detected! Compiled order might be partial.\n";
		}

		// 3.5. Calculate Barriers (Phase 3)
		// Track current state of each resource as we traverse
		struct ResourceStateTracker {
			ResourceState current_state = ResourceState::Undefined;
		};
		std::vector<ResourceStateTracker> resource_states(resources.size());
		
		// Initialize external resources state (e.g. swapchain is Present/Undefined)
		for (size_t i = 1; i < resources.size(); ++i) {
			if (resources[i].is_external) {
				// Assume external starts as Undefined or Present. For backbuffer usually starts effectively undefined for us until we acquire it
				// But we imported it with 'current_state'.
				resource_states[i].current_state = resources[i].initial_state; // [FIX] Use stored initial state
			}
		}

		for (int pass_idx : sorted_passes) {
			auto& pass = passes[pass_idx];

			// Process Reads (Transition to ReadState)
			for (auto& access : pass.reads) {
				uint32_t rid = access.handle.id;
				ResourceState old_state = resource_states[rid].current_state;
				ResourceState new_state = access.state;

				// [FIX] Always transition if old_state is Undefined to ensure initial layout is set correctly
				if (old_state != new_state || old_state == ResourceState::Undefined) {
					// Add barrier
					pass.before_barriers.push_back({ access.handle, old_state, new_state });
					resource_states[rid].current_state = new_state;
				}
			}

			// Process Writes
			for (auto& access : pass.writes) {
				uint32_t rid = access.handle.id;
				ResourceState old_state = resource_states[rid].current_state;
				ResourceState new_state = access.state;

				// [FIX] Always transition if old_state is Undefined
				bool needs_barrier = (old_state != new_state) || (old_state == ResourceState::RenderTarget) || (old_state == ResourceState::Undefined); 

				if (needs_barrier) {
					pass.before_barriers.push_back({ access.handle, old_state, new_state });
					resource_states[rid].current_state = new_state;
				}
			}
		}

	
		// 4. Resource Allocation (Phase 2)
		auto* pool = rhi->get_resource_pool();
		if (pool) {
			for (auto& node : resources) {
				// Allocate only if transient and not already allocated (redundant check)
				if (node.is_transient && !node.physical_texture && node.name != "") {
					node.physical_texture = pool->acquire_texture(node.desc);
				}
			}
		}
	}

	void RenderGraph::execute(CommandHandle cmd) {
		for (int pass_idx : sorted_passes) {
			auto& pass = passes[pass_idx];

			rhi->cmd_begin_debug_label(cmd, pass.name, 1.0f, 0.7f, 0.0f);

			// Inject Barriers (Phase 3)
			for (auto& barrier : pass.before_barriers) {
				auto tex = get_texture(barrier.handle);
				auto& debug_name = resources[barrier.handle.id].name;
				if (tex) {
					rhi->set_debug_name(tex, ObjectType::Texture, debug_name);
					rhi->resource_barrier(cmd, tex, barrier.old_state, barrier.new_state);
				}
			}

			if (pass.execute) {
				pass.execute(rhi, cmd);
			}

			rhi->cmd_end_debug_label(cmd);
		}
		
		reset(); 
	}

	void RenderGraph::execute_parallel(CommandHandle cmd, bud::threading::TaskScheduler* task_scheduler) {
		if (!task_scheduler) {
			execute(cmd);
			return;
		}

		std::cerr << "[RenderGraph] execute_parallel is not implemented. Falling back to serial execution.\n";
		execute(cmd);
	}

}
