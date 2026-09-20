#include <iostream>
#include <string>
#include <filesystem>
#include <algorithm>
#include "src/tools/asset_pipeline/cache/asset_cache.hpp"
#include "src/tools/asset_pipeline/cache/asset_registry.hpp"
#include "src/tools/asset_pipeline/builders/cascade_builder.hpp"
#include "src/tools/asset_pipeline/builders/texture_builder.hpp"
#include "src/tools/asset_pipeline/importers/obj_importer.hpp"
#include "src/tools/asset_pipeline/importers/gltf_importer.hpp"
#include "src/tools/asset_pipeline/importers/fbx_importer.hpp"
#include "src/tools/asset_pipeline/importers/dae_importer.hpp"
#include "src/tools/asset_pipeline/importers/stl_importer.hpp"
#include "src/tools/asset_pipeline/importers/texture_importer.hpp"
#include "src/tools/asset_pipeline/builders/urdf_builder.hpp"
#include "src/tools/asset_pipeline/builders/mjcf_builder.hpp"
#include "src/tools/asset_pipeline/core/support.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

void print_usage() {
    std::cout << "Usage: BudAssetImporter --input <file> --output <dir/path> [options]" << std::endl;
    std::cout << "       --input <file.obj/gltf/fbx/dae/stl/urdf/xml/mjcf/png> (Required: Input source file)" << std::endl;
    std::cout << "       --output <path>               (Required: Output directory or target file)" << std::endl;
    std::cout << "       --package-root <dir>          (ROS package root for URDF, auto-detected from input path if omitted)" << std::endl;
    std::cout << "       --max-convex-verts <count>    (Max vertices per convex hull, default: 128)" << std::endl;
    std::cout << "       --scene <path>                (Optional: Append imported model to an existing scene file created via SceneTool)" << std::endl;
    std::cout << "       --target-scene <path>         (Alias for --scene)" << std::endl;
    std::cout << "       --entity-prefix <prefix>      (Prefix for appended scene entity names)" << std::endl;
    std::cout << "       --scale <factor>              (Uniform scale factor)" << std::endl;
    std::cout << "       --cache-dir <dir>             (Custom cache directory, default: Cache)" << std::endl;
    std::cout << "       --dump-text                   (Dump JSON debug metadata)" << std::endl;
}

