#include "samples/triangle/triangle.hpp"


using namespace bud::game;
namespace py = pybind11;

PYBIND11_MODULE(bud_rl, m) {
	m.doc() = "BudEngine Pybind11 RL Bridge";

	py::class_<AppConfig>(m, "AppConfig")
		.def(py::init<>())
		.def_readwrite("scene_file", &AppConfig::scene_file)
		.def_readwrite("window_title", &AppConfig::window_title)
		.def_readwrite("width", &AppConfig::width)
		.def_readwrite("height", &AppConfig::height)
		.def_readwrite("is_puppet_mode", &AppConfig::is_puppet_mode)
		.def_readwrite("is_headless", &AppConfig::is_headless);

	py::class_<TriangleApp, std::shared_ptr<TriangleApp>>(m, "TriangleApp")
		.def_static("create", [](const AppConfig& cfg) {
			auto inst = std::make_shared<TriangleApp>();
			inst->on_init(cfg);
			return inst;
		}, py::arg("app_config") = AppConfig())
		.def("step", &TriangleApp::step, py::arg("dt") = 0.016f)
		.def("is_fully_loaded", &TriangleApp::is_fully_loaded);
}
