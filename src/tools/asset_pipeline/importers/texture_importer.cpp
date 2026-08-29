#include "texture_importer.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define BCDEC_IMPLEMENTATION
#include "../../../third_party/bcdec.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cstring>
#include <algorithm>

namespace bud::asset_pipeline {

namespace {

#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DDS_HEADER {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};

struct DDS_HEADER_DXT10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
constexpr uint32_t FOURCC_DXT1 = 0x31545844;
constexpr uint32_t FOURCC_DXT2 = 0x32545844;
constexpr uint32_t FOURCC_DXT3 = 0x33545844;
constexpr uint32_t FOURCC_DXT4 = 0x34545844;
constexpr uint32_t FOURCC_DXT5 = 0x35545844;
constexpr uint32_t FOURCC_ATI1 = 0x31495441;
constexpr uint32_t FOURCC_BC4U = 0x55344342;
constexpr uint32_t FOURCC_ATI2 = 0x32495441;
constexpr uint32_t FOURCC_BC5U = 0x55354342;
constexpr uint32_t FOURCC_DX10 = 0x30315844;

enum class DdsCompressionType {
    None,
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
    BC7,
    RGBA8,
    BGRA8
};

std::optional<RawTexture> load_dds_file(const std::string& path, const std::vector<uint8_t>& file_data) {
    if (file_data.size() < sizeof(uint32_t) + sizeof(DDS_HEADER)) {
        std::cerr << "[BudAssetPipeline] DDS file too small: " << path << std::endl;
        return std::nullopt;
    }

    uint32_t magic = *reinterpret_cast<const uint32_t*>(file_data.data());
    if (magic != DDS_MAGIC) {
        std::cerr << "[BudAssetPipeline] Invalid DDS magic in: " << path << std::endl;
        return std::nullopt;
    }

    const auto* header = reinterpret_cast<const DDS_HEADER*>(file_data.data() + sizeof(uint32_t));
    if (header->dwSize != sizeof(DDS_HEADER) || header->ddspf.dwSize != sizeof(DDS_PIXELFORMAT)) {
        std::cerr << "[BudAssetPipeline] Invalid DDS header size in: " << path << std::endl;
        return std::nullopt;
    }

    uint32_t width = header->dwWidth;
    uint32_t height = header->dwHeight;
    size_t header_offset = sizeof(uint32_t) + sizeof(DDS_HEADER);

    DdsCompressionType compression = DdsCompressionType::None;
    bool is_srgb = true;

    if (header->ddspf.dwFlags & 0x4) { // DDPF_FOURCC
        if (header->ddspf.dwFourCC == FOURCC_DXT1) {
            compression = DdsCompressionType::BC1;
        } else if (header->ddspf.dwFourCC == FOURCC_DXT2 || header->ddspf.dwFourCC == FOURCC_DXT3) {
            compression = DdsCompressionType::BC2;
        } else if (header->ddspf.dwFourCC == FOURCC_DXT4 || header->ddspf.dwFourCC == FOURCC_DXT5) {
            compression = DdsCompressionType::BC3;
        } else if (header->ddspf.dwFourCC == FOURCC_ATI1 || header->ddspf.dwFourCC == FOURCC_BC4U) {
            compression = DdsCompressionType::BC4;
        } else if (header->ddspf.dwFourCC == FOURCC_ATI2 || header->ddspf.dwFourCC == FOURCC_BC5U) {
            compression = DdsCompressionType::BC5;
        } else if (header->ddspf.dwFourCC == FOURCC_DX10) {
            if (file_data.size() < header_offset + sizeof(DDS_HEADER_DXT10)) {
                std::cerr << "[BudAssetPipeline] DDS DX10 header missing in: " << path << std::endl;
                return std::nullopt;
            }
            const auto* dxt10 = reinterpret_cast<const DDS_HEADER_DXT10*>(file_data.data() + header_offset);
            header_offset += sizeof(DDS_HEADER_DXT10);

            switch (dxt10->dxgiFormat) {
            case 70: case 71: case 72: // BC1
                compression = DdsCompressionType::BC1;
                is_srgb = (dxt10->dxgiFormat == 72);
                break;
            case 73: case 74: case 75: // BC2
                compression = DdsCompressionType::BC2;
                is_srgb = (dxt10->dxgiFormat == 75);
                break;
            case 76: case 77: case 78: // BC3
                compression = DdsCompressionType::BC3;
                is_srgb = (dxt10->dxgiFormat == 78);
                break;
            case 79: case 80: case 81: // BC4
                compression = DdsCompressionType::BC4;
                is_srgb = false;
                break;
            case 82: case 83: case 84: // BC5
                compression = DdsCompressionType::BC5;
                is_srgb = false;
                break;
            case 97: case 98: case 99: // BC7
                compression = DdsCompressionType::BC7;
                is_srgb = (dxt10->dxgiFormat == 99);
                break;
            case 27: case 28: case 29: // R8G8B8A8
                compression = DdsCompressionType::RGBA8;
                is_srgb = (dxt10->dxgiFormat == 29);
                break;
            case 87: case 91: // B8G8R8A8
                compression = DdsCompressionType::BGRA8;
                is_srgb = (dxt10->dxgiFormat == 91);
                break;
            default:
                std::cerr << "[BudAssetPipeline] Unsupported DXGI format (" << dxt10->dxgiFormat << ") in: " << path << std::endl;
                return std::nullopt;
            }
        }
    } else if (header->ddspf.dwFlags & 0x40) { // DDPF_RGB
        if (header->ddspf.dwRGBBitCount == 32) {
            if (header->ddspf.dwRBitMask == 0x000000FF && header->ddspf.dwBBitMask == 0x00FF0000)
                compression = DdsCompressionType::RGBA8;
            else if (header->ddspf.dwRBitMask == 0x00FF0000 && header->ddspf.dwBBitMask == 0x000000FF)
                compression = DdsCompressionType::BGRA8;
            else
                compression = DdsCompressionType::RGBA8;
        }
    }

    if (compression == DdsCompressionType::None) {
        std::cerr << "[BudAssetPipeline] Unknown or unsupported DDS format in: " << path << std::endl;
        return std::nullopt;
    }

    RawTexture raw_tex;
    raw_tex.source_path = path;
    raw_tex.width = width;
    raw_tex.height = height;
    raw_tex.channels = 4;
    raw_tex.is_srgb = is_srgb;
    raw_tex.pixels.resize(static_cast<size_t>(width) * height * 4);

    const uint8_t* src_ptr = file_data.data() + header_offset;

    if (compression == DdsCompressionType::RGBA8) {
        size_t copy_size = std::min(raw_tex.pixels.size(), file_data.size() - header_offset);
        std::memcpy(raw_tex.pixels.data(), src_ptr, copy_size);
        return raw_tex;
    } else if (compression == DdsCompressionType::BGRA8) {
        size_t pixel_count = static_cast<size_t>(width) * height;
        for (size_t p = 0; p < pixel_count; ++p) {
            raw_tex.pixels[p * 4 + 0] = src_ptr[p * 4 + 2]; // R
            raw_tex.pixels[p * 4 + 1] = src_ptr[p * 4 + 1]; // G
            raw_tex.pixels[p * 4 + 2] = src_ptr[p * 4 + 0]; // B
            raw_tex.pixels[p * 4 + 3] = src_ptr[p * 4 + 3]; // A
        }
        return raw_tex;
    }

    // Decompress block-compressed textures (BC1..BC7) using bcdec
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    int dst_pitch = static_cast<int>(width * 4);

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            uint32_t px = bx * 4;
            uint32_t py = by * 4;
            uint8_t* dst_block = &raw_tex.pixels[(py * width + px) * 4];

            switch (compression) {
            case DdsCompressionType::BC1:
                bcdec_bc1(src_ptr, dst_block, dst_pitch);
                src_ptr += BCDEC_BC1_BLOCK_SIZE;
                break;
            case DdsCompressionType::BC2:
                bcdec_bc2(src_ptr, dst_block, dst_pitch);
                src_ptr += BCDEC_BC2_BLOCK_SIZE;
                break;
            case DdsCompressionType::BC3:
                bcdec_bc3(src_ptr, dst_block, dst_pitch);
                src_ptr += BCDEC_BC3_BLOCK_SIZE;
                break;
            case DdsCompressionType::BC4:
            {
                uint8_t r_block[16];
                bcdec_bc4(src_ptr, r_block, 4);
                src_ptr += BCDEC_BC4_BLOCK_SIZE;
                for (int row = 0; row < 4 && (py + row) < height; ++row) {
                    for (int col = 0; col < 4 && (px + col) < width; ++col) {
                        uint8_t val = r_block[row * 4 + col];
                        uint8_t* out = &dst_block[row * dst_pitch + col * 4];
                        out[0] = val; out[1] = val; out[2] = val; out[3] = 255;
                    }
                }
                break;
            }
            case DdsCompressionType::BC5:
            {
                uint8_t rg_block[32];
                bcdec_bc5(src_ptr, rg_block, 8);
                src_ptr += BCDEC_BC5_BLOCK_SIZE;
                for (int row = 0; row < 4 && (py + row) < height; ++row) {
                    for (int col = 0; col < 4 && (px + col) < width; ++col) {
                        uint8_t r = rg_block[(row * 4 + col) * 2 + 0];
                        uint8_t g = rg_block[(row * 4 + col) * 2 + 1];
                        uint8_t* out = &dst_block[row * dst_pitch + col * 4];
                        out[0] = r; out[1] = g; out[2] = 255; out[3] = 255;
                    }
                }
                break;
            }
            case DdsCompressionType::BC7:
                bcdec_bc7(src_ptr, dst_block, dst_pitch);
                src_ptr += BCDEC_BC7_BLOCK_SIZE;
                break;
            default:
                break;
            }
        }
    }

