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

    bud::math::mat4 Camera::get_view_matrix() const {
        return bud::math::lookAt(position, position + front, up);
    }

    void Camera::process_keyboard(int direction, float delta_time) {
        float velocity = movement_speed * delta_time;
        if (direction == 0) position += front * velocity;
        if (direction == 1) position -= front * velocity;
        if (direction == 2) position -= right * velocity;
        if (direction == 3) position += right * velocity;
        if (direction == 4) position += world_up * velocity;
        if (direction == 5) position -= world_up * velocity;
    }

    void Camera::process_mouse_movement(float x_offset, float y_offset, bool constrain_pitch) {
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
        zoom -= y_offset;
        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > 45.0f) zoom = 45.0f;
    }

    void Camera::process_mouse_drag_zoom(float yoffset) {
        float zoom_sensitivity = 0.1f;
        zoom -= yoffset * zoom_sensitivity;
        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > 45.0f) zoom = 45.0f;
    }

    bud::math::quaternion Camera::get_rotation() const {
        // Build quaternion from yaw (Y-axis) and pitch (X-axis), no roll
        bud::math::quaternion q_yaw   = glm::angleAxis(bud::math::radians(yaw),   bud::math::vec3(0.0f, 1.0f, 0.0f));
        bud::math::quaternion q_pitch = glm::angleAxis(bud::math::radians(pitch), bud::math::vec3(1.0f, 0.0f, 0.0f));
        return glm::normalize(q_yaw * q_pitch);
    }

    void Camera::set_rotation(const bud::math::quaternion& rot) {
        // Rotate the camera-space forward (0,0,-1) by the quaternion to get the world-space
        // forward vector, then invert the same formula used in update_camera_vectors:
        //   f = (cos(yaw)*cos(pitch), sin(pitch), sin(yaw)*cos(pitch))
        // get_rotation builds q = q_yaw(Y) * q_pitch(X), so rotating (0,0,-1) gives:
        //   f.world = (-sin(yaw)*cos(pitch),  sin(pitch),  -cos(yaw)*cos(pitch))
        // Therefore:
        //   pitch = asin(f.y)
        //   yaw   = atan2(-f.x, -f.z)   <-- correct inverse of (-sin, -cos) pair
        bud::math::vec3 f = glm::normalize(rot * bud::math::vec3(0.0f, 0.0f, -1.0f));
        pitch = bud::math::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
        yaw   = bud::math::degrees(std::atan2(-f.x, -f.z));
        update_camera_vectors();
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

}
