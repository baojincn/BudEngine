#pragma once

#include <unordered_map>

namespace bud::platform {
	class Window;
}

namespace bud::input {

	 enum class Key {
		Unknown = 0,
		Escape,
		Space,
		Enter,
		W,
		A,
		S,
		D,
		R,
		Q,
		E,
		LShift,
		V,
		Tab,
		F1,
		F2,
		F3,
		F4,
		F5,
		F6,
		F7,
		F8,
		F9,
		F10,
		F11,
		F12,
		LCtrl,
		Plus,
		Minus,
		Num0,
		Num1,
		Num2,
		Num3,
		Num4,
		Num5,
		Num6,
		Num7,
		Num8,
		Num9,
	};

	 enum class MouseButton {
		Left,
		Right,
		Middle
	};

	enum class GamepadButton {
		A,
		B,
		X,
		Y,
		LB,
		RB,
		Start,
		Back,
		LS,
		RS,
		DPadUp,
		DPadDown,
		DPadLeft,
		DPadRight,
	};

	enum class GamepadAxis {
		LeftX,
		LeftY,
		RightX,
		RightY,
		LeftTrigger,
		RightTrigger,
	};

	static constexpr int GAMEPAD_AXIS_COUNT = 6;

	 template<typename T>
	class PassKey {
	private:
		friend T;
		PassKey() = default;
	public:
		PassKey(const PassKey&) = default;
	};

	 class Input {
	public:
		static Input& get();

		Input(const Input&) = delete;
		Input& operator=(const Input&) = delete;

		bool is_key_down(Key key) const;
		bool is_mouse_button_down(MouseButton btn) const;
		void get_mouse_delta(float& x, float& y) const;
		float get_mouse_scroll() const;

		bool is_gamepad_connected() const;
		bool is_gamepad_button_down(GamepadButton btn) const;
		float get_gamepad_axis(GamepadAxis axis) const;
		float get_gamepad_axis_delta(GamepadAxis axis) const;

		template<typename T>
		void internal_new_frame(PassKey<T> pass_key) {
			(void)pass_key;
			mouse_delta_x = 0.0f;
			mouse_delta_y = 0.0f;
			scroll_y = 0.0f;
			for (int i = 0; i < GAMEPAD_AXIS_COUNT; ++i)
				gamepad_axis_prev[i] = gamepad_axis_values[i];
		}

		template<typename T>
		void internal_set_key(PassKey<T> pass_key, Key key, bool is_down) {
			(void)pass_key;
			keys[key] = is_down;
		}

		template<typename T>
		void internal_set_mouse_btn(PassKey<T> pass_key, MouseButton btn, bool is_down) {
			(void)pass_key;
			mouse_buttons[btn] = is_down;
		}

		template<typename T>
		void internal_update_mouse_pos(PassKey<T> pass_key, float x, float y, float dx, float dy) {
			(void)pass_key;
			mouse_x = x;
			mouse_y = y;
			mouse_delta_x += dx;
			mouse_delta_y += dy;
		}

		template<typename T>
		void internal_update_scroll(PassKey<T> pass_key, float y) {
			(void)pass_key;
			scroll_y += y;
		}

		template<typename T>
		void internal_set_gamepad_connected(PassKey<T> pass_key, bool connected) {
			(void)pass_key;
			gamepad_connected = connected;
		}

		template<typename T>
		void internal_set_gamepad_button(PassKey<T> pass_key, GamepadButton btn, bool is_down) {
			(void)pass_key;
			gamepad_buttons[btn] = is_down;
		}

		template<typename T>
		void internal_set_gamepad_axis(PassKey<T> pass_key, GamepadAxis axis, float value) {
			(void)pass_key;
			gamepad_axis_values[static_cast<int>(axis)] = value;
		}

	private:
		Input() = default;

		std::unordered_map<Key, bool> keys;
		std::unordered_map<MouseButton, bool> mouse_buttons;
		float mouse_x = 0.0f;
		float mouse_y = 0.0f;
		float mouse_delta_x = 0.0f;
		float mouse_delta_y = 0.0f;
		float scroll_y = 0.0f;

		bool gamepad_connected = false;
		std::unordered_map<GamepadButton, bool> gamepad_buttons;
		float gamepad_axis_values[GAMEPAD_AXIS_COUNT] = {};
		float gamepad_axis_prev[GAMEPAD_AXIS_COUNT] = {};
	};

}