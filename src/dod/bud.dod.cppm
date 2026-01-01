// src/bud.dod.cppm

#include <vector>
#include <algorithm>
#include <thread>
#include <execution>

export module bud.dod;

import bud.core;

export namespace bud::dod {
    // Data-Oriented Design Registry
    template<typename... Components>
    class Registry {
        std::vector<std::vector<std::byte>> component_data_;

    public:
        template<typename T>
        std::vector<T>& get_component_vector() {
            // This is a simplified implementation
            // In real code, you'd need proper component tracking
            static std::vector<T> dummy;
            return dummy;
        }

        template<typename Func>
        void parallel_for(Func&& func) {
            // Placeholder for parallel iteration
            // Real implementation would iterate over component data
        }
    };
}