    return raw_tex;
}

static void apply_companion_mask_if_present(RawTexture& tex, const std::string& resolved_path) {
    if (tex.pixels.empty() || tex.width == 0 || tex.height == 0)
        return;

    // Check if current texture already has significant alpha variation
    size_t total_px = static_cast<size_t>(tex.width) * tex.height;
    size_t non_solid_alpha = 0;
    for (size_t k = 0; k < total_px; ++k) {
        if (tex.pixels[k * 4 + 3] < 250) {
            non_solid_alpha++;
            if (non_solid_alpha > total_px / 1000)
                return; // Already has true alpha channel
        }
    }

    std::filesystem::path p(resolved_path);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    auto parent = p.parent_path();

    std::vector<std::string> mask_candidates;
    const std::string test_exts[] = { ext, ".tga", ".TGA", ".png", ".PNG", ".dds", ".DDS", ".jpg", ".JPG" };

    auto add_stem_candidates = [&](const std::string& base) {
        for (const auto& e : test_exts) {
            mask_candidates.push_back((parent / (base + e)).string());
        }
    };

    // 1. _0_D -> _0_A or _D -> _A
    if (stem.ends_with("_D") || stem.ends_with("_d")) {
        std::string s = stem;
        s.back() = (stem.back() == 'D') ? 'A' : 'a';
        add_stem_candidates(s);
    }
    if (stem.find("_0_D") != std::string::npos) {
        std::string s = stem;
        auto pos = s.find("_0_D");
        s.replace(pos, 4, "_0_A");
        add_stem_candidates(s);
    }
    if (stem.find("_0_d") != std::string::npos) {
        std::string s = stem;
        auto pos = s.find("_0_d");
        s.replace(pos, 4, "_0_a");
        add_stem_candidates(s);
    }

    // 2. _diff -> _mask / _opacity / _alpha
    if (stem.find("_diff") != std::string::npos) {
        std::string s = stem;
        auto pos = s.find("_diff");
        std::string prefix = s.substr(0, pos);
        std::string suffix = s.substr(pos + 5);
        add_stem_candidates(prefix + "_mask" + suffix);
        add_stem_candidates(prefix + "_opacity" + suffix);
        add_stem_candidates(prefix + "_alpha" + suffix);
    }

    // 3. _Albedo / _albedo / _BaseColor / _basecolor -> _Opacity / _opacity / _Mask / _mask
    if (stem.find("_Albedo") != std::string::npos) {
        std::string s = stem;
        auto pos = s.find("_Albedo");
        std::string prefix = s.substr(0, pos);
        std::string suffix = s.substr(pos + 7);
        add_stem_candidates(prefix + "_Opacity" + suffix);
        add_stem_candidates(prefix + "_Mask" + suffix);
        add_stem_candidates(prefix + "_Alpha" + suffix);
    }
    if (stem.find("_albedo") != std::string::npos) {
        std::string s = stem;
        auto pos = s.find("_albedo");
        std::string prefix = s.substr(0, pos);
        std::string suffix = s.substr(pos + 7);
        add_stem_candidates(prefix + "_opacity" + suffix);
        add_stem_candidates(prefix + "_mask" + suffix);
        add_stem_candidates(prefix + "_alpha" + suffix);
    }

    // 4. General suffix append
    add_stem_candidates(stem + "_mask");
    add_stem_candidates(stem + "_Mask");
    add_stem_candidates(stem + "_opacity");
    add_stem_candidates(stem + "_Opacity");
    add_stem_candidates(stem + "_alpha");
    add_stem_candidates(stem + "_Alpha");

    for (const auto& mcand : mask_candidates) {
        if (std::filesystem::exists(mcand) && mcand != resolved_path) {
            int mw = 0, mh = 0, mc = 0;
            stbi_uc* mdata = stbi_load(mcand.c_str(), &mw, &mh, &mc, 1);
            if (mdata && mw > 0 && mh > 0) {
                if (mw == static_cast<int>(tex.width) && mh == static_cast<int>(tex.height)) {
                    for (size_t k = 0; k < total_px; ++k) {
                        tex.pixels[k * 4 + 3] = mdata[k];
                    }
                } else {
                    for (uint32_t y = 0; y < tex.height; ++y) {
                        int my = std::min(mh - 1, static_cast<int>((static_cast<uint64_t>(y) * mh) / tex.height));
                        for (uint32_t x = 0; x < tex.width; ++x) {
                            int mx = std::min(mw - 1, static_cast<int>((static_cast<uint64_t>(x) * mw) / tex.width));
                            tex.pixels[(y * tex.width + x) * 4 + 3] = mdata[my * mw + mx];
                        }
                    }
                }
                std::cout << "[BudAssetPipeline] Merged companion alpha mask: " << mcand << " -> " << resolved_path << std::endl;
                stbi_image_free(mdata);
                break;
            }
            if (mdata)
                stbi_image_free(mdata);
        }
    }
}

} // namespace

