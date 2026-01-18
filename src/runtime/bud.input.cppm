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
			auto it = keys_.find(key);
			return it != keys_.end() && it->second;
		}

		bool is_mouse_button_down(MouseButton btn) const {
			auto it = mouse_buttons_.find(btn);
			return it != mouse_buttons_.end() && it->second;
		}

		void get_mouse_delta(float& x, float& y) const {
			x = mouse_delta_x_;
			y = mouse_delta_y_;
		}

		float get_mouse_scroll() const {
			return scroll_y_;
		}

		template<typename T>
		void internal_new_frame(PassKey<T> pass_key) {
			mouse_delta_x_ = 0.0f;
			mouse_delta_y_ = 0.0f;
			scroll_y_ = 0.0f;
		}

		template<typename T>
		void internal_set_key(PassKey<T> pass_key, Key key, bool is_down) {
			keys_[key] = is_down;
		}

		template<typename T>
		void internal_set_mouse_btn(PassKey<T> pass_key, MouseButton btn, bool is_down) {
			mouse_buttons_[btn] = is_down;
		}

		template<typename T>
		void internal_update_mouse_pos(PassKey<T> pass_key, float x, float y, float dx, float dy) {
			mouse_x_ = x;
			mouse_y_ = y;
			mouse_delta_x_ += dx;
			mouse_delta_y_ += dy;
		}

		template<typename T>
		void internal_update_scroll(PassKey<T> pass_key, float y) {
			scroll_y_ += y;
		}

	private:
		Input() = default;

		std::unordered_map<Key, bool> keys_;
		std::unordered_map<MouseButton, bool> mouse_buttons_;
		float mouse_x_ = 0.0f;
		float mouse_y_ = 0.0f;
		float mouse_delta_x_ = 0.0f;
		float mouse_delta_y_ = 0.0f;
		float scroll_y_ = 0.0f;
	};
}
