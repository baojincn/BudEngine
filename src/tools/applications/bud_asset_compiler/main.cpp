#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include "bud.asset.processor.hpp"
#include "src/tools/asset_pipeline/builders/mesh_builder.hpp"
#include "src/tools/asset_pipeline/builders/cascade_builder.hpp"

void print_usage() {
    std::cout << "Usage: BudAssetCompiler --input <file.budasset/rawmesh> [options]" << std::endl;
    std::cout << "       --output <file.budasset>          (Optional explicit output path, defaults to in-place update)" << std::endl;
    std::cout << "       --output-dir <dir> --cascade       (Full multi-asset cascade package mode)" << std::endl;
    std::cout << "       --dump-text / --dump-json         (Dump companion .budasset.json inspection file)" << std::endl;
    std::cout << "       --no-cache                        (Disable Derived Data Cache)" << std::endl;
    std::cout << "       --cache-dir <dir>                 (Custom cache directory, default: Cache)" << std::endl;
    std::cout << "       --validate-shaders <dir> [--report <path>]" << std::endl;
    std::cout << "\nNote: BudAssetCompiler compiles/rebuilds from .budasset (with embedded RawMesh) or .rawmesh." << std::endl;
    std::cout << "      Use BudAssetImporter to convert DCC source files (.obj/.fbx/.gltf) to .budasset first." << std::endl;
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    std::string output_dir;
    bool cascade = false;
    bool use_cache = true;
    bool dump_text = false;
    std::string cache_dir = "Cache";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "--cascade") {
            cascade = true;
        } else if (arg == "--no-cache") {
            use_cache = false;
        } else if (arg == "--dump-text" || arg == "--dump-json" || arg == "--text") {
            dump_text = true;
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
    }

    // Shader validation mode
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--validate-shaders" && i + 1 < argc) {
            std::string shader_dir = argv[++i];
            std::string report_path;
            unsigned int cli_workers = 0;
            for (int j = i + 1; j < argc; ++j) {
                std::string a = argv[j];
                if (a == "--report" && j + 1 < argc) {
                    report_path = argv[j + 1];
                }
                if ((a == "--workers" || a == "--max-workers") && j + 1 < argc) {
                    try { cli_workers = std::stoul(argv[j + 1]); } catch (...) { cli_workers = 0; }
                }
            }
            std::cout << "[BudAssetCompiler] Validating shaders in: " << shader_dir << std::endl;
            if (bud::tool::AssetProcessor::validate_shaders_in_directory(shader_dir, report_path, cli_workers)) {
                std::cout << "[BudAssetCompiler] Shader validation succeeded." << std::endl;
                return 0;
            } else {
                std::cerr << "[BudAssetCompiler] Shader validation failed." << std::endl;
                return 2;
            }
        }
    }

    if (input_path.empty()) {
        std::cerr << "[BudAssetCompiler] Error: --input <file.budasset/rawmesh> is required." << std::endl;
        print_usage();
        return 1;
    }

    if (cascade) {
        if (output_dir.empty()) {
            output_dir = "Content/DefaultPackage";
        }
        std::cout << "[BudAssetCompiler] Cascade Build: " << input_path << " -> " << output_dir << std::endl;

        bud::asset_pipeline::CascadeBuildOptions cascade_opts{};
        cascade_opts.dump_text = dump_text;
        cascade_opts.use_cache = use_cache;
        cascade_opts.cache_root = cache_dir;

        if (bud::asset_pipeline::CascadeBuilder::build_package(input_path, output_dir, cascade_opts)) {
            std::cout << "[BudAssetCompiler] Cascade package built successfully!" << std::endl;
            return 0;
        } else {
            std::cerr << "[BudAssetCompiler] Cascade package build failed." << std::endl;
            return 1;
        }
    }

    if (output_path.empty()) {
        output_path = input_path;
    }

    bud::asset_pipeline::MeshBuildOptions options{};
    options.enable_virtual_geometry = true;
    options.use_cache = use_cache;
    options.dump_text = dump_text;
    options.cache_root = cache_dir;

    if (bud::asset_pipeline::MeshBuilder::build(input_path, output_path, options)) {
        std::cout << "[BudAssetCompiler] Cooked .budasset successfully." << std::endl;
        return 0;
    } else {
        std::cerr << "[BudAssetCompiler] Cooked .budasset failed." << std::endl;
        return 1;
    }
}
