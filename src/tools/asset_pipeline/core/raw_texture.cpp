#include "raw_texture.hpp"
#include <fstream>
#include <iostream>

namespace bud::asset_pipeline {

bool RawTexture::save_binary(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
        return false;

    uint64_t magic = BUD_RAWTEXTURE_MAGIC;
    uint32_t version = BUD_RAWTEXTURE_VERSION;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));

    uint32_t path_len = static_cast<uint32_t>(source_path.length());
    out.write(reinterpret_cast<const char*>(&path_len), sizeof(path_len));
    if (path_len > 0)
        out.write(source_path.data(), path_len);

    out.write(reinterpret_cast<const char*>(&width), sizeof(width));
    out.write(reinterpret_cast<const char*>(&height), sizeof(height));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    uint8_t srgb_byte = is_srgb ? 1 : 0;
    out.write(reinterpret_cast<const char*>(&srgb_byte), sizeof(srgb_byte));

    uint64_t data_size = pixels.size();
    out.write(reinterpret_cast<const char*>(&data_size), sizeof(data_size));
    if (data_size > 0)
        out.write(reinterpret_cast<const char*>(pixels.data()), data_size);

    return out.good();
}

std::optional<RawTexture> RawTexture::load_binary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return std::nullopt;

    uint64_t magic = 0;
    uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));

    if (magic != BUD_RAWTEXTURE_MAGIC || version != BUD_RAWTEXTURE_VERSION)
        return std::nullopt;

    RawTexture tex;
    uint32_t path_len = 0;
    in.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
    if (path_len > 0) {
        tex.source_path.resize(path_len);
        in.read(tex.source_path.data(), path_len);
    }

    in.read(reinterpret_cast<char*>(&tex.width), sizeof(tex.width));
    in.read(reinterpret_cast<char*>(&tex.height), sizeof(tex.height));
    in.read(reinterpret_cast<char*>(&tex.channels), sizeof(tex.channels));
    uint8_t srgb_byte = 0;
    in.read(reinterpret_cast<char*>(&srgb_byte), sizeof(srgb_byte));
    tex.is_srgb = (srgb_byte != 0);

    uint64_t data_size = 0;
    in.read(reinterpret_cast<char*>(&data_size), sizeof(data_size));
    if (data_size > 0) {
        tex.pixels.resize(data_size);
        in.read(reinterpret_cast<char*>(tex.pixels.data()), data_size);
    }

    if (!in.good() && !in.eof())
        return std::nullopt;

    return tex;
}

} // namespace bud::asset_pipeline
