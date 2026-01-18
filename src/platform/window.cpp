module;

#include <SDL3/SDL.h>
#include <stdexcept>
#include <print>
#include <string>
#include <memory>

module bud.platform;

namespace bud::platform {

	bud::input::Key sdl_to_bud_key(SDL_Keycode sdl_key) {
		switch (sdl_key) {
		case SDLK_ESCAPE: return bud::input::Key::Escape;
		case SDLK_SPACE:  return bud::input::Key::Space;
		case SDLK_RETURN: return bud::input::Key::Enter;
		case SDLK_W:      return bud::input::Key::W;
		case SDLK_A:      return bud::input::Key::A;
		case SDLK_S:      return bud::input::Key::S;
		case SDLK_D:      return bud::input::Key::D;
		case SDLK_R:      return bud::input::Key::R;

		default:          return bud::input::Key::Unknown;
		}
	}

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
			auto& input = bud::input::Input::get();

			auto pass_key = create_pass_key();

			input.internal_new_frame(pass_key);

			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				switch (event.type) {
				case SDL_EVENT_QUIT:
					should_close_ = true;
					break;

				case SDL_EVENT_KEY_DOWN:
					if (event.key.key == SDLK_ESCAPE)
						should_close_ = true;
					input.internal_set_key(pass_key, sdl_to_bud_key(event.key.key), true);
					break;

				case SDL_EVENT_KEY_UP:
					input.internal_set_key(pass_key, sdl_to_bud_key(event.key.key), false);
					break;

				case SDL_EVENT_MOUSE_MOTION:
					input.internal_update_mouse_pos(
						pass_key,
						event.motion.x,
						event.motion.y,
						event.motion.xrel,
						event.motion.yrel
					);
					break;

				case SDL_EVENT_MOUSE_WHEEL:
					input.internal_update_scroll(pass_key, event.wheel.y);
					break;

				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
				{
					auto is_down = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
					auto btn = bud::input::MouseButton::Left;

					if (event.button.button == SDL_BUTTON_RIGHT)
						btn = bud::input::MouseButton::Right;
					else if (event.button.button == SDL_BUTTON_MIDDLE)
						btn = bud::input::MouseButton::Middle;

					input.internal_set_mouse_btn(pass_key, btn, is_down);
				}
				break;
				}
			}
		}


	private:
		SDL_Window* window_ = nullptr;
		int width_ = 0;
		int height_ = 0;
		bool should_close_ = false;

	};

	std::unique_ptr<Window> create_window(const std::string& title, int width, int height) {
#ifdef _WIN32
		return std::make_unique<WindowWin>(title, width, height);
#else
		throw std::runtime_error("Platform not supported");
#endif
	}
}
