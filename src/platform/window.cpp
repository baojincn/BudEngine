module;

#include <SDL3/SDL.h>
#include <stdexcept>
#include <print>
#include <string>
#include <memory>

module bud.platform;

namespace bud::platform {
	class WindowWin : public Window {
	public:
		WindowWin(const std::string& title, int width, int height)
			: width_(width), height_(height)
		{
			if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
				throw std::runtime_error("Failed to initialize SDL3");
			}

			window_ = SDL_CreateWindow(
				title.c_str(),
				width,
				height,
				SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE
			);

			if (!window_) {
				auto err = SDL_GetError();
				auto msg = std::format("SDL Error: {}", err ? err : "Unknown error");
				std::println(stderr, "CRITICAL FAILURE: {}", msg);

				SDL_Quit();
				throw std::runtime_error("Failed to create SDL window");
			}

			SDL_ShowWindow(window_);
			std::print("Created window: {} ({}x{})\n", title, width, height);
		}

		~WindowWin() override {
			if (window_) {
				SDL_DestroyWindow(window_);
				window_ = nullptr;
			}
			SDL_Quit();
		}

		SDL_Window* get_sdl_window() const override {
			return window_;
		}

		void set_title(const std::string& title) override {
			if (window_) {
				SDL_SetWindowTitle(window_, title.c_str());
			}
		}

		const char* get_title() const override {
			if (window_) {
				return SDL_GetWindowTitle(window_);
			}
			return "";
		}

		void get_size(int& width, int& height) const override {
			width = width_;
			height = height_;
		}

		bool should_close() const override {
			return should_close_;
		}

		void poll_events() override {
			// Reset scroll each frame
			scroll_y_ = 0.0f;
			mouse_delta_x_ = 0.0f;
			mouse_delta_y_ = 0.0f;

			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				switch (event.type) {
				case SDL_EVENT_QUIT:
					should_close_ = true;
					break;
				case SDL_EVENT_KEY_DOWN:
					if (event.key.key == SDLK_ESCAPE) {
						should_close_ = true;
					}
					break;
				case SDL_EVENT_MOUSE_MOTION:
					mouse_delta_x_ += event.motion.xrel;
					mouse_delta_y_ += event.motion.yrel;
					break;

				case SDL_EVENT_MOUSE_WHEEL:
					scroll_y_ += event.wheel.y;
					break;
				default:
					break;
				}
			}
		}

		SDL_Scancode map_key(bud::platform::Key key) const {
			switch (key) {
			case bud::platform::Key::Escape: return SDL_SCANCODE_ESCAPE;
			case bud::platform::Key::Space:  return SDL_SCANCODE_SPACE;
			case bud::platform::Key::R:      return SDL_SCANCODE_R;
			case bud::platform::Key::W:      return SDL_SCANCODE_W;
			case bud::platform::Key::A:      return SDL_SCANCODE_A;
			case bud::platform::Key::S:      return SDL_SCANCODE_S;
			case bud::platform::Key::D:      return SDL_SCANCODE_D;
				// ...
			default: return SDL_SCANCODE_UNKNOWN;
			}
		}


		bool is_key_pressed(bud::platform::Key key) const override {
			auto state = SDL_GetKeyboardState(nullptr);

			if (!state)
				return false;

			SDL_Scancode sdl_code = map_key(key);
			if (sdl_code == SDL_SCANCODE_UNKNOWN)
				return false;

			return state[sdl_code];
		}

		float get_mouse_scroll_y() const override {
			return scroll_y_;
		}

		void get_mouse_delta(float& x, float& y) const override {
			x = mouse_delta_x_;
			y = mouse_delta_y_;
		}

		bool is_mouse_button_down(bud::platform::MouseButton button) const override {
			// 获取全局鼠标状态
			float x, y;
			SDL_MouseButtonFlags flags = SDL_GetMouseState(&x, &y);

			switch (button) {
			case bud::platform::MouseButton::Left:
				return (flags & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0;
			case bud::platform::MouseButton::Right:
				return (flags & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
			case bud::platform::MouseButton::Middle:
				return (flags & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) != 0;
			default: return false;
			}
		}

	private:
		SDL_Window* window_ = nullptr;
		int width_ = 0;
		int height_ = 0;
		bool should_close_ = false;
		float scroll_y_ = 0.0f;
		float mouse_delta_x_ = 0.0f;
		float mouse_delta_y_ = 0.0f;

	};

		std::unique_ptr<Window> create_window(const std::string& title, int width, int height) {
#ifdef _WIN32
			return std::make_unique<WindowWin>(title, width, height);
#else
			throw std::runtime_error("Platform not supported");
#endif
		}
	}
