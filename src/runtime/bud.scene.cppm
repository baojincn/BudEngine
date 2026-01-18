

#include <glm/glm.hpp>

export module bud.scene;

import bud.math;

export namespace bud::scene {
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
