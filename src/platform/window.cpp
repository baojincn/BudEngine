#include <SDL3/SDL.h>
#include <stdexcept>
#include "src/platform/bud.platform.hpp"
#include "src/core/bud.core.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <print>
#include <string>
#include <memory>
#include <cmath>

#include <imgui_impl_sdl3.h>

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
		case SDLK_Q:      return bud::input::Key::Q;
		case SDLK_E:      return bud::input::Key::E;
		case SDLK_LSHIFT: return bud::input::Key::LShift;
		case SDLK_V:      return bud::input::Key::V;
		case SDLK_TAB:    return bud::input::Key::Tab;
		case SDLK_F1:     return bud::input::Key::F1;
		case SDLK_F2:     return bud::input::Key::F2;
		case SDLK_F3:     return bud::input::Key::F3;
		case SDLK_F4:     return bud::input::Key::F4;
		case SDLK_F5:     return bud::input::Key::F5;
		case SDLK_F6:     return bud::input::Key::F6;
		case SDLK_F7:     return bud::input::Key::F7;
		case SDLK_F8:     return bud::input::Key::F8;
		case SDLK_F9:     return bud::input::Key::F9;
		case SDLK_F10:    return bud::input::Key::F10;
		case SDLK_F11:    return bud::input::Key::F11;
		case SDLK_F12:    return bud::input::Key::F12;
		case SDLK_LCTRL:  return bud::input::Key::LCtrl;
		case SDLK_EQUALS: return bud::input::Key::Plus;
		case SDLK_KP_PLUS:return bud::input::Key::Plus;
		case SDLK_MINUS:  return bud::input::Key::Minus;
		case SDLK_KP_MINUS:return bud::input::Key::Minus;
		case SDLK_0:      return bud::input::Key::Num0;
		case SDLK_1:      return bud::input::Key::Num1;
		case SDLK_2:      return bud::input::Key::Num2;
		case SDLK_3:      return bud::input::Key::Num3;
		case SDLK_4:      return bud::input::Key::Num4;
		case SDLK_5:      return bud::input::Key::Num5;
		case SDLK_6:      return bud::input::Key::Num6;
		case SDLK_7:      return bud::input::Key::Num7;
		case SDLK_8:      return bud::input::Key::Num8;
		case SDLK_9:      return bud::input::Key::Num9;
		default:          return bud::input::Key::Unknown;
		}
	}

	bud::input::GamepadButton sdl_to_bud_gamepad_button(uint8_t button) {
		switch (button) {
		case SDL_GAMEPAD_BUTTON_SOUTH:          return bud::input::GamepadButton::A;
		case SDL_GAMEPAD_BUTTON_EAST:           return bud::input::GamepadButton::B;
		case SDL_GAMEPAD_BUTTON_WEST:           return bud::input::GamepadButton::X;
		case SDL_GAMEPAD_BUTTON_NORTH:          return bud::input::GamepadButton::Y;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return bud::input::GamepadButton::LB;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return bud::input::GamepadButton::RB;
		case SDL_GAMEPAD_BUTTON_START:          return bud::input::GamepadButton::Start;
		case SDL_GAMEPAD_BUTTON_BACK:           return bud::input::GamepadButton::Back;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return bud::input::GamepadButton::LS;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return bud::input::GamepadButton::RS;
		case SDL_GAMEPAD_BUTTON_DPAD_UP:        return bud::input::GamepadButton::DPadUp;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return bud::input::GamepadButton::DPadDown;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return bud::input::GamepadButton::DPadLeft;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return bud::input::GamepadButton::DPadRight;
		default: return bud::input::GamepadButton::A;
		}
	}

	bud::input::GamepadAxis sdl_to_bud_gamepad_axis(uint8_t axis) {
		switch (axis) {
		case SDL_GAMEPAD_AXIS_LEFTX:        return bud::input::GamepadAxis::LeftX;
		case SDL_GAMEPAD_AXIS_LEFTY:        return bud::input::GamepadAxis::LeftY;
		case SDL_GAMEPAD_AXIS_RIGHTX:       return bud::input::GamepadAxis::RightX;
		case SDL_GAMEPAD_AXIS_RIGHTY:       return bud::input::GamepadAxis::RightY;
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return bud::input::GamepadAxis::LeftTrigger;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:return bud::input::GamepadAxis::RightTrigger;
		default: return bud::input::GamepadAxis::LeftX;
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
		WindowWin(const std::string& title, int width, int height, WindowFlags flags)
			: width(width), height(height)
		{
			if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
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
				bud::eprint("CRITICAL FAILURE: {}", msg);

				SDL_Quit();
				throw std::runtime_error("Failed to create SDL window");
			}

			if ((static_cast<uint32_t>(flags) & static_cast<uint32_t>(WindowFlags::Hidden)) == 0) {
				SDL_ShowWindow(window);
			}

			update_window_size();
			bud::print("Created window: {} ({}x{}){}", title, width, height,
				(static_cast<uint32_t>(flags) & static_cast<uint32_t>(WindowFlags::Hidden)) ? " [HIDDEN]" : "");
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
			if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
				width_out = 0;
				height_out = 0;
				return;
			}
			width_out = width;
			height_out = height;
		}

		void get_size_in_pixels(int& width_out, int& height_out) const override {
			if (!window || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
				width_out = 0;
				height_out = 0;
				return;
			}

			if (!SDL_GetWindowSizeInPixels(window, &width_out, &height_out)) {
				SDL_GetWindowSize(window, &width_out, &height_out);
			}
		}

		bool should_close() const override {
			return close_requested;
		}

		void set_mouse_relative_mode(bool enabled) override {
			if (window)
				SDL_SetWindowRelativeMouseMode(window, enabled);
		}

		void set_cursor_visible(bool visible) override {
			SDL_ShowCursor();
			if (visible)
				SDL_ShowCursor();
			else
				SDL_HideCursor();
		}

		void poll_events() override {
			auto& input = bud::input::Input::get();

			auto pass_key = create_pass_key();

			input.internal_new_frame(pass_key);

			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				ImGui_ImplSDL3_ProcessEvent(&event);

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

				case SDL_EVENT_GAMEPAD_ADDED:
					SDL_OpenGamepad(event.gdevice.which);
					input.internal_set_gamepad_connected(pass_key, true);
					bud::print("[Input] Gamepad connected (id={})", event.gdevice.which);
					break;

				case SDL_EVENT_GAMEPAD_REMOVED:
					bud::print("[Input] Gamepad disconnected (id={})", event.gdevice.which);
					input.internal_set_gamepad_connected(pass_key, false);
					break;

				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
					input.internal_set_gamepad_button(pass_key,
						sdl_to_bud_gamepad_button(event.gbutton.button), true);
					break;

				case SDL_EVENT_GAMEPAD_BUTTON_UP:
					input.internal_set_gamepad_button(pass_key,
						sdl_to_bud_gamepad_button(event.gbutton.button), false);
					break;

				case SDL_EVENT_GAMEPAD_AXIS_MOTION:
				{
					float norm = static_cast<float>(event.gaxis.value) / 32767.0f;
					norm = std::max(-1.0f, std::min(1.0f, norm));
					if (std::abs(norm) < 0.1f) norm = 0.0f;
					input.internal_set_gamepad_axis(pass_key,
						sdl_to_bud_gamepad_axis(event.gaxis.axis), norm);
				}
				break;
				}
			}
		}

		void create_surface(VkInstance instance, VkSurfaceKHR& out_surface) const override {
			if (!window) {
				out_surface = nullptr;
				return;
			}

            if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &out_surface)) {
                out_surface = nullptr;
                auto err = SDL_GetError();
                bud::eprint("Window::create_surface: SDL_Vulkan_CreateSurface failed: {}", err ? err : "Unknown");
#if defined(_DEBUG)
                throw std::runtime_error("Failed to create Vulkan surface");
#else
                // In Release, fail gracefully and let the caller handle the missing surface
                return;
#endif
            }
		}

	private:
		void update_window_size() {
			if (!window)
				return;

			int w = 0;
			int h = 0;
			if (!SDL_GetWindowSizeInPixels(window, &w, &h)) {
				SDL_GetWindowSize(window, &w, &h);
			}
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

	std::unique_ptr<Window> create_window(const std::string& title, int width, int height, WindowFlags flags) {
#ifdef _WIN32
		return std::make_unique<WindowWin>(title, width, height, flags);
#else
		throw std::runtime_error("Platform not supported");
#endif
	}
}