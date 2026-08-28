#include "asset_format.hpp"

namespace bud::asset_pipeline {

uint64_t compute_asset_id(const std::string& path) {
    // 64-bit FNV-1a hash
    uint64_t hash = 14695981039346656037ull;
    for (char c : path) {
        char normalized_c = (c == '\\') ? '/' : (char)tolower(c);
        hash ^= (uint64_t)(unsigned char)normalized_c;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace bud::asset_pipeline
