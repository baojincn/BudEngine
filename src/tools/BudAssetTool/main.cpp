#include <iostream>
#include <string>
#include <vector>
#include "bud.asset.processor.hpp"

void print_usage() {
    std::cout << "Usage: BudAssetTool --input <file.gltf> --output <file.budmesh>" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
    }
    // If --validate-shaders is provided use AssetProcessor shader validation mode
    std::string report_path;
    unsigned int cli_workers = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--validate-shaders" && i + 1 < argc) {
            std::string shader_dir = argv[++i];
            // optional --report <path> and --workers <n>
            for (int j = i+1; j < argc; ++j) {
                std::string a = argv[j];
                if (a == "--report" && j + 1 < argc) {
                    report_path = argv[j+1];
                }
                if ((a == "--workers" || a == "--max-workers") && j + 1 < argc) {
                    try { cli_workers = std::stoul(argv[j+1]); } catch(...) { cli_workers = 0; }
                }
            }
            std::cout << "[BudAssetTool] Validating shaders in: " << shader_dir << std::endl;
            if (bud::tool::AssetProcessor::validate_shaders_in_directory(shader_dir, report_path, cli_workers)) {
                std::cout << "[BudAssetTool] Shader validation succeeded." << std::endl;
                return 0;
            } else {
                std::cerr << "[BudAssetTool] Shader validation failed." << std::endl;
                return 2;
            }
        }
    }

    if (input_path.empty() || output_path.empty()) {
        std::cerr << "[BudAssetTool] Error: Missing input or output path." << std::endl;
        print_usage();
        return 1;
    }

    std::cout << "[BudAssetTool] Processing glTF: " << input_path << " -> " << output_path << std::endl;

    if (bud::tool::AssetProcessor::process_gltf_to_budmesh(input_path, output_path)) {
        std::cout << "[BudAssetTool] Processed successfully." << std::endl;
        return 0;
    } else {
        std::cerr << "[BudAssetTool] Processed failed." << std::endl;
        return 1;
    }
}
