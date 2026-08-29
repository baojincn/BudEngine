#pragma once

#include "src/graphics/bud.graphics.types.hpp"
#include <functional>

namespace bud { namespace scene { enum class SequencerState : int; } }

namespace bud::ui {

    class StatsUI {
    public:
        static void render(const bud::graphics::RenderStats& stats, float delta_time,
                           bud::scene::SequencerState sequencer_state,
                           size_t keyframe_count,
                           size_t playback_index,
                           bool is_paused,
                           bool is_looping,
                           bool show_stats = true,
                           std::function<void(float)> set_occluder = nullptr,
                           float current_occluder = -1.0f,
                           std::function<void(bool)> set_occluder_enable = nullptr,
                           bool current_occluder_enable = true,
                           std::function<void(bud::graphics::AOMode)> set_ao_mode = nullptr,
                           bud::graphics::AOMode current_ao_mode = bud::graphics::AOMode::GTAO,
                           std::function<void(bool)> set_ssr_enable = nullptr,
                           bool current_ssr_enable = true,
                           std::function<void(bool)> set_ssgi_enable = nullptr,
                           bool current_ssgi_enable = true,
                           std::function<void(float)> set_ssgi_intensity = nullptr,
                           float current_ssgi_intensity = 1.5f,
                           std::function<void(float)> set_ssgi_blend = nullptr,
                           float current_ssgi_blend = 0.05f);
    };

}
