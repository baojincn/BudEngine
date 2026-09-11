#include <vector>
#include <cmath>

#include "src/runtime/bud.scene.hpp"

namespace bud::scene {

    Camera::Camera(bud::math::vec3 start_pos, bud::math::vec3 start_up, float start_yaw, float start_pitch)
        : front(bud::math::vec3(0.0f, 0.0f, -1.0f)), movement_speed(2.5f), mouse_sensitivity(0.1f), zoom(45.0f)
    {
        position = start_pos;
        world_up = start_up;
        yaw = start_yaw;
        pitch = start_pitch;
        update_camera_vectors();
    }

    void Camera::set_mode(CameraMode m) {
        if (mode == m) return;

        if (m == CameraMode::ThirdPerson) {
            target_position = position + front * orbit_distance;
            spring_position = position;
            spring_velocity = bud::math::vec3(0.0f);
        }

        mode = m;
    }

    bud::math::mat4 Camera::get_view_matrix() const {
        return bud::math::lookAt(position, position + front, up);
    }

    void Camera::process_keyboard(int direction, float delta_time) {
        if (mode == CameraMode::ThirdPerson) {
            float velocity = movement_speed * delta_time;
            if (direction == 0) target_position += front * velocity;
            if (direction == 1) target_position -= front * velocity;
            if (direction == 2) target_position -= right * velocity;
            if (direction == 3) target_position += right * velocity;
            if (direction == 4) target_position += world_up * velocity;
            if (direction == 5) target_position -= world_up * velocity;
            return;
        }

        float velocity = movement_speed * delta_time;
        if (direction == 0) position += front * velocity;
        if (direction == 1) position -= front * velocity;
        if (direction == 2) position -= right * velocity;
        if (direction == 3) position += right * velocity;
        if (direction == 4) position += world_up * velocity;
        if (direction == 5) position -= world_up * velocity;
    }

    void Camera::process_mouse_movement(float x_offset, float y_offset, bool constrain_pitch) {
        if (mode == CameraMode::ThirdPerson) {
            orbit_yaw += x_offset * mouse_sensitivity;
            orbit_pitch -= y_offset * mouse_sensitivity;
            if (constrain_pitch) {
                if (orbit_pitch > 89.0f) orbit_pitch = 89.0f;
                if (orbit_pitch < -89.0f) orbit_pitch = -89.0f;
            }
            return;
        }

        x_offset *= mouse_sensitivity;
        y_offset *= mouse_sensitivity;
        yaw += x_offset;
        pitch -= y_offset;
        if (constrain_pitch) {
            if (pitch > 89.0f) pitch = 89.0f;
            if (pitch < -89.0f) pitch = -89.0f;
        }
        update_camera_vectors();
    }

    void Camera::process_mouse_scroll(float y_offset) {
        if (mode == CameraMode::ThirdPerson) {
            orbit_distance -= y_offset;
            if (orbit_distance < 1.0f) orbit_distance = 1.0f;
            if (orbit_distance > 50.0f) orbit_distance = 50.0f;
            return;
        }

        zoom -= y_offset;
        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > 45.0f) zoom = 45.0f;
    }

    void Camera::process_mouse_drag_zoom(float yoffset) {
        if (mode == CameraMode::ThirdPerson) {
            float zoom_sensitivity = 0.1f;
            orbit_distance -= yoffset * zoom_sensitivity;
            if (orbit_distance < 1.0f) orbit_distance = 1.0f;
            if (orbit_distance > 50.0f) orbit_distance = 50.0f;
            return;
        }

        float zoom_sensitivity = 0.1f;
        zoom -= yoffset * zoom_sensitivity;
        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > 45.0f) zoom = 45.0f;
    }

    bud::math::quaternion Camera::get_rotation() const {
        bud::math::quaternion q_yaw   = glm::angleAxis(bud::math::radians(yaw),   bud::math::vec3(0.0f, 1.0f, 0.0f));
        bud::math::quaternion q_pitch = glm::angleAxis(bud::math::radians(pitch), bud::math::vec3(1.0f, 0.0f, 0.0f));
        return glm::normalize(q_yaw * q_pitch);
    }

    void Camera::set_rotation(const bud::math::quaternion& rot) {
        bud::math::vec3 f = glm::normalize(rot * bud::math::vec3(0.0f, 0.0f, -1.0f));
        pitch = bud::math::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
        yaw   = bud::math::degrees(std::atan2(f.z, f.x));
        update_camera_vectors();
    }

    void Camera::update(float dt) {
        (void)dt;
        if (mode != CameraMode::ThirdPerson) return;

        // UE5-style rigid follow (camera lag disabled): the camera pose derives
        // directly from the orbit yaw/pitch and distance. A spring-damper here made
        // the view sway and drift everywhere.
        float pitch_rad = bud::math::radians(orbit_pitch);
        float yaw_rad = bud::math::radians(orbit_yaw);
        bud::math::vec3 offset;
        offset.x = cos(pitch_rad) * sin(yaw_rad);
        offset.y = sin(pitch_rad);
        offset.z = cos(pitch_rad) * cos(yaw_rad);
        position = target_position - offset * orbit_distance;

        front = bud::math::normalize(target_position - position);
        right = bud::math::normalize(bud::math::cross(front, world_up));
        up = bud::math::normalize(bud::math::cross(right, front));
    }

    void Camera::update_camera_vectors() {
        bud::math::vec3 f;
        f.x = cos(bud::math::radians(yaw)) * cos(bud::math::radians(pitch));
        f.y = sin(bud::math::radians(pitch));
        f.z = sin(bud::math::radians(yaw)) * cos(bud::math::radians(pitch));
        front = bud::math::normalize(f);
        right = bud::math::normalize(bud::math::cross(front, world_up));
        up = bud::math::normalize(bud::math::cross(right, front));
    }

    void Camera::update_freefly_vectors() {
        update_camera_vectors();
    }

    void Camera::update_thirdperson_vectors() {
        // Vectors are computed in update()
    }

}