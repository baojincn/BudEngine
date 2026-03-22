#pragma once

#include <cstddef>
#include <concepts>
#include <type_traits>
#include <iostream>
#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include <array>
#include <functional>

namespace bud::core {
    using byte = std::byte;

    template<typename T>
    concept arithmetic = std::integral<T> || std::floating_point<T>;

    template<typename T>
    concept numeric = arithmetic<T> || requires(T t) {
        t + t; t - t; t* t; t / t;
    };

    // C++23 helper function
    template<typename T>
    inline constexpr bool is_numeric_v = numeric<T>;
}

// Logging implementation moved to a dedicated header/source pair.
#ifndef NDEBUG
#include "bud.logger.hpp"
#else
namespace bud {
    template<typename... Args>
    constexpr void print([[maybe_unused]] Args&&...) noexcept {}
    template<typename... Args>
    constexpr void eprint([[maybe_unused]] Args&&...) noexcept {}
    inline void set_log_backend_mask([[maybe_unused]] uint32_t) noexcept {}
    inline uint32_t get_log_backend_mask() noexcept { return 0; }
    inline void set_log_file([[maybe_unused]] const std::string&) noexcept {}
}
#endif
