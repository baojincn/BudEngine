#include "src/graphics/bud.graphics.graph.hpp"
#include "src/io/bud.io.hpp"

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

	void RGBuilder::set_color_attachment(uint32_t slot, RGHandle handle, bool clear, const bud::math::vec4& clear_color) {
		if (!handle.is_valid())
			return;
		write(handle, ResourceState::RenderTarget);
		if (slot >= pass_node.color_attachments.size()) {
			pass_node.color_attachments.resize(slot + 1);
		}
		pass_node.color_attachments[slot] = {
			.handle = handle,
			.is_depth = false,
			.clear = clear,
			.read_only = false,
			.clear_color = clear_color
		};
	}

	void RGBuilder::set_depth_attachment(RGHandle handle, bool clear, float clear_depth, bool read_only) {
		if (!handle.is_valid())
			return;
		if (read_only) {
			read(handle, ResourceState::DepthRead);
		} else {
			write(handle, ResourceState::DepthWrite);
		}
		pass_node.has_depth_attachment = true;
		pass_node.depth_attachment = {
			.handle = handle,
			.is_depth = true,
			.clear = clear,
			.read_only = read_only,
			.clear_depth = clear_depth
		};
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
			auto* phys_tex = rhi->get_texture(texture);
			if (phys_tex) {
				if (current_state == ResourceState::Undefined) {
					if (name != "Backbuffer" && phys_tex->current_state != ResourceState::Undefined) {
						current_state = phys_tex->current_state;
					}
				} else if (phys_tex->current_state != ResourceState::Undefined) {
					// Physical tracked state is the ground truth
					current_state = phys_tex->current_state;
				}
			}
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
			auto* phys_buf = rhi->get_buffer(buffer);
			if (phys_buf && phys_buf->current_state != ResourceState::Undefined) {
				current_state = phys_buf->current_state;
			}
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
		for (auto& pass : passes) {
			pass.dependencies.clear();
			pass.before_barriers.clear();
			pass.after_barriers.clear();
			pass.is_culled = false;
			// NOTE: Do not reset pass.has_side_effects; it may have been set during pass setup via builder.set_side_effect()
		}

		// 1. Identify Resource Producers and Mark External Roots
		std::unordered_map<uint32_t, std::vector<int>> resource_producers;
		for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
			auto& pass = passes[i];

			for (auto& access : pass.writes) {
				if (!access.handle.is_valid())
					continue;
				resource_producers[access.handle.id].push_back(i);

				// If this writes to an external resource (swapchain, persistent buffer/texture), it has side effects (root)
				if (access.handle.id < resources.size() && resources[access.handle.id].is_external) {
					pass.has_side_effects = true;
				}
			}
		}

		// 2. Dead Pass Culling (Phase 1.5 - Reverse Reachability from Side-Effect Roots)
		std::vector<bool> pass_active(passes.size(), false);
		std::vector<bool> resource_active(resources.size(), false);
		std::deque<uint32_t> resource_worklist;

		// 2.1 Mark root passes as active and enqueue their read resources
		for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
			if (passes[i].has_side_effects) {
				pass_active[i] = true;
				for (auto& access : passes[i].reads) {
					if (access.handle.is_valid()) {
						resource_worklist.push_back(access.handle.id);
					}
				}
				for (auto& access : passes[i].writes) {
					if (access.handle.is_valid() && access.handle.id < resources.size()) {
						resource_active[access.handle.id] = true;
					}
				}
			}
		}

		// 2.2 Propagate backwards to find all upstream passes & resources required by roots
		while (!resource_worklist.empty()) {
			uint32_t res_id = resource_worklist.front();
			resource_worklist.pop_front();

			if (res_id >= resources.size() || resource_active[res_id])
				continue;
			resource_active[res_id] = true;

			auto it = resource_producers.find(res_id);
			if (it != resource_producers.end()) {
				for (int producer_idx : it->second) {
					if (!pass_active[producer_idx]) {
						pass_active[producer_idx] = true;
						for (auto& access : passes[producer_idx].reads) {
							if (access.handle.is_valid()) {
								resource_worklist.push_back(access.handle.id);
							}
						}
					}
				}
			}
		}

		culled_pass_count = 0;
		for (size_t i = 0; i < passes.size(); ++i) {
			passes[i].is_culled = !pass_active[i];
			if (passes[i].is_culled) {
				culled_pass_count++;
			}
		}
		for (size_t i = 0; i < resources.size(); ++i) {
			resources[i].is_active = resource_active[i];
		}

		// 3. Topological Sort on Active Subgraph (Kahn's Algorithm)
		adjacency_list.assign(passes.size(), {});
		std::vector<int> in_degree(passes.size(), 0);

		auto add_dependency = [&](int producer, int consumer) {
			if (producer < 0 || consumer < 0 || producer == consumer || !pass_active[producer] || !pass_active[consumer])
				return;

			auto& deps = passes[consumer].dependencies;
			if (std::find(deps.begin(), deps.end(), producer) != deps.end())
				return;

			adjacency_list[producer].push_back(consumer);
			deps.push_back(producer);
			in_degree[consumer]++;
		};

		// Track active writers and active readers per resource id among active passes
		struct ActiveAccessRecord {
			int pass_idx;
			SubresourceRange range;
		};
		std::unordered_map<uint32_t, std::vector<ActiveAccessRecord>> active_writers;
		std::unordered_map<uint32_t, std::vector<ActiveAccessRecord>> active_readers;

		for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
			if (!pass_active[i])
				continue;
			auto& pass = passes[i];

			// Process writes (WAW and WAR dependencies)
			for (auto& access : pass.writes) {
				if (!access.handle.is_valid())
					continue;
				uint32_t rid = access.handle.id;
				uint32_t total_mips = resources[rid].is_buffer ? 1 : (resources[rid].desc.mips > 0 ? resources[rid].desc.mips : 1);
				uint32_t total_layers = resources[rid].is_buffer ? 1 : (resources[rid].desc.array_layers > 0 ? resources[rid].desc.array_layers : 1);

				// WAW: depend on previous writers of overlapping subresources
				if (auto it = active_writers.find(rid); it != active_writers.end()) {
					for (auto& rec : it->second) {
						if (access.handle.subresource.overlaps(rec.range, total_mips, total_layers))
							add_dependency(rec.pass_idx, i);
					}
				}

				// WAR: depend on previous readers of overlapping subresources
				if (auto it = active_readers.find(rid); it != active_readers.end()) {
					for (auto& rec : it->second) {
						if (access.handle.subresource.overlaps(rec.range, total_mips, total_layers))
							add_dependency(rec.pass_idx, i);
					}
				}

				if (access.handle.subresource.is_all()) {
					active_writers[rid].clear();
					active_readers[rid].clear();
				}
				active_writers[rid].push_back({ i, access.handle.subresource });
			}

			// Process reads (RAW dependencies)
			for (auto& access : pass.reads) {
				if (!access.handle.is_valid())
					continue;
				uint32_t rid = access.handle.id;
				uint32_t total_mips = resources[rid].is_buffer ? 1 : (resources[rid].desc.mips > 0 ? resources[rid].desc.mips : 1);
				uint32_t total_layers = resources[rid].is_buffer ? 1 : (resources[rid].desc.array_layers > 0 ? resources[rid].desc.array_layers : 1);

				if (auto it = active_writers.find(rid); it != active_writers.end()) {
					for (auto& rec : it->second) {
						if (access.handle.subresource.overlaps(rec.range, total_mips, total_layers))
							add_dependency(rec.pass_idx, i);
					}
				}
				active_readers[rid].push_back({ i, access.handle.subresource });
			}
		}

		std::deque<int> queue;
		int total_active_passes = 0;
		for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
			if (pass_active[i]) {
				total_active_passes++;
				if (in_degree[i] == 0) {
					queue.push_back(i);
				}
			}
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

		if (static_cast<int>(sorted_passes.size()) != total_active_passes) {
			std::cerr << "[RenderGraph] Cycle detected among active passes! Compiled order might be partial.\n";
		}

		// 3.5. Calculate Barriers (Phase 3)
		// Track current state and queue of each subresource as we traverse
		struct SubresourceStateRecord {
			ResourceState state = ResourceState::Undefined;
			QueueType queue = QueueType::Graphics;
			int last_pass_idx = -1;
			bool last_was_write = false;
		};

		struct ResourceStateTracker {
			SubresourceStateRecord whole;
			std::vector<SubresourceStateRecord> subresources; // empty if whole applies to all
			uint32_t mips = 1;
			uint32_t layers = 1;

			void init(uint32_t m, uint32_t l, ResourceState initial_state) {
				mips = m > 0 ? m : 1;
				layers = l > 0 ? l : 1;
				whole.state = initial_state;
				whole.queue = QueueType::Graphics;
				whole.last_pass_idx = -1;
				whole.last_was_write = false;
				subresources.clear();
			}
		};
		std::vector<ResourceStateTracker> resource_states(resources.size());

		uint32_t gfx_family = rhi->get_graphics_queue_family();
		uint32_t compute_family = rhi->get_compute_queue_family();
		uint32_t transfer_family = rhi->get_transfer_queue_family();

		auto get_family = [&](QueueType q) -> uint32_t {
			if (q == QueueType::AsyncCompute)
				return compute_family;
			if (q == QueueType::Transfer)
				return transfer_family;
			return gfx_family;
		};
		
		// Initialize external resources state (e.g. swapchain is Present/Undefined)
		for (size_t i = 1; i < resources.size(); ++i) {
			uint32_t m = resources[i].is_buffer ? 1 : (resources[i].desc.mips > 0 ? resources[i].desc.mips : 1);
			uint32_t l = resources[i].is_buffer ? 1 : (resources[i].desc.array_layers > 0 ? resources[i].desc.array_layers : 1);
			resource_states[i].init(m, l, resources[i].initial_state);

			if (resources[i].is_external && resources[i].physical_texture.is_valid() && rhi) {
				auto* phys_tex = rhi->get_texture(resources[i].physical_texture);
				if (phys_tex && !phys_tex->subresource_states.empty()) {
					resource_states[i].subresources.resize(m * l);
					for (uint32_t s = 0; s < m * l && s < phys_tex->subresource_states.size(); ++s) {
						resource_states[i].subresources[s].state = phys_tex->subresource_states[s];
					}
				}
			}
		}

		for (int pass_idx : sorted_passes) {
			auto& pass = passes[pass_idx];
			uint32_t current_family = get_family(pass.queue_type);

			auto handle_access = [&](const RGPassNode::Access& access, bool is_write) {
				if (!access.handle.is_valid())
					return;
				uint32_t rid = access.handle.id;
				auto& tracker = resource_states[rid];
				const auto& range = access.handle.subresource;
				uint32_t total_mips = tracker.mips;
				uint32_t total_layers = tracker.layers;

				uint32_t start_m = range.base_mip;
				uint32_t count_m = (range.mip_count == ALL_MIPS || range.mip_count == 0) ? (total_mips > start_m ? total_mips - start_m : 1) : range.mip_count;
				uint32_t start_l = range.base_layer;
				uint32_t count_l = (range.layer_count == ALL_LAYERS || range.layer_count == 0) ? (total_layers > start_l ? total_layers - start_l : 1) : range.layer_count;

				// Fast path: Whole resource access with uniform state
				if (range.is_all() && tracker.subresources.empty()) {
					ResourceState old_state = tracker.whole.state;
					ResourceState new_state = access.state;
					QueueType old_queue = tracker.whole.queue;
					uint32_t old_family = get_family(old_queue);

					bool is_diff_pass = (tracker.whole.last_pass_idx != pass_idx);
					bool is_uav_hazard = (old_state == ResourceState::UnorderedAccess &&
					                      new_state == ResourceState::UnorderedAccess &&
					                      is_diff_pass &&
					                      (tracker.whole.last_was_write || is_write));

					bool needs_barrier = (old_state != new_state) ||
					                     (old_state == ResourceState::RenderTarget) ||
					                     (old_state == ResourceState::Undefined) ||
					                     is_uav_hazard;

					bool is_queue_transfer = (old_family != current_family && old_state != ResourceState::Undefined && tracker.whole.last_pass_idx >= 0);

					if (is_queue_transfer) {
						passes[tracker.whole.last_pass_idx].after_barriers.push_back({
							access.handle, old_state, new_state, old_family, current_family, false, true
						});
						pass.before_barriers.push_back({
							access.handle, old_state, new_state, old_family, current_family, true, false
						});
						tracker.whole.state = new_state;
					} else if (needs_barrier) {
						pass.before_barriers.push_back({
							access.handle, old_state, new_state, 0xFFFFFFFF, 0xFFFFFFFF, false, false
						});
						tracker.whole.state = new_state;
					}

					tracker.whole.queue = pass.queue_type;
					if (is_write) {
						tracker.whole.last_was_write = true;
					} else if (is_diff_pass) {
						tracker.whole.last_was_write = false;
					}
					tracker.whole.last_pass_idx = pass_idx;
					return;
				}

				// Subresource-specific or partitioned access
				if (tracker.subresources.empty()) {
					tracker.subresources.assign(total_mips * total_layers, tracker.whole);
				}

				for (uint32_t l = start_l; l < start_l + count_l && l < total_layers; ++l) {
					uint32_t m_curr = start_m;
					while (m_curr < start_m + count_m && m_curr < total_mips) {
						uint32_t idx = l * total_mips + m_curr;
						auto& rec = tracker.subresources[idx];
						ResourceState old_state = rec.state;
						ResourceState new_state = access.state;
						QueueType old_queue = rec.queue;
						uint32_t old_family = get_family(old_queue);

						bool is_diff_pass = (rec.last_pass_idx != pass_idx);
						bool is_uav_hazard = (old_state == ResourceState::UnorderedAccess &&
						                      new_state == ResourceState::UnorderedAccess &&
						                      is_diff_pass &&
						                      (rec.last_was_write || is_write));

						bool needs_barrier = (old_state != new_state) ||
						                     (old_state == ResourceState::RenderTarget) ||
						                     (old_state == ResourceState::Undefined) ||
						                     is_uav_hazard;

						bool is_queue_transfer = (old_family != current_family && old_state != ResourceState::Undefined && rec.last_pass_idx >= 0);

						// Determine contiguous run of identical transitions
						uint32_t run_len = 1;
						while (m_curr + run_len < start_m + count_m && m_curr + run_len < total_mips) {
							uint32_t next_idx = l * total_mips + (m_curr + run_len);
							auto& next_rec = tracker.subresources[next_idx];
							uint32_t next_old_family = get_family(next_rec.queue);
							bool next_diff_pass = (next_rec.last_pass_idx != pass_idx);
							bool next_uav_hazard = (next_rec.state == ResourceState::UnorderedAccess &&
							                       new_state == ResourceState::UnorderedAccess &&
							                       next_diff_pass &&
							                       (next_rec.last_was_write || is_write));
							bool next_needs_barrier = (next_rec.state != new_state) ||
							                          (next_rec.state == ResourceState::RenderTarget) ||
							                          (next_rec.state == ResourceState::Undefined) ||
							                          next_uav_hazard;
							bool next_queue_transfer = (next_old_family != current_family && next_rec.state != ResourceState::Undefined && next_rec.last_pass_idx >= 0);

							if (next_rec.state == old_state &&
								next_rec.queue == old_queue &&
								next_needs_barrier == needs_barrier &&
								next_queue_transfer == is_queue_transfer &&
								(!is_queue_transfer || next_rec.last_pass_idx == rec.last_pass_idx)) {
								run_len++;
							} else {
								break;
							}
						}

						RGHandle slice_handle = access.handle.subresource_range(m_curr, run_len, l, 1);
						if (is_queue_transfer) {
							passes[rec.last_pass_idx].after_barriers.push_back({
								slice_handle, old_state, new_state, old_family, current_family, false, true
							});
							pass.before_barriers.push_back({
								slice_handle, old_state, new_state, old_family, current_family, true, false
							});
						} else if (needs_barrier) {
							pass.before_barriers.push_back({
								slice_handle, old_state, new_state, 0xFFFFFFFF, 0xFFFFFFFF, false, false
							});
						}

						for (uint32_t k = 0; k < run_len; ++k) {
							uint32_t update_idx = l * total_mips + (m_curr + k);
							auto& r = tracker.subresources[update_idx];
							if (needs_barrier || is_queue_transfer) {
								r.state = new_state;
							}
							r.queue = pass.queue_type;
							if (is_write) {
								r.last_was_write = true;
							} else if (is_diff_pass) {
								r.last_was_write = false;
							}
							r.last_pass_idx = pass_idx;
						}

						m_curr += run_len;
					}
				}

				// Check if all subresources now share the same state and queue
				bool all_same = true;
				for (size_t s = 1; s < tracker.subresources.size(); ++s) {
					if (tracker.subresources[s].state != tracker.subresources[0].state ||
						tracker.subresources[s].queue != tracker.subresources[0].queue) {
						all_same = false;
						break;
					}
				}
				if (all_same) {
					tracker.whole = tracker.subresources[0];
					tracker.subresources.clear();
				}
			};

			for (auto& access : pass.reads) {
				handle_access(access, false);
			}
			for (auto& access : pass.writes) {
				handle_access(access, true);
			}
		}

	
		// 3.8 Calculate Resource Lifetime Intervals [first_pass, last_pass]
		for (size_t i = 0; i < resources.size(); ++i) {
			resources[i].first_pass = -1;
			resources[i].last_pass = -1;
			resources[i].is_aliased = false;
			resources[i].heap_offset = 0;
			resources[i].allocated_size = 0;
			resources[i].heap_chunk_index = 0;
			resources[i].virtual_alloc_handle = nullptr;
		}

		for (int p = 0; p < static_cast<int>(sorted_passes.size()); ++p) {
			int pass_idx = sorted_passes[p];
			const auto& pass = passes[pass_idx];

			auto track_res = [&](const RGPassNode::Access& access) {
				if (!access.handle.is_valid())
					return;
				uint32_t rid = access.handle.id;
				if (rid >= resources.size())
					return;
				auto& res = resources[rid];
				if (res.first_pass == -1) {
					res.first_pass = p;
					res.last_pass = p;
				} else {
					res.last_pass = std::max(res.last_pass, p);
				}
			};

			for (const auto& access : pass.reads)
				track_res(access);
			for (const auto& access : pass.writes)
				track_res(access);
		}

		// 4. Resource Allocation
		auto* pool = rhi->get_resource_pool();
		auto* transient_heap = rhi->get_transient_heap();
		const bool use_aliasing = rhi->get_render_config().enable_vram_aliasing && (transient_heap != nullptr);

		total_transient_unaliased_bytes = 0;
		peak_aliased_bytes = 0;

		if (use_aliasing && !sorted_passes.empty()) {
			std::vector<std::vector<uint32_t>> res_starting_at(sorted_passes.size());
			std::vector<std::vector<uint32_t>> res_ending_at(sorted_passes.size());

			for (size_t rid = 0; rid < resources.size(); ++rid) {
				auto& node = resources[rid];
				if (node.is_transient && node.is_active && !node.name.empty() && node.first_pass >= 0 && node.last_pass >= 0) {
					if (!node.is_buffer && !node.physical_texture.is_valid()) {
						res_starting_at[node.first_pass].push_back(static_cast<uint32_t>(rid));
						res_ending_at[node.last_pass].push_back(static_cast<uint32_t>(rid));
					}
				}
			}

			// Chronological pass simulation
			for (int p = 0; p < static_cast<int>(sorted_passes.size()); ++p) {
				// 1. Allocate placed textures whose lifetime starts at pass p
				for (uint32_t rid : res_starting_at[p]) {
					auto& node = resources[rid];
					TransientAllocation alloc{};
					auto handle = transient_heap->allocate_texture(node.desc, alloc);
					if (handle.is_valid() && alloc.is_valid) {
						node.physical_texture = handle;
						node.is_aliased = true;
						node.heap_offset = alloc.offset;
						node.allocated_size = alloc.size;
						node.heap_chunk_index = alloc.chunk_index;
						node.virtual_alloc_handle = alloc.virtual_alloc_handle;
						total_transient_unaliased_bytes += alloc.size;
					} else if (pool) {
						node.physical_texture = pool->acquire_texture(node.desc);
						node.is_aliased = false;
					}
				}

				// 2. Free virtual allocations for resources whose active lifetime ends at pass p
				for (uint32_t rid : res_ending_at[p]) {
					auto& node = resources[rid];
					if (node.is_aliased && node.virtual_alloc_handle) {
						TransientAllocation alloc{};
						alloc.chunk_index = node.heap_chunk_index;
						alloc.offset = node.heap_offset;
						alloc.size = node.allocated_size;
						alloc.virtual_alloc_handle = node.virtual_alloc_handle;
						alloc.is_valid = true;
						transient_heap->virtual_free(alloc);
					}
				}
			}

			peak_aliased_bytes = transient_heap->get_stats().peak_allocated_bytes;

			if (total_transient_unaliased_bytes > 0 && (total_transient_unaliased_bytes != last_logged_unaliased || peak_aliased_bytes != last_logged_peak)) {
				last_logged_unaliased = total_transient_unaliased_bytes;
				last_logged_peak = peak_aliased_bytes;
				bud::print("[RenderGraph] VRAM Aliasing Active: unaliased={:.1f}MB, peak_aliased={:.1f}MB, savings={:.1f}%",
					static_cast<double>(total_transient_unaliased_bytes) / (1024.0 * 1024.0),
					static_cast<double>(peak_aliased_bytes) / (1024.0 * 1024.0),
					100.0 * (1.0 - static_cast<double>(peak_aliased_bytes) / static_cast<double>(total_transient_unaliased_bytes)));
			}
		}

		// Fallback & buffers: allocate from pool if not already allocated
		if (pool) {
			for (auto& node : resources) {
				if (node.is_transient && node.is_active && !node.name.empty()) {
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
							node.is_aliased = false;
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
						rhi->resource_barrier_acquire(target, tex, barrier.old_state, barrier.new_state, barrier.src_queue_family, barrier.dst_queue_family, barrier.handle.subresource);
					} else if (buf.is_valid()) {
						rhi->resource_barrier_acquire(target, buf, barrier.old_state, barrier.new_state, barrier.src_queue_family, barrier.dst_queue_family);
					}
				} else {
					if (tex.is_valid()) {
						rhi->resource_barrier(target, tex, barrier.old_state, barrier.new_state, barrier.handle.subresource);
					} else if (buf.is_valid()) {
						rhi->resource_barrier(target, buf, barrier.old_state, barrier.new_state);
					}
				}
			}

			// If declarative attachments are specified, begin/end render pass automatically
			bool has_render_pass = (!pass.color_attachments.empty() || pass.has_depth_attachment);
			if (has_render_pass) {
				RenderPassBeginInfo info;
				for (auto& att : pass.color_attachments) {
					if (att.handle.is_valid()) {
						info.color_attachments.push_back(get_texture(att.handle));
						if (att.clear) {
							info.clear_color = true;
							info.clear_color_value = att.clear_color;
						}
					}
				}
				if (pass.has_depth_attachment && pass.depth_attachment.handle.is_valid()) {
					info.depth_attachment = get_texture(pass.depth_attachment.handle);
					info.depth_read_only = pass.depth_attachment.read_only;
					if (pass.depth_attachment.clear) {
						info.clear_depth = true;
						info.clear_depth_value = pass.depth_attachment.clear_depth;
					}
				}
				rhi->cmd_begin_render_pass(target, info);
			}

			if (pass.execute) {
				pass.execute(rhi, target);
			}

			if (has_render_pass) {
				rhi->cmd_end_render_pass(target);
			}

			// Inject After Barriers (Release operations)
			for (auto& barrier : pass.after_barriers) {
				if (!barrier.handle.is_valid())
					continue;
				auto tex = get_texture(barrier.handle);
				auto buf = get_buffer(barrier.handle);
				if (barrier.is_release) {
					if (tex.is_valid()) {
						rhi->resource_barrier_release(target, tex, barrier.old_state, barrier.new_state, barrier.src_queue_family, barrier.dst_queue_family, barrier.handle.subresource);
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

	static const char* to_resource_state_string(ResourceState state) {
		switch (state) {
		case ResourceState::Undefined: return "Undefined";
		case ResourceState::RenderTarget: return "RenderTarget";
		case ResourceState::ShaderResource: return "ShaderResource";
		case ResourceState::DepthWrite: return "DepthWrite";
		case ResourceState::DepthRead: return "DepthRead";
		case ResourceState::Present: return "Present";
		case ResourceState::TransferDst: return "TransferDst";
		case ResourceState::TransferSrc: return "TransferSrc";
		case ResourceState::UnorderedAccess: return "UnorderedAccess";
		case ResourceState::IndirectArgument: return "IndirectArgument";
		case ResourceState::VertexBuffer: return "VertexBuffer";
		case ResourceState::IndexBuffer: return "IndexBuffer";
		case ResourceState::Common: return "Common";
		default: return "Unknown";
		}
	}

	void RenderGraph::export_graphviz(const std::string& filepath) const {
		std::string dot;
		dot += "digraph RenderGraph {\n";
		dot += "  rankdir=LR;\n";
		dot += "  node [fontname=\"Helvetica\", fontsize=10];\n";
		dot += "  edge [fontname=\"Helvetica\", fontsize=9];\n\n";

		// Passes Subgraph
		dot += "  subgraph cluster_passes {\n";
		dot += "    label = \"Passes (Active: " + std::to_string(sorted_passes.size()) + ", Culled: " + std::to_string(culled_pass_count) + ")\";\n";
		dot += "    style = filled;\n";
		dot += "    color = \"#f8fafc\";\n";
		dot += "    node [shape=box, style=\"filled,rounded\"];\n";

		for (size_t i = 0; i < passes.size(); ++i) {
			const auto& p = passes[i];
			std::string p_id = "pass_" + std::to_string(i);
			std::string color = p.is_culled ? "#f1f5f9" : (p.async_compute ? "#e0f2fe" : "#dcfce7");
			std::string border = p.is_culled ? "#94a3b8" : (p.async_compute ? "#0284c7" : "#16a34a");
			std::string style = p.is_culled ? "style=\"filled,dashed,rounded\"" : "style=\"filled,rounded\"";
			std::string status = p.is_culled ? " [CULLED]" : (p.async_compute ? " [ASYNC]" : "");
			
			dot += "    " + p_id + " [label=\"" + p.name + status + "\", fillcolor=\"" + color + "\", color=\"" + border + "\", " + style + "];\n";
		}
		dot += "  }\n\n";

		// Resources Subgraph
		dot += "  subgraph cluster_resources {\n";
		dot += "    label = \"Resources\";\n";
		dot += "    style = filled;\n";
		dot += "    color = \"#f1f5f9\";\n";

		for (size_t i = 1; i < resources.size(); ++i) {
			const auto& r = resources[i];
			std::string r_id = "res_" + std::to_string(i);
			std::string shape = r.is_external ? "cylinder" : (r.is_buffer ? "box" : "ellipse");
			std::string color = r.is_external ? "#fed7aa" : (r.is_active ? "#c7d2fe" : "#e2e8f0");
			std::string border = r.is_external ? "#ea580c" : (r.is_active ? "#4338ca" : "#94a3b8");
			std::string type_label = r.is_external ? " (External)" : (r.is_buffer ? " (Buffer)" : " (Texture)");
			std::string active_label = r.is_active ? "" : " [INACTIVE]";
			std::string lifetime_label = "";
			if (r.first_pass >= 0 && r.last_pass >= 0) {
				lifetime_label = " [P" + std::to_string(r.first_pass) + "..P" + std::to_string(r.last_pass) + "]";
			}
			std::string alias_label = "";
			if (r.is_aliased) {
				alias_label = " [Aliased C" + std::to_string(r.heap_chunk_index) + "@" + std::to_string(r.heap_offset / 1024) + "KB]";
			}

			dot += "    " + r_id + " [label=\"" + r.name + type_label + active_label + lifetime_label + alias_label + "\", shape=" + shape + ", fillcolor=\"" + color + "\", color=\"" + border + "\", style=filled];\n";
		}
		dot += "  }\n\n";

		// Edges
		for (size_t i = 0; i < passes.size(); ++i) {
			const auto& p = passes[i];
			std::string p_id = "pass_" + std::to_string(i);

			// Reads: Resource -> Pass
			for (const auto& access : p.reads) {
				if (access.handle.is_valid() && access.handle.id < resources.size()) {
					std::string r_id = "res_" + std::to_string(access.handle.id);
					std::string edge_color = p.is_culled ? "#cbd5e1" : "#2563eb";
					std::string edge_style = p.is_culled ? "style=dashed" : "";
					std::string sub_info = "";
					if (!access.handle.subresource.is_all()) {
						sub_info = " [m" + std::to_string(access.handle.subresource.base_mip) + "]";
					}
					dot += "  " + r_id + " -> " + p_id + " [color=\"" + edge_color + "\", label=\"" + to_resource_state_string(access.state) + sub_info + "\", " + edge_style + "];\n";
				}
			}

			// Writes: Pass -> Resource
			for (const auto& access : p.writes) {
				if (access.handle.is_valid() && access.handle.id < resources.size()) {
					std::string r_id = "res_" + std::to_string(access.handle.id);
					std::string edge_color = p.is_culled ? "#cbd5e1" : "#16a34a";
					std::string edge_style = p.is_culled ? "style=dashed" : "";
					std::string sub_info = "";
					if (!access.handle.subresource.is_all()) {
						sub_info = " [m" + std::to_string(access.handle.subresource.base_mip) + "]";
					}
					dot += "  " + p_id + " -> " + r_id + " [color=\"" + edge_color + "\", label=\"" + to_resource_state_string(access.state) + sub_info + "\", " + edge_style + "];\n";
				}
			}
		}

		dot += "}\n";

		bud::io::VirtualFileSystem vfs;
		vfs.write_text_async(filepath, std::move(dot));
	}

}
