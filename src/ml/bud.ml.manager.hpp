#pragma once
#include <memory>
#include "bud.ml.types.hpp"
#include "bud.ml.interactor.hpp"
#include "bud.ml.environment.hpp"
#include "bud.ml.policy.hpp" // 包含你的 ONNX 推理后端

namespace bud::ml::rl {

	class Manager {
	public:
		void add_agent(AgentID id) { m_active_agents.push_back(id); }
		void remove_agent(AgentID id) { /* 从数组移除 */ }

		void set_interactor(std::shared_ptr<Interactor> interactor) { m_interactor = interactor; }
		void set_environment(std::shared_ptr<Environment> env) { m_environment = env; }
		void set_policy(std::shared_ptr<IPolicy> policy) { m_policy = policy; }

		// 🌟 核心数据流管线 (C++ 独立推理模式下调用)
		void tick_inference() {
			if (m_active_agents.empty() || !m_interactor || !m_policy) return;

			// 1. Game -> Tensor
			m_interactor->gather_observations(m_active_agents, m_obs_buffer);

			// 2. 神经网络批量推理 (GPU 瞬间完成)
			m_policy->evaluate(m_obs_buffer, m_action_buffer);

			// 3. Tensor -> Game
			m_interactor->perform_actions(m_active_agents, m_action_buffer);
		}

		// --- 留给 Python (PyBind11) 调用的数据接口 ---
		const std::vector<AgentID>& get_active_agents() const { return m_active_agents; }
		BatchedObservations& get_obs_buffer() { return m_obs_buffer; }
		BatchedActions& get_action_buffer() { return m_action_buffer; }

	private:
		std::vector<AgentID> m_active_agents;

		std::shared_ptr<Interactor> m_interactor;
		std::shared_ptr<Environment> m_environment;
		std::shared_ptr<IPolicy> m_policy;

		BatchedObservations m_obs_buffer;
		BatchedActions m_action_buffer;
	};

} // namespace bud::ml::rl
