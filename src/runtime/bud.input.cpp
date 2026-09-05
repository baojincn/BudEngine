#include <unordered_map>
#include "src/runtime/bud.input.hpp"

namespace bud::input {

    Input& Input::get() {
        static Input instance;
        return instance;
    }

    bool Input::is_key_down(Key key) const {
        auto it = keys.find(key);
        return it != keys.end() && it->second;
    }

    bool Input::is_mouse_button_down(MouseButton btn) const {
        auto it = mouse_buttons.find(btn);
        return it != mouse_buttons.end() && it->second;
    }

    void Input::get_mouse_delta(float& x, float& y) const {
        x = mouse_delta_x;
        y = mouse_delta_y;
    }

    float Input::get_mouse_scroll() const {
        return scroll_y;
    }

    bool Input::is_gamepad_connected() const {
        return gamepad_connected;
    }

    bool Input::is_gamepad_button_down(GamepadButton btn) const {
        auto it = gamepad_buttons.find(btn);
        return it != gamepad_buttons.end() && it->second;
    }

    float Input::get_gamepad_axis(GamepadAxis axis) const {
        return gamepad_axis_values[static_cast<int>(axis)];
    }

    float Input::get_gamepad_axis_delta(GamepadAxis axis) const {
        int i = static_cast<int>(axis);
        return gamepad_axis_values[i] - gamepad_axis_prev[i];
    }

}