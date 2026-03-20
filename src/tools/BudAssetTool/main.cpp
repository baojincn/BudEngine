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