std::optional<RawTexture> TextureImporter::import_from_file(const std::string& path) {
    std::string resolved_path = path;

    // Path resolution with extension fallbacks if direct file does not exist
    if (!std::filesystem::exists(resolved_path)) {
        const std::string candidate_exts[] = { ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
        for (const auto& ext : candidate_exts) {
            auto cand = std::filesystem::path(path).replace_extension(ext).string();
            if (std::filesystem::exists(cand)) {
                resolved_path = cand;
                break;
            }
        }
    }

    if (!std::filesystem::exists(resolved_path)) {
        std::cerr << "[BudAssetPipeline] Texture file not found: " << path << std::endl;
        return std::nullopt;
    }

    // Read full file into memory
    std::ifstream file(resolved_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[BudAssetPipeline] Failed to open texture: " << resolved_path << std::endl;
        return std::nullopt;
    }

    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);

    // 1. Check for DDS Header Magic
    if (file_size >= sizeof(uint32_t) && *reinterpret_cast<const uint32_t*>(file_data.data()) == DDS_MAGIC) {
        auto dds_tex = load_dds_file(resolved_path, file_data);
        if (dds_tex) {
            apply_companion_mask_if_present(*dds_tex, resolved_path);
        }
        return dds_tex;
    }

    // 2. Fallback to STB Image for standard formats (PNG, JPG, TGA, BMP, etc.)
    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load_from_memory(file_data.data(), static_cast<int>(file_size), &w, &h, &comp, STBI_rgb_alpha);
    if (!data || w <= 0 || h <= 0) {
        std::cerr << "[BudAssetPipeline] Failed to load image: " << resolved_path << std::endl;
        if (data)
            stbi_image_free(data);
        return std::nullopt;
    }

    RawTexture tex;
    tex.source_path = resolved_path;
    tex.width = static_cast<uint32_t>(w);
    tex.height = static_cast<uint32_t>(h);
    tex.channels = 4;
    tex.is_srgb = true;

    size_t byte_count = static_cast<size_t>(w) * h * 4;
    tex.pixels.resize(byte_count);
    std::memcpy(tex.pixels.data(), data, byte_count);
    stbi_image_free(data);

    // Apply companion mask if alpha is completely solid
    apply_companion_mask_if_present(tex, resolved_path);

    return tex;
}

