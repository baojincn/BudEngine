#pragma once
#include <vector>
#include <string>

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

		bud::math::quaternion get_rotation() const;
		void set_rotation(const bud::math::quaternion& rot);
		inline bud::math::AABB get_collision_aabb(float radius = 0.2f) const {
			return { position - bud::math::vec3(radius), position + bud::math::vec3(radius) };
		}

	private:
		void update_camera_vectors();
	};

	 struct Entity {
		std::string asset_path = "";
		uint32_t mesh_index = 0xFFFFFFFF;
		uint32_t material_index = 0;
		bud::math::mat4 transform = bud::math::mat4(1.0f);
		bool is_static = true;
		bool is_active = true;
	};

	struct DirectionalLight {
		bud::math::vec3 direction = { 0.5f, 1.0f, 0.3f };
		bud::math::vec3 color = { 1.0f, 1.0f, 1.0f };
		float intensity = 5.0f;
	};

	 struct Scene {
		Camera main_camera;
		DirectionalLight directional_light;
		float ambient_strength = 0.05f;
		std::vector<Entity> entities;
	};
}
