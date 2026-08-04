#pragma once
#include <cstdint>
#include <vector>

namespace bud::ml {
	// 智能体仅仅是一个整型 ID，它可以映射到你的 ECS Entity ID，或者 Instance Index
	using AgentID = int32_t;

	// 批处理观察数据 (SoA 布局优先，这里用一维连续数组表示 Batch)
	struct BatchedObservations {
		std::vector<float> data;  // 长度 = agent_count * obs_dim
		size_t obs_dim = 0;
	};

	struct BatchedActions {
		std::vector<float> data;  // 长度 = agent_count * action_dim
		size_t action_dim = 0;
	};
}
