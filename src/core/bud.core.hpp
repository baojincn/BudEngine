#pragma once

#include <cstddef>
#include <concepts>
#include <type_traits>
#include <iostream>
#include <string>
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

// 
// bud::print / bud::eprint  --  debug-only logging wrappers
//   Debug builds  : forwards to std::println (stdout / stderr)
//   Release/Profile: empty no-op, fully eliminated by the optimizer
// Usage:
//   bud::print("Hello {}", value);
//   bud::eprint("Error: {}", msg);
// 
#ifndef NDEBUG
    #include <print>
    namespace bud {
        template<typename... Args>
        inline void print(std::format_string<Args...> fmt, Args&&... args) {
            std::println(fmt, std::forward<Args>(args)...);
        }
        template<typename... Args>
        inline void eprint(std::format_string<Args...> fmt, Args&&... args) {
            std::println(stderr, fmt, std::forward<Args>(args)...);
        }
    }
#else
    namespace bud {
        template<typename... Args>
        constexpr void print([[maybe_unused]] Args&&...) noexcept {}
        template<typename... Args>
        constexpr void eprint([[maybe_unused]] Args&&...) noexcept {}
    }
#endif
