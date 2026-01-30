module;
#include <unordered_map>

export module bud.input;

namespace bud::platform {
	class Window;
}

export namespace bud::input {

	export enum class Key {
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
		E
	};

	export enum class MouseButton {
		Left,
		Right,
		Middle
	};

	export template<typename T>
	class PassKey {
	private:
		friend T;
		PassKey() = default;
	public:
		PassKey(const PassKey&) = default;
	};

	export class Input {
	public:
		static Input& get() {
			static Input instance;
			return instance;
		}

		Input(const Input&) = delete;
		Input& operator=(const Input&) = delete;

		bool is_key_down(Key key) const {
			auto it = keys.find(key);
			return it != keys.end() && it->second;
		}

		bool is_mouse_button_down(MouseButton btn) const {
			auto it = mouse_buttons.find(btn);
			return it != mouse_buttons.end() && it->second;
		}

		void get_mouse_delta(float& x, float& y) const {
			x = mouse_delta_x;
			y = mouse_delta_y;
		}

		float get_mouse_scroll() const {
			return scroll_y;
		}

		template<typename T>
		void internal_new_frame(PassKey<T> pass_key) {
			mouse_delta_x = 0.0f;
			mouse_delta_y = 0.0f;
			scroll_y = 0.0f;
		}

		template<typename T>
		void internal_set_key(PassKey<T> pass_key, Key key, bool is_down) {
			keys[key] = is_down;
		}

		template<typename T>
		void internal_set_mouse_btn(PassKey<T> pass_key, MouseButton btn, bool is_down) {
			mouse_buttons[btn] = is_down;
		}

		template<typename T>
		void internal_update_mouse_pos(PassKey<T> pass_key, float x, float y, float dx, float dy) {
			mouse_x = x;
			mouse_y = y;
			mouse_delta_x += dx;
			mouse_delta_y += dy;
		}

		template<typename T>
		void internal_update_scroll(PassKey<T> pass_key, float y) {
			scroll_y += y;
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
	};
}
