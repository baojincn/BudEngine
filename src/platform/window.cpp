#include <SDL3/SDL.h>
#include <stdexcept>
#include "src/platform/bud.platform.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <print>
#include <string>
#include <memory>

#include "src/runtime/bud.input.hpp"

namespace bud::platform {

	bud::input::Key sdl_to_bud_key(SDL_Keycode key) {
		switch (key) {
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

	static void ensure_video_initialized() {
		if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
			if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
				auto err = SDL_GetError();
				auto msg = std::format("SDL Error: {}", err ? err : "Unknown error");
				throw std::runtime_error(msg);
			}
		}
	}

	static SDL_DisplayID get_primary_display_id() {
		ensure_video_initialized();

		int display_count = 0;
		auto displays = SDL_GetDisplays(&display_count);
		if (!displays || display_count == 0) {
			if (displays) SDL_free(displays);
			return 0;
		}

		SDL_DisplayID display = displays[0];
		SDL_free(displays);
		return display;
	}

	static ScreenResolution get_display_resolution(SDL_DisplayID display) {
		ensure_video_initialized();

		SDL_DisplayMode mode{};
		const SDL_DisplayMode* currentMode = SDL_GetCurrentDisplayMode(display);
		if (display == 0 || currentMode == nullptr) {
			return ScreenResolution{};
		}

		return ScreenResolution{ currentMode->w, currentMode->h, currentMode->refresh_rate };
	}

	class WindowWin : public Window {
	public:
		WindowWin(const std::string& title, int width, int height)
			: width(width), height(height)
		{
			if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
				throw std::runtime_error("Failed to initialize SDL3");
			}

			window = SDL_CreateWindow(
				title.c_str(),
				width,
				height,
				SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE
			);

			if (!window) {
				auto err = SDL_GetError();
				auto msg = std::format("SDL Error: {}", err ? err : "Unknown error");
				std::println(stderr, "CRITICAL FAILURE: {}", msg);

				SDL_Quit();
				throw std::runtime_error("Failed to create SDL window");
			}

			SDL_ShowWindow(window);
			update_window_size();
			std::print("Created window: {} ({}x{})\n", title, width, height);
		}

		~WindowWin() override {
			if (window) {
				SDL_DestroyWindow(window);
				window = nullptr;
			}
			SDL_Quit();
		}

		SDL_Window* get_sdl_window() const override {
			return window;
		}

		void set_title(const std::string& title) override {
			if (window) {
				SDL_SetWindowTitle(window, title.c_str());
			}
		}

		const char* get_title() const override {
			if (window) {
				return SDL_GetWindowTitle(window);
			}
			return "";
		}

		void get_size(int& width_out, int& height_out) const override {
			width_out = width;
			height_out = height;
		}

		void get_size_in_pixels(int& width_out, int& height_out) const override {
			if (!window) {
				width_out = 0;
				height_out = 0;
				return;
			}

			if (SDL_GetWindowSizeInPixels(window, &width_out, &height_out) == 0) {
				return;
			}

			SDL_GetWindowSize(window, &width_out, &height_out);
		}

		bool should_close() const override {
			return close_requested;
		}

		void poll_events() override {
			auto& input = bud::input::Input::get();

			auto pass_key = create_pass_key();

			input.internal_new_frame(pass_key);

			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				switch (event.type) {
				case SDL_EVENT_QUIT:
					close_requested = true;
					break;

				case SDL_EVENT_WINDOW_RESIZED:
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
					update_window_size();
					break;

				case SDL_EVENT_KEY_DOWN:
					if (event.key.key == SDLK_ESCAPE)
						close_requested = true;
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

		void create_surface(VkInstance instance, VkSurfaceKHR& out_surface) const override {
			//if (!window) {
			//	out_surface = nullptr;
			//	return;
			//}

			if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &out_surface)) {
				out_surface = nullptr;
				throw std::runtime_error("Failed to create Vulkan surface");
			}
		}

	private:
		void update_window_size() {
			if (!window) {
				return;
			}

			int w = 0;
			int h = 0;
			if (SDL_GetWindowSizeInPixels(window, &w, &h) == 0) {
				width = w;
				height = h;
				return;
			}

			SDL_GetWindowSize(window, &w, &h);
			width = w;
			height = h;
		}

		SDL_Window* window = nullptr;
		int width = 0;
		int height = 0;
		bool close_requested = false;

	};


	ScreenResolution get_current_screen_resolution() {
		return get_display_resolution(get_primary_display_id());
	}

	ScreenResolution get_window_screen_resolution(const Window& window) {
		auto sdl_window = window.get_sdl_window();
		auto display = sdl_window ? SDL_GetDisplayForWindow(sdl_window) : get_primary_display_id();
		if (display == 0) {
			return ScreenResolution{};
		}
		return get_display_resolution(display);
	}

	std::unique_ptr<Window> create_window(const std::string& title, int width, int height) {
#ifdef _WIN32
		return std::make_unique<WindowWin>(title, width, height);
#else
		throw std::runtime_error("Platform not supported");
#endif
	}
} // namespace bud::platform
