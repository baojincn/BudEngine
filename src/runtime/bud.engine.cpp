

#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <print>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#else
#define ZoneScoped
#define FrameMark
#endif

#include "src/runtime/bud.engine.hpp"
#include "src/graphics/vulkan/bud.graphics.vulkan.hpp"

namespace bud::engine {

	BudEngine::BudEngine(const bud::graphics::EngineConfig config) : engine_config(config) {

		window = bud::platform::create_window(engine_config.name, engine_config.width, engine_config.height);
		task_scheduler = std::make_unique<bud::threading::TaskScheduler>();

		asset_manager = std::make_unique<bud::io::AssetManager>(task_scheduler.get());

		rhi = bud::graphics::create_rhi(engine_config.backend);

		auto enable_validation = engine_config.enable_validation;
		rhi->init(window.get(), task_scheduler.get(), enable_validation, engine_config.inflight_frame_count);

		renderer = std::make_unique<bud::graphics::Renderer>(rhi.get(), asset_manager.get(), task_scheduler.get());

		render_scenes.resize(engine_config.inflight_frame_count);

		scene.main_camera = bud::scene::Camera(bud::math::vec3(0.0f, 100.0f, 0.0f));
		scene.main_camera.movement_speed = 70.0f;
	}

	BudEngine::~BudEngine() {
		task_scheduler->stop();
		asset_manager.reset();
		renderer.reset();
		rhi->cleanup();
		rhi.reset();
	}

	void BudEngine::run(GameLogic perform_game_logic) {
		// Initialize the main thread as a worker
		task_scheduler->init_main_thread_worker();

		// 获取配置的固定步长 (例如 1/60 秒)
		const double fixed_dt = renderer->get_config().fixed_logic_timestep;

		using Clock = std::chrono::high_resolution_clock;
		auto last_time = Clock::now();

		while (!window->should_close()) {
			task_scheduler->pump_main_thread_tasks();
			handle_events();

			auto now = Clock::now();
			double frame_time = std::chrono::duration<double>(now - last_time).count();
			last_time = now;

			// 防止螺旋死亡
			if (frame_time > 0.25) frame_time = 0.25;

			accumulator += frame_time;

			// --- 阶段 A: 逻辑更新 (追赶时间) ---
			while (accumulator >= fixed_dt) {
				// 1. 切换到下一个可写的 Buffer
				current_write_index = (current_write_index + 1) % render_scenes.size();

				// 2. 执行逻辑 (Spawn + Wait)
				if (perform_game_logic) {
					bud::threading::Counter logic_counter;
					task_scheduler->spawn("GameLogic", [&]() {
						perform_game_logic((float)fixed_dt);
					}, &logic_counter);

					task_scheduler->wait_for_counter(logic_counter);
				}

				// 3. 提取数据 (Sync)
				// 写入到 current_write_index 指向的 buffer
				extract_scene_data(render_scenes[current_write_index]);

				// 4. 提交这一帧
				last_committed_index.store(current_write_index, std::memory_order_release);

				accumulator -= fixed_dt;
			}

			// --- 阶段 B: 渲染 (尽可能快) ---

			// 1. 获取最新已提交的帧数据
			uint32_t render_idx = last_committed_index.load(std::memory_order_acquire);

			perform_rendering((float)frame_time, render_idx);

			// 这里的 FrameMark 统计的是主循环频率
			FrameMark;
		}
		rhi->wait_idle();
	}

	void BudEngine::handle_events() {
		window->poll_events();
	}

