#include "src/input/bud.input.manager.hpp"

#include "src/runtime/bud.input.hpp"

namespace bud::input {

void InputManager::update() {
    // snapshot previous
    prev_keys_ = curr_keys_;
    prev_mouse_buttons_ = curr_mouse_buttons_;

    // reset deltas
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    scroll_y_ = 0.0f;

    auto& low = Input::get();

    // Sample all known Key enum values (assumes contiguous enum starting at 0)
    const int max_key = static_cast<int>(Key::LCtrl);
    for (int i = 0; i <= max_key; ++i) {
        Key k = static_cast<Key>(i);
        bool down = low.is_key_down(k);
        curr_keys_[k] = down;
    }

    // Sample mouse buttons (Left..Middle)
    const int max_btn = static_cast<int>(MouseButton::Middle);
    for (int i = 0; i <= max_btn; ++i) {
        MouseButton b = static_cast<MouseButton>(i);
        bool down = low.is_mouse_button_down(b);
        curr_mouse_buttons_[b] = down;
    }

    // mouse delta/scroll
    low.get_mouse_delta(mouse_dx_, mouse_dy_);
    scroll_y_ = low.get_mouse_scroll();

    // Collect triggered actions (rising edge) and invoke callbacks after sampling
    std::vector<std::string> triggered;
    triggered.reserve(bindings_.size());

    for (const auto& entry : bindings_) {
        const auto& action = entry.first;
        const auto& bind = entry.second;

        bool was_pressed = false;

        for (auto key : bind.keys) {
            bool prev = false;
            auto itp = prev_keys_.find(key);
            if (itp != prev_keys_.end()) prev = itp->second;
            bool curr = false;
            auto itc = curr_keys_.find(key);
            if (itc != curr_keys_.end()) curr = itc->second;
            if (!prev && curr) { was_pressed = true; break; }
        }

        if (!was_pressed) {
            for (auto btn : bind.mouse_buttons) {
                bool prev = false;
                auto itp = prev_mouse_buttons_.find(btn);
                if (itp != prev_mouse_buttons_.end()) prev = itp->second;
                bool curr = false;
                auto itc = curr_mouse_buttons_.find(btn);
                if (itc != curr_mouse_buttons_.end()) curr = itc->second;
                if (!prev && curr) { was_pressed = true; break; }
            }
        }

        if (was_pressed) triggered.push_back(action);
    }

    for (const auto& a : triggered) {
        auto it = callbacks_.find(a);
        if (it != callbacks_.end() && it->second) {
            it->second();
        }
    }
}

bool InputManager::is_key_down(Key key) const {
    auto it = curr_keys_.find(key);
    return it != curr_keys_.end() ? it->second : false;
}

bool InputManager::was_key_pressed(Key key) const {
    bool prev = false;
    auto itp = prev_keys_.find(key);
    if (itp != prev_keys_.end()) prev = itp->second;
    bool curr = false;
    auto itc = curr_keys_.find(key);
    if (itc != curr_keys_.end()) curr = itc->second;
    return !prev && curr;
}

bool InputManager::is_mouse_down(MouseButton btn) const {
    auto it = curr_mouse_buttons_.find(btn);
    return it != curr_mouse_buttons_.end() ? it->second : false;
}

bool InputManager::was_mouse_pressed(MouseButton btn) const {
    bool prev = false;
    auto itp = prev_mouse_buttons_.find(btn);
    if (itp != prev_mouse_buttons_.end()) prev = itp->second;
    bool curr = false;
    auto itc = curr_mouse_buttons_.find(btn);
    if (itc != curr_mouse_buttons_.end()) curr = itc->second;
    return !prev && curr;
}

void InputManager::get_mouse_delta(float& out_dx, float& out_dy) const {
    out_dx = mouse_dx_;
    out_dy = mouse_dy_;
}

float InputManager::get_mouse_scroll() const {
    return scroll_y_;
}

void InputManager::bind_key(const std::string& action, Key key) {
    bindings_[action].keys.push_back(key);
}

void InputManager::bind_mouse_button(const std::string& action, MouseButton btn) {
    bindings_[action].mouse_buttons.push_back(btn);
}

void InputManager::unbind_action(const std::string& action) {
    bindings_.erase(action);
    callbacks_.erase(action);
}

bool InputManager::is_action_down(const std::string& action) const {
    auto it = bindings_.find(action);
    if (it == bindings_.end()) return false;
    const auto& bind = it->second;
    for (auto k : bind.keys) if (is_key_down(k)) return true;
    for (auto b : bind.mouse_buttons) if (is_mouse_down(b)) return true;
    return false;
}

bool InputManager::was_action_pressed(const std::string& action) const {
    auto it = bindings_.find(action);
    if (it == bindings_.end()) return false;
    const auto& bind = it->second;
    for (auto k : bind.keys) if (was_key_pressed(k)) return true;
    for (auto b : bind.mouse_buttons) if (was_mouse_pressed(b)) return true;
    return false;
}

void InputManager::register_action_callback(const std::string& action, std::function<void()> callback) {
    callbacks_[action] = std::move(callback);
}

void InputManager::unregister_action_callback(const std::string& action) {
    callbacks_.erase(action);
}

} // namespace bud::input
