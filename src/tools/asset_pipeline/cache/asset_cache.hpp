#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace bud::asset_pipeline {

class AssetCache {
public:
    static inline std::string cache_root_dir = "Cache";

    static void init(const std::string& cache_root) {
        cache_root_dir = cache_root.empty() ? "Cache" : cache_root;
        std::error_code ec;
        std::filesystem::create_directories(cache_root_dir, ec);
        std::filesystem::create_directories(cache_root_dir + "/Registry", ec);
    }

    static std::string compute_build_key(const std::string& input_path, uint32_t build_version) {
        std::error_code ec;
        auto last_write = std::filesystem::last_write_time(input_path, ec);
        auto file_size = std::filesystem::file_size(input_path, ec);
        auto time_val = last_write.time_since_epoch().count();

        std::hash<std::string> str_hash;
        uint64_t h1 = str_hash(input_path);
        uint64_t h2 = static_cast<uint64_t>(time_val) ^ (static_cast<uint64_t>(file_size) << 16) ^ static_cast<uint64_t>(build_version);

        std::stringstream ss;
        ss << std::hex << std::setfill('0') << std::setw(16) << h1 << "_" << std::setw(16) << h2;
        return ss.str();
    }

    static bool has_mesh_cache(const std::string& build_key) {
        return std::filesystem::exists(get_mesh_cache_path(build_key));
    }

    static bool has_mesh_bulk_cache(const std::string& build_key) {
        return std::filesystem::exists(get_mesh_bulk_cache_path(build_key));
    }

    // Both .budasset and .budbulk are co-located in the same Cache root directory
    static std::string get_mesh_cache_path(const std::string& build_key) {
        return (std::filesystem::path(cache_root_dir) / (build_key + ".budasset")).string();
    }

    static std::string get_mesh_bulk_cache_path(const std::string& build_key) {
        return (std::filesystem::path(cache_root_dir) / (build_key + ".budbulk")).string();
    }
};

} // namespace bud::asset_pipeline
