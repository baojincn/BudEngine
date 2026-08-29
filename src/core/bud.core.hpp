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

namespace bud::core::units {
    // -----------------------------------------------------------------------
    // Standard Coordinate & Physical Units Specification (1 Unit == 1 m):
    // Length: 1.0f == 1.0 m  (Meter, SI standard)
    // Time:   1.0f == 1.0 s  (Second)
    // Mass:   1.0f == 1.0 kg (Kilogram)
    // Angle:  Radians internally; degree conversion multipliers provided.
    // -----------------------------------------------------------------------

    // Spatial Length (Base: m)
    inline constexpr float m  = 1.0f;
    inline constexpr float dm = 0.1f * m;
    inline constexpr float cm = 0.01f * m;
    inline constexpr float mm = 0.001f * m;
    inline constexpr float km = 1000.0f * m;

    // Time (Base: s)
    inline constexpr float s   = 1.0f;
    inline constexpr float ms  = 0.001f * s;
    inline constexpr float min = 60.0f * s;
    inline constexpr float h   = 3600.0f * s;

    // Speed / Velocity (Base: m/s)
    inline constexpr float m_per_s  = m / s;
    inline constexpr float cm_per_s = cm / s;
    inline constexpr float km_per_h = km / (3600.0f * s);

    // Acceleration (Base: m/s^2)
    inline constexpr float gravity = 9.80665f * m_per_s / s; // 9.80665 m/s^2

    // Angle conversions
    inline constexpr float pi = 3.14159265358979323846f;
    inline constexpr float deg_to_rad = pi / 180.0f;
    inline constexpr float rad_to_deg = 180.0f / pi;
}

namespace bud::literals {
    // Length Literals (Base: Meter)
    constexpr float operator""_m(long double val) {
        return static_cast<float>(val);
    }
    constexpr float operator""_m(unsigned long long val) {
        return static_cast<float>(val);
    }

    constexpr float operator""_cm(long double val) {
        return static_cast<float>(val * 0.01);
    }
    constexpr float operator""_cm(unsigned long long val) {
        return static_cast<float>(val) * 0.01f;
    }

    constexpr float operator""_mm(long double val) {
        return static_cast<float>(val * 0.001);
    }
    constexpr float operator""_mm(unsigned long long val) {
        return static_cast<float>(val) * 0.001f;
    }

    constexpr float operator""_km(long double val) {
        return static_cast<float>(val * 1000.0);
    }
    constexpr float operator""_km(unsigned long long val) {
        return static_cast<float>(val) * 1000.0f;
    }

    // Time Literals
    constexpr float operator""_s(long double val) {
        return static_cast<float>(val);
    }
    constexpr float operator""_s(unsigned long long val) {
        return static_cast<float>(val);
    }

    constexpr float operator""_ms(long double val) {
        return static_cast<float>(val * 0.001);
    }
    constexpr float operator""_ms(unsigned long long val) {
        return static_cast<float>(val) * 0.001f;
    }

    // Speed / Velocity Literals
    constexpr float operator""_mps(long double val) {
        return static_cast<float>(val * 100.0);
    }
    constexpr float operator""_mps(unsigned long long val) {
        return static_cast<float>(val) * 100.0f;
    }

    constexpr float operator""_kmh(long double val) {
        return static_cast<float>(val * (100000.0 / 3600.0));
    }
    constexpr float operator""_kmh(unsigned long long val) {
        return static_cast<float>(val * (100000.0f / 3600.0f));
    }

    constexpr float operator""_cmps(long double val) {
        return static_cast<float>(val);
    }
    constexpr float operator""_cmps(unsigned long long val) {
        return static_cast<float>(val);
    }

    // Acceleration Literals
    constexpr float operator""_mps2(long double val) {
        return static_cast<float>(val * 100.0);
    }
    constexpr float operator""_mps2(unsigned long long val) {
        return static_cast<float>(val) * 100.0f;
    }

    // Angle Literals
    constexpr float operator""_deg(long double val) {
        return static_cast<float>(val * (3.14159265358979323846 / 180.0));
    }
    constexpr float operator""_deg(unsigned long long val) {
        return static_cast<float>(val) * (3.14159265358979323846f / 180.0f);
    }

    constexpr float operator""_rad(long double val) {
        return static_cast<float>(val);
    }
    constexpr float operator""_rad(unsigned long long val) {
        return static_cast<float>(val);
    }
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
