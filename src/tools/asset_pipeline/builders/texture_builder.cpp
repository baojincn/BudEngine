#include "texture_builder.hpp"
#include "../importers/texture_importer.hpp"
#include "../core/serializer.hpp"
#include "../core/support.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace bud::asset_pipeline {

namespace {

struct MipLevelData {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

MipLevelData downsample_2x(const MipLevelData& src) {
    MipLevelData dst;
    dst.width = std::max(1u, src.width / 2);
    dst.height = std::max(1u, src.height / 2);
    dst.pixels.resize(dst.width * dst.height * 4);

    for (uint32_t y = 0; y < dst.height; ++y) {
        for (uint32_t x = 0; x < dst.width; ++x) {
            uint32_t sx0 = x * 2;
            uint32_t sx1 = std::min(sx0 + 1, src.width - 1);
            uint32_t sy0 = y * 2;
            uint32_t sy1 = std::min(sy0 + 1, src.height - 1);

            const uint8_t* p00 = &src.pixels[(sy0 * src.width + sx0) * 4];
            const uint8_t* p10 = &src.pixels[(sy0 * src.width + sx1) * 4];
            const uint8_t* p01 = &src.pixels[(sy1 * src.width + sx0) * 4];
            const uint8_t* p11 = &src.pixels[(sy1 * src.width + sx1) * 4];

            uint8_t* out = &dst.pixels[(y * dst.width + x) * 4];
            for (int c = 0; c < 4; ++c) {
                uint32_t sum = static_cast<uint32_t>(p00[c]) + p10[c] + p01[c] + p11[c];
                out[c] = static_cast<uint8_t>(sum / 4);
            }
        }
    }
    return dst;
}

} // namespace

bool TextureBuilder::build_from_file(const std::string& input_path, const std::string& output_path, const TextureBuildOptions& options) {
    auto raw_opt = TextureImporter::import_from_file(input_path);
    if (!raw_opt) {
        return false;
    }
    return build(*raw_opt, output_path, options);
}

bool TextureBuilder::build(const RawTexture& raw, const std::string& output_path, const TextureBuildOptions& options) {
    if (raw.width == 0 || raw.height == 0 || raw.pixels.empty())
        return false;

    // 1. Generate Mipmap chain
    std::vector<MipLevelData> mips;
    MipLevelData base_mip;
    base_mip.width = raw.width;
    base_mip.height = raw.height;
    base_mip.pixels = raw.pixels;
    mips.push_back(std::move(base_mip));

    if (options.generate_mips) {
        while (mips.back().width > 1 || mips.back().height > 1) {
            mips.push_back(downsample_2x(mips.back()));
        }
    }

    // 2. Prepare TextureHeader & Mip entries
    bud::asset::TextureHeader tex_header{};
    tex_header.magic = bud::asset::TEXTURE_MAGIC;
    tex_header.version = bud::asset::TEXTURE_VERSION;
    tex_header.width = raw.width;
    tex_header.height = raw.height;
    tex_header.depth = 1;
    tex_header.mip_levels = static_cast<uint32_t>(mips.size());
    tex_header.format = static_cast<uint32_t>(bud::asset::TextureFormat::RGBA8_UNORM);
    tex_header.flags = raw.is_srgb ? 1 : 0;

    std::vector<bud::asset::TextureMipEntry> mip_entries(mips.size());
    std::vector<uint8_t> inline_mip_payload;
    std::vector<uint8_t> bulk_mip_payload;

    std::filesystem::path out_p(output_path);
    std::string output_bulk_path = (out_p.parent_path() / (out_p.stem().string() + ".budbulk")).string();

    uint64_t current_inline_offset = 0;
    for (size_t i = 0; i < mips.size(); ++i) {
        const auto& mip = mips[i];
        mip_entries[i].mip_level = static_cast<uint32_t>(i);
        mip_entries[i].width = mip.width;
        mip_entries[i].height = mip.height;
        mip_entries[i].size_in_bytes = static_cast<uint32_t>(mip.pixels.size());

        if (options.split_bulk && i == 0 && mips.size() > 1) {
            // High-res Mip 0 goes to .budbulk
            mip_entries[i].offset_in_payload = 0;
            bulk_mip_payload.insert(bulk_mip_payload.end(), mip.pixels.begin(), mip.pixels.end());
        } else {
            // Lower-res mips stay inline
            mip_entries[i].offset_in_payload = current_inline_offset;
            inline_mip_payload.insert(inline_mip_payload.end(), mip.pixels.begin(), mip.pixels.end());
            current_inline_offset += mip.pixels.size();
        }
    }

    tex_header.total_data_size = inline_mip_payload.size() + bulk_mip_payload.size();

    // 3. Serialize Texture Chunk
    size_t header_chunk_size = sizeof(bud::asset::TextureHeader) + mip_entries.size() * sizeof(bud::asset::TextureMipEntry);
    std::vector<uint8_t> metadata_chunk(header_chunk_size);
    uint8_t* dst = metadata_chunk.data();
    std::memcpy(dst, &tex_header, sizeof(bud::asset::TextureHeader));
    dst += sizeof(bud::asset::TextureHeader);
    std::memcpy(dst, mip_entries.data(), mip_entries.size() * sizeof(bud::asset::TextureMipEntry));

    // Compute Asset ID from source path
    std::hash<std::string> hasher;
    uint64_t asset_id = hasher(raw.source_path.empty() ? output_path : raw.source_path);

    BudAssetWriter writer(AssetType::Texture, asset_id);
    writer.add_chunk(AssetChunkType::Texture, metadata_chunk.data(), metadata_chunk.size());

    if (!inline_mip_payload.empty()) {
        writer.add_chunk(AssetChunkType::AssetManifest, inline_mip_payload.data(), inline_mip_payload.size());
    }

    if (options.split_bulk && !bulk_mip_payload.empty()) {
        std::error_code ec;
        if (out_p.has_parent_path())
            std::filesystem::create_directories(out_p.parent_path(), ec);

        std::ofstream bulk_out(output_bulk_path, std::ios::binary);
        if (bulk_out.is_open()) {
            bulk_out.write(reinterpret_cast<const char*>(bulk_mip_payload.data()), bulk_mip_payload.size());
            bulk_out.flush();
        }
        writer.set_flags(bud::asset::BUD_ASSET_FLAG_HAS_BULK_DATA);
        writer.set_bulk_data_size(bulk_mip_payload.size());
    }

    std::error_code ec;
    if (out_p.has_parent_path())
        std::filesystem::create_directories(out_p.parent_path(), ec);

    if (!writer.save_to_file(output_path)) {
        std::cerr << "[BudAssetPipeline] Failed to write Texture .budasset: " << output_path << std::endl;
        return false;
    }

    return true;
}

} // namespace bud::asset_pipeline
