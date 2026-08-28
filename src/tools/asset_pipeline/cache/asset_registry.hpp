#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include "../core/serializer.hpp"

namespace bud::asset_pipeline {

struct AssetRegistryEntry {
    uint64_t asset_id = 0;
    uint32_t asset_type = 0;
    std::string asset_path;
    std::vector<std::string> dependencies;
};

class AssetRegistry {
public:
    static inline std::string registry_file_path = "Cache/Registry/asset_registry.bin";
    static inline std::unordered_map<uint64_t, AssetRegistryEntry> entries;

    static void init(const std::string& path) {
        registry_file_path = path;
        std::error_code ec;
        std::filesystem::path p(registry_file_path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        load();
    }

    static void register_asset(const AssetRegistryEntry& entry) {
        entries[entry.asset_id] = entry;
    }

    static bool has_asset(uint64_t asset_id) {
        return entries.find(asset_id) != entries.end();
    }

    static AssetRegistryEntry get_asset(uint64_t asset_id) {
        if (auto it = entries.find(asset_id); it != entries.end()) {
            return it->second;
        }
        return {};
    }

    static void save() {
        std::ofstream out(registry_file_path, std::ios::binary);
        if (!out.is_open()) {
            return;
        }
        uint32_t count = static_cast<uint32_t>(entries.size());
        out.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
        for (const auto& [id, entry] : entries) {
            out.write(reinterpret_cast<const char*>(&entry.asset_id), sizeof(uint64_t));
            out.write(reinterpret_cast<const char*>(&entry.asset_type), sizeof(uint32_t));
            uint32_t path_len = static_cast<uint32_t>(entry.asset_path.size());
            out.write(reinterpret_cast<const char*>(&path_len), sizeof(uint32_t));
            if (path_len > 0) {
                out.write(entry.asset_path.data(), path_len);
            }
            uint32_t dep_count = static_cast<uint32_t>(entry.dependencies.size());
            out.write(reinterpret_cast<const char*>(&dep_count), sizeof(uint32_t));
            for (const auto& dep : entry.dependencies) {
                uint32_t dep_len = static_cast<uint32_t>(dep.size());
                out.write(reinterpret_cast<const char*>(&dep_len), sizeof(uint32_t));
                if (dep_len > 0) {
                    out.write(dep.data(), dep_len);
                }
            }
        }
    }

    static void load() {
        entries.clear();
        if (!std::filesystem::exists(registry_file_path)) {
            return;
        }
        std::ifstream in(registry_file_path, std::ios::binary);
        if (!in.is_open()) {
            return;
        }
        uint32_t count = 0;
        if (!in.read(reinterpret_cast<char*>(&count), sizeof(uint32_t))) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            AssetRegistryEntry entry;
            in.read(reinterpret_cast<char*>(&entry.asset_id), sizeof(uint64_t));
            in.read(reinterpret_cast<char*>(&entry.asset_type), sizeof(uint32_t));
            uint32_t path_len = 0;
            in.read(reinterpret_cast<char*>(&path_len), sizeof(uint32_t));
            if (path_len > 0) {
                entry.asset_path.resize(path_len);
                in.read(entry.asset_path.data(), path_len);
            }
            uint32_t dep_count = 0;
            in.read(reinterpret_cast<char*>(&dep_count), sizeof(uint32_t));
            for (uint32_t d = 0; d < dep_count; ++d) {
                uint32_t dep_len = 0;
                in.read(reinterpret_cast<char*>(&dep_len), sizeof(uint32_t));
                std::string dep;
                if (dep_len > 0) {
                    dep.resize(dep_len);
                    in.read(dep.data(), dep_len);
                }
                entry.dependencies.push_back(std::move(dep));
            }
            entries[entry.asset_id] = std::move(entry);
        }
    }
};

} // namespace bud::asset_pipeline
