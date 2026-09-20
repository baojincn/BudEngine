#include <pybind11/pybind11.h>
#include "gym_env.hpp"

using namespace bud::game;
using namespace bud::application;
namespace py = pybind11;

PYBIND11_MODULE(bud_rl, m) {
	m.doc() = "BudEngine Gym RL Bridge";

	py::class_<AppConfig>(m, "AppConfig")
		.def(py::init<>())
		.def_readwrite("scene_file", &AppConfig::scene_file)
		.def_readwrite("window_title", &AppConfig::window_title)
		.def_readwrite("width", &AppConfig::width)
		.def_readwrite("height", &AppConfig::height)
		.def_readwrite("is_puppet_mode", &AppConfig::is_puppet_mode)
		.def_readwrite("is_headless", &AppConfig::is_headless);

	py::class_<GymEnvApp, std::shared_ptr<GymEnvApp>>(m, "GymEnv")
		.def_static("create", [](const AppConfig& cfg) {
			auto inst = std::make_shared<GymEnvApp>();
			inst->init_puppet(cfg);
			return inst;
		}, py::arg("app_config") = AppConfig())
		.def("step", &GymEnvApp::step, py::arg("dt") = 0.016f)
		.def("reset", &GymEnvApp::reset)
		.def("is_fully_loaded", &GymEnvApp::is_fully_loaded);
}
