#pragma once

#include "src/graphics/bud.graphics.types.hpp"
#include <functional>

// Forward-declare SequencerState so UI header doesn't need to include sequencer implementation.
namespace bud { namespace scene { enum class SequencerState : int; } }

namespace bud::ui {

    class StatsUI {
    public:
        // Call this every frame inside the ImGui block.
        // The UI will build sequencer status string itself if a CameraSequencer pointer is provided.
        // set_occluder: optional callback that will be invoked with a new occluder fraction [0,1]
        // current_occluder: optional current value to display; set to negative to hide control.
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
                           bool current_occluder_enable = true);
    };

}
