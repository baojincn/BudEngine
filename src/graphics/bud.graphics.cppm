module;

#include <cmath>
#include <string>
#include <memory>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>


export module bud.graphics;

import bud.math;
import bud.platform;
import bud.threading;

export namespace bud::graphics {

	export enum class Backend {
		Vulkan,
		D3D12,
		Metal
	};

	export struct MemoryAllocation {
		void* internal_handle = nullptr;
		uint64_t offset = 0;
		uint64_t size = 0;
		void* mapped_ptr = nullptr;
	};

	export class MemoryManager {
	public:
		virtual ~MemoryManager() = default;

		virtual void init() = 0;
		virtual void cleanup() = 0;

		virtual bool allocate_staging(uint64_t size, MemoryAllocation& out_alloc) = 0;

		virtual void mark_submitted(const MemoryAllocation& alloc, void* sync_handle) = 0;
	};


	export struct RenderConfig {
		uint32_t shadowMapSize = 4096;
		float shadowBiasConstant = 1.25f;
		float shadowBiasSlope = 1.75f;
		float shadowOrthoSize = 35.0f;
		float shadowNear = 0.1f;
		float shadowFar = 100.0f;

		bud::math::vec3 lightPos = { 5.0f, 15.0f, 5.0f };
		bud::math::vec3 lightColor = { 1.0f, 1.0f, 1.0f };
		float lightIntensity = 5.0f;
		float ambientStrength = 0.05f;

		bool enableSoftShadows = true;
	};

	export class RHI {
	public:
		virtual ~RHI() = default;
		virtual void init(bud::platform::Window* window, bud::threading::TaskScheduler* task_scheduler, bool enable_validation) = 0;
		virtual void draw_frame(const bud::math::mat4& view, const bud::math::mat4& proj) = 0;
		virtual void wait_idle() = 0;
		virtual void cleanup() = 0;
		virtual void reload_shaders_async() = 0;
		virtual void load_model_async(const std::string& filepath) = 0;
		virtual void set_config(const RenderConfig& new_settings) = 0;
	};

	export std::unique_ptr<RHI> create_rhi(Backend backend);

	// 摄像机类
	export class Camera {
	public:
		// 状态数据
		bud::math::vec3 position;
		bud::math::vec3 front;
		bud::math::vec3 up;
		bud::math::vec3 right;
		bud::math::vec3 world_up;

		// 欧拉角
		float yaw;
		float pitch;

		// 选项
		float movement_speed;
		float mouse_sensitivity;
		float zoom;

		// 构造函数
		Camera(bud::math::vec3 start_pos = bud::math::vec3(0.0f, 0.0f, 3.0f),
			bud::math::vec3 start_up = bud::math::vec3(0.0f, 1.0f, 0.0f),
			float start_yaw = bud::math::YAW,
			float start_pitch = bud::math::PITCH)
			: front(bud::math::vec3(0.0f, 0.0f, -1.0f)), movement_speed(bud::math::SPEED), mouse_sensitivity(bud::math::SENSITIVITY), zoom(bud::math::ZOOM)
		{
			position = start_pos;
			world_up = start_up;
			yaw = start_yaw;
			pitch = start_pitch;
			update_camera_vectors();
		}

		// 获取 View 矩阵
		bud::math::mat4 get_view_matrix() const {
			return bud::math::lookAt(position, position + front, up);
		}

		// 处理键盘移动 (方向枚举稍后在 Core 或 Input 定义，这里先传方向向量)
		void process_keyboard(int direction, float delta_time) {
			float velocity = movement_speed * delta_time;
			if (direction == 0) position += front * velocity; // FORWARD
			if (direction == 1) position -= front * velocity; // BACKWARD
			if (direction == 2) position -= right * velocity; // LEFT
			if (direction == 3) position += right * velocity; // RIGHT
			if (direction == 4) position += world_up * velocity; // UP
			if (direction == 5) position -= world_up * velocity; // DOWN
		}

		// 处理鼠标移动
		void process_mouse_movement(float x_offset, float y_offset, bool constrain_pitch = true) {
			x_offset *= mouse_sensitivity;
			y_offset *= mouse_sensitivity;

			yaw += x_offset;
			pitch -= y_offset;

			if (constrain_pitch) {
				if (pitch > 89.0f)
					pitch = 89.0f;

				if (pitch < -89.0f)
					pitch = -89.0f;
			}
			update_camera_vectors();
		}

		// 处理鼠标滚轮缩放
		void process_mouse_scroll(float y_offset) {
			zoom -= y_offset;

			if (zoom < 1.0f)
				zoom = 1.0f;

			if (zoom > 45.0f)
				zoom = 45.0f;
		}

		// yoffset: 鼠标上下移动量
		void process_mouse_drag_zoom(float yoffset) {
			float zoom_sensitivity = 0.1f;
			zoom -= yoffset * zoom_sensitivity;

			if (zoom < 1.0f)
				zoom = 1.0f;

			if (zoom > 45.0f)
				zoom = 45.0f;
		}

	private:
		void update_camera_vectors() {
			bud::math::vec3 f;
			f.x = cos(bud::math::radians(yaw)) * cos(bud::math::radians(pitch));
			f.y = sin(bud::math::radians(pitch));
			f.z = sin(bud::math::radians(yaw)) * cos(bud::math::radians(pitch));
			front = bud::math::normalize(f);
			right = bud::math::normalize(bud::math::cross(front, world_up));
			up = bud::math::normalize(bud::math::cross(right, front));
		}
	};
}
