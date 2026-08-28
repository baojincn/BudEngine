#pragma once

#include "asset_format.hpp"
#include <string>
#include <vector>
#include <optional>
#include <span>

namespace bud::asset_pipeline {

class BudAssetWriter {
public:
    BudAssetWriter(AssetType asset_type, uint64_t asset_id);

    void set_flags(uint32_t flags) { m_flags = flags; }
    void set_bulk_data_size(uint64_t size) { m_bulk_data_size = size; }

    void add_chunk(AssetChunkType type, const void* data, size_t size, uint32_t flags = 0);
    bool save_to_file(const std::string& output_path);

private:
    AssetType m_asset_type;
    uint64_t m_asset_id;
    uint32_t m_flags = 0;
    uint64_t m_bulk_data_size = 0;
    std::vector<AssetChunkPayload> m_chunks;
};

class BudAssetReader {
public:
    static std::optional<BudAssetReader> load_from_memory(const uint8_t* data, size_t size);

    const BudAssetHeader& get_header() const { return m_header; }
    const std::vector<AssetChunkEntry>& get_chunk_table() const { return m_chunk_table; }
    std::optional<std::span<const uint8_t>> get_chunk_data(AssetChunkType type) const;

private:
    BudAssetHeader m_header{};
    std::vector<AssetChunkEntry> m_chunk_table;
    const uint8_t* m_raw_data = nullptr;
    size_t m_raw_size = 0;
};

} // namespace bud::asset_pipeline
