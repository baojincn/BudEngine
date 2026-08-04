#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

#include "src/runtime/bud.input.hpp"

namespace bud::input {

// Lightweight InputManager that sits on top of the low-level `bud::input::Input` singleton.
// Responsibilities (keyboard & mouse only for now):
//  - per-frame update() to compute edge events (was pressed)
//  - query helpers: is_key_down/was_key_pressed, is_mouse_down/was_mouse_pressed
//  - simple action mapping: bind a named action to one or more keys or mouse buttons
//  - query actions by name (is_action_down / was_action_pressed)
//  - optional callback registration for actions
//
// Design notes:
//  - Only a thin abstraction; all device polling remains in bud::input::Input.
//  - No global state added; users should create and own an InputManager instance (BudEngine will hold one).

class InputManager {
public:
    InputManager() = default;
    ~InputManager() = default;

    // Must be called once per frame (BudEngine will call this at the start of frame)
    void update();

    // Raw queries (wraps bud::input::Input)
    bool is_key_down(Key key) const;
    bool was_key_pressed(Key key) const; // rising edge this frame

    bool is_mouse_down(MouseButton btn) const;
    bool was_mouse_pressed(MouseButton btn) const;

    void get_mouse_delta(float& out_dx, float& out_dy) const;
    float get_mouse_scroll() const;

    // Action mapping API (single-key/mouse binding per action supported; can extend later)
    void bind_key(const std::string& action, Key key);
    void bind_mouse_button(const std::string& action, MouseButton btn);
    void unbind_action(const std::string& action);

    bool is_action_down(const std::string& action) const;
    bool was_action_pressed(const std::string& action) const;

    // Callback registration for convenience (called from update() when action pressed)
    void register_action_callback(const std::string& action, std::function<void()> callback);
    void unregister_action_callback(const std::string& action);

private:
    // internal state snapshots per-frame
    std::unordered_map<Key, bool> prev_keys_;
    std::unordered_map<Key, bool> curr_keys_;

    std::unordered_map<MouseButton, bool> prev_mouse_buttons_;
    std::unordered_map<MouseButton, bool> curr_mouse_buttons_;

    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;
    float scroll_y_ = 0.0f;

    // action bindings
    struct Binding {
        std::vector<Key> keys;
        std::vector<MouseButton> mouse_buttons;
    };

    std::unordered_map<std::string, Binding> bindings_;

    // action callbacks
    std::unordered_map<std::string, std::function<void()>> callbacks_;
};

} // namespace bud::input
