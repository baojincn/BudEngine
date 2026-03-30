#pragma once

#include "src/graphics/bud.graphics.types.hpp"

// Forward-declare SequencerState so UI header doesn't need to include sequencer implementation.
namespace bud { namespace scene { enum class SequencerState : int; } }

namespace bud::ui {

    class StatsUI {
    public:
        // Call this every frame inside the ImGui block.
        // The UI will build sequencer status string itself if a CameraSequencer pointer is provided.
        static void render(const bud::graphics::RenderStats& stats, float delta_time,
                           bud::scene::SequencerState sequencer_state,
                           size_t keyframe_count,
                           size_t playback_index,
                           bool is_paused,
                           bool is_looping,
                           bool show_stats = true);
    };

}
