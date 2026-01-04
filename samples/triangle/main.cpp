#include <print>
#include <exception>

import bud.engine;

int main(int argc, char* argv[]) {
    try {
        bud::engine::BudEngine engine("Bud Engine - Triangle Sample", 1920, 1080);

        engine.run();

    }
    catch (const std::exception& e) {
        std::println(stderr, "Fatal Error: {}", e.what());
        return -1;
    }
    catch (...) {
        std::println(stderr, "Unknown Error occurred.");
        return -1;
    }

    std::println("Engine shutdown gracefully.");
    return 0;
}
