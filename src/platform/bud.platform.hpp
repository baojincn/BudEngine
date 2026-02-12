#pragma once

#include <string>
#include <memory>
#include <SDL3/SDL.h>

#include "src/runtime/bud.input.hpp"

namespace bud::platform {

	struct ScreenResolution {
		int width = 0;
		int height = 0;
		float refresh_rate = 0.0f;
	};

    class Window {
    public:
        virtual ~Window() = default;

		virtual SDL_Window* get_sdl_window() const = 0;
        virtual void get_size(int& width, int& height) const = 0;
        virtual bool should_close() const = 0;
        virtual void poll_events() = 0;

		virtual void set_title(const std::string& title) = 0;
		virtual const char* get_title() const = 0;

        int get_width() const {
            int w, h;
            get_size(w, h);
            return w;
        }
        int get_height() const {
            int w, h;
            get_size(w, h);
            return h;
        }

	protected:
		auto create_pass_key() const {
			return bud::input::PassKey<Window>{};
		}
    };

	ScreenResolution get_current_screen_resolution();
	ScreenResolution get_window_screen_resolution(const Window& window);

    std::unique_ptr<Window> create_window(const std::string& title, int width, int height);
}
