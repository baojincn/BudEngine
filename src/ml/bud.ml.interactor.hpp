#pragma once
#include "src/ml/bud.ml.types.hpp"

namespace bud::ml::rl {

	class Interactor {
	public:
		virtual ~Interactor() = default;

		// 【数据流向 1】：Game Data -> Tensor
		// 一次性收集所有传入 ID 的数据，压入连续的内存块中，极致的 Cache 友好！
		virtual void gather_observations(const std::vector<AgentID>& agents,
			BatchedObservations& out_obs) = 0;

		// 【数据流向 2】：Tensor -> Game Data
		// 拿到神经网络或 Python 推理出的一整块动作数组，批量应用到游戏世界
		virtual void perform_actions(const std::vector<AgentID>& agents,
			const BatchedActions& actions) = 0;
	};

} // namespace bud::ml::rl