TextureAlphaInfo TextureImporter::analyze_alpha(const RawTexture& tex) {
    TextureAlphaInfo info{};
    if (tex.pixels.empty() || tex.channels < 4) {
        return info;
    }

    size_t total_px = static_cast<size_t>(tex.width) * tex.height;
    if (total_px == 0)
        return info;

    size_t transparent_px = 0;
    size_t semi_transparent_px = 0;

    const uint8_t* p = tex.pixels.data();
    for (size_t k = 0; k < total_px; ++k) {
        uint8_t a = p[k * 4 + 3];
        if (a < 250) {
            if (a < 25) {
                transparent_px++;
            } else {
                semi_transparent_px++;
            }
        }
    }

    // If at least 0.05% of the texture has transparency:
    if (transparent_px + semi_transparent_px > (total_px / 2000)) {
        info.has_alpha = true;
        // If there are significant fully transparent void pixels (cutout foliage / thorns / vines / fences):
        if (transparent_px > (total_px / 1000)) {
            info.alpha_mode = bud::asset::AlphaMode::Mask;
            info.alpha_cutoff = 0.5f;
        } else if (semi_transparent_px > (total_px / 20)) {
            // Smooth translucent tint without empty void (e.g. tinted glass pane):
            info.alpha_mode = bud::asset::AlphaMode::Blend;
        } else {
            info.alpha_mode = bud::asset::AlphaMode::Mask;
            info.alpha_cutoff = 0.5f;
        }
    }

    return info;
}

TextureAlphaInfo TextureImporter::analyze_alpha(const std::string& path) {
    auto tex = import_from_file(path);
    if (!tex)
        return {};
    return analyze_alpha(*tex);
}

} // namespace bud::asset_pipeline
