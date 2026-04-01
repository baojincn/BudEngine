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
    // Meshlet generation defaults
    size_t max_vertices = 64;
    size_t max_triangles = 128;
    float cone_weight = 0.5f;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--max-vertices" && i + 1 < argc) {
            try { max_vertices = std::stoul(argv[++i]); } catch(...) { max_vertices = 64; }
        } else if (arg == "--max-triangles" && i + 1 < argc) {
            try { max_triangles = std::stoul(argv[++i]); } catch(...) { max_triangles = 128; }
        } else if (arg == "--cone-weight" && i + 1 < argc) {
            try { cone_weight = std::stof(argv[++i]); } catch(...) { cone_weight = 0.5f; }
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
    std::cout << "[BudAssetTool] Meshlet params: max_vertices=" << max_vertices << " max_triangles=" << max_triangles << " cone_weight=" << cone_weight << std::endl;

    if (bud::tool::AssetProcessor::process_gltf_to_budmesh(input_path, output_path, max_vertices, max_triangles, cone_weight)) {
        std::cout << "[BudAssetTool] Processed successfully." << std::endl;
        return 0;
    } else {
        std::cerr << "[BudAssetTool] Processed failed." << std::endl;
        return 1;
    }
}
