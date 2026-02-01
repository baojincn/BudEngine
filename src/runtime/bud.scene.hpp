#pragma once

#include <vector>

#include "src/core/bud.math.hpp"

namespace bud::scene {

	 class Camera {
	public:
		bud::math::vec3 position;
		bud::math::vec3 front;
		bud::math::vec3 up;
		bud::math::vec3 right;
		bud::math::vec3 world_up;
		float yaw;
		float pitch;
		float movement_speed;
		float mouse_sensitivity;
		float zoom;

		Camera(bud::math::vec3 start_pos = bud::math::vec3(0.0f, 0.0f, 3.0f),
			bud::math::vec3 start_up = bud::math::vec3(0.0f, 1.0f, 0.0f),
			float start_yaw = -90.0f,
			float start_pitch = 0.0f);

		bud::math::mat4 get_view_matrix() const;
		void process_keyboard(int direction, float delta_time);
		void process_mouse_movement(float x_offset, float y_offset, bool constrain_pitch = true);
		void process_mouse_scroll(float y_offset);
		void process_mouse_drag_zoom(float yoffset);

	private:
		void update_camera_vectors();
	};

	 struct Entity {
		uint32_t mesh_index;
		bud::math::mat4 transform = bud::math::mat4(1.0f);
		bool is_static = true;
	};

	 struct Scene {
		Camera main_camera;
		std::vector<Entity> entities;
	};
}
