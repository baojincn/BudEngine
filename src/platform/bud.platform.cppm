module;

#include <string>
#include <memory>
#include <SDL3/SDL.h>

export module bud.platform;
import bud.input;

export namespace bud::platform {

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

    std::unique_ptr<Window> create_window(const std::string& title, int width, int height);
}
