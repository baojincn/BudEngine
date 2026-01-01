module;

#include <cstddef>
#include <concepts>
#include <type_traits>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <functional>

export module bud.core;

export namespace bud::core {
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
