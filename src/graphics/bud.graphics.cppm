module;

#include <memory>

export module bud.graphics;

export import bud.graphics.types;

export import bud.graphics.rhi;

export namespace bud::graphics {
	export std::unique_ptr<RHI> create_rhi(Backend backend);
}
