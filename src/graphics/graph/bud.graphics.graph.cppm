module;
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <algorithm> // for std::remove_if
#include <iostream>  // for logging
#include <stack>     // for topology sort

export module bud.graphics.graph;

import bud.graphics;
import bud.graphics.defs;
import bud.graphics.types;
import bud.graphics.pool; // ResourcePool
import bud.threading; // TaskScheduler

export namespace bud::graphics {

	// 1. Defitions
	export struct RGHandle {
		uint32_t id = 0;
		bool is_valid() const { return id != 0; }
		auto operator<=>(const RGHandle&) const = default;
	};

	export struct RGResourceNode {
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

	export struct RGPassNode {
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
		struct BarrierInfo { RGHandle handle; ResourceState old_state; ResourceState new_state; };
		std::vector<BarrierInfo> before_barriers;
	};

	// 2. Builder
	export class RGBuilder {
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
	export class RenderGraph {
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

	void RenderGraph::execute_parallel(bud::threading::TaskScheduler* task_scheduler) {
		if (!task_scheduler) return;

		// 1. Create counters for all passes
		// We use unique_ptr because Counter is not movable
		std::vector<std::unique_ptr<bud::threading::Counter>> counters;
		counters.reserve(passes.size());
		for (size_t i = 0; i < passes.size(); ++i) {
			counters.push_back(std::make_unique<bud::threading::Counter>(0));
		}

		// 2. Spawn tasks for all passes
		// Note: We spawn them all, they will cooperatively wait for dependencies
		// Iterate in topological order just to be nice to the scheduler, but any order works valid-wise
		for (int pass_idx : sorted_passes) {
			auto& pass = passes[pass_idx];
			auto* my_counter = counters[pass_idx].get();

			task_scheduler->spawn(pass.name.c_str(), [this, pass_idx, &counters, &pass, task_scheduler](void) {
				// A. Wait for dependencies
				for (int dep_idx : pass.dependencies) {
					task_scheduler->wait_for_counter(*counters[dep_idx]);
				}

				// B. Barriers (Automatic)
				// Note: In parallel recording, these commands need a valid command buffer.
				// Since we don't have secondary cmd buffers fully hooked up, we assume
				// 'execute' in pass does the recording.
				// BUT barriers must happen *before* the pass logic.
				// We can't record them into the same main command buffer easily if parallel.
				// For Prototype: We print them. In Real Engine: Record to thread-local CMD.
				
				for (auto& barrier : pass.before_barriers) {
				   // Just log for prototype safety. Real engine would record to secondary cmd.
				   // std::cout << "Barrier: " << barrier.handle.id << " " << (int)barrier.old_state << "->" << (int)barrier.new_state << "\n";
				}

				// C. Execute
				// ...
				
				// Placeholder:
				// CommandHandle thread_cmd = rhi->allocate_secondary_command_buffer();
				// pass.execute(rhi, thread_cmd);
				// rhi->submit_secondary(thread_cmd);
				
				// For SAFETY in this prototype step, we just log execution order:
				// std::cout << "Executing Pass Parallel: " << pass.name << "\n";
				
			}, my_counter);
		}

		// 3. Wait for frame completion (Leaf Passes)
		// Leaves are passes that no one depends on (dependencies list is usually for *input*, here we need Reverse.
		// Wait, adjacency_list[u] contains 'v' where u -> v.
		// So if adjacency_list[u] is empty, u is a leaf (no one reads from u).
		// We should wait for these.
		
		for (size_t i = 0; i < passes.size(); ++i) {
			if (adjacency_list[i].empty()) {
				task_scheduler->wait_for_counter(*counters[i]);
			}
		}

		reset();
	}
}
