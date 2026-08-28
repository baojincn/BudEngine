#include "serializer.hpp"
#include <fstream>
#include <iostream>
#include <cstring>
#include <span>

namespace bud::asset_pipeline {

BudAssetWriter::BudAssetWriter(AssetType asset_type, uint64_t asset_id)
    : m_asset_type(asset_type), m_asset_id(asset_id) {
}

void BudAssetWriter::add_chunk(AssetChunkType type, const void* data, size_t size, uint32_t flags) {
    AssetChunkPayload chunk;
    chunk.type = type;
    chunk.flags = flags;
    if (data && size > 0) {
        chunk.data.resize(size);
        std::memcpy(chunk.data.data(), data, size);
    }
    m_chunks.push_back(std::move(chunk));
}

bool BudAssetWriter::save_to_file(const std::string& output_path) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[BudAssetPipeline] Failed to open file for writing: " << output_path << std::endl;
        return false;
    }

    BudAssetHeader header{};
    header.magic = bud::asset::BUD_ASSET_MAGIC;
    header.version = bud::asset::BUD_ASSET_VERSION;
    header.asset_type = static_cast<uint32_t>(m_asset_type);
    header.asset_id = m_asset_id;
    header.flags = m_flags;
    header.chunk_count = static_cast<uint32_t>(m_chunks.size());
    header.chunk_table_offset = sizeof(BudAssetHeader);
    header.bulk_data_size = m_bulk_data_size;

    std::vector<AssetChunkEntry> chunk_entries(m_chunks.size());
    uint64_t current_offset = sizeof(BudAssetHeader) + m_chunks.size() * sizeof(AssetChunkEntry);

    for (size_t i = 0; i < m_chunks.size(); ++i) {
        chunk_entries[i].chunk_type = static_cast<uint32_t>(m_chunks[i].type);
        chunk_entries[i].flags = m_chunks[i].flags;
        chunk_entries[i].offset = current_offset;
        chunk_entries[i].size = m_chunks[i].data.size();
        current_offset += m_chunks[i].data.size();
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(BudAssetHeader));
    if (!chunk_entries.empty()) {
        out.write(reinterpret_cast<const char*>(chunk_entries.data()), chunk_entries.size() * sizeof(AssetChunkEntry));
    }

    for (const auto& chunk : m_chunks) {
        if (!chunk.data.empty()) {
            out.write(reinterpret_cast<const char*>(chunk.data.data()), chunk.data.size());
        }
    }

    out.flush();
    if (!out.good()) {
        std::cerr << "[BudAssetPipeline] Error writing output: " << output_path << std::endl;
        return false;
    }

    return true;
}

std::optional<BudAssetReader> BudAssetReader::load_from_memory(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(BudAssetHeader))
        return std::nullopt;

    BudAssetReader reader;
    std::memcpy(&reader.m_header, data, sizeof(BudAssetHeader));

    if (reader.m_header.magic != bud::asset::BUD_ASSET_MAGIC)
        return std::nullopt;
    if (reader.m_header.version != bud::asset::BUD_ASSET_VERSION)
        return std::nullopt;

    uint64_t table_end = reader.m_header.chunk_table_offset + static_cast<uint64_t>(reader.m_header.chunk_count) * sizeof(AssetChunkEntry);
    if (table_end > size)
        return std::nullopt;

    reader.m_chunk_table.resize(reader.m_header.chunk_count);
    std::memcpy(reader.m_chunk_table.data(), data + reader.m_header.chunk_table_offset, reader.m_header.chunk_count * sizeof(AssetChunkEntry));

    reader.m_raw_data = data;
    reader.m_raw_size = size;

    return reader;
}

std::optional<std::span<const uint8_t>> BudAssetReader::get_chunk_data(AssetChunkType type) const {
    uint32_t target_type = static_cast<uint32_t>(type);
    for (const auto& entry : m_chunk_table) {
        if (entry.chunk_type == target_type) {
            if (entry.offset + entry.size <= m_raw_size) {
                return std::span<const uint8_t>(m_raw_data + entry.offset, entry.size);
            }
        }
    }
    return std::nullopt;
}

} // namespace bud::asset_pipeline
