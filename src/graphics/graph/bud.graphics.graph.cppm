module;
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>

export module bud.graphics.graph;

import bud.graphics;
import bud.graphics.defs;

export namespace bud::graphics {

	export struct RGHandle {
		uint32_t id = 0;
		bool is_valid() const { return id != 0; }
		bool operator==(const RGHandle& other) const { return id == other.id; }
	};


	export class RGBuilder {
	public:
		RGBuilder(class RenderGraph& graph, uint32_t pass_index)
			: graph_(graph), pass_index_(pass_index) {
		}

		RGHandle read(RGHandle handle, ResourceState state);
		RGHandle write(RGHandle handle, ResourceState state);

	private:
		class RenderGraph& graph_;
		uint32_t pass_index_;
	};


	export class RenderGraph {
		friend class RGBuilder;
	public:
		RenderGraph(RHI* rhi) : rhi_(rhi) {}

		template <typename SetupFn, typename ExecFn>
		void add_pass(const std::string& name, SetupFn setup, ExecFn execute) {
			auto& pass = passes_.emplace_back();
			pass.name = name;
			pass.execute_fn = [exec = std::move(execute)](RHI* rhi, CommandHandle cmd) {
				exec(rhi, cmd);
				};

			RGBuilder builder(*this, static_cast<uint32_t>(passes_.size() - 1));
			setup(builder);
		}

		RGHandle import_texture(const std::string& name, RHITexture* texture, ResourceState current_state);

		RHITexture* get_texture(RGHandle handle);

		void execute(CommandHandle cmd);

	private:
		struct PassInfo {
			std::string name;
			std::function<void(RHI*, CommandHandle)> execute_fn;
			std::vector<std::pair<RGHandle, ResourceState>> transitions;
		};

		struct ResourceInfo {
			std::string name;
			RHITexture* physical_texture = nullptr;
			ResourceState current_state = ResourceState::Undefined;
		};

		RHI* rhi_;
		std::vector<PassInfo> passes_;
		std::vector<ResourceInfo> resources_;
	};

	RGHandle RGBuilder::read(RGHandle handle, ResourceState state) {
		graph_.passes_[pass_index_].transitions.push_back({ handle, state });
		return handle;
	}

	RGHandle RGBuilder::write(RGHandle handle, ResourceState state) {
		graph_.passes_[pass_index_].transitions.push_back({ handle, state });
		return handle;
	}

	RGHandle RenderGraph::import_texture(const std::string& name, RHITexture* texture, ResourceState current_state) {
		ResourceInfo info;
		info.name = name;
		info.physical_texture = texture;
		info.current_state = current_state;
		resources_.push_back(info);
		return RGHandle{ static_cast<uint32_t>(resources_.size()) };
	}

	RHITexture* RenderGraph::get_texture(RGHandle handle) {
		if (handle.id == 0 || handle.id > resources_.size()) return nullptr;
		return resources_[handle.id - 1].physical_texture;
	}

	void RenderGraph::execute(CommandHandle cmd) {
		for (auto& pass : passes_) {
			// 自动处理屏障
			for (auto& [handle, target_state] : pass.transitions) {
				auto& res = resources_[handle.id - 1];
				if (res.current_state != target_state) {

					rhi_->cmd_resource_barrier(cmd, res.physical_texture, res.current_state, target_state);
					res.current_state = target_state;
				}
			}

			// 执行 Pass
			pass.execute_fn(rhi_, cmd);
		}
	}
}
