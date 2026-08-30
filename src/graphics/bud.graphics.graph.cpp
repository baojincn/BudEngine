#include "src/graphics/bud.graphics.graph.hpp"

#include <iostream>

namespace bud::graphics {
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

		return handle;
	}

	RGHandle RGBuilder::create(const std::string& name, const BufferDesc& desc) {
		if (render_graph.resources.empty())
			render_graph.resources.emplace_back();

		RGResourceNode node;
		node.name = name;
		node.buffer_desc = desc;
		node.is_buffer = true;
		node.is_transient = true;
		node.is_external = false;
		
		render_graph.resources.push_back(node);
		RGHandle handle = RGHandle{ static_cast<uint32_t>(render_graph.resources.size() - 1) };

		return handle;
	}

	void RGBuilder::set_side_effect(bool value) {
		pass_node.has_side_effects = value;
	}

	RGHandle RenderGraph::import_texture(const std::string& name, TextureHandle texture, ResourceState current_state) {
		if (resources.empty())
			resources.emplace_back(); // Ensure index 0 is invalid

		RGResourceNode node;
		node.name = name;
		node.physical_texture = texture;
		if (texture.is_valid() && rhi) {
			node.desc = rhi->get_texture_desc(texture);
		}
		node.is_external = texture.is_valid();
		node.is_transient = !node.is_external;
		node.initial_state = current_state;
		
		resources.push_back(node);
		return RGHandle{ static_cast<uint32_t>(resources.size() - 1) };
	}

	RGHandle RenderGraph::import_buffer(const std::string& name, bud::graphics::BufferHandle buffer, ResourceState current_state) {
		if (resources.empty())
			resources.emplace_back();

		RGResourceNode node;
		node.name = name;
		node.physical_buffer = buffer;
		node.is_buffer = true;
		if (buffer.is_valid() && rhi) {
			node.buffer_desc = rhi->get_buffer_desc(buffer);
		}
		node.is_external = buffer.is_valid();
		node.is_transient = !node.is_external;
		node.initial_state = current_state;

		resources.push_back(node);
		return RGHandle{ static_cast<uint32_t>(resources.size() - 1) };
	}

	TextureHandle RenderGraph::get_texture(RGHandle handle) const {
		if (handle.id >= resources.size()) {
			return TextureHandle{};
		}
		return resources[handle.id].physical_texture;
	}

	bud::graphics::BufferHandle RenderGraph::get_buffer(RGHandle handle) const {
		if (handle.id == 0 || handle.id >= resources.size()) {
			std::string err = std::format("RenderGraph::get_buffer invalid handle: id={} size={}", handle.id, resources.size());
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return bud::graphics::BufferHandle{};
#endif
		}

		return resources[handle.id].physical_buffer;
	}

	void RenderGraph::compile() {
		// 1. Build Adjacency List (Naive O(N^2) for prototype)
		// Better: Map<ResourceId, ProducerPassId>
		adjacency_list.assign(passes.size(), {});
		std::vector<int> in_degree(passes.size(), 0);

		auto add_dependency = [&](int producer, int consumer) {
			if (producer < 0 || consumer < 0 || producer == consumer)
				return;

			auto& deps = passes[consumer].dependencies;
			if (std::find(deps.begin(), deps.end(), producer) != deps.end())
				return;

			adjacency_list[producer].push_back(consumer);
			deps.push_back(producer);
			in_degree[consumer]++;
		};

		for (auto& pass : passes) {
			pass.dependencies.clear();
			pass.before_barriers.clear();
			pass.has_side_effects = false;
		}

		// Map: ResourceHandle -> Last Writing Pass Index
		std::unordered_map<uint32_t, int> resource_writers;

		for (int i = 0; i < passes.size(); ++i) {
			auto& pass = passes[i];
			
			// Who writes what?
			for (auto& access : pass.writes) {
				if (!access.handle.is_valid())
					continue;
				if (resource_writers.contains(access.handle.id)) {
					add_dependency(resource_writers[access.handle.id], i);
				}

				resource_writers[access.handle.id] = i;

				// If this writes to backbuffer, it has side effects (root)
				if (resources[access.handle.id].is_external) {
					pass.has_side_effects = true;
				}
			}

			// Who reads what?
			for (auto& access : pass.reads) {
				if (!access.handle.is_valid())
					continue;
				if (resource_writers.contains(access.handle.id)) {
					int producer = resource_writers[access.handle.id];
					add_dependency(producer, i);
				}
			}
		}

		// 2. Culling (Start from side effects and traverse up)
		// For prototype, skip complex culling, just execute all connected to root
		// Or naive: execute topological sort, all passes are executed
		// TODO: Implement real culling (Phase 1.5)
		// (Skipped for conciseness)

		// 3. Topological Sort (Kahn's Algorithm)
		std::deque<int> queue;
		for (int i = 0; i < passes.size(); ++i) {
			if (in_degree[i] == 0) queue.push_back(i);
		}

		sorted_passes.clear();
		while (!queue.empty()) {
			int u = queue.front();
			queue.pop_front();
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
		// Track current state and queue of each resource as we traverse
		struct ResourceStateTracker {
			ResourceState current_state = ResourceState::Undefined;
			QueueType last_queue = QueueType::Graphics;
			int last_pass_idx = -1;
		};
		std::vector<ResourceStateTracker> resource_states(resources.size());

		uint32_t gfx_family = rhi->get_graphics_queue_family();
		uint32_t compute_family = rhi->get_compute_queue_family();
		uint32_t transfer_family = rhi->get_transfer_queue_family();

		auto get_family = [&](QueueType q) -> uint32_t {
			if (q == QueueType::AsyncCompute) return compute_family;
			if (q == QueueType::Transfer) return transfer_family;
			return gfx_family;
		};
		
		// Initialize external resources state (e.g. swapchain is Present/Undefined)
		for (size_t i = 1; i < resources.size(); ++i) {
			if (resources[i].is_external) {
				resource_states[i].current_state = resources[i].initial_state;
			}
		}

		for (int pass_idx : sorted_passes) {
			auto& pass = passes[pass_idx];
			uint32_t current_family = get_family(pass.queue_type);

			auto handle_access = [&](const RGPassNode::Access& access, bool is_write) {
				if (!access.handle.is_valid())
					return;
				uint32_t rid = access.handle.id;
				ResourceState old_state = resource_states[rid].current_state;
				ResourceState new_state = access.state;
				QueueType old_queue = resource_states[rid].last_queue;
				uint32_t old_family = get_family(old_queue);

				bool needs_barrier = (old_state != new_state) || (old_state == ResourceState::RenderTarget) || (old_state == ResourceState::Undefined);

				if (needs_barrier) {
					pass.before_barriers.push_back({
						access.handle, old_state, new_state, 0xFFFFFFFF, 0xFFFFFFFF, false, false
					});
					resource_states[rid].current_state = new_state;
					resource_states[rid].last_queue = pass.queue_type;
					resource_states[rid].last_pass_idx = pass_idx;
				}
			};

			for (auto& access : pass.reads) {
				handle_access(access, false);
			}
			for (auto& access : pass.writes) {
				handle_access(access, true);
			}
		}

	
		// 4. Resource Allocation (Phase 2)
		auto* pool = rhi->get_resource_pool();
		if (pool) {
			for (auto& node : resources) {
				// Allocate only if transient and not already allocated
				if (node.is_transient && !node.name.empty()) {
					if (!node.is_buffer && !node.physical_texture.is_valid()) {
						auto handle = pool->acquire_texture(node.desc);
						if (!handle.is_valid()) {
							std::string err = std::format("RenderGraph::compile: resource pool failed to allocate texture '{}'", node.name);
							bud::eprint("{}", err);
#if defined(_DEBUG)
							throw std::runtime_error(err);
#else
							node.physical_texture.reset();
#endif
						} else {
							node.physical_texture = handle;
						}
					} else if (node.is_buffer && !node.physical_buffer.is_valid()) {
						auto handle = pool->acquire_buffer(node.buffer_desc);
						if (!handle.is_valid()) {
							std::string err = std::format("RenderGraph::compile: resource pool failed to allocate buffer '{}'", node.name);
							bud::eprint("{}", err);
#if defined(_DEBUG)
							throw std::runtime_error(err);
#else
							node.physical_buffer.reset();
#endif
						} else {
							node.physical_buffer = handle;
						}
					}
				}
			}
		} else {
			bud::eprint("RenderGraph::compile: no resource pool available; transient resources will not be allocated");
		}
	}

	void RenderGraph::execute(CommandHandle cmd) {
		const bool use_async = rhi->has_dedicated_compute_queue();
		CommandHandle async_cmd = nullptr;
		bool async_active = false;
		bool async_used = false;

		for (size_t p = 0; p < sorted_passes.size(); ++p) {
			int pass_idx = sorted_passes[p];
			auto& pass = passes[pass_idx];
			const bool is_async = use_async && (pass.queue_type == QueueType::AsyncCompute || pass.async_compute);
			if (is_async) async_used = true;

			if (is_async) {
				if (!async_active) {
					async_cmd = rhi->begin_async_compute();
					async_active = (async_cmd != nullptr);
				}
				if (!async_active) {
					// Fallback: no dedicated compute queue; run on the graphics segment.
					async_cmd = rhi->get_current_graphics_command_buffer();
				}
			}
			CommandHandle target = is_async ? async_cmd : rhi->get_current_graphics_command_buffer();

			rhi->cmd_begin_debug_label(target, pass.name, 1.0f, 0.7f, 0.0f);

			// Inject Before Barriers (State transitions & Acquire operations)
			for (auto& barrier : pass.before_barriers) {
				if (!barrier.handle.is_valid())
					continue;
				auto tex = get_texture(barrier.handle);
				auto buf = get_buffer(barrier.handle);
				if (barrier.is_acquire) {
					if (tex.is_valid()) {
						rhi->resource_barrier_acquire(target, tex, barrier.old_state, barrier.new_state, barrier.src_queue_family, barrier.dst_queue_family);
					} else if (buf.is_valid()) {
						rhi->resource_barrier_acquire(target, buf, barrier.old_state, barrier.new_state, barrier.src_queue_family, barrier.dst_queue_family);
					}
				} else {
					if (tex.is_valid()) {
						rhi->resource_barrier(target, tex, barrier.old_state, barrier.new_state);
					} else if (buf.is_valid()) {
						rhi->resource_barrier(target, buf, barrier.old_state, barrier.new_state);
					}
				}
			}

			if (pass.execute) {
				pass.execute(rhi, target);
			}

			// Inject After Barriers (Release operations)
			for (auto& barrier : pass.after_barriers) {
				if (!barrier.handle.is_valid())
					continue;
				auto tex = get_texture(barrier.handle);
				auto buf = get_buffer(barrier.handle);
				if (barrier.is_release) {
					if (tex.is_valid()) {
						rhi->resource_barrier_release(target, tex, barrier.old_state, barrier.new_state, barrier.src_queue_family, barrier.dst_queue_family);
					} else if (buf.is_valid()) {
						rhi->resource_barrier_release(target, buf, barrier.old_state, barrier.new_state, barrier.src_queue_family, barrier.dst_queue_family);
					}
				}
			}

			rhi->cmd_end_debug_label(target);

			// If the next pass is not async compute (or this is the last pass), end compute command recording and submit
			bool next_is_async = false;
			if (p + 1 < sorted_passes.size()) {
				int next_pass_idx = sorted_passes[p + 1];
				next_is_async = use_async && (passes[next_pass_idx].queue_type == QueueType::AsyncCompute || passes[next_pass_idx].async_compute);
			}
			if (is_async && use_async && async_active && !next_is_async) {
				rhi->end_async_compute();
				async_active = false;
			}
		}
		// reset() is called externally by the renderer after execute() returns
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