int main(int argc, char* argv[]) {
    JPH::RegisterDefaultAllocator();
    if (!JPH::Factory::sInstance) {
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    std::string input_path;
    std::string output_target;
    std::string package_root;
    std::string cache_dir = "Cache";
    std::string target_scene_path;
    std::string entity_prefix;
    bool dump_text = false;
    bool use_cache = true;
    int max_convex_verts = 128;
    float scale = 0.0f; // 0.0f = auto-detect unit scale based on metadata/bounds, converts to meters

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_target = argv[++i];
        } else if (arg == "--package-root" && i + 1 < argc) {
            package_root = argv[++i];
        } else if (arg == "--max-convex-verts" && i + 1 < argc) {
            max_convex_verts = std::stoi(argv[++i]);
        } else if ((arg == "--scene" || arg == "--target-scene") && i + 1 < argc) {
            target_scene_path = argv[++i];
        } else if (arg == "--entity-prefix" && i + 1 < argc) {
            entity_prefix = argv[++i];
        } else if (arg == "--scale" && i + 1 < argc) {
            scale = std::stof(argv[++i]);
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

    if (input_path.empty()) {
        std::cerr << "[BudAssetImporter] Error: --input is required." << std::endl;
        print_usage();
        return 1;
    }

    if (output_target.empty()) {
        std::cerr << "[BudAssetImporter] Error: --output is required." << std::endl;
        print_usage();
        return 1;
    }

    bud::asset_pipeline::AssetCache::init(cache_dir);
    bud::asset_pipeline::AssetRegistry::init(cache_dir + "/Registry/asset_registry.bin");

    std::filesystem::path p(input_path);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // 1. Texture Image Import (Single texture asset)
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".dds" || ext == ".webp") {
        std::string output_path = output_target;
        std::filesystem::path out_p(output_target);
        if (!out_p.has_extension())
            output_path = (out_p / (stem + ".budasset")).string();

        bud::asset_pipeline::TextureBuildOptions tex_opts{};
        tex_opts.cache_root = cache_dir;
        tex_opts.use_cache = use_cache;

        if (bud::asset_pipeline::TextureBuilder::build_from_file(input_path, output_path, tex_opts)) {
            std::cout << "[BudAssetImporter] Successfully imported texture: " << input_path << " -> " << output_path << std::endl;

            bud::asset_pipeline::AssetRegistryEntry entry;
            entry.asset_path = output_path;
            entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Texture);
            std::hash<std::string> hasher;
            entry.asset_id = hasher(input_path);
            bud::asset_pipeline::AssetRegistry::register_asset(entry);
            bud::asset_pipeline::AssetRegistry::save();
            return 0;
        } else {
            std::cerr << "[BudAssetImporter] Failed to import texture: " << input_path << std::endl;
            return 1;
        }
    }

    std::string output_dir = output_target;
    std::filesystem::path out_p(output_target);
    if (out_p.has_extension()) {
        std::string parent_name = out_p.parent_path().filename().string();
        if (parent_name == "Meshes" || parent_name == "meshes")
            output_dir = out_p.parent_path().parent_path().string();
        else
            output_dir = out_p.parent_path().string();
    }

    bud::asset_pipeline::CascadeBuildOptions cascade_opts{};
    cascade_opts.cache_root = cache_dir;
    cascade_opts.use_cache = use_cache;
    cascade_opts.dump_text = dump_text;
    cascade_opts.scale = scale;
    cascade_opts.target_scene_path = target_scene_path;
    cascade_opts.entity_prefix = entity_prefix;
    cascade_opts.scene_mode = target_scene_path.empty() ? bud::asset_pipeline::SceneImportMode::None : bud::asset_pipeline::SceneImportMode::Append;

    // 2. Scene Import with 1:1 Directory Mirroring (glTF / glb / DAE)
    if (ext == ".gltf" || ext == ".glb") {
        auto raw_scene_opt = bud::asset_pipeline::GltfImporter::import_scene_from_file(input_path);
        if (raw_scene_opt && raw_scene_opt->instances.size() > 1) {
            std::cout << "[BudAssetImporter] Importing scene package for " << stem << " -> " << output_dir << std::endl;
            if (bud::asset_pipeline::CascadeBuilder::build_scene_package(*raw_scene_opt, output_dir, cascade_opts)) {
                std::cout << "[BudAssetImporter] Successfully imported full scene package to: " << output_dir << std::endl;
                return 0;
            } else {
                std::cerr << "[BudAssetImporter] Failed to import scene package." << std::endl;
                return 1;
            }
        }
    } else if (ext == ".dae") {
        auto raw_scene_opt = bud::asset_pipeline::DaeImporter::import_scene_from_file(input_path, scale);
        if (raw_scene_opt && raw_scene_opt->instances.size() > 1) {
            std::cout << "[BudAssetImporter] Importing scene package for DAE " << stem << " -> " << output_dir << std::endl;
            if (bud::asset_pipeline::CascadeBuilder::build_scene_package(*raw_scene_opt, output_dir, cascade_opts)) {
                std::cout << "[BudAssetImporter] Successfully imported full scene package to: " << output_dir << std::endl;
                return 0;
            } else {
                std::cerr << "[BudAssetImporter] Failed to import scene package." << std::endl;
                return 1;
            }
        }
    }

    // 3. Fallback / Single Mesh Package Import
    std::optional<bud::asset_pipeline::RawMesh> raw_mesh_opt;
    if (ext == ".obj") {
        raw_mesh_opt = bud::asset_pipeline::ObjImporter::import_from_file(input_path, scale);
    } else if (ext == ".gltf" || ext == ".glb") {
        raw_mesh_opt = bud::asset_pipeline::GltfImporter::import_from_file(input_path);
    } else if (ext == ".fbx") {
        raw_mesh_opt = bud::asset_pipeline::FbxImporter::import_from_file(input_path, scale);
    } else if (ext == ".dae") {
        raw_mesh_opt = bud::asset_pipeline::DaeImporter::import_from_file(input_path, scale);
    } else if (ext == ".stl") {
        raw_mesh_opt = bud::asset_pipeline::StlImporter::import_from_file(input_path, scale);
    } else if (ext == ".rawmesh") {
        raw_mesh_opt = bud::asset_pipeline::RawMesh::load_from_file(input_path);
    } else if (ext == ".urdf") {
        if (output_dir.empty()) {
            std::cerr << "[BudAssetImporter] Error: --output is required." << std::endl;
            return 1;
        }

        if (package_root.empty())
            package_root = std::filesystem::path(input_path).parent_path().string();

        bud::asset_pipeline::UrdfBuildOptions build_opts{};
        build_opts.package_root = package_root;
        build_opts.max_convex_vertices = max_convex_verts;
        build_opts.scale = scale;
        build_opts.dump_json = true;
        build_opts.use_cache = use_cache;

        if (bud::asset_pipeline::UrdfBuilder::build_urdf(input_path, output_dir, build_opts)) {
            std::cout << "[BudAssetImporter] Successfully cooked robot package to: " << output_dir << std::endl;
            return 0;
        } else {
            std::cerr << "[BudAssetImporter] Failed to cook robot package from: " << input_path << std::endl;
            return 1;
        }
    } else if (ext == ".xml" || ext == ".mjcf") {
        if (output_dir.empty()) {
            std::cerr << "[BudAssetImporter] Error: --output is required." << std::endl;
            return 1;
        }

        bud::asset_pipeline::MjcfBuildOptions build_opts{};
        build_opts.max_convex_vertices = max_convex_verts;
        build_opts.scale = scale;
        build_opts.dump_json = true;
        build_opts.use_cache = use_cache;

        if (bud::asset_pipeline::MjcfBuilder::build_mjcf(input_path, output_dir, build_opts)) {
            std::cout << "[BudAssetImporter] Successfully cooked native MJCF robot package to: " << output_dir << std::endl;
            return 0;
        } else {
            std::cerr << "[BudAssetImporter] Failed to cook MJCF robot package from: " << input_path << std::endl;
            return 1;
        }
    } else if (ext == ".budasset") {
        std::ifstream in(input_path, std::ios::binary);
        if (in.is_open()) {
            bud::asset::BudAssetHeader h{};
            in.read(reinterpret_cast<char*>(&h), sizeof(h));
            if (h.magic == bud::asset::BUD_ASSET_MAGIC &&
                h.asset_type == static_cast<uint32_t>(bud::asset::AssetType::Articulation)) {
                std::cout << "[BudAssetImporter] Successfully verified Articulation .budasset container: " << input_path << std::endl;
                return 0;
            }
        }
        if (bud::asset_pipeline::CascadeBuilder::build_package(input_path, output_dir, cascade_opts)) {
            std::cout << "[BudAssetImporter] Successfully imported full asset package to: " << output_dir << std::endl;
            return 0;
        } else {
            std::cerr << "[BudAssetImporter] Failed to import full asset package from: " << input_path << std::endl;
            return 1;
        }
    } else {
        std::cerr << "[BudAssetImporter] Unsupported input format: " << ext << std::endl;
        return 1;
    }

    if (!raw_mesh_opt) {
        std::cerr << "[BudAssetImporter] Failed to parse mesh: " << input_path << std::endl;
        return 1;
    }

    std::cout << "[BudAssetImporter] Importing complete package for " << stem << " -> " << output_dir << std::endl;

    if (bud::asset_pipeline::CascadeBuilder::build_package_from_raw(*raw_mesh_opt, output_dir, cascade_opts)) {
        std::cout << "[BudAssetImporter] Successfully imported full asset package to: " << output_dir << std::endl;
        return 0;
    } else {
        std::cerr << "[BudAssetImporter] Failed to import full asset package." << std::endl;
        return 1;
    }
}
