#include "texture_importer.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define BCDEC_IMPLEMENTATION
#include "../../../third_party/bcdec.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_set>

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

void normalize_ddna_normals_if_needed(RawTexture& tex, const std::string& path) {
    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    if (lower_path.find("_ddna") != std::string::npos || lower_path.find("_ddn") != std::string::npos) {
        size_t total_px = static_cast<size_t>(tex.width) * tex.height;
        if (tex.pixels.size() >= total_px * 4) {
            uint8_t* p = tex.pixels.data();
            for (size_t k = 0; k < total_px; ++k) {
                float nx = (p[k * 4 + 0] / 255.0f) * 2.0f - 1.0f;
                float ny = (p[k * 4 + 1] / 255.0f) * 2.0f - 1.0f;
                float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));

                p[k * 4 + 0] = static_cast<uint8_t>(std::clamp((nx * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                p[k * 4 + 1] = static_cast<uint8_t>(std::clamp((ny * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                p[k * 4 + 2] = static_cast<uint8_t>(std::clamp((nz * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                p[k * 4 + 3] = 255;
            }
            tex.is_srgb = false;
        }
    }
}

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
            case 87: case 90: case 91: // B8G8R8A8
                compression = DdsCompressionType::BGRA8;
                is_srgb = (dxt10->dxgiFormat == 90 || dxt10->dxgiFormat == 91);
                break;
            default:
                std::cerr << "[BudAssetPipeline] Unsupported DXGI format: " << dxt10->dxgiFormat << " in: " << path << std::endl;
                return std::nullopt;
            }
        }
    } else if (header->ddspf.dwFlags & 0x40) { // DDPF_RGB
        if (header->ddspf.dwRGBBitCount == 32) {
            if (header->ddspf.dwRBitMask == 0x00FF0000 && header->ddspf.dwBBitMask == 0x000000FF) {
                compression = DdsCompressionType::BGRA8;
            } else {
                compression = DdsCompressionType::RGBA8;
            }
        }
    }

    if (compression == DdsCompressionType::None) {
        std::cerr << "[BudAssetPipeline] Unsupported or uncompressed DDS format in: " << path << std::endl;
        return std::nullopt;
    }

    RawTexture tex;
    tex.source_path = path;
    tex.width = width;
    tex.height = height;
    tex.channels = 4;
    tex.is_srgb = is_srgb;
    tex.pixels.resize(static_cast<size_t>(width) * height * 4);

    const uint8_t* src_ptr = file_data.data() + header_offset;
    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;

    if (compression == DdsCompressionType::RGBA8 || compression == DdsCompressionType::BGRA8) {
        size_t expected_size = static_cast<size_t>(width) * height * 4;
        if (file_data.size() < header_offset + expected_size) {
            std::cerr << "[BudAssetPipeline] Incomplete uncompressed DDS data in: " << path << std::endl;
            return std::nullopt;
        }
        if (compression == DdsCompressionType::BGRA8) {
            for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p) {
                tex.pixels[p * 4 + 0] = src_ptr[p * 4 + 2];
                tex.pixels[p * 4 + 1] = src_ptr[p * 4 + 1];
                tex.pixels[p * 4 + 2] = src_ptr[p * 4 + 0];
                tex.pixels[p * 4 + 3] = src_ptr[p * 4 + 3];
            }
        } else {
            std::memcpy(tex.pixels.data(), src_ptr, expected_size);
        }
        normalize_ddna_normals_if_needed(tex, path);
        return tex;
    }

    size_t block_bytes = (compression == DdsCompressionType::BC1 || compression == DdsCompressionType::BC4) ? 8 : 16;
    size_t required_bytes = static_cast<size_t>(blocks_x) * blocks_y * block_bytes;
    if (file_data.size() < header_offset + required_bytes) {
        std::cerr << "[BudAssetPipeline] Incomplete block compressed DDS data in: " << path << std::endl;
        return std::nullopt;
    }

    uint8_t decompressed_block[16 * 4];

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const uint8_t* block_src = src_ptr + (by * blocks_x + bx) * block_bytes;

            switch (compression) {
            case DdsCompressionType::BC1:
                bcdec_bc1(block_src, decompressed_block, 4 * 4);
                break;
            case DdsCompressionType::BC2:
                bcdec_bc2(block_src, decompressed_block, 4 * 4);
                break;
            case DdsCompressionType::BC3:
                bcdec_bc3(block_src, decompressed_block, 4 * 4);
                break;
            case DdsCompressionType::BC4: {
                uint8_t r_block[16];
                bcdec_bc4(block_src, r_block, 4);
                for (int k = 0; k < 16; ++k) {
                    decompressed_block[k * 4 + 0] = r_block[k];
                    decompressed_block[k * 4 + 1] = r_block[k];
                    decompressed_block[k * 4 + 2] = r_block[k];
                    decompressed_block[k * 4 + 3] = 255;
                }
                break;
            }
            case DdsCompressionType::BC5: {
                uint8_t rg_block[16 * 2];
                bcdec_bc5(block_src, rg_block, 4 * 2);
                for (int k = 0; k < 16; ++k) {
                    float nx = (rg_block[k * 2 + 0] / 255.0f) * 2.0f - 1.0f;
                    float ny = (rg_block[k * 2 + 1] / 255.0f) * 2.0f - 1.0f;
                    float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                    decompressed_block[k * 4 + 0] = rg_block[k * 2 + 0];
                    decompressed_block[k * 4 + 1] = rg_block[k * 2 + 1];
                    decompressed_block[k * 4 + 2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
                    decompressed_block[k * 4 + 3] = 255;
                }
                break;
            }
            case DdsCompressionType::BC7:
                bcdec_bc7(block_src, decompressed_block, 4 * 4);
                break;
            default:
                break;
            }

            for (uint32_t py = 0; py < 4; ++py) {
                uint32_t y = by * 4 + py;
                if (y >= height) break;
                for (uint32_t px = 0; px < 4; ++px) {
                    uint32_t x = bx * 4 + px;
                    if (x >= width) break;
                    size_t dst_idx = (static_cast<size_t>(y) * width + x) * 4;
                    size_t src_idx = (static_cast<size_t>(py) * 4 + px) * 4;
                    tex.pixels[dst_idx + 0] = decompressed_block[src_idx + 0];
                    tex.pixels[dst_idx + 1] = decompressed_block[src_idx + 1];
                    tex.pixels[dst_idx + 2] = decompressed_block[src_idx + 2];
                    tex.pixels[dst_idx + 3] = decompressed_block[src_idx + 3];
                }
            }
        }
    }

    normalize_ddna_normals_if_needed(tex, path);
    return tex;
}

void apply_companion_mask_if_present(RawTexture& tex, const std::string& path) {
    if (tex.pixels.empty() || tex.channels < 4) return;

    size_t total_px = static_cast<size_t>(tex.width) * tex.height;
    bool all_opaque = true;
    const uint8_t* p = tex.pixels.data();
    for (size_t k = 0; k < total_px; ++k) {
        if (p[k * 4 + 3] < 250) {
            all_opaque = false;
            break;
        }
    }

    if (!all_opaque) return;

    std::filesystem::path p_path(path);
    std::filesystem::path dir = p_path.parent_path();
    std::string stem = p_path.stem().string();

    std::vector<std::string> stem_candidates;
    stem_candidates.push_back(stem + "_mask");
    stem_candidates.push_back(stem + "_a");
    stem_candidates.push_back(stem + "_alpha");
    stem_candidates.push_back(stem + "_opacity");

    // Replace _D or _d with _A, _a, _Opacity, _Mask
    auto try_replace_token = [&](const std::string& target_tok, const std::string& rep_tok) {
        size_t pos = stem.rfind(target_tok);
        if (pos != std::string::npos) {
            std::string s = stem;
            s.replace(pos, target_tok.length(), rep_tok);
            stem_candidates.push_back(s);
        }
    };
    try_replace_token("_D", "_A");
    try_replace_token("_d", "_a");
    try_replace_token("_D", "_Alpha");
    try_replace_token("_d", "_alpha");
    try_replace_token("_D", "_Opacity");
    try_replace_token("_d", "_opacity");
    try_replace_token("_D", "_Mask");
    try_replace_token("_d", "_mask");
    try_replace_token("_Diffuse", "_Alpha");
    try_replace_token("_diffuse", "_alpha");
    try_replace_token("_BaseColor", "_Alpha");
    try_replace_token("_basecolor", "_alpha");

    const std::string exts[] = { ".tga", ".TGA", ".png", ".PNG", ".dds", ".DDS", ".jpg", ".jpeg", ".bmp" };

    for (const auto& sc : stem_candidates) {
        for (const auto& ext : exts) {
            std::filesystem::path mask_file = dir / (sc + ext);
            if (std::filesystem::exists(mask_file)) {
                int mw = 0, mh = 0, mc_channels = 0;
                stbi_uc* mdata = stbi_load(mask_file.string().c_str(), &mw, &mh, &mc_channels, 1);
                if (mdata && mw > 0 && mh > 0) {
                    std::cout << "[BudAssetPipeline] Merged companion alpha mask: " << mask_file.string() << " -> " << path << std::endl;
                    uint8_t* dst = tex.pixels.data();
                    for (uint32_t y = 0; y < tex.height; ++y) {
                        uint32_t my = (static_cast<uint64_t>(y) * mh) / tex.height;
                        for (uint32_t x = 0; x < tex.width; ++x) {
                            uint32_t mx = (static_cast<uint64_t>(x) * mw) / tex.width;
                            size_t dst_idx = (static_cast<size_t>(y) * tex.width + x) * 4 + 3;
                            size_t src_idx = static_cast<size_t>(my) * mw + mx;
                            dst[dst_idx] = mdata[src_idx];
                        }
                    }
                    stbi_image_free(mdata);
                    return;
                }
                if (mdata)
                    stbi_image_free(mdata);
            }
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

    // Normalize DDNA normals if present
    normalize_ddna_normals_if_needed(tex, resolved_path);

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

    size_t cutout_transparent_px = 0; // alpha < 25 (binary transparent)
    size_t translucent_px = 0;        // 25 <= alpha <= 230 (continuous semi-transparent)
    size_t opaque_px = 0;             // alpha > 230 (binary solid)

    const uint8_t* p = tex.pixels.data();
    for (size_t k = 0; k < total_px; ++k) {
        uint8_t a = p[k * 4 + 3];
        if (a < 25) {
            cutout_transparent_px++;
        } else if (a <= 230) {
            translucent_px++;
        } else {
            opaque_px++;
        }
    }

    size_t non_opaque_px = cutout_transparent_px + translucent_px;

    // If at least 0.05% of the texture has non-opaque pixels:
    if (non_opaque_px > (total_px / 2000)) {
        info.has_alpha = true;

        // Smooth Translucent (Blend):
        // 1. Significant continuous semi-transparent pixels (> 2%) that dominate over cutout background (cutout_transparent_px * 2 < translucent_px).
        // 2. OR almost zero cutout background (cutout_transparent_px <= 0.1%) but broad semi-transparent surface (> 1%).
        if ((translucent_px > (total_px / 50) && translucent_px > cutout_transparent_px * 2) ||
            (cutout_transparent_px <= (total_px / 1000) && translucent_px > (total_px / 100))) {
            info.alpha_mode = bud::asset::AlphaMode::Blend;
            info.alpha_cutoff = 0.0f;
        } else {
            // Cutout / Mask (AlphaTest):
            // Foliage leaves, branches, flowers, grass, fences, grates, paper cards.
            // Dominated by binary transparent cutout background with thin anti-aliased edge filter.
            info.alpha_mode = bud::asset::AlphaMode::Mask;
            info.alpha_cutoff = 0.5f;
        }
    } else {
        info.has_alpha = false;
        info.alpha_mode = bud::asset::AlphaMode::Opaque;
        info.alpha_cutoff = 0.0f;
    }

    return info;
}

TextureAlphaInfo TextureImporter::analyze_alpha(const std::string& path) {
    auto tex = import_from_file(path);
    if (!tex)
        return {};
    return analyze_alpha(*tex);
}

TextureSemantic TextureImporter::detect_semantic_from_filename(const std::string& filename) {
    std::string s = filename;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    std::filesystem::path fp(s);
    std::string stem = fp.stem().string();

    // Check composite packed formats first
    if (stem.find("_orm") != std::string::npos || stem.find("_arm") != std::string::npos ||
        stem.find("_mro") != std::string::npos || stem.find("metallicroughness") != std::string::npos ||
        stem.find("metallic_roughness") != std::string::npos) {
        return TextureSemantic::MetallicRoughness;
    }

    // Split stem into tokens separated by '_', '-', '.', ' '
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : stem) {
        if (c == '_' || c == '-' || c == '.' || c == ' ') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);

    for (const auto& tok : tokens) {
        // Normal
        if (tok == "ddna" || tok == "ddn" || tok == "normal" || tok == "norm" || tok == "nrm" ||
            tok == "nor" || tok == "nm" || tok == "bump" || tok == "n") {
            return TextureSemantic::Normal;
        }
        // Specular / Glossiness
        if (tok == "spec" || tok == "specular" || tok == "spc") {
            return TextureSemantic::SpecularGlossiness;
        }
        // Roughness
        if (tok == "rough" || tok == "roughness" || tok == "rgh" || tok == "gloss" ||
            tok == "glossiness" || tok == "gls" || tok == "smoothness" || tok == "r") {
            return TextureSemantic::Roughness;
        }
        // Metallic
        if (tok == "metal" || tok == "metallic" || tok == "metalness" || tok == "met" || tok == "m") {
            return TextureSemantic::Metallic;
        }
        // Emissive
        if (tok == "em" || tok == "emissive" || tok == "emit" || tok == "glow" ||
            tok == "illum" || tok == "light" || tok == "e") {
            return TextureSemantic::Emissive;
        }
        // AO
        if (tok == "ao" || tok == "occ" || tok == "occlusion" || tok == "ambientocclusion") {
            return TextureSemantic::Occlusion;
        }
        // Mask / Alpha
        if (tok == "mask" || tok == "alpha" || tok == "opacity" || tok == "cutout" || tok == "a") {
            return TextureSemantic::AlphaMask;
        }
        // Albedo / Diffuse
        if (tok == "diff" || tok == "diffuse" || tok == "albedo" || tok == "alb" ||
            tok == "color" || tok == "col" || tok == "basecolor" || tok == "base_color" ||
            tok == "bc" || tok == "d") {
            return TextureSemantic::Albedo;
        }
    }

    return TextureSemantic::Unknown;
}

TextureSemantic TextureImporter::detect_semantic_from_pixels(const RawTexture& tex) {
    if (tex.pixels.empty() || tex.width == 0 || tex.height == 0)
        return TextureSemantic::Unknown;

    // Sparse sampling: 16x16 grid = 256 samples (< 0.001 ms)
    uint32_t step_x = std::max(1u, tex.width / 16);
    uint32_t step_y = std::max(1u, tex.height / 16);

    uint32_t sample_count = 0;
    float sum_r = 0, sum_g = 0, sum_b = 0;
    uint32_t normal_like_count = 0;
    uint32_t grayscale_count = 0;

    for (uint32_t y = step_y / 2; y < tex.height; y += step_y) {
        for (uint32_t x = step_x / 2; x < tex.width; x += step_x) {
            size_t idx = (static_cast<size_t>(y) * tex.width + x) * 4;
            uint8_t r = tex.pixels[idx + 0];
            uint8_t g = tex.pixels[idx + 1];
            uint8_t b = tex.pixels[idx + 2];

            sum_r += r; sum_g += g; sum_b += b;
            sample_count++;

            // Tangent space normal signature: R ~ 128, G ~ 128, B > 180
            if (b > 180 && r >= 90 && r <= 165 && g >= 90 && g <= 165) {
                normal_like_count++;
            }

            // Grayscale signature: R ~= G ~= B
            if (std::abs(static_cast<int>(r) - g) <= 6 && std::abs(static_cast<int>(g) - b) <= 6) {
                grayscale_count++;
            }
        }
    }

    if (sample_count == 0) return TextureSemantic::Unknown;

    if (normal_like_count > sample_count * 0.75f) {
        return TextureSemantic::Normal;
    }

    if (grayscale_count > sample_count * 0.90f) {
        return TextureSemantic::Roughness;
    }

    return TextureSemantic::Albedo;
}

PBRCompanionTextures TextureImporter::find_companion_pbr_textures(const std::string& base_color_path) {
    PBRCompanionTextures pbr{};
    if (base_color_path.empty())
        return pbr;

    std::filesystem::path p(base_color_path);
    std::filesystem::path dir = p.parent_path();
    std::string stem = p.stem().string();

    // Extract prefix stem by stripping known albedo suffixes
    std::string base_prefix = stem;
    const std::string albedo_suffixes[] = {
        "_diff", "_diffuse", "_albedo", "_alb", "_color", "_col",
        "_basecolor", "_base_color", "_bc", "_d", "_d_0", "_0_d", "_D"
    };
    for (const auto& suf : albedo_suffixes) {
        size_t pos = base_prefix.rfind(suf);
        if (pos != std::string::npos && pos + suf.length() == base_prefix.length()) {
            base_prefix = base_prefix.substr(0, pos);
            break;
        }
    }

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec))
        return pbr;

    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file()) {
            std::string estem = entry.path().stem().string();
            if (estem.rfind(base_prefix, 0) == 0) { // Starts with base_prefix
                candidates.push_back(entry.path());
            }
        }
    }

    // Also check sibling "textures/" folder if applicable
    std::filesystem::path sibling_tex_dir = dir / "textures";
    if (std::filesystem::exists(sibling_tex_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(sibling_tex_dir, ec)) {
            if (entry.is_regular_file()) {
                std::string estem = entry.path().stem().string();
                if (estem.rfind(base_prefix, 0) == 0) {
                    candidates.push_back(entry.path());
                }
            }
        }
    }

    for (const auto& cand : candidates) {
        std::string cand_path = cand.generic_string();
        if (cand_path == base_color_path) continue;

        TextureSemantic sem = detect_semantic_from_filename(cand.filename().string());

        // Tier 3: If unknown, sparsely probe pixels
        if (sem == TextureSemantic::Unknown) {
            auto tex_opt = import_from_file(cand_path);
            if (tex_opt) {
                sem = detect_semantic_from_pixels(*tex_opt);
            }
        }

        switch (sem) {
        case TextureSemantic::Normal:
            if (pbr.normal_path.empty()) pbr.normal_path = cand_path;
            break;
        case TextureSemantic::Roughness:
            if (pbr.roughness_path.empty()) pbr.roughness_path = cand_path;
            break;
        case TextureSemantic::Metallic:
            if (pbr.metallic_path.empty()) pbr.metallic_path = cand_path;
            break;
        case TextureSemantic::MetallicRoughness:
            if (pbr.roughness_path.empty()) pbr.roughness_path = cand_path;
            break;
        case TextureSemantic::SpecularGlossiness:
            if (pbr.roughness_path.empty()) {
                std::string mr = convert_spec_gloss_to_metallic_roughness(cand_path);
                if (!mr.empty()) pbr.roughness_path = mr;
            }
            break;
        case TextureSemantic::Emissive:
            if (pbr.emissive_path.empty()) pbr.emissive_path = cand_path;
            break;
        default:
            break;
        }
    }

    return pbr;
}

