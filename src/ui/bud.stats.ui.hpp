#pragma once

#include "src/graphics/bud.graphics.types.hpp"

namespace bud::ui {

	class StatsUI {
	public:
		// Call this every frame inside the ImGui block
		static void render(const bud::graphics::RenderStats& stats, float delta_time);
	};

}
