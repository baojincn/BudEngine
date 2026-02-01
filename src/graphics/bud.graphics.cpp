
#include <memory>
#include <stdexcept>

#include "src/graphics/bud.graphics.hpp"
#include "src/graphics/vulkan/bud.graphics.vulkan.hpp"
// #include "src/graphics/d3d12/bud.graphics.d3d12.hpp"

namespace bud::graphics {

	std::unique_ptr<RHI> create_rhi(Backend backend) {
		switch (backend) {
		case Backend::Vulkan:
			return std::make_unique<bud::graphics::vulkan::VulkanRHI>();

		case Backend::D3D12:
			throw std::runtime_error("D3D12 backend not implemented yet.");

		default:
			throw std::runtime_error("Unknown graphics backend.");
		}
	}
}