std::string TextureImporter::convert_spec_gloss_to_metallic_roughness(const std::string& spec_gloss_path, const std::string& output_dir) {
    if (spec_gloss_path.empty()) return "";

    std::filesystem::path p(spec_gloss_path);
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    std::string out_filename = p.stem().string() + "_metallicRoughness.png";
    std::string out_full_path = (std::filesystem::path(output_dir) / out_filename).generic_string();

    if (std::filesystem::exists(out_full_path)) {
        return out_full_path;
    }

    auto tex_opt = import_from_file(spec_gloss_path);
    if (!tex_opt || tex_opt->pixels.empty()) {
        return "";
    }

    // Look for companion _ddna texture to extract high-res glossiness if spec alpha is solid
    std::filesystem::path dir = p.parent_path();
    std::string stem = p.stem().string();
    std::string ddna_path;
    size_t spec_pos = stem.rfind("_spec");
    if (spec_pos != std::string::npos) {
        std::string ddna_stem = stem.substr(0, spec_pos) + "_ddna";
        const std::string exts[] = { ".dds", ".png", ".tga" };
        for (const auto& ext : exts) {
            auto cand = dir / (ddna_stem + ext);
            if (std::filesystem::exists(cand)) {
                ddna_path = cand.generic_string();
                break;
            }
        }
    }

    std::optional<RawTexture> ddna_tex_opt;
    if (!ddna_path.empty()) {
        ddna_tex_opt = import_from_file(ddna_path);
    }

    RawTexture mr_tex;
    mr_tex.width = tex_opt->width;
    mr_tex.height = tex_opt->height;
    mr_tex.channels = 4;
    mr_tex.is_srgb = false;
    mr_tex.pixels.resize(static_cast<size_t>(mr_tex.width) * mr_tex.height * 4);

    const uint8_t* src = tex_opt->pixels.data();
    uint8_t* dst = mr_tex.pixels.data();
    size_t pixel_count = static_cast<size_t>(mr_tex.width) * mr_tex.height;

    for (size_t i = 0; i < pixel_count; ++i) {
        float r = src[i * 4 + 0] / 255.0f;
        float g = src[i * 4 + 1] / 255.0f;
        float b = src[i * 4 + 2] / 255.0f;
        float a = (tex_opt->channels >= 4) ? (src[i * 4 + 3] / 255.0f) : 1.0f;

        // If specular alpha is solid, check if ddna glossiness is available
        if (a > 0.98f && ddna_tex_opt && ddna_tex_opt->pixels.size() == tex_opt->pixels.size()) {
            a = ddna_tex_opt->pixels[i * 4 + 3] / 255.0f;
        }

        // Calculate physical Roughness: Roughness = 1.0 - Glossiness
        // Ensure dielectric surfaces (leather, paint, plastic) don't become mirror 0.05
        float roughness = std::clamp(1.0f - a * 0.85f, 0.15f, 0.95f);

        // Dielectric F0 is ~0.04 (4%). Values significantly higher (> 0.45) indicate conductive metals.
        float max_spec = std::max({ r, g, b });
        float metallic = 0.0f;
        if (max_spec > 0.55f) {
            metallic = std::clamp((max_spec - 0.55f) / 0.45f, 0.0f, 1.0f);
            roughness = std::clamp(1.0f - a, 0.04f, 1.0f);
        }

        // Channel R (Red) = Occlusion (1.0 default)
        // Channel G (Green) = Roughness
        // Channel B (Blue) = Metallic
        // Channel A (Alpha) = 1.0
        dst[i * 4 + 0] = 255;
        dst[i * 4 + 1] = static_cast<uint8_t>(std::clamp(roughness * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 2] = static_cast<uint8_t>(std::clamp(metallic * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 3] = 255;
    }

    if (stbi_write_png(out_full_path.c_str(), static_cast<int>(mr_tex.width), static_cast<int>(mr_tex.height), 4, dst, static_cast<int>(mr_tex.width * 4))) {
        std::cout << "[BudAssetPipeline] Baked Specular-Glossiness -> Standard Metallic-Roughness: "
                  << spec_gloss_path << " -> " << out_full_path << std::endl;
        return out_full_path;
    }
    return "";
}

} // namespace bud::asset_pipeline
