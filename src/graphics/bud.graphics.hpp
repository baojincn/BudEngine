#pragma once

#include <memory>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"

namespace bud::platform { class Window; }

namespace bud::graphics {
	std::unique_ptr<RHI> create_rhi(Backend backend);
}