	void BudEngine::extract_scene_data(bud::graphics::RenderScene& render_scene) {

		task_scheduler->wait_for_counter(render_task_counter);

		// 1. [主线程] 准备阶段
		auto& logic_entities = scene.entities;
		size_t total_logic_count = logic_entities.size();

		// 预估这一帧需要渲染多少物体 (简单起见，假设所有逻辑实体都可能渲染)
		// 这一步非常快，只是设置 capacity

		constexpr size_t buffering_size = 128; // 预留一些余量，避免频繁越界

		render_scene.reset(total_logic_count + buffering_size);

		// 2. [任务系统] 启动并行提取
		bud::threading::Counter extract_scene_counter;

		// 设定分块大小 (Granularity)。太小会导致调度开销，太大导致负载不均。
		// 经验值：64 ~ 256 个实体一个 Job。
		constexpr size_t CHUNK_SIZE = 128;

		task_scheduler->ParallelFor(total_logic_count, CHUNK_SIZE,
			[&](size_t start, size_t end) {
				// --- Worker 线程内部 ---

				// 缓存局部变量，减少指针跳转
				const auto& meshes = renderer->get_meshes(); // 假设能获取 mesh 列表

				for (size_t i = start; i <= end; ++i) {
					const auto& entity = logic_entities[i];

					// a. 过滤无效实体 (Logic Culling)
					// 比如：隐藏物体、没有 Mesh 的空节点等
					if (entity.mesh_index >= meshes.size()) [[unlikely]] {
						continue;
					}
					// if (!entity.is_active) continue; 

					// b. 计算数据 (最耗时的部分)
					// 假设 entity.transform 已经是 World Matrix (或者在这里做 Parent * Local 矩阵乘法)
					const auto& world_matrix = entity.transform;

					// 获取 Mesh 的局部 AABB
					const auto& local_aabb = meshes[entity.mesh_index].aabb;

					// 计算变换后的 AABB (SIMD 优化的数学库在这里发光)
					// 中有 AABB::transform
					auto world_aabb = local_aabb.transform(world_matrix);

					// c. 写入 RenderScene (原子申请 Slot，无锁写入)
					render_scene.add_instance(
						world_matrix,
						world_aabb,
						entity.mesh_index,
						entity.material_index,
						entity.is_static
					);

				}
			},
			&extract_scene_counter // 传入 Counter 以便等待
		);

		// 3. [主线程] 等待提取完成 (Yielding Wait)
		// 此时主线程不会傻等，它会去“偷”别的 Job 来做 (Work Stealing)
		task_scheduler->wait_for_counter(extract_scene_counter);

		// extraction 结束！render_scene 现在填满了这一帧的数据。
		// render_scene.instance_count.load() 就是最终的渲染物体数量。
	}


	void BudEngine::sync_game_to_rendering(uint32_t render_scene_index) {
		ZoneScoped;
		
		extract_scene_data(render_scenes[render_scene_index]);

		FrameMark;
	}

	void BudEngine::perform_rendering(float delta_time, uint32_t render_scene_index) {
		// Wait for previous frame's render task to complete
		task_scheduler->wait_for_counter(render_task_counter);

		int width, height;
		window->get_size(width, height);
		if (height == 0)
			height = 1;

		// 更新 SceneView
		bud::graphics::SceneView view_snapshot;
		view_snapshot.viewport_width = static_cast<float>(width);
		view_snapshot.viewport_height = static_cast<float>(height);

		// 计算总运行时间
		using Clock = std::chrono::high_resolution_clock;
		static auto start_time = Clock::now();
		view_snapshot.time = std::chrono::duration<float>(Clock::now() - start_time).count();
		view_snapshot.delta_time = delta_time;

		auto aspect = view_snapshot.viewport_width / view_snapshot.viewport_height;
		view_snapshot.view_matrix = scene.main_camera.get_view_matrix();
		view_snapshot.proj_matrix = bud::math::perspective_vk(scene.main_camera.zoom, aspect, near_plane, far_plane);
		view_snapshot.camera_position = scene.main_camera.position;
		view_snapshot.near_plane = near_plane;
		view_snapshot.far_plane = far_plane;

		auto& config = renderer->get_config();
		view_snapshot.light_dir = bud::math::normalize(config.directional_light_position);
		view_snapshot.light_color = config.directional_light_color;

		view_snapshot.update_matrices();

		// 3. 发射渲染任务 (Fire and Forget)
		task_scheduler->spawn("RenderTask", [this, render_scene_index, view_snapshot]() {
			// 这里调用新的 render 接口
			renderer->render(render_scenes[render_scene_index], const_cast<bud::graphics::SceneView&>(view_snapshot));
		}, &render_task_counter);
	}
} // namespace bud::engine
