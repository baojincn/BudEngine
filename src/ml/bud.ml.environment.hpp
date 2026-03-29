#pragma once
#include "bud.ml.types.hpp"

namespace bud::ml::rl {

	class Environment {
	public:
		virtual ~Environment() = default;

		// 批量获取所有 Agent 的当前奖励 (比如：被剔除的三角形数量)
		virtual void gather_rewards(const std::vector<AgentID>& agents,
			std::vector<float>& out_rewards) = 0;

		// 批量获取所有 Agent 的存活状态
		virtual void gather_completions(const std::vector<AgentID>& agents,
			std::vector<bool>& out_dones) = 0;

		// 重置被标记为 Done 的 Agent 的游戏状态
		virtual void reset_agents(const std::vector<AgentID>& agents_to_reset) = 0;
	};

} // namespace bud::ml::rl
