#pragma once

#include "src/graphics/bud.graphics.types.hpp"

namespace bud::ui {

	class StatsUI {
	public:
		// Call this every frame inside the ImGui block.
		// sequencer_status: optional string appended same-line after FPS (e.g. "| REC [12 kf]")
		static void render(const bud::graphics::RenderStats& stats, float delta_time,
		                   std::string_view sequencer_status = "");
	};

}
