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

		void get_size(int& width, int& height) const override {
			width = width_;
			height = height_;
		}

		bool should_close() const override {
			return should_close_;
		}

		void poll_events() override {
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
