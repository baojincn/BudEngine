#include "bud.asset.processor.hpp"
#include "src/tools/asset_pipeline/core/support.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

#if defined(__has_include)
# if __has_include(<spirv_reflect.h>)
#  ifndef SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
#   define SPIRV_REFLECT_USE_SYSTEM_SPIRV_H 1
#  endif
#  include <spirv_reflect.h>
#  define BUD_HAVE_SPIRV_REFLECT 1
# endif
#endif

namespace bud::tool {

#if !defined(BUD_HAVE_SPIRV_REFLECT)
bool AssetProcessor::validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path, unsigned int /*max_workers*/) {
    (void)shader_dir;
    (void)report_path;
    std::cerr << "[BudAssetCompiler] SPIRV-Reflect not available in this build." << std::endl;
    return false;
}
#else

static bool compile_shader_with_glslc(const std::filesystem::path& src, const std::filesystem::path& out_spv) {
    std::ostringstream cmd;
    cmd << "glslc \"" << src.string() << "\" -o \"" << out_spv.string() << "\"";
    auto res = asset_pipeline::support::run_process_capture(cmd.str());
    if (!res.stderr_str.empty()) {
        std::filesystem::path log_path = out_spv;
        log_path += ".log";
        asset_pipeline::support::write_text_file_atomic(log_path, res.stderr_str);
    }
    return res.exit_code == 0;
}

static bool reflect_and_validate_spv(const std::filesystem::path& spv_path) {
    std::ifstream in(spv_path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        return false;
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<char> data(size);
    in.read(data.data(), size);
    SpvReflectShaderModule module;
    SpvReflectResult res = spvReflectCreateShaderModule(data.size(), data.data(), &module);
    if (res != SPV_REFLECT_RESULT_SUCCESS)
        return false;
    spvReflectDestroyShaderModule(&module);
    return true;
}

bool AssetProcessor::validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path, unsigned int max_workers) {
    namespace fs = std::filesystem;
    fs::path dir(shader_dir);
    fs::path tmp_dir = std::filesystem::current_path() / "tmp";
    std::error_code tmp_ec;
    fs::create_directories(tmp_dir, tmp_ec);

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::cerr << "[BudAssetCompiler] Shader directory does not exist: " << shader_dir << std::endl;
        return false;
    }

    std::vector<std::string> exts = { ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese" };
    nlohmann::json report_json;
    report_json["shaders"] = nlohmann::json::array();

    std::vector<fs::path> shader_files;
    for (auto const& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
            shader_files.push_back(entry.path());
        }
    }

    unsigned int hc = std::thread::hardware_concurrency();
    unsigned int max_workers_final = max_workers ? max_workers : (hc ? hc : 4);
    std::mutex report_mutex;
    std::atomic<bool> overall_success{ true };
    std::atomic<size_t> next_index{ 0 };

    auto worker = [&]() {
        while (true) {
            size_t idx = next_index.fetch_add(1);
            if (idx >= shader_files.size())
                break;
            const auto& path = shader_files[idx];
            fs::path rel = fs::relative(path, dir);
            fs::path out_spv = tmp_dir / (rel.string() + ".spv");
            fs::create_directories(out_spv.parent_path(), tmp_ec);

            bool compiled = compile_shader_with_glslc(path, out_spv);
            bool reflected = false;
            if (compiled) {
                reflected = reflect_and_validate_spv(out_spv);
            }

            nlohmann::json item;
            item["path"] = path.string();
            item["relative_path"] = rel.string();
            item["compiled"] = compiled;
            item["reflected"] = reflected;

            if (!compiled || !reflected)
                overall_success = false;

            std::lock_guard<std::mutex> lock(report_mutex);
            report_json["shaders"].push_back(item);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(max_workers_final);
    for (unsigned int t = 0; t < max_workers_final; ++t)
        threads.emplace_back(worker);
    for (auto& th : threads)
        th.join();

    if (!report_path.empty()) {
        std::ofstream report_file(report_path);
        if (report_file.is_open()) {
            report_file << report_json.dump(4);
            report_file.close();
        }
    }

    return overall_success.load();
}
#endif

} // namespace bud::tool
