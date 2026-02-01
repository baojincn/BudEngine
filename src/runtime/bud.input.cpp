module;
#include <unordered_map>

module bud.input;

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

}
