#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include "src/runtime/bud.game.hpp"

class TriangleApp : public bud::game::GameFramework {
private:
	std::shared_ptr<std::atomic<int>> pending_mesh_loads = std::make_shared<std::atomic<int>>(1); // Block until json load completes
public:
	bool is_fully_loaded() const override;

	void on_init(const bud::game::AppConfig& config) override;

	void on_update(float delta_time) override;

	void on_shutdown() override;

	void init_environment(const bud::game::AppConfig& config) {
		init_puppet(config);
	}

	pybind11::array_t<uint8_t> step(float dt);
};
