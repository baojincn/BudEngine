module;

#include <string>
#include <memory>

export module bud.platform;

export namespace bud::platform {

    class Window {
    public:
        virtual ~Window() = default;

        virtual void get_size(int& width, int& height) const = 0;
        virtual bool should_close() const = 0;
        virtual void poll_events() = 0;

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
    };

    std::unique_ptr<Window> create_window(const std::string& title, int width, int height);
}
